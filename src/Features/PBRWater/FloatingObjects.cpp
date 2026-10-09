#include "FloatingObjects.h"

#include "Features/PBRWater.h"
#include "Utils/Game.h"

#include "RE/B/bhkPickData.h"
#include "RE/T/TESHavokUtilities.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float MetresPerUnit = 1.0f / UnitsPerMetre;
	constexpr float Gravity = static_cast<float>(PBRWaterModel::Gravity);
	constexpr float WaterDensity = 1000.0f;  // kg/m^3
	constexpr float ActorMassKg = 80.0f;     // an adult on board
	constexpr float SkyrimToHavok = 0.0142875f;
	constexpr float ScanInterval = 1.0f;       // s between looks for new floating objects
	constexpr float RescanDistance = 2048.0f;  // units the player may move before an early rescan
	constexpr float CollisionRange = 4500.0f;  // units: within this distance collision follows the hull
	constexpr float MaxTilt = 0.35f;           // rad
	constexpr float SampleSpread = 0.7f;       // share of the half extents sampled for the tilt
	constexpr float HeaveDamping = 0.3f;       // damping ratios (radiation damping of a small hull)
	constexpr float TiltDamping = 0.35f;
	constexpr float MaxStep = 1.0f / 60.0f;
	constexpr float FootprintMargin = 50.0f;  // units around a hull in which its parts are looked for

	using MotionType = RE::hkpMotion::MotionType;

	struct WorldBox
	{
		RE::NiPoint3 centre;
		RE::NiPoint3 axis[3];  // unit axes of the bounds
		RE::NiPoint3 half;     // half extents along them (units)
		float minZ = 0.0f;
		float maxZ = 0.0f;
		bool degenerate = false;  // no usable bounds (lights, markers): a point at the reference
	};

	RE::NiPoint3 Column(const RE::NiMatrix3& m, int c)
	{
		return { m.entry[0][c], m.entry[1][c], m.entry[2][c] };
	}

	RE::NiMatrix3 FromColumns(const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c)
	{
		RE::NiMatrix3 m;
		m.entry[0][0] = a.x, m.entry[0][1] = b.x, m.entry[0][2] = c.x;
		m.entry[1][0] = a.y, m.entry[1][1] = b.y, m.entry[1][2] = c.y;
		m.entry[2][0] = a.z, m.entry[2][1] = b.z, m.entry[2][2] = c.z;
		return m;
	}

	float Smoothstep(float e0, float e1, float x)
	{
		const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-3f), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/// World box of a reference from its base object's bounds and its 3D's world transform.
	WorldBox GetWorldBox(RE::TESObjectREFR* ref, const RE::NiAVObject* node)
	{
		WorldBox box;
		const auto& t = node->world;
		const float scale = t.scale > 0.0f ? t.scale : 1.0f;
		const RE::NiPoint3 bmin = ref->GetBoundMin();
		const RE::NiPoint3 bmax = ref->GetBoundMax();
		const RE::NiPoint3 extent = bmax - bmin;
		for (int i = 0; i < 3; ++i)
			box.axis[i] = Column(t.rotate, i);
		box.degenerate = !(extent.x > 1.0f && extent.y > 1.0f && extent.z > 1.0f);
		if (box.degenerate) {
			box.centre = t.translate;
			box.half = { 1.0f, 1.0f, 1.0f };
		} else {
			box.centre = t.translate + t.rotate * ((bmin + bmax) * (0.5f * scale));
			box.half = extent * (0.5f * scale);
		}
		const float reach = std::abs(box.axis[0].z) * box.half.x + std::abs(box.axis[1].z) * box.half.y + std::abs(box.axis[2].z) * box.half.z;
		box.minZ = box.centre.z - reach;
		box.maxZ = box.centre.z + reach;
		return box;
	}

	bool FlatWaterHeight(RE::TESObjectREFR* ref, const RE::NiPoint3& position, float& height)
	{
		auto* cell = ref->GetParentCell();
		return cell && PBRWater::TESObjectCELL_GetWaterHeight::func(cell, position, height) && height > -1e6f;
	}

	bool IsMarker(const RE::TESBoundObject* base)
	{
		const auto* stat = base ? base->As<RE::TESObjectSTAT>() : nullptr;
		return stat && (stat->GetFormFlags() & RE::TESObjectSTAT::RecordFlags::kIsMarker) != 0;
	}

	bool IsHullType(RE::FormType type)
	{
		using enum RE::FormType;
		return type == Static || type == MovableStatic || type == Activator || type == Furniture;
	}

	bool IsPartType(RE::FormType type)
	{
		using enum RE::FormType;
		return IsHullType(type) || type == Container || type == Light || type == Door || type == Flora || type == TalkingActivator;
	}

	bool IsDynamicMotion(const RE::hkpRigidBody* body)
	{
		using Type = RE::hkpMotion::MotionType;
		const auto type = body->motion.type.get();
		return type == Type::kDynamic || type == Type::kSphereInertia || type == Type::kBoxInertia || type == Type::kThinBoxInertia;
	}

	/// Havok props are dynamic bodies; they float through PBR Water's buoyancy instead.
	RE::bhkRigidBody* FindDynamicBody(RE::NiAVObject* node)
	{
		RE::bhkRigidBody* dynamic = nullptr;
		RE::BSVisit::TraverseScenegraphCollision(node, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
			auto* body = object->body.get() ? object->body.get()->AsBhkRigidBody() : nullptr;
			auto* rigid = body ? body->GetRigidBody() : nullptr;
			if (rigid && IsDynamicMotion(rigid)) {
				dynamic = body;
				return RE::BSVisit::BSVisitControl::kStop;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return dynamic;
	}

	/// Waterfalls, foam, fog and water planes are effect or water shader meshes; hulls are lit.
	bool HasLitGeometry(RE::NiAVObject* node)
	{
		bool lit = false;
		RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* geometry) -> RE::BSVisit::BSVisitControl {
			auto* property = static_cast<RE::BSShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
			if (property && property->GetMaterialType() == RE::BSShaderMaterial::Type::kLighting) {
				lit = true;
				return RE::BSVisit::BSVisitControl::kStop;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return lit;
	}

	/// The landscape reaches up to within `clearance` of the box's bottom anywhere under it.
	bool LandBelow(RE::TES* tes, const WorldBox& box, float clearance)
	{
		const float u[5] = { 0.0f, SampleSpread, -SampleSpread, 0.0f, 0.0f };
		const float v[5] = { 0.0f, 0.0f, 0.0f, SampleSpread, -SampleSpread };
		for (int i = 0; i < 5; ++i) {
			RE::NiPoint3 p = box.centre + box.axis[0] * (u[i] * box.half.x) + box.axis[1] * (v[i] * box.half.y);
			p.z = box.minZ;
			float land = 0.0f;
			if (tes->GetLandHeight(p, land) && land > box.minZ - clearance)
				return true;
		}
		return false;
	}

	/// Casts a ray straight down from `from` over `length` units. Returns whether anything solid was hit and
	/// the reference it belongs to (null for the landscape).
	bool PickBelow(RE::TES* tes, const RE::NiPoint3& from, float length, RE::TESObjectREFR*& hitRef, float& hitZ)
	{
		RE::bhkPickData pick{};
		const RE::NiPoint3 to = { from.x, from.y, from.z - length };
		pick.rayInput.from = RE::hkVector4(from.x * SkyrimToHavok, from.y * SkyrimToHavok, from.z * SkyrimToHavok, 0.0f);
		pick.rayInput.to = RE::hkVector4(to.x * SkyrimToHavok, to.y * SkyrimToHavok, to.z * SkyrimToHavok, 0.0f);
		tes->Pick(pick);
		hitRef = nullptr;
		if (!pick.rayOutput.rootCollidable)
			return false;
		hitRef = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
		hitZ = from.z - length * pick.rayOutput.hitFraction;
		return true;
	}

	void ApplyTransform(RE::NiAVObject* node, const RE::NiPoint3& position, const RE::NiMatrix3& rotation)
	{
		node->local.translate = position;
		node->local.rotate = rotation;
		RE::NiUpdateData data;
		data.flags = RE::NiUpdateData::Flag::kNone;
		data.time = 0.0f;
		node->UpdateTransformAndBounds(data);
	}

	RE::FormID ParseBobbingForm(const std::string& text)
	{
		// "0xLOCALID~Plugin.esp"
		const auto separator = text.find('~');
		if (separator == std::string::npos)
			return 0;
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler)
			return 0;
		try {
			const RE::FormID local = static_cast<RE::FormID>(std::stoul(text.substr(0, separator), nullptr, 16));
			return dataHandler->LookupFormID(local, text.substr(separator + 1));
		} catch (...) {
			return 0;
		}
	}
}

struct FloatingObjects::Candidate
{
	RE::TESObjectREFR* ref = nullptr;
	WorldBox box;
	float flatZ = 0.0f;
	float area = 0.0f;
	bool merged = false;
};

// ============================================================================
// Detection
// ============================================================================

void FloatingObjects::LoadBobbingFrameworkExclusions()
{
	bobbingLoaded = true;
	if (!GetModuleHandleW(L"BobbingFramework.dll"))
		return;
	const std::filesystem::path folder = "Data/SKSE/Plugins/BobbingFramework";
	std::error_code ec;
	if (!std::filesystem::is_directory(folder, ec))
		return;
	for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".json")
			continue;
		try {
			std::ifstream file(entry.path());
			const json config = json::parse(file, nullptr, false);
			if (config.is_discarded())
				continue;
			if (config.contains("FormID") && config["FormID"].is_string()) {
				if (const auto id = ParseBobbingForm(config["FormID"].get<std::string>()))
					bobbingExcluded.insert(id);
			}
			if (config.contains("children") && config["children"].is_array()) {
				for (const auto& child : config["children"]) {
					if (child.is_string()) {
						if (const auto id = ParseBobbingForm(child.get<std::string>()))
							bobbingExcluded.insert(id);
					}
				}
			}
		} catch (...) {
		}
	}
	logger::info("[PBR Water] Bobbing Framework animates {} forms; floating objects leave them to it", bobbingExcluded.size());
}

