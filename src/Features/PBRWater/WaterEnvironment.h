#pragma once

#include "WaveModel.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

/** @brief Minimal view of a cell coverage grid (decoupled from Unified Water's cache type). */
struct WaterCoverageView
{
	int32_t minX = 0;
	int32_t minY = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	const uint8_t* water = nullptr;
};

/** @brief Still water level of each cell of a worldspace (absolute z, NaN where a cell has no water). */
struct CellLevels
{
	int32_t minX = 0;
	int32_t minY = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<float> level;

	/** @brief The cell's level, NaN outside the grid or where it has no water. */
	float At(int32_t cellX, int32_t cellY) const;
};

/**
 * @brief Exact terrain heights of every exterior cell loaded so far in the worldspace (immutable once published).
 *
 * Skyrim streams only the cells around the player, so the exact heights used to exist for a 5 x 5 block that moved with
 * the camera; everything else fell back to the coarse Terrain Shadows heightmap, which is metres off. Kept here, the
 * bathymetry stays exact wherever the player has been, and the shore field no longer changes each time a cell loads.
 */
struct LandCache
{
	static constexpr int32_t QuadsPerCell = 32;
	static constexpr uint32_t CellVertices = QuadsPerCell + 1;

	std::unordered_map<uint64_t, std::shared_ptr<const std::vector<float>>> cells;  ///< CellVertices^2 absolute z per cell, row-major from the south-west
	std::unordered_map<uint64_t, float> levels;                                     ///< still water level of each cached cell (NaN: none)
	uint32_t version = 0;                                                           ///< bumped whenever a cell is added

	static uint64_t Key(int32_t cellX, int32_t cellY) { return (static_cast<uint64_t>(static_cast<uint32_t>(cellX)) << 32) | static_cast<uint32_t>(cellY); }
	/** @brief Exact terrain z at absolute (x, y) (bilinear), NaN where no cached cell covers it. */
	float Sample(float x, float y) const;
	/** @brief The cached cell's still water level, NaN when it is not cached or has none. */
	float Level(int32_t cellX, int32_t cellY) const;
};

/**
 * @brief Per-worldspace data the wave model needs: directional fetch and bathymetry.
 *
 * Both are rebuilt on a background thread whenever the player's worldspace changes:
 *  - Fetch: for every water cell and 16 wind directions, the open-water distance upwind, ray-marched
 *    over Unified Water's cell coverage. Short fetch makes small, short waves (ponds, rivers), long
 *    fetch lets swell develop (open sea). Uploaded to the GPU as a Texture2DArray.
 *  - Bathymetry: a downsampled CPU copy of the Terrain Shadows heightmap so gameplay queries
 *    (swimming, buoyancy) see the same depth-limited and shoreline waves as the GPU.
 */
class WaterEnvironment
{
public:
	~WaterEnvironment();

	/** @brief Main thread: detects worldspace changes and starts a rebuild when needed. */
	void Update(const RE::TESWorldSpace* worldSpace);

	/**
	 * @brief Main thread: rebuilds the exact terrain grid of the loaded exterior cells when they change.
	 * The streamed LAND records give the true terrain (33 x 33 vertices per cell, 128 units apart), far
	 * finer than the 8-bit Terrain Shadows heightmap, which cannot resolve beaches at all.
	 */
	void UpdateLoadedLand(bool exterior);
	std::shared_ptr<const PBRWaterModel::Bathymetry> GetLand() const { return land.load(std::memory_order_acquire); }
	uint32_t GetLandGeneration() const { return landGeneration.load(std::memory_order_acquire); }

	std::shared_ptr<const PBRWaterModel::FetchField> GetFetch() const { return fetch.load(std::memory_order_acquire); }
	std::shared_ptr<const PBRWaterModel::Bathymetry> GetBathymetry() const { return bathymetry.load(std::memory_order_acquire); }

	/** @brief Incremented every time a new fetch field is published (render thread re-uploads on change). */
	uint32_t GetFetchGeneration() const { return fetchGeneration.load(std::memory_order_acquire); }

	bool IsBuilding() const { return building.load(std::memory_order_acquire); }

	/** @brief Bakes the directional fetch field from a cell coverage grid. */
	static std::shared_ptr<PBRWaterModel::FetchField> BakeFetch(const WaterCoverageView& coverage);

	/**
	 * @brief Main thread: keeps the shore field around the player current (the coastline map: the shore waves' travel
	 * times to the waterline, beach slopes, exposure, room). It is rebuilt on a worker whenever the player leaves its
	 * middle, the terrain or water data change (cells load), or the sea's dominant waves change period or the swell its
	 * direction.
	 * @param shore      the shore waves to trace
	 * @param fetch      open water upwind, for the exposure to the swell (null: every shore is open)
	 * @param fetchScale multiplies the baked fetch
	 */
	void UpdateShore(float playerX, float playerY, const PBRWaterModel::ShoreParams& shore, std::shared_ptr<const PBRWaterModel::FetchField> fetch, float fetchScale, bool enabled);
	std::shared_ptr<const PBRWaterModel::ShoreField> GetShore() const { return shore.load(std::memory_order_acquire); }
	bool IsBuildingShore() const { return shoreBuilding.load(std::memory_order_acquire); }

	/// Shore field size (texels per side) and texel size (units: the loaded terrain's vertex spacing).
	static constexpr uint32_t ShoreTexels = 768;
	static constexpr float ShoreStep = 128.0f;

private:
	void Rebuild(const RE::TESWorldSpace* worldSpace);
	void PublishShore(std::shared_ptr<const PBRWaterModel::ShoreField> field);

	const RE::TESWorldSpace* currentWorldSpace = nullptr;
	std::jthread worker;
	std::atomic_bool building{ false };
	std::atomic_uint32_t fetchGeneration{ 0 };

	std::atomic<std::shared_ptr<const PBRWaterModel::FetchField>> fetch;
	std::atomic<std::shared_ptr<const PBRWaterModel::Bathymetry>> bathymetry;
	std::atomic<std::shared_ptr<const PBRWaterModel::Bathymetry>> land;
	std::atomic_uint32_t landGeneration{ 0 };
	uint64_t landSignature = 0;

	std::atomic<std::shared_ptr<const CellLevels>> cellLevels;  ///< Unified Water's cache for the worldspace
	std::atomic<std::shared_ptr<const LandCache>> landCache;    ///< exact heights of every cell loaded so far (main thread writes)

	// Shore field (main thread state, worker build)
	std::jthread shoreWorker;
	std::atomic_bool shoreBuilding{ false };
	std::atomic_bool shoreCancel{ false };
	std::atomic<std::shared_ptr<const PBRWaterModel::ShoreField>> shore;
	std::atomic_uint32_t shoreEpoch{ 0 };  ///< bumped per worldspace: a build for an old one is never published
	float shoreCentreX = 0.0f, shoreCentreY = 0.0f;
	PBRWaterModel::ShoreParams shoreTraced;  ///< the waves the current field was traced for
	std::shared_ptr<const PBRWaterModel::Bathymetry> shoreCoarse;
	std::shared_ptr<const CellLevels> shoreLevels;
	uint32_t shoreLandVersion = 0;
	std::chrono::steady_clock::time_point shoreBuiltAt{};
};
