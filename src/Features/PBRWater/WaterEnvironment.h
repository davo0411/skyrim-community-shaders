#pragma once

#include "WaveModel.h"

#include <atomic>
#include <memory>
#include <thread>
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
	 * @brief Main thread: keeps the shore field around the player current (the shore waves' arrival times, beach
	 * slopes, exposure). It is rebuilt on a worker whenever the player leaves its middle, the terrain or water data
	 * change (cells load), or the sea's dominant waves change period or direction.
	 * @param shore      the shore wave components to trace
	 * @param fetch      open water upwind, for the exposure to the swell (null: every shore is open)
	 * @param fetchScale multiplies the baked fetch
	 */
	void UpdateShore(float playerX, float playerY, const PBRWaterModel::ShoreParams& shore, std::shared_ptr<const PBRWaterModel::FetchField> fetch, float fetchScale, bool enabled);
	std::shared_ptr<const PBRWaterModel::ShoreField> GetShore() const { return shore.load(std::memory_order_acquire); }
	/** @brief Incremented every time a new shore field is published (render thread re-uploads on change). */
	uint32_t GetShoreGeneration() const { return shoreGeneration.load(std::memory_order_acquire); }
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

	std::atomic<std::shared_ptr<const CellLevels>> cellLevels;    ///< Unified Water's cache for the worldspace
	std::atomic<std::shared_ptr<const CellLevels>> loadedLevels;  ///< the loaded cells (fallback)

	// Shore field (main thread state, worker build)
	std::jthread shoreWorker;
	std::atomic_bool shoreBuilding{ false };
	std::atomic_bool shoreCancel{ false };
	std::atomic<std::shared_ptr<const PBRWaterModel::ShoreField>> shore;
	std::atomic_uint32_t shoreGeneration{ 0 };
	std::atomic_uint32_t shoreEpoch{ 0 };  ///< bumped per worldspace: a build for an old one is never published
	float shoreCentreX = 0.0f, shoreCentreY = 0.0f;
	PBRWaterModel::ShoreParams shoreTraced;  ///< the waves the current field was traced for
	std::shared_ptr<const PBRWaterModel::Bathymetry> shoreLand, shoreCoarse;
	std::shared_ptr<const CellLevels> shoreLevels, shoreLoaded;
};