void FloatingObjects::Scan(const Settings& settings)
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;

	auto isExcluded = [&](RE::TESObjectREFR* ref, const RE::TESBoundObject* base) {
		return bobbingExcluded.contains(ref->GetFormID()) || (base && bobbingExcluded.contains(base->GetFormID()));
	};

	std::vector<RE::TESObjectREFR*> refs;
	std::unordered_set<RE::FormID> present;
	tes->ForEachReferenceInRange(player, settings.range, [&](RE::TESObjectREFR* ref) {
		if (ref && !ref->IsDisabled() && !ref->IsDeleted() && ref->Is3DLoaded() && !ref->As<RE::Actor>()) {
			refs.push_back(ref);
			present.insert(ref->GetFormID());
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	// Hulls that left the area or unloaded go back where they were placed.
	for (auto it = floaters.begin(); it != floaters.end();) {
		if (!present.contains(it->first)) {
			Release(it->second);
			it = floaters.erase(it);
		} else {
			++it;
		}
	}
	std::erase_if(rejected, [&](RE::FormID id) { return !present.contains(id); });

	// Look at the references not examined yet.
	std::vector<RE::TESObjectREFR*> fresh;
	std::vector<Candidate> hulls;
	for (auto* ref : refs) {
		const auto id = ref->GetFormID();
		if (owner.contains(id) || rejected.contains(id))
			continue;
		fresh.push_back(ref);

		auto* base = ref->GetBaseObject();
		auto* node = ref->Get3D();
		if (!base || !node || !IsHullType(base->GetFormType()) || IsMarker(base) || ref->IsWater() || isExcluded(ref, base))
			continue;

		Candidate c;
		c.ref = ref;
		c.box = GetWorldBox(ref, node);
		const auto& box = c.box;
		// Placed upright, through the water plane.
		if (box.degenerate || box.axis[2].z < 0.85f || !FlatWaterHeight(ref, box.centre, c.flatZ))
			continue;
		const float draft = c.flatZ - box.minZ;
		const float freeboard = box.maxZ - c.flatZ;
		const float height = box.maxZ - box.minZ;
		const float length = 2.0f * std::max(box.half.x, box.half.y);
		if (draft < 2.0f || freeboard < 2.0f)
			continue;
		// Mostly under water (a wreck, a reef) or shaped like a post, a column or a waterfall.
		if (draft > 0.8f * height || height > 3.0f * length)
			continue;
		if (length > settings.maxSize || length < 0.3f * UnitsPerMetre)
			continue;
		if (!HasLitGeometry(node) || FindDynamicBody(node))
			continue;
		// Not resting on the bottom: the landscape lies below the keel and nothing solid is right under it.
		const float clearance = std::max(0.3f * draft, 0.25f * UnitsPerMetre);
		if (LandBelow(tes, box, clearance))
			continue;
		RE::TESObjectREFR* below = nullptr;
		float belowZ = 0.0f;
		if (PickBelow(tes, { box.centre.x, box.centre.y, box.minZ - 1.0f }, clearance, below, belowZ))
			continue;

		c.area = box.half.x * box.half.y;
		hulls.push_back(c);
	}

	// Hull parts placed as separate references (hull and deck, hull halves) become one hull: a candidate
	// whose centre lies inside a larger one's footprint joins it. Ice floes merely touching stay apart.
	std::sort(hulls.begin(), hulls.end(), [](const Candidate& a, const Candidate& b) { return a.area > b.area; });

	auto insideFootprint = [](const Floater& f, const RE::NiPoint3& point, float margin) {
		const RE::NiPoint3 d = point - f.pivot;
		return std::abs(d.Dot(f.axisX)) <= f.halfLength + margin && std::abs(d.Dot(f.axisY)) <= f.halfWidth + margin;
	};

	auto makePart = [](RE::TESObjectREFR* ref) {
		Part part;
		part.ref = ref->GetHandle();
		part.id = ref->GetFormID();
		if (auto* node = ref->Get3D()) {
			part.node = node;
			part.restPos = node->local.translate;
			part.restRot = node->local.rotate;
		}
		return part;
	};

	// Grow a hull to cover another box: half extents about its pivot, draft and freeboard.
	auto extendHull = [](Floater& f, const WorldBox& box) {
		for (int corner = 0; corner < 8; ++corner) {
			const RE::NiPoint3 p = box.centre + box.axis[0] * ((corner & 1) ? box.half.x : -box.half.x) +
			                       box.axis[1] * ((corner & 2) ? box.half.y : -box.half.y) +
			                       box.axis[2] * ((corner & 4) ? box.half.z : -box.half.z);
			const RE::NiPoint3 d = p - f.pivot;
			f.halfLength = std::max(f.halfLength, std::abs(d.Dot(f.axisX)));
			f.halfWidth = std::max(f.halfWidth, std::abs(d.Dot(f.axisY)));
		}
		f.draft = std::max(f.draft, f.flatZ - box.minZ);
		f.freeboard = std::max(f.freeboard, box.maxZ - f.flatZ);
	};

	std::vector<RE::FormID> created;
	for (size_t i = 0; i < hulls.size(); ++i) {
		auto& lead = hulls[i];
		if (lead.merged)
			continue;

		// A part of a hull found earlier.
		bool joined = false;
		for (auto& [hullId, existing] : floaters) {
			if (std::abs(existing.flatZ - lead.flatZ) < 8.0f && insideFootprint(existing, lead.box.centre, 0.0f)) {
				auto part = makePart(lead.ref);
				existing.parts.push_back(part);
				owner[part.id] = hullId;
				joined = true;
				break;
			}
		}
		if (joined)
			continue;

		Floater f;
		f.hull = makePart(lead.ref);
		RE::NiPoint3 axisX = { lead.box.axis[0].x, lead.box.axis[0].y, 0.0f };
		const float axisLength = axisX.Length();
		f.axisX = axisLength > 1e-3f ? axisX / axisLength : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
		f.axisY = RE::NiPoint3{ 0.0f, 0.0f, 1.0f }.Cross(f.axisX);
		f.pivot = { lead.box.centre.x, lead.box.centre.y, lead.flatZ };
		f.flatZ = lead.flatZ;
		extendHull(f, lead.box);

		for (size_t j = i + 1; j < hulls.size(); ++j) {
			auto& other = hulls[j];
			if (!other.merged && std::abs(other.flatZ - f.flatZ) < 8.0f && insideFootprint(f, other.box.centre, 0.0f)) {
				other.merged = true;
				f.parts.push_back(makePart(other.ref));
				extendHull(f, other.box);
			}
		}

		// A box hull: displaced mass rho A d, heave period 2 pi sqrt(d / g); it tilts somewhat slower.
		const float lengthM = 2.0f * f.halfLength * MetresPerUnit;
		const float widthM = 2.0f * f.halfWidth * MetresPerUnit;
		const float draftM = std::clamp(f.draft * MetresPerUnit, 0.05f, 8.0f);
		f.massKg = WaterDensity * std::max(lengthM * widthM, 0.01f) * draftM;
		f.omegaHeave = std::clamp(std::sqrt(Gravity / draftM), 0.5f, 8.0f);
		f.omegaTilt = f.omegaHeave * 0.75f;

		const auto hullId = f.hull.id;
		owner[hullId] = hullId;
		for (const auto& part : f.parts)
			owner[part.id] = hullId;
		floaters.emplace(hullId, std::move(f));
		created.push_back(hullId);
	}

	// Attach what rides a hull: references resting on it, or within its footprint and held by nothing else
	// (masts, sails, lanterns). New hulls look at every reference, known hulls only at new ones.
	auto tryAttach = [&](Floater& f, RE::FormID hullId, RE::TESObjectREFR* ref) {
		auto* base = ref->GetBaseObject();
		auto* node = ref->Get3D();
		if (!base || !node || !IsPartType(base->GetFormType()) || IsMarker(base) || ref->IsWater() || isExcluded(ref, base))
			return false;
		const WorldBox box = GetWorldBox(ref, node);
		if (!insideFootprint(f, box.centre, FootprintMargin))
			return false;
		const float keelZ = f.flatZ - f.draft;
		if (box.minZ < keelZ - 10.0f || box.minZ > f.flatZ + f.freeboard + 10.0f)
			return false;
		if (FindDynamicBody(node))
			return false;
		if (!box.degenerate && LandBelow(tes, box, 10.0f))
			return false;
		// Whatever is directly under it, down to the keel, must be this hull or something riding it.
		RE::TESObjectREFR* below = nullptr;
		float belowZ = 0.0f;
		const float start = box.minZ - 1.0f;
		if (start > keelZ && PickBelow(tes, { box.centre.x, box.centre.y, start }, start - keelZ + 5.0f, below, belowZ)) {
			const auto it = below ? owner.find(below->GetFormID()) : owner.end();
			if (it == owner.end() || it->second != hullId)
				return false;
		}
		f.parts.push_back(makePart(ref));
		owner[ref->GetFormID()] = hullId;
		return true;
	};

	std::unordered_set<RE::FormID> createdSet(created.begin(), created.end());
	for (auto* ref : refs) {
		const auto id = ref->GetFormID();
		if (owner.contains(id))
			continue;
		const bool isFresh = !rejected.contains(id);
		for (auto& [hullId, f] : floaters) {
			if (!isFresh && !createdSet.contains(hullId))
				continue;
			if (tryAttach(f, hullId, ref))
				break;
		}
	}

	for (auto* ref : fresh) {
		if (!owner.contains(ref->GetFormID()))
			rejected.insert(ref->GetFormID());
	}

	// Keep loose props on the hulls near the player awake, so they react as the deck moves under them.
	for (auto* ref : refs) {
		if (owner.contains(ref->GetFormID()) || ref->GetPosition().GetSquaredDistance(player->GetPosition()) > CollisionRange * CollisionRange)
			continue;
		auto* node = ref->Get3D();
		auto* body = node ? FindDynamicBody(node) : nullptr;
		if (!body)
			continue;
		for (auto& [hullId, f] : floaters) {
			if (f.keyframed && insideFootprint(f, ref->GetPosition(), 0.0f) && ref->GetPosition().z > f.flatZ - f.draft) {
				body->SetLinearImpulse(RE::hkVector4(0.0f, 0.0f, 0.0f, 0.0f));
				break;
			}
		}
	}
}

// ============================================================================
// Motion
// ============================================================================

void FloatingObjects::UpdateFloater(Floater& f, const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt, const RE::NiPoint3& camera)
{
	// Same distance fade as the rendered wave geometry, so hulls far away settle with the flattening sea.
	const float distance = (f.pivot - camera).Length();
	const float fade = (1.0f - Smoothstep(settings.fadeStart, settings.fadeEnd, distance)) *
	                   (1.0f - Smoothstep(settings.range * 0.85f, settings.range, distance));
	const float gain = settings.response * fade;

	// The water parcels under the hull: centre, then both ends of each axis.
	const float su = SampleSpread * f.halfLength;
	const float sv = SampleSpread * f.halfWidth;
	const RE::NiPoint3 offsets[5] = { {}, f.axisX * su, f.axisX * -su, f.axisY * sv, f.axisY * -sv };
	double xs[5], ys[5];
	for (int i = 0; i < 5; ++i) {
		xs[i] = static_cast<double>(f.pivot.x) + offsets[i].x;
		ys[i] = static_cast<double>(f.pivot.y) + offsets[i].y;
	}
	PBRWaterModel::WaveSnapshot::Displacement d[5];
	snapshot.ParcelDisplacements(xs, ys, 5, f.flatZ, d);

	float meanDz = 0.0f, meanDx = 0.0f, meanDy = 0.0f;
	for (const auto& p : d) {
		meanDz += p.dz;
		meanDx += p.dx;
		meanDy += p.dy;
	}
	meanDz *= 0.2f;
	meanDx *= 0.2f;
	meanDy *= 0.2f;

	// Plane through the parcels: a long hull averages short waves out of its heave and tilt.
	const float slopeX = (d[1].dz - d[2].dz) / std::max(2.0f * su, 1.0f);
	const float slopeY = (d[3].dz - d[4].dz) / std::max(2.0f * sv, 1.0f);
	const float targetHeave = meanDz * gain + f.loadSink;
	const float targetPitch = std::clamp(-std::atan(slopeX) * gain + f.loadPitch, -MaxTilt, MaxTilt);
	const float targetRoll = std::clamp(std::atan(slopeY) * gain + f.loadRoll, -MaxTilt, MaxTilt);

	if (!f.initialised) {
		f.heave = targetHeave;
		f.pitch = targetPitch;
		f.roll = targetRoll;
		f.heaveVel = f.pitchVel = f.rollVel = 0.0f;
		f.initialised = true;
	} else {
		// Second-order response with the hull's own natural frequency (semi-implicit Euler, fixed sub-steps).
		const int steps = std::clamp(static_cast<int>(std::ceil(dt / MaxStep)), 1, 8);
		const float h = dt / static_cast<float>(steps);
		auto spring = [h](float& x, float& v, float target, float omega, float zeta) {
			v += (omega * omega * (target - x) - 2.0f * zeta * omega * v) * h;
			x += v * h;
		};
		for (int s = 0; s < steps; ++s) {
			spring(f.heave, f.heaveVel, targetHeave, f.omegaHeave, HeaveDamping);
			spring(f.pitch, f.pitchVel, targetPitch, f.omegaTilt, TiltDamping);
			spring(f.roll, f.rollVel, targetRoll, f.omegaTilt, TiltDamping);
		}
		f.pitch = std::clamp(f.pitch, -MaxTilt, MaxTilt);
		f.roll = std::clamp(f.roll, -MaxTilt, MaxTilt);
	}

	// Rigid motion about the waterline centre: tilt in the hull's frame, then heave and orbital drift.
	const float cp = std::cos(f.pitch), sp = std::sin(f.pitch);
	const float cr = std::cos(f.roll), sr = std::sin(f.roll);
	RE::NiMatrix3 rollMatrix;  // about the hull's length axis
	rollMatrix.entry[1][1] = cr, rollMatrix.entry[1][2] = -sr;
	rollMatrix.entry[2][1] = sr, rollMatrix.entry[2][2] = cr;
	RE::NiMatrix3 pitchMatrix;  // about its width axis
	pitchMatrix.entry[0][0] = cp, pitchMatrix.entry[0][2] = sp;
	pitchMatrix.entry[2][0] = -sp, pitchMatrix.entry[2][2] = cp;
	const RE::NiMatrix3 frame = FromColumns(f.axisX, f.axisY, { 0.0f, 0.0f, 1.0f });
	f.rotation = frame * (rollMatrix * pitchMatrix) * frame.Transpose();
	f.offset = { meanDx * gain, meanDy * gain, f.heave };

	auto place = [&](Part& part) {
		auto* ref = part.ref.get().get();
		auto* node = ref ? ref->Get3D() : nullptr;
		if (!node)
			return;
		ApplyTransform(node, f.pivot + f.offset + f.rotation * (part.restPos - f.pivot), f.rotation * part.restRot);
	};
	place(f.hull);
	for (auto& part : f.parts)
		place(part);
}

void FloatingObjects::CarryActors(std::vector<Floater*>& moved, const std::vector<RE::Actor*>& actors, const std::vector<RE::NiPoint3>& previousOffsets, const std::vector<RE::NiMatrix3>& previousRotations)
{
	for (auto* f : moved) {
		f->loadSink = 0.0f;
		f->loadPitch = 0.0f;
		f->loadRoll = 0.0f;
	}

	for (auto* actor : actors) {
		for (size_t i = 0; i < moved.size(); ++i) {
			Floater& f = *moved[i];
			// Where the actor stands in the hull's rest frame, from last frame's motion.
			const RE::NiPoint3 position = actor->GetPosition();
			const RE::NiPoint3 local = previousRotations[i].Transpose() * (position - f.pivot - previousOffsets[i]);
			const float u = local.Dot(f.axisX);
			const float v = local.Dot(f.axisY);
			if (std::abs(u) > f.halfLength + 20.0f || std::abs(v) > f.halfWidth + 20.0f || local.z < -f.draft - 20.0f || local.z > f.freeboard + 80.0f)
				continue;

			// Their weight sinks and tilts the hull a little (box waterplane: area A, inertia A L^2 / 12).
			const float lengthM = 2.0f * f.halfLength * MetresPerUnit;
			const float widthM = 2.0f * f.halfWidth * MetresPerUnit;
			const float areaM = std::max(lengthM * widthM, 0.01f);
			const float mass = ActorMassKg * std::max(actor->GetScale(), 0.1f);
			f.loadSink -= mass / (WaterDensity * areaM) * UnitsPerMetre;
			f.loadPitch += mass * (u * MetresPerUnit) / (WaterDensity * areaM * std::max(lengthM * lengthM, 0.01f) / 12.0f);
			f.loadRoll -= mass * (v * MetresPerUnit) / (WaterDensity * areaM * std::max(widthM * widthM, 0.01f) / 12.0f);
			f.loadPitch = std::clamp(f.loadPitch, -0.6f * MaxTilt, 0.6f * MaxTilt);
			f.loadRoll = std::clamp(f.loadRoll, -0.6f * MaxTilt, 0.6f * MaxTilt);
			++stats.carried;

			// Carry them with the deck (Bobbing Framework): move the reference and its character controller
			// together, keeping the controller's forward vector and collector distance as they were.
			if (actor->IsInJumpState())
				break;
			auto* controller = actor->GetCharController();
			if (!controller || controller->context.currentState == RE::hkpCharacterStateType::kJumping)
				break;
			const RE::NiPoint3 next = f.pivot + f.offset + f.rotation * local;
			if ((next - position).SqrLength() < 1e-4f)
				break;
			auto* proxyController = static_cast<RE::bhkCharProxyController*>(controller);
			const float earlyOutDistance = proxyController->proxy.ignoredCollisionStartCollector.earlyOutDistance;
			const RE::hkVector4 forwardVec = controller->forwardVec;
			actor->SetPosition(next, false);
			controller->SetPositionImpl(RE::hkVector4(next.x * SkyrimToHavok, next.y * SkyrimToHavok, next.z * SkyrimToHavok, 0.0f), true, false);
			controller->forwardVec = forwardVec;
			proxyController->proxy.ignoredCollisionStartCollector.earlyOutDistance = earlyOutDistance;
			// A deck dropping away under the feet is not a fall.
			if (controller->context.currentState == RE::hkpCharacterStateType::kInAir)
				controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			break;
		}
	}
}

void FloatingObjects::Update(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt)
{
	stats = {};
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!settings.enabled || !player || !tes) {
		if (!floaters.empty())
			Reset();
		return;
	}
	if (!bobbingLoaded)
		LoadBobbingFrameworkExclusions();

	// A new worldspace or an interior: everything known belongs to the old place.
	const void* worldSpace = tes->GetRuntimeData2().worldSpace;
	const bool exterior = player->GetParentCell() && player->GetParentCell()->IsExteriorCell();
	if (worldSpace != lastWorldSpace || exterior != lastExterior) {
		Reset();
		lastWorldSpace = worldSpace;
		lastExterior = exterior;
	}

	const RE::NiPoint3 playerPosition = player->GetPosition();
	scanTimer -= dt;
	if (scanTimer <= 0.0f || playerPosition.GetDistance(lastScanPosition) > RescanDistance) {
		Scan(settings);
		scanTimer = ScanInterval;
		lastScanPosition = playerPosition;
	}

	// A reference that unloaded or got new 3D (cell reattached, model swapped) is found again by the next scan.
	for (auto it = floaters.begin(); it != floaters.end();) {
		if (!IsIntact(it->second)) {
			Release(it->second);
			it = floaters.erase(it);
			scanTimer = 0.0f;
		} else {
			++it;
		}
	}

	const RE::NiPoint3 camera = Util::GetEyePosition();
	std::vector<Floater*> nearPlayer;
	std::vector<RE::NiPoint3> previousOffsets;
	std::vector<RE::NiMatrix3> previousRotations;
	for (auto& [hullId, f] : floaters) {
		const bool close = f.pivot.GetDistance(playerPosition) < CollisionRange + f.halfLength;
		if (!close || !settings.carryActors)
			f.loadSink = f.loadPitch = f.loadRoll = 0.0f;
		// Collision follows the hull near the player: keyframed through the 3D only, so nothing is saved.
		if (close && !f.keyframed) {
			auto keyframe = [](Part& part) {
				auto* ref = part.ref.get().get();
				if (auto* node = ref ? ref->Get3D() : nullptr)
					part.keyframed = node->SetMotionType(MotionType::kKeyframed, true, false, true);
			};
			keyframe(f.hull);
			for (auto& part : f.parts)
				keyframe(part);
			f.keyframed = true;
		}
		if (close && settings.carryActors) {
			nearPlayer.push_back(&f);
			previousOffsets.push_back(f.offset);
			previousRotations.push_back(f.rotation);
		}

		UpdateFloater(f, snapshot, settings, dt, camera);
		++stats.floaters;
		stats.parts += static_cast<uint32_t>(f.parts.size());
	}

	if (!nearPlayer.empty()) {
		std::vector<RE::Actor*> actors;
		auto consider = [&](RE::Actor* actor) {
			if (!actor || !actor->Is3DLoaded() || actor->IsOnMount() || actor->GetPosition().GetDistance(playerPosition) > CollisionRange * 2.0f)
				return;
			if (const auto state = actor->AsActorState(); state && state->IsSwimming())
				return;
			actors.push_back(actor);
		};
		consider(player);
		if (const auto processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles) {
				if (auto actor = handle.get())
					consider(actor.get());
			}
		}
		CarryActors(nearPlayer, actors, previousOffsets, previousRotations);
	}
}

