#include "FloatingObjects.h"

#include "Features/PBRWater.h"
#include "Utils/Game.h"

#include "RE/B/bhkPickData.h"
#include "RE/T/TESHavokUtilities.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>

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
	constexpr float DriftDamping = 0.7f;       // surge / sway: the water moves the hull sideways without rocking it back
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
		// Persistent references (named ships) live in the worldspace's persistent cell, which has no water:
		// ask the loaded exterior cell under the position first.
		auto* tes = RE::TES::GetSingleton();
		auto* cell = tes ? tes->GetCell(position) : nullptr;
		if (cell && PBRWater::TESObjectCELL_GetWaterHeight::func(cell, position, height) && height > -1e6f)
			return true;
		auto* parent = ref->GetParentCell();
		if (parent && PBRWater::TESObjectCELL_GetWaterHeight::func(parent, position, height) && height > -1e6f)
			return true;
		// Open sea cells often carry no water data of their own: the worldspace's sea level applies.
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (worldSpace && (!parent || parent->IsExteriorCell())) {
			height = worldSpace->GetDefaultWaterHeight();
			return height > -1e6f;
		}
		return false;
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

	/// Things that can be picked up: they lie loose wherever they were put or dropped, havok-settled or not.
	bool IsLooseItemType(RE::FormType type)
	{
		using enum RE::FormType;
		return type == Misc || type == Weapon || type == Armor || type == Ingredient || type == AlchemyItem || type == Book ||
		       type == Scroll || type == Ammo || type == KeyMaster || type == SoulGem;
	}

	/// A loose prop: all of its collision is dynamic. Hulls often carry a few dynamic parts (ropes,
	/// lanterns, oars) on top of their fixed collision; those still float as a whole.
	bool IsLooseProp(RE::NiAVObject* node)
	{
		bool dynamic = false;
		bool fixed = false;
		RE::BSVisit::TraverseScenegraphCollision(node, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
			auto* body = object->body.get() ? object->body.get()->AsBhkRigidBody() : nullptr;
			auto* rigid = body ? body->GetRigidBody() : nullptr;
			if (rigid)
				(IsDynamicMotion(rigid) ? dynamic : fixed) = true;
			return fixed ? RE::BSVisit::BSVisitControl::kStop : RE::BSVisit::BSVisitControl::kContinue;
		});
		return dynamic && !fixed;
	}

	std::string LowerModelPath(RE::TESBoundObject* base)
	{
		auto* model = base ? base->As<RE::TESModel>() : nullptr;
		if (!model || !model->GetModel())
			return {};
		std::string path = model->GetModel();
		std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return path;
	}

	bool PathHasAny(const std::string& path, std::initializer_list<const char*> words)
	{
		return std::ranges::any_of(words, [&](const char* word) { return path.find(word) != std::string::npos; });
	}

	/// Structures standing through the water on piles: pier, dock and jetty sections and the houses and
	/// shipyards built out over the water. Never float them, nor anything standing on them.
	bool IsPierModel(const std::string& path)
	{
		return PathHasAny(path, { "dock", "pier", "jetty", "wharf", "quay", "pile", "stilt", "walkway", "boardwalk", "house", "shipyard", "wreck" });
	}

	/// Vessels, by the name of their model. Words that merely contain a vessel's name ("crafting", "worship") do not count.
	bool IsBoatModel(const std::string& path)
	{
		return PathHasAny(path, { "boat", "ship", "raft", "canoe", "skiff", "barge", "kayak", "dinghy" }) && !PathHasAny(path, { "craft", "draft", "worship" }) &&
		       !IsPierModel(path);
	}

	/// Drifting sea ice.
	bool IsIceFloeModel(const std::string& path)
	{
		return PathHasAny(path, { "floe" });
	}

	/// Rocks, boulders and cliffs are part of the land, however they sit in the water.
	bool IsTerrainFeatureModel(const std::string& path)
	{
		if (IsIceFloeModel(path) || IsBoatModel(path))
			return false;
		return PathHasAny(path, { "rock", "stone", "boulder", "pebble", "cliff", "crag", "mountain", "rubble", "stalag", "landscape\\" });
	}

	/// Small statics that drift: plants lying on the water and flotsam. Other small statics placed in the water
	/// are mostly stones and props resting on the bed.
	bool IsDriftModel(const std::string& path)
	{
		return PathHasAny(path, { "lily", "seaweed", "kelp", "algae", "duckweed", "pondweed", "driftwood", "debris", "flotsam", "plank", "barrel", "crate", "buoy", "leaf", "leaves" }) &&
		       !IsTerrainFeatureModel(path);
	}

	/**
	 * Height of the terrain under (x, y): the exact terrain of the loaded cells, else the heightmap, else a
	 * landscape query from `aboveZ`. The query from above matters: started at the bottom of something sunk
	 * into the river bed, a land query starts under the ground and misses it.
	 */
	bool BedHeight(RE::TES* tes, const PBRWaterModel::WaveSnapshot& snapshot, float x, float y, float aboveZ, float& z)
	{
		if (snapshot.land && snapshot.land->Contains(x, y)) {
			z = snapshot.land->Sample(x, y);
			if (z > -1.0e5f)
				return true;
		}
		if (snapshot.bathymetry && snapshot.bathymetry->Contains(x, y)) {
			z = snapshot.bathymetry->Sample(x, y);
			return true;
		}
		RE::NiPoint3 from{ x, y, aboveZ };
		return tes && tes->GetLandHeight(from, z);
	}

	/**
	 * Why something placed through the water rests on the bottom, or null when it does not. A hull's keel may
	 * graze the bed of a shallow harbour and still float, but the terrain may not rise into it, and something
	 * that is mostly under water while touching the bed (a rock in a river) stands on it. Most of the
	 * footprint over dry land or the last hand's breadth of water is a boat lying in the shallows.
	 */
	const char* GroundedReason(RE::TES* tes, const PBRWaterModel::WaveSnapshot& snapshot, const WorldBox& box, float flatZ)
	{
		const float u[5] = { 0.0f, SampleSpread, -SampleSpread, 0.0f, 0.0f };
		const float v[5] = { 0.0f, 0.0f, 0.0f, SampleSpread, -SampleSpread };
		const float draft = flatZ - box.minZ;
		const float height = box.maxZ - box.minZ;
		float bedCentre = 0.0f;
		bool hasCentre = false;
		int beached = 0;
		for (int i = 0; i < 5; ++i) {
			const RE::NiPoint3 p = box.centre + box.axis[0] * (u[i] * box.half.x) + box.axis[1] * (v[i] * box.half.y);
			float bed = 0.0f;
			if (!BedHeight(tes, snapshot, p.x, p.y, box.maxZ + 1000.0f, bed))
				continue;
			if (i == 0) {
				bedCentre = bed;
				hasCentre = true;
			}
			if (bed > flatZ - 0.15f * UnitsPerMetre)
				++beached;
		}
		if (beached >= 3)
			return "lies in the shallows";
		if (!hasCentre)
			return nullptr;
		if (bedCentre > box.minZ + 0.25f * draft)
			return "rests on the bottom: the terrain rises into it";
		if (bedCentre > box.minZ - std::max(0.1f * draft, 5.0f) && draft > 0.55f * height)
			return "rests on the bottom: mostly under water and touching the bed";
		return nullptr;
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

void FloatingObjects::Reject(RE::FormID id, const char* reason)
{
	rejected.insert(id);
	reasons[id] = reason;
}

void FloatingObjects::Scan(const Settings& settings, const PBRWaterModel::WaveSnapshot& snapshot)
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;

	auto isExcluded = [&](RE::TESObjectREFR* ref, const RE::TESBoundObject* base) {
		return bobbingExcluded.contains(ref->GetFormID()) || (base && bobbingExcluded.contains(base->GetFormID()));
	};

	std::vector<RE::TESObjectREFR*> refs;
	std::vector<RE::TESObjectREFR*> bodies;
	std::unordered_set<RE::FormID> present;
	tes->ForEachReferenceInRange(player, settings.range, [&](RE::TESObjectREFR* ref) {
		if (ref && !ref->IsDisabled() && !ref->IsDeleted() && ref->Is3DLoaded()) {
			if (!ref->As<RE::Actor>()) {
				refs.push_back(ref);
				present.insert(ref->GetFormID());
			} else if (ref->IsDead()) {
				bodies.push_back(ref);
			}
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	// Hulls and riders that left the area or unloaded go back where they were placed.
	for (auto it = floaters.begin(); it != floaters.end();) {
		if (!present.contains(it->first)) {
			Release(it->second);
			it = floaters.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = riders.begin(); it != riders.end();) {
		if (!present.contains(it->first)) {
			if (auto ref = it->second.ref.get(); ref && ref->Get3D() == it->second.node)
				ApplyTransform(ref->Get3D(), ref->GetPosition(), ref->Get3D()->local.rotate);
			it = riders.erase(it);
		} else {
			++it;
		}
	}
	std::erase_if(rejected, [&](RE::FormID id) { return !present.contains(id); });
	std::erase_if(reasons, [&](const auto& entry) { return !present.contains(entry.first); });

	// Look at the references not examined yet.
	std::vector<RE::TESObjectREFR*> fresh;
	std::vector<Candidate> hulls;
	std::vector<WorldBox> anchors;  // fixed structure standing in the water: piles, grounded pier sections
	const float riderReach = std::max(snapshot.VerticalBound(), 1.5f * UnitsPerMetre) + 20.0f;
	for (auto* ref : refs) {
		const auto id = ref->GetFormID();
		if (owner.contains(id) || rejected.contains(id) || riders.contains(id))
			continue;
		fresh.push_back(ref);

		auto* base = ref->GetBaseObject();
		auto* node = ref->Get3D();
		if (!base || !node || IsMarker(base) || ref->IsWater()) {
			Reject(id, "marker or water");
			continue;
		}
		if (isExcluded(ref, base)) {
			Reject(id, "animated by Bobbing Framework");
			continue;
		}
		const WorldBox box = GetWorldBox(ref, node);
		float flatZ = 0.0f;
		if (box.degenerate || !FlatWaterHeight(ref, box.centre, flatZ)) {
			Reject(id, "no water here");
			continue;
		}
		const float draft = flatZ - box.minZ;
		const float freeboard = box.maxZ - flatZ;
		const float height = box.maxZ - box.minZ;
		const float length = 2.0f * std::max(box.half.x, box.half.y);
		const std::string path = LowerModelPath(base);

		// Small things floating at the surface or swimming just under it ride the water around them.
		if (length <= 2.5f * UnitsPerMetre && height <= 2.5f * UnitsPerMetre && box.minZ < flatZ + 2.0f && box.maxZ > flatZ - riderReach) {
			// Critters and plants by their type; statics only when their model names flotsam or a floating plant, as
			// most small statics placed in the water are stones and props resting on the bed.
			const auto type = base->GetFormType();
			const bool riderType = type == RE::FormType::Activator || type == RE::FormType::Flora ||
			                       ((type == RE::FormType::Static || type == RE::FormType::MovableStatic) && IsDriftModel(path));
			const bool atSurface = height <= 0.5f * UnitsPerMetre && box.maxZ > flatZ - 2.0f;
			const bool underSurface = box.maxZ < flatZ + 0.25f * height;
			float bed = 0.0f;
			const bool free = !BedHeight(tes, snapshot, box.centre.x, box.centre.y, box.maxZ + 1000.0f, bed) || bed < box.minZ - 5.0f;
			const bool topLevel = (node->local.translate - ref->GetPosition()).Length() < 1.0f;
			if (riderType && (atSurface || underSurface) && free && topLevel && HasLitGeometry(node) && !FindDynamicBody(node) && !IsTerrainFeatureModel(path)) {
				Rider rider;
				rider.ref = ref->GetHandle();
				rider.id = id;
				rider.node = node;
				rider.flatZ = flatZ;
				rider.halfHeight = 0.5f * height;
				rider.centreOffset = 0.5f * (box.minZ + box.maxZ) - ref->GetPosition().z;
				rider.atSurface = atSurface;
				riders.emplace(id, rider);
				reasons.erase(id);
				continue;
			}
		}

		if (!IsHullType(base->GetFormType())) {
			Reject(id, "not a static, activator or furniture");
			continue;
		}
		// Only vessels and drifting ice float. Pier sections, rocks and houses on stilts stand through the water just
		// like a hull, and nothing about their shape or placement tells them apart; standing through the water,
		// they are what a raft moored on piles stands on.
		if (!IsBoatModel(path) && !IsIceFloeModel(path)) {
			if (box.minZ < flatZ && box.maxZ > flatZ)
				anchors.push_back(box);
			Reject(id, IsPierModel(path) ? "pier, dock or a building on piles" : (IsTerrainFeatureModel(path) ? "rock, cliff or other part of the land" : "not a boat, ship, raft or ice floe"));
			continue;
		}

		Candidate c;
		c.ref = ref;
		c.box = box;
		c.flatZ = flatZ;
		// Placed upright, through the water plane.
		if (box.axis[2].z < 0.85f) {
			Reject(id, "not upright");
			continue;
		}
		if (draft < 2.0f || freeboard < 2.0f) {
			Reject(id, "not placed through the water surface");
			continue;
		}
		// Almost entirely under water (a reef; ice floats ~90% submerged, so floes stay in) or shaped like a
		// post, a column or a waterfall. Wrecks resting on the bottom are caught by the bed test below.
		if (draft > 0.92f * height || height > 3.0f * length) {
			anchors.push_back(box);
			Reject(id, "under water or shaped like a post");
			continue;
		}
		// Under a metre: fish, bottles and other clutter, never a hull.
		if (length > settings.maxSize) {
			Reject(id, "longer than the Largest Hull setting");
			continue;
		}
		if (length < 1.0f * UnitsPerMetre) {
			Reject(id, "too small for a hull");
			continue;
		}
		if (!HasLitGeometry(node) || IsLooseProp(node)) {
			Reject(id, "effect mesh or loose physics prop");
			continue;
		}
		// Not resting on the bottom (the bed may graze the keel, but not rise into the hull)...
		if (const char* grounded = GroundedReason(tes, snapshot, box, flatZ)) {
			anchors.push_back(box);
			Reject(id, grounded);
			continue;
		}
		// ...and not standing on a structure (a pile, a pier, a wreck) right under it. The terrain was tested
		// above; a ray that only finds the bed is no reason to stay put.
		RE::TESObjectREFR* below = nullptr;
		float belowZ = 0.0f;
		// The ray starts just under the bounds, which can still be inside the reference's own collision.
		if (PickBelow(tes, { box.centre.x, box.centre.y, box.minZ - 1.0f }, std::max(0.1f * draft, 10.0f), below, belowZ) && below && below != ref) {
			anchors.push_back(box);
			Reject(id, "stands on another object");
			continue;
		}

		c.area = box.half.x * box.half.y;
		hulls.push_back(c);
	}

	// Part of a pier or a jetty: a pile or a grounded section stands inside the footprint (a boat moored
	// alongside only touches the piles at its side). Only for low platforms: a ship's box reaches far
	// beyond its waterline (bowsprit, rigging) and takes in the dock it is moored to.
	std::erase_if(hulls, [&](const Candidate& c) {
		if (c.box.maxZ - c.box.minZ > 4.0f * UnitsPerMetre)
			return false;
		const bool onPiles = std::ranges::any_of(anchors, [&](const WorldBox& anchor) {
			const RE::NiPoint3 d = anchor.centre - c.box.centre;
			const float u = std::abs(d.Dot(c.box.axis[0])) - 0.8f * c.box.half.x;
			const float v = std::abs(d.Dot(c.box.axis[1])) - 0.8f * c.box.half.y;
			return u < 0.0f && v < 0.0f && anchor.maxZ > c.flatZ && anchor.minZ < c.box.minZ + 8.0f;
		});
		if (onPiles)
			Reject(c.ref->GetFormID(), "pier section: stands on piles");
		return onPiles;
	});

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
		if (IsPierModel(LowerModelPath(base)))
			return false;
		const WorldBox box = GetWorldBox(ref, node);
		if (!insideFootprint(f, box.centre, FootprintMargin))
			return false;
		const float keelZ = f.flatZ - f.draft;
		if (box.minZ < keelZ - 10.0f || box.minZ > f.flatZ + f.freeboard + 10.0f)
			return false;
		if (FindDynamicBody(node))
			return false;
		float bed = 0.0f;
		if (!box.degenerate && BedHeight(tes, snapshot, box.centre.x, box.centre.y, box.maxZ + 1000.0f, bed) && bed > box.minZ - 10.0f)
			return false;
		// Directly under it, down to the keel, there must be this hull or something riding it: open water
		// under it means it stands beside the hull (a dock, a crate on the dock), not on it.
		RE::TESObjectREFR* below = nullptr;
		float belowZ = 0.0f;
		const float start = box.minZ - 1.0f;
		if (start > keelZ) {
			if (!PickBelow(tes, { box.centre.x, box.centre.y, start }, start - keelZ + 5.0f, below, belowZ))
				return false;
			const auto it = below ? owner.find(below->GetFormID()) : owner.end();
			if (it == owner.end() || it->second != hullId)
				return false;
		}
		f.parts.push_back(makePart(ref));
		owner[ref->GetFormID()] = hullId;
		rejected.erase(ref->GetFormID());
		reasons.erase(ref->GetFormID());
		return true;
	};

	std::unordered_set<RE::FormID> createdSet(created.begin(), created.end());
	std::unordered_set<RE::FormID> freshSet;
	for (auto* ref : fresh)
		freshSet.insert(ref->GetFormID());
	for (auto* ref : refs) {
		const auto id = ref->GetFormID();
		if (owner.contains(id) || riders.contains(id))
			continue;
		const bool isFresh = freshSet.contains(id);
		for (auto& [hullId, f] : floaters) {
			if (!isFresh && !createdSet.contains(hullId))
				continue;
			if (tryAttach(f, hullId, ref))
				break;
		}
	}

	for (auto* ref : fresh) {
		const auto id = ref->GetFormID();
		if (!owner.contains(id) && !riders.contains(id)) {
			rejected.insert(id);
			reasons.try_emplace(id, "not a hull");
		}
	}

	// Loose things lying on the hulls near the player: havok clutter (settled or not), dropped items and the dead.
	// They are carried with the deck every frame (CarryCargo).
	cargo.clear();
	auto addCargo = [&](RE::TESObjectREFR* ref, bool candidate) {
		if (!candidate || owner.contains(ref->GetFormID()) || riders.contains(ref->GetFormID()) ||
			ref->GetPosition().GetSquaredDistance(player->GetPosition()) > CollisionRange * CollisionRange)
			return;
		for (auto& [hullId, f] : floaters) {
			if (insideFootprint(f, ref->GetPosition(), FootprintMargin) && ref->GetPosition().z > f.flatZ - f.draft - 10.0f) {
				cargo.push_back(ref->GetHandle());
				return;
			}
		}
	};
	for (auto* ref : refs) {
		auto* base = ref->GetBaseObject();
		auto* node = ref->Get3D();
		addCargo(ref, base && node && (IsLooseItemType(base->GetFormType()) || FindDynamicBody(node)));
	}
	for (auto* ref : bodies)
		addCargo(ref, true);

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

	const float targetDriftX = meanDx * gain;
	const float targetDriftY = meanDy * gain;
	if (!f.initialised) {
		f.heave = targetHeave;
		f.pitch = targetPitch;
		f.roll = targetRoll;
		f.driftX = targetDriftX;
		f.driftY = targetDriftY;
		f.heaveVel = f.pitchVel = f.rollVel = f.driftVelX = f.driftVelY = 0.0f;
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
			// The hull's mass also smooths the orbital surge and sway: short waves only shake it a little.
			spring(f.driftX, f.driftVelX, targetDriftX, f.omegaHeave, DriftDamping);
			spring(f.driftY, f.driftVelY, targetDriftY, f.omegaHeave, DriftDamping);
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
	f.offset = { f.driftX, f.driftY, f.heave };

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
		// Only what the character stands on carries it: a box around the hull also takes in the dock it is
		// moored to. Between steps, or while the deck drops away, there is no support: keep the last boat.
		const RE::FormID actorId = actor->GetFormID();
		RE::FormID supportHull = 0;
		if (auto* controller = actor->GetCharController()) {
			if (auto* body = controller->supportBody.get()) {
				if (auto* ref = RE::TESHavokUtilities::FindCollidableRef(body->collidable)) {
					if (const auto it = owner.find(ref->GetFormID()); it != owner.end())
						supportHull = it->second;
				}
			} else if (const auto it = riding.find(actorId); it != riding.end()) {
				supportHull = it->second;
			}
		}
		if (!supportHull) {
			riding.erase(actorId);
			continue;
		}
		riding[actorId] = supportHull;

		for (size_t i = 0; i < moved.size(); ++i) {
			Floater& f = *moved[i];
			if (f.hull.id != supportHull)
				continue;
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

void FloatingObjects::CarryCargo(const std::vector<Floater*>& moved, const std::vector<RE::NiPoint3>& previousOffsets, const std::vector<RE::NiMatrix3>& previousRotations)
{
	const float scale = RE::bhkWorld::GetWorldScale();
	for (const auto& handle : cargo) {
		const auto refPtr = handle.get();
		auto* ref = refPtr.get();
		auto* node = ref && !ref->IsDisabled() ? ref->Get3D() : nullptr;
		if (!node)
			continue;
		// Where it lies in the hull's rest frame, from last frame's motion (the 3D follows its bodies).
		const RE::NiPoint3 position = node->world.translate;
		for (size_t i = 0; i < moved.size(); ++i) {
			const Floater& f = *moved[i];
			const RE::NiPoint3 local = previousRotations[i].Transpose() * (position - f.pivot - previousOffsets[i]);
			if (std::abs(local.Dot(f.axisX)) > f.halfLength + 10.0f || std::abs(local.Dot(f.axisY)) > f.halfWidth + 10.0f ||
				local.z < -f.draft - 10.0f || local.z > f.freeboard + 100.0f)
				continue;
			const RE::NiPoint3 delta = f.pivot + f.offset + f.rotation * local - position;
			if (delta.SqrLength() < 1e-6f)
				break;
			// The deck moves under it by setting its 3D, which never pushes what lies on it: move it along, every one of
			// its bodies (a ragdoll has many) and, for what havok does not drive (settled clutter), the reference.
			bool simulated = false;
			RE::BSVisit::TraverseScenegraphCollision(node, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
				auto* body = object->body.get() ? object->body.get()->AsBhkRigidBody() : nullptr;
				auto* rigid = body ? body->GetRigidBody() : nullptr;
				if (rigid && rigid->motion.type.get() != RE::hkpMotion::MotionType::kFixed) {
					RE::hkVector4 p;
					body->GetPosition(p);
					float q[4];
					_mm_storeu_ps(q, p.quad);
					RE::hkVector4 next(q[0] + delta.x * scale, q[1] + delta.y * scale, q[2] + delta.z * scale, 0.0f);
					body->SetPosition(next);
					simulated = simulated || IsDynamicMotion(rigid);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			if (!simulated)
				ref->SetPosition(ref->GetPosition() + delta);
			++stats.cargo;
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
		if (!floaters.empty() || !riders.empty())
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
		Scan(settings, snapshot);
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

	const RE::NiPoint3 camera = Util::GetMainThreadEyePosition();
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
		CarryCargo(nearPlayer, previousOffsets, previousRotations);
	}

	UpdateRiders(snapshot, settings, camera);

	std::string report = "Select a reference in the console to see why it floats or stays put";
	if (const auto selected = RE::Console::GetSelectedRef())
		report = std::format("Console selection {:08X}: {}", selected->GetFormID(), Describe(selected.get()));
	std::scoped_lock lock(reportMutex);
	selectionReport = std::move(report);
}

void FloatingObjects::UpdateRiders(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, const RE::NiPoint3& camera)
{
	auto* tes = RE::TES::GetSingleton();
	const float windSpeed = snapshot.ocean ? snapshot.ocean->windSpeed : 0.5f;
	const float openPeak = snapshot.ocean ? snapshot.ocean->peakOmega : 1.0f;
	for (auto it = riders.begin(); it != riders.end();) {
		Rider& rider = it->second;
		const auto refPtr = rider.ref.get();
		auto* ref = refPtr.get();
		auto* node = ref ? ref->Get3D() : nullptr;
		// Gone or given new 3D: that 3D stands where the engine put it.
		if (!ref || ref->IsDisabled() || node != rider.node) {
			it = riders.erase(it);
			continue;
		}

		// The reference's own position is the rest position: critters keep swimming their paths, and the
		// motion is added to wherever they are.
		const RE::NiPoint3 position = ref->GetPosition();
		const float distance = position.GetDistance(camera);
		const float fade = (1.0f - Smoothstep(settings.fadeStart, settings.fadeEnd, distance)) *
		                   (1.0f - Smoothstep(settings.range * 0.85f, settings.range, distance)) * settings.response;
		RE::NiPoint3 offset{};
		if (fade > 0.0f) {
			const double x = position.x;
			const double y = position.y;
			PBRWaterModel::WaveSnapshot::Displacement d;
			snapshot.ParcelDisplacements(&x, &y, 1, rider.flatZ, &d);
			const float centreZ = position.z + rider.centreOffset;
			if (rider.atSurface) {
				// Floating on the surface: it rides it.
				offset = { d.dx * fade, d.dy * fade, d.dz * fade };
			} else {
				// Under it: the orbital motion of the water fades as e^(-k depth) (linear wave theory), for the
				// dominant waves here.
				const float fetchMetres = snapshot.fetch ? snapshot.fetch->SampleMetres(position.x, position.y, snapshot.windDirection) * snapshot.fetchScale :
				                                           (snapshot.exterior ? 500000.0f : 50.0f);
				const float omega = PBRWaterModel::FetchPeakOmega(fetchMetres, windSpeed, openPeak);
				const float k = omega * omega / Gravity * MetresPerUnit;
				const float decay = std::exp(-k * std::max(rider.flatZ - centreZ, 0.0f)) * fade;
				offset = { d.dx * decay, d.dy * decay, d.dz * decay };
				// Never out of the water: a trough deeper than it swims pushes it down with the surface...
				const float limit = rider.flatZ + d.dz * fade - 4.0f;
				const float top = centreZ + rider.halfHeight + offset.z;
				if (top > limit)
					offset.z -= top - limit;
				// ...but not into the bed.
				float bed = 0.0f;
				if (BedHeight(tes, snapshot, position.x, position.y, rider.flatZ + 1000.0f, bed))
					offset.z = std::max(offset.z, bed + 2.0f - (centreZ - rider.halfHeight));
			}
		}
		rider.offset = offset;
		ApplyTransform(node, position + offset, node->local.rotate);
		++stats.riders;
		++it;
	}
}

std::string FloatingObjects::Describe(const RE::TESObjectREFR* ref) const
{
	if (!ref)
		return "No reference selected";
	const auto id = ref->GetFormID();
	if (const auto it = owner.find(id); it != owner.end())
		return it->second == id ? std::string("Floats: hull") : std::format("Floats: part of hull {:08X}", it->second);
	if (riders.contains(id))
		return "Rides the water: small object";
	if (const auto it = reasons.find(id); it != reasons.end())
		return std::format("Stays put: {}", it->second);
	return "Not examined: out of range, not loaded or not in water";
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
	for (const auto& [id, rider] : riders) {
		auto* ref = rider.ref.get().get();
		if (auto* node = ref ? ref->Get3D() : nullptr; node && node == rider.node)
			ApplyTransform(node, ref->GetPosition(), node->local.rotate);
	}
}

void FloatingObjects::Reset()
{
	for (auto& [hullId, f] : floaters)
		Release(f);
	RestorePlacements();
	floaters.clear();
	owner.clear();
	riding.clear();
	rejected.clear();
	reasons.clear();
	riders.clear();
	cargo.clear();
	scanTimer = 0.0f;
	stats = {};
}
