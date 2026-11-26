#pragma once

#include "WaveModel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Objects floating on PBR Water: ice floes, boats, rafts and ships ride the rendered waves. They are
 * found automatically; nothing is configured per object and no mesh is changed.
 *
 * Builds on Bobbing Framework by RavenKZP (GPL-3.0-or-later WITH Modding Exception): an object's 3D node
 * is moved at runtime with keyframed collision so its collision follows, the objects resting on it move
 * with it, and actors standing on it are carried. Instead of a configured sine motion, every hull is a
 * floating body on the PBR Water surface:
 *  - Detection: a boat, ship, raft or ice floe by the name of its model (piers, rocks and houses on stilts
 *    stand through the water just like hulls), placed through the water plane, sized like a hull, drawn with
 *    lit materials and not resting on the bottom (the terrain does not rise into its hull and no structure
 *    stands under it). Hull parts placed as separate references merge into one hull.
 *  - Attachment: references resting on a hull (a ray down from them hits it) or carried within its
 *    footprint (masts, sails, lanterns) move with it.
 *  - Motion: heave, pitch and roll follow a plane fitted to the water parcels under the hull, and surge and
 *    sway their mean orbital displacement, so long hulls average out short waves. The hull responds through
 *    second-order dynamics with its own natural period (a box hull heaves with T = 2 pi sqrt(draft / g)).
 *  - Load: actors on board sink and tilt the hull by their weight, from its waterplane area and inertia.
 *
 * Small things in the water ride it too ("riders"): fish and other critters, plants and debris floating at
 * or just under the surface move with the orbital motion of the water around them, which fades with depth,
 * and are kept under the surface when a trough passes over them.
 *
 * Hull masks: the water is not drawn inside a hull (HullMask). Every hull found, floating or resting on the bottom,
 * gets its outline at the waterline measured once against its own collision, so the waves passing through it stay
 * outside and its deck and bilge stay dry.
 *
 * Nothing is written to the save game: the references keep their placed positions, collision becomes
 * keyframed through the 3D only, and every object is put back in place before the game saves.
 */
class FloatingObjects
{
public:
	static constexpr uint32_t HullStations = 12;  ///< widths measured along a hull's length (HullMask::width)

	/**
	 * @brief The room a hull takes in the water, in which the water surface is not drawn (PBRWater.hlsli InsideHull).
	 * Its own frame: x along the length, y across, z up, from the waterline centre as the hull lies now.
	 */
	struct HullMask
	{
		RE::NiPoint3 centre;  ///< waterline centre (absolute units)
		RE::NiPoint3 axisX;   ///< unit axes (tilted with the hull)
		RE::NiPoint3 axisY;
		RE::NiPoint3 axisZ;
		float bottom = 0.0f;  ///< keel below the centre (units, negative)
		float top = 0.0f;     ///< top of the hull above it (units)
		float front = 0.0f;   ///< its outline's ends along +x and -x at the waterline (units)
		float back = 0.0f;
		float width[HullStations] = {};  ///< half widths at the stations spread evenly between -back and front (units)
	};

	struct Settings
	{
		bool enabled = true;         ///< hulls and riders are looked for
		bool move = true;            ///< and moved (off: found for their masks only)
		float range = 10500.0f;      ///< units around the player in which objects float
		float maxSize = 4200.0f;     ///< longest hull (units) that floats
		float response = 1.0f;       ///< scales the motion
		bool carryActors = true;     ///< actors standing on a hull move with it
		float fadeStart = 16384.0f;  ///< wave displacement distance fade (units), as the water shader
		float fadeEnd = 32768.0f;
	};

	struct Stats
	{
		uint32_t floaters = 0;  ///< hulls
		uint32_t parts = 0;     ///< references moving with them (merged hull parts and attached objects)
		uint32_t carried = 0;   ///< actors on board this frame
		uint32_t cargo = 0;     ///< loose things (clutter, dropped items, bodies) carried on a deck this frame
		uint32_t riders = 0;    ///< small things carried by the water (fish, plants, debris)
		uint32_t masks = 0;     ///< hulls keeping the water out (floating and resting ones)
		uint32_t measured = 0;  ///< of them, outlined against their collision (the rest from their bounds)
	};

	/** @brief Main thread, once per frame after the wave snapshot is published. */
	void Update(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt);

	/** @brief Any thread: what the detection made of the reference selected in the console (debug view). */
	std::string GetSelectionReport() const
	{
		std::scoped_lock lock(reportMutex);
		return selectionReport;
	}

	/** @brief Puts every object back where it was placed and forgets them (disabled, new game loaded). */
	void Reset();

	/**
	 * @brief Puts every object back where it was placed for the save, keeping their motion: they move on
	 * from the next frame.
	 */
	void RestorePlacements();

	Stats GetStats() const { return stats; }

	/** @brief Any thread: the hulls the water is kept out of, as they lie this frame, nearest the camera first. */
	std::vector<HullMask> GetHullMasks() const
	{
		std::scoped_lock lock(maskMutex);
		return masks;
	}

private:
	struct Part
	{
		RE::ObjectRefHandle ref;
		RE::FormID id = 0;
		const RE::NiAVObject* node = nullptr;  ///< the 3D it was found with; another 3D starts over
		RE::NiPoint3 restPos;
		RE::NiMatrix3 restRot;
		bool keyframed = false;
	};

	/** @brief A hull's outline at the waterline, in its rest frame (HullMask). */
	struct HullProfile
	{
		float front = 0.0f;
		float back = 0.0f;
		float width[HullStations] = {};
		bool measured = false;  ///< against its collision, not only from its bounds
	};