// ============================================================================
// Release
// ============================================================================

bool FloatingObjects::IsIntact(const Floater& f)
{
	auto intact = [](const Part& part) {
		const auto ref = part.ref.get();
		return ref && !ref->IsDisabled() && ref->Get3D() == part.node;
	};
	return intact(f.hull) && std::ranges::all_of(f.parts, intact);
}

void FloatingObjects::Release(Floater& f)
{
	auto restore = [&](Part& part) {
		auto* ref = part.ref.get().get();
		// A new 3D already stands where it was placed.
		if (auto* node = ref ? ref->Get3D() : nullptr; node && node == part.node) {
			ApplyTransform(node, part.restPos, part.restRot);
			if (part.keyframed)
				node->SetMotionType(MotionType::kFixed, true, false, true);
		}
		owner.erase(part.id);
	};
	restore(f.hull);
	for (auto& part : f.parts)
		restore(part);
}

void FloatingObjects::RestorePlacements()
{
	auto restore = [](const Part& part) {
		auto* ref = part.ref.get().get();
		if (auto* node = ref ? ref->Get3D() : nullptr; node && node == part.node)
			ApplyTransform(node, part.restPos, part.restRot);
	};
	for (const auto& [hullId, f] : floaters) {
		restore(f.hull);
		for (const auto& part : f.parts)
			restore(part);
	}
}

void FloatingObjects::Reset()
{
	for (auto& [hullId, f] : floaters)
		Release(f);
	floaters.clear();
	owner.clear();
	rejected.clear();
	scanTimer = 0.0f;
	stats = {};
}
