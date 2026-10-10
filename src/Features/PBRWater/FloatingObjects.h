#pragma once

#include "WaveModel.h"

#include <cstdint>
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
 *  - Detection: a static placed through the water plane, sized like a hull, drawn with lit materials and
 *    not resting on the bottom (the landscape lies below its keel and nothing solid is directly under it).
 *    Hull parts placed as separate references merge into one hull.
 *  - Attachment: references resting on a hull (a ray down from them hits it) or carried within its
 *    footprint (masts, sails, lanterns) move with it.
 *  - Motion: heave, pitch and roll follow a plane fitted to the water parcels under the hull, and surge and
 *    sway their mean orbital displacement, so long hulls average out short waves. The hull responds through
 *    second-order dynamics with its own natural period (a box hull heaves with T = 2 pi sqrt(draft / g)).
 *  - Load: actors on board sink and tilt the hull by their weight, from its waterplane area and inertia.
 *
 * Nothing is written to the save game: the references keep their placed positions, collision becomes
 * keyframed through the 3D only, and every object is put back in place before the game saves.
 */
class FloatingObjects
{
public:
	struct Settings
	{
		bool enabled = true;
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
	};

	/** @brief Main thread, once per frame after the wave snapshot is published. */
	void Update(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt);

	/** @brief Puts every object back where it was placed and forgets them (disabled, new game loaded). */
	void Reset();

	/**
	 * @brief Puts every object back where it was placed for the save, keeping their motion: they move on
	 * from the next frame.
	 */
	void RestorePlacements();

	Stats GetStats() const { return stats; }

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
		float pitch = 0.0f, pitchVel = 0.0f;  ///< about the hull's width axis
		float roll = 0.0f, rollVel = 0.0f;    ///< about its length axis
		bool initialised = false;
		bool keyframed = false;

		RE::NiPoint3 offset;  ///< current rigid motion: x -> pivot + offset + rotation (x - pivot)
		RE::NiMatrix3 rotation;

		float loadSink = 0.0f;  ///< from the actors on board (units, rad)
		float loadPitch = 0.0f;
		float loadRoll = 0.0f;
	};

	struct Candidate;

	void Scan(const Settings& settings);
	void UpdateFloater(Floater& floater, const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings, float dt, const RE::NiPoint3& camera);
	void CarryActors(std::vector<Floater*>& moved, const std::vector<RE::Actor*>& actors, const std::vector<RE::NiPoint3>& previousOffsets, const std::vector<RE::NiMatrix3>& previousRotations);
	void Release(Floater& floater);
	static bool IsIntact(const Floater& floater);
	void LoadBobbingFrameworkExclusions();

	std::unordered_map<RE::FormID, Floater> floaters;   ///< by hull form ID
	std::unordered_map<RE::FormID, RE::FormID> owner;   ///< any moving reference -> its hull
	std::unordered_map<RE::FormID, RE::FormID> riding;  ///< actor -> the hull it last stood on
	std::unordered_set<RE::FormID> rejected;            ///< examined, not floating
	std::unordered_set<RE::FormID> bobbingExcluded;     ///< animated by Bobbing Framework
	bool bobbingLoaded = false;
	float scanTimer = 0.0f;
	RE::NiPoint3 lastScanPosition;
	const void* lastWorldSpace = nullptr;
	bool lastExterior = false;
	Stats stats;
};
