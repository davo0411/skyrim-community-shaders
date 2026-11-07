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
 * Both are built on a background thread when the player's worldspace changes:
 *  - Fetch: for every water cell and 16 wind directions, the open-water distance upwind, ray-marched
 *    over Unified Water's cell coverage. Short fetch makes small, short waves (ponds, rivers), long
 *    fetch lets swell develop (open sea). Uploaded to the GPU as a Texture2DArray.
 *  - Bathymetry: the terrain of the whole worldspace, read from the plugins' LAND records (PBRWaterLand): the GPU's
 *    depth, the shore field's bed, and what gameplay queries (swimming, buoyancy) see. Kept while the player stays on
 *    the same land: child worldspaces share their parent's, and interiors have none.
 */
class WaterEnvironment
{
public:
	~WaterEnvironment();

	/** @brief Main thread: detects worldspace changes and starts a rebuild when needed. */
	void Update(const RE::TESWorldSpace* worldSpace);

	std::shared_ptr<const PBRWaterModel::FetchField> GetFetch() const { return fetch.load(std::memory_order_acquire); }
	std::shared_ptr<const PBRWaterModel::Bathymetry> GetBathymetry() const { return bathymetry.load(std::memory_order_acquire); }

	/** @brief Incremented every time a new fetch field is published (render thread re-uploads on change). */
	uint32_t GetFetchGeneration() const { return fetchGeneration.load(std::memory_order_acquire); }
	/** @brief The same for the bathymetry. */
	uint32_t GetBathymetryGeneration() const { return bathymetryGeneration.load(std::memory_order_acquire); }

	bool IsBuilding() const { return building.load(std::memory_order_acquire); }

	/** @brief Bakes the directional fetch field from a cell coverage grid. */
	static std::shared_ptr<PBRWaterModel::FetchField> BakeFetch(const WaterCoverageView& coverage);

	/**
	 * @brief Main thread: keeps the shore field around the player current (the coastline map: the shore waves' travel
	 * times to the waterline, beach slopes, exposure, room). It is rebuilt on a worker whenever the player leaves its
	 * middle, the terrain or water data change, or the sea's dominant waves change period or the swell its
	 * direction.
	 * @param shore      the shore waves to trace
	 * @param fetch      open water upwind, for the exposure to the swell (null: every shore is open)
	 * @param fetchScale multiplies the baked fetch
	 */
	void UpdateShore(float playerX, float playerY, const PBRWaterModel::ShoreParams& shore, std::shared_ptr<const PBRWaterModel::FetchField> fetch, float fetchScale, bool enabled);
	std::shared_ptr<const PBRWaterModel::ShoreField> GetShore() const { return shore.load(std::memory_order_acquire); }
	bool IsBuildingShore() const { return shoreBuilding.load(std::memory_order_acquire); }
	/** @brief Wall time of the last finished fetch / terrain build (worker thread, ms; Debug section). */
	float GetLastBuildMs() const { return lastBuildMs.load(std::memory_order_relaxed); }
	/** @brief Wall time of the last finished shore field trace (worker thread, ms; Debug section). */
	float GetLastShoreBuildMs() const { return lastShoreBuildMs.load(std::memory_order_relaxed); }
	/**
	 * @brief Main thread: forgets what the shore field was traced for. A build in flight is cancelled and the next
	 * UpdateShore traces a new field at once; the current one stays published until that is ready.
	 */
	void InvalidateShore();

	/// Shore field size (texels per side) and texel size (units: the terrain's vertex spacing).
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
	std::atomic_uint32_t bathymetryGeneration{ 0 };
	std::atomic<const RE::TESWorldSpace*> bathymetryWorld{ nullptr };  ///< whose land `bathymetry` holds

	std::atomic<std::shared_ptr<const CellLevels>> cellLevels;  ///< Unified Water's cache for the worldspace

	// Shore field (main thread state, worker build)
	std::jthread shoreWorker;
	std::atomic_bool shoreBuilding{ false };
	std::atomic<float> lastBuildMs{ 0.0f };
	std::atomic<float> lastShoreBuildMs{ 0.0f };
	std::atomic_bool shoreCancel{ false };
	std::atomic<std::shared_ptr<const PBRWaterModel::ShoreField>> shore;
	std::atomic_uint32_t shoreEpoch{ 0 };  ///< bumped per worldspace: a build for an old one is never published
	float shoreCentreX = 0.0f, shoreCentreY = 0.0f;
	PBRWaterModel::ShoreParams shoreTraced;  ///< the waves the current field was traced for
	std::shared_ptr<const PBRWaterModel::Bathymetry> shoreBathymetry;
	std::shared_ptr<const CellLevels> shoreLevels;
};