	/** @brief A hull resting on the bottom, on a structure or too large to float: it keeps the water out where it lies. */
	struct StillHull
	{
		RE::ObjectRefHandle ref;
		const RE::NiAVObject* node = nullptr;
		RE::NiPoint3 pivot;
		RE::NiPoint3 axisX;
		RE::NiPoint3 axisY;
		float draft = 0.0f;
		float freeboard = 0.0f;
		HullProfile profile;
	};

	struct Floater
	{
		Part hull;                ///< the main hull reference
		std::vector<Part> parts;  ///< merged hull parts and attached references
		RE::NiPoint3 pivot;       ///< waterline centre at rest
		RE::NiPoint3 axisX;       ///< horizontal hull axes (unit)
		RE::NiPoint3 axisY;
		float halfLength = 0.0f;  ///< waterplane half extents along the axes (units)
		float halfWidth = 0.0f;
		float draft = 0.0f;      ///< units below the flat water at rest
		float freeboard = 0.0f;  ///< units above it, to the top of the hull's box
		float flatZ = 0.0f;
		float omegaHeave = 1.0f;  ///< rad/s
		float omegaTilt = 1.0f;
		float massKg = 1.0f;  ///< displaced water

		float heave = 0.0f, heaveVel = 0.0f;
		float driftX = 0.0f, driftVelX = 0.0f;  ///< orbital surge / sway (units)
		float driftY = 0.0f, driftVelY = 0.0f;
		float pitch = 0.0f, pitchVel = 0.0f;  ///< about the hull's width axis
		float roll = 0.0f, rollVel = 0.0f;    ///< about its length axis
		bool initialised = false;
		bool keyframed = false;

		RE::NiPoint3 offset;  ///< current rigid motion: x -> pivot + offset + rotation (x - pivot)
		RE::NiMatrix3 rotation;

		float loadSink = 0.0f;  ///< from the actors on board (units, rad)
		float loadPitch = 0.0f;
		float loadRoll = 0.0f;

		HullProfile profile;  ///< measured when it is found, at rest
	};

	/** @brief A small thing carried by the water around it. */
	struct Rider
	{
		RE::ObjectRefHandle ref;
		RE::FormID id = 0;
		const RE::NiAVObject* node = nullptr;
		float flatZ = 0.0f;
		float halfHeight = 0.0f;    ///< of its bounds
		float centreOffset = 0.0f;  ///< bounds centre above the reference position
		bool atSurface = false;     ///< floats on the surface (rides it fully) rather than under it
		RE::NiPoint3 offset;        ///< displacement applied this frame
	};

	struct Candidate;

	void Scan(const Settings& settings, const PBRWaterModel::WaveSnapshot& snapshot);
	void UpdateRiders(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, const RE::NiPoint3& camera);
	void Reject(RE::FormID id, const char* reason);
	/** @brief Main thread: what the detection made of a reference. */
	std::string Describe(const RE::TESObjectREFR* ref) const;
	void UpdateFloater(Floater& floater, const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt, const RE::NiPoint3& camera);
	void CarryActors(std::vector<Floater*>& moved, const std::vector<RE::Actor*>& actors, const std::vector<RE::NiPoint3>& previousOffsets, const std::vector<RE::NiMatrix3>& previousRotations);
	void CarryCargo(const std::vector<Floater*>& moved, const std::vector<RE::NiPoint3>& previousOffsets, const std::vector<RE::NiMatrix3>& previousRotations);
	void Release(Floater& floater);
	static bool IsIntact(const Floater& floater);
	void LoadBobbingFrameworkExclusions();
	/**
	 * @brief Main thread, at rest: the outline at the waterline of a hull with these axes and extents, from rays cast
	 * in from its sides against the collision of `hull` (the hull reference and its merged parts).
	 */
	static HullProfile MeasureProfile(RE::TES* tes, const std::unordered_set<RE::FormID>& hull, const RE::NiPoint3& pivot, const RE::NiPoint3& axisX,
		const RE::NiPoint3& axisY, float halfLength, float halfWidth, float freeboard);
	/** @brief Main thread: this frame's hull masks for the render thread. */
	void PublishMasks(const RE::NiPoint3& camera);

	std::unordered_map<RE::FormID, Floater> floaters;     ///< by hull form ID
	std::unordered_map<RE::FormID, RE::FormID> owner;     ///< any moving reference -> its hull
	std::unordered_map<RE::FormID, RE::FormID> riding;    ///< actor -> the hull it last stood on
	std::unordered_set<RE::FormID> rejected;              ///< examined, not floating
	std::unordered_map<RE::FormID, const char*> reasons;  ///< why a reference does not float (debug view)
	std::unordered_map<RE::FormID, Rider> riders;         ///< by form ID
	std::vector<RE::ObjectRefHandle> cargo;               ///< loose things lying on the hulls near the player (found by each scan)

	std::unordered_map<RE::FormID, StillHull> stillHulls;  ///< hulls that do not float, by form ID (masks only)
	mutable std::mutex maskMutex;
	std::vector<HullMask> masks;  ///< guarded by maskMutex
	bool lastMove = true;
	mutable std::mutex reportMutex;
	std::string selectionReport;
	std::unordered_set<RE::FormID> bobbingExcluded;  ///< animated by Bobbing Framework
	bool bobbingLoaded = false;
	float scanTimer = 0.0f;
	RE::NiPoint3 lastScanPosition;
	const void* lastWorldSpace = nullptr;
	bool lastExterior = false;
	Stats stats;
};
