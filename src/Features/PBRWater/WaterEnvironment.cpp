#include "WaterEnvironment.h"

#include "Features/UnifiedWater.h"
#include "Globals.h"
#include "LandRecords.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>

namespace
{
	constexpr float CellSize = static_cast<float>(PBRWaterModel::CellSize);
	constexpr float MaxFetchCells = 160.0f;  // ~9.6 km marched; a ray that gets this far reaches the open sea
	constexpr float OpenSeaKm = 500.0f;      // fully developed: no fetch limit for any wind
	constexpr float StepCells = 0.5f;

	/// The worldspace whose LAND records are this one's terrain: child worldspaces can share their parent's.
	const RE::TESWorldSpace* LandWorld(const RE::TESWorldSpace* worldSpace)
	{
		while (worldSpace->parentWorld && worldSpace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
			worldSpace = worldSpace->parentWorld;
		return worldSpace;
	}

	std::optional<PBRWaterLand::Request> LandRequest(const RE::TESWorldSpace* worldSpace)
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		const auto* owner = worldSpace->GetFile(0);
		if (!dataHandler || !owner)
			return std::nullopt;

		PBRWaterLand::Request request;
		request.owner = owner->GetFilename();
		request.worldSpace = worldSpace->GetLocalFormID();
		for (const auto* file : dataHandler->files) {
			if (file && file->compileIndex != 0xFF)
				request.plugins.push_back(std::filesystem::path(file->path) / file->fileName);
		}
		return request;
	}
}

float CellLevels::At(int32_t cellX, int32_t cellY) const
{
	const int32_t x = cellX - minX;
	const int32_t y = cellY - minY;
	if (x < 0 || y < 0 || x >= static_cast<int32_t>(width) || y >= static_cast<int32_t>(height))
		return std::numeric_limits<float>::quiet_NaN();
	return level[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
}

WaterEnvironment::~WaterEnvironment()
{
	shoreCancel.store(true, std::memory_order_release);
	if (shoreWorker.joinable())
		shoreWorker.join();
	if (worker.joinable()) {
		worker.request_stop();
		worker.join();
	}
}

std::shared_ptr<PBRWaterModel::FetchField> WaterEnvironment::BakeFetch(const WaterCoverageView& coverage)
{
	using PBRWaterModel::FetchField;
	auto field = std::make_shared<FetchField>();
	field->minCellX = coverage.minX;
	field->minCellY = coverage.minY;
	field->width = coverage.width;
	field->height = coverage.height;
	field->openSeaKm = OpenSeaKm;
	field->kilometres.assign(static_cast<size_t>(FetchField::Directions) * coverage.width * coverage.height, 0.0f);

	auto isWater = [&](int32_t x, int32_t y) {
		// Outside the cached bounds is treated as open water: worldspaces end at the sea, not at a wall.
		if (x < 0 || y < 0 || x >= static_cast<int32_t>(coverage.width) || y >= static_cast<int32_t>(coverage.height))
			return true;
		return coverage.water[static_cast<size_t>(y) * coverage.width + x] != 0;
	};

	const size_t cells = static_cast<size_t>(coverage.width) * coverage.height;
	const float kmPerCell = CellSize * static_cast<float>(PBRWaterModel::MetresPerUnit) / 1000.0f;

	// Straight-line open water upwind of each cell, per direction.
	std::vector<float> radial(static_cast<size_t>(FetchField::Directions) * cells, 0.0f);
	for (uint32_t d = 0; d < FetchField::Directions; ++d) {
		// Slice d stores the fetch for wind blowing *towards* angle d; we march upwind.
		const float angle = static_cast<float>(PBRWaterModel::TwoPi) * d / FetchField::Directions;
		const float upwindX = -std::cos(angle) * StepCells;
		const float upwindY = -std::sin(angle) * StepCells;
		float* slice = &radial[d * cells];

		for (uint32_t y = 0; y < coverage.height; ++y) {
			for (uint32_t x = 0; x < coverage.width; ++x) {
				if (!coverage.water[static_cast<size_t>(y) * coverage.width + x])
					continue;
				float px = x + 0.5f;
				float py = y + 0.5f;
				float travelled = 0.0f;
				bool openSea = true;
				while (travelled < MaxFetchCells) {
					px += upwindX;
					py += upwindY;
					travelled += StepCells;
					const int32_t cx = static_cast<int32_t>(std::floor(px));
					const int32_t cy = static_cast<int32_t>(std::floor(py));
					if (!isWater(cx, cy)) {
						openSea = false;
						break;
					}
					// Past the edge of the map is the open ocean.
					if (cx < 0 || cy < 0 || cx >= static_cast<int32_t>(coverage.width) || cy >= static_cast<int32_t>(coverage.height))
						break;
				}
				// A ray that reaches the open sea has unlimited fetch; half a cell of open water exists even in a
				// single water cell.
				slice[static_cast<size_t>(y) * coverage.width + x] = openSea ? OpenSeaKm : std::max(travelled, 0.5f) * kmPerCell;
			}
		}
	}

	// A single upwind ray is far too sensitive: one headland or an offshore wind zeroes the sea. Waves
	// arrive from a spread of directions around the wind (the Shore Protection Manual's effective fetch,
	// USACE 1984, weights the radials within 45 degrees of upwind by cos a). The radials are averaged in log
	// space: a linear mean lets the one radial that sees the open sea dominate a sheltered harbour, while the
	// geometric mean grows with how much of the upwind sector is open (all open: the open sea; three of five:
	// tens of km; a harbour mouth: a few km).
	constexpr int SpreadSteps = 2;  // 2 x 22.5 degrees each side
	const float stepAngle = static_cast<float>(PBRWaterModel::TwoPi) / FetchField::Directions;
	for (size_t c = 0; c < cells; ++c) {
		if (!coverage.water[c])
			continue;
		for (uint32_t d = 0; d < FetchField::Directions; ++d) {
			float logSum = 0.0f;
			float weights = 0.0f;
			for (int k = -SpreadSteps; k <= SpreadSteps; ++k) {
				const uint32_t dk = (d + FetchField::Directions + k) % FetchField::Directions;
				const float w = std::cos(k * stepAngle);
				logSum += std::log(std::max(radial[dk * cells + c], 1e-3f)) * w;
				weights += w;
			}
			field->kilometres[d * cells + c] = std::exp(logSum / weights);
		}
	}
	return field;
}

void WaterEnvironment::Update(const RE::TESWorldSpace* worldSpace)
{
	if (worldSpace == currentWorldSpace)
		return;
	currentWorldSpace = worldSpace;
	Rebuild(worldSpace);
}

void WaterEnvironment::Rebuild(const RE::TESWorldSpace* worldSpace)
{
	if (worker.joinable()) {
		worker.request_stop();
		worker.join();
	}

	fetch.store(nullptr, std::memory_order_release);
	cellLevels.store(nullptr, std::memory_order_release);
	// The old worldspace's shore field (and any build still running for it) no longer applies.
	shoreEpoch.fetch_add(1, std::memory_order_acq_rel);
	PublishShore(nullptr);
	fetchGeneration.fetch_add(1, std::memory_order_acq_rel);

	if (!worldSpace)
		return;

	// Gather inputs on the calling (main) thread; the heavy work runs in the background.
	auto coverage = std::make_shared<WaterCache::Coverage>();
	bool hasCoverage = false;
	if (auto& unifiedWater = globals::features::unifiedWater; unifiedWater.loaded) {
		if (auto* cache = unifiedWater.GetWaterCache())
			hasCoverage = cache->GetCoverage(worldSpace, *coverage);
	}
	if (hasCoverage && coverage->level.size() == coverage->water.size()) {
		auto levels = std::make_shared<CellLevels>();
		levels->minX = coverage->minX;
		levels->minY = coverage->minY;
		levels->width = coverage->width;
		levels->height = coverage->height;
		levels->level = coverage->level;
		cellLevels.store(std::move(levels), std::memory_order_release);
	}

	// The land stays what it was on the way into an interior and back, or into a city that shares it.
	const auto* landWorld = LandWorld(worldSpace);
	std::optional<PBRWaterLand::Request> landRequest;
	if (landWorld != bathymetryWorld.load(std::memory_order_acquire)) {
		bathymetryWorld.store(nullptr, std::memory_order_release);
		bathymetry.store(nullptr, std::memory_order_release);
		bathymetryGeneration.fetch_add(1, std::memory_order_acq_rel);
		landRequest = LandRequest(landWorld);
	}

	if (!hasCoverage && !landRequest)
		return;

	building.store(true, std::memory_order_release);
	worker = std::jthread([this, coverage, hasCoverage, landRequest = std::move(landRequest), landWorld](std::stop_token stop) {
		if (hasCoverage && !stop.stop_requested()) {
			WaterCoverageView view{ coverage->minX, coverage->minY, coverage->width, coverage->height, coverage->water.data() };
			auto baked = BakeFetch(view);
			if (!stop.stop_requested()) {
				fetch.store(std::move(baked), std::memory_order_release);
				fetchGeneration.fetch_add(1, std::memory_order_acq_rel);
				logger::info("[PBR Water] Fetch field baked ({}x{} cells)", view.width, view.height);
			}
		}
		if (landRequest && !stop.stop_requested()) {
			const auto start = std::chrono::steady_clock::now();
			if (auto loaded = PBRWaterLand::Load(*landRequest, stop); loaded && !stop.stop_requested()) {
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
				logger::info("[PBR Water] Terrain read from {} plugins in {} ms ({}x{}, z {:.0f}..{:.0f})", landRequest->plugins.size(), elapsed.count(), loaded->width, loaded->height, loaded->minZ, loaded->maxZ);
				bathymetry.store(std::move(loaded), std::memory_order_release);
				bathymetryWorld.store(landWorld, std::memory_order_release);
				bathymetryGeneration.fetch_add(1, std::memory_order_acq_rel);
			}
		}
		building.store(false, std::memory_order_release);
	});
}

// ============================================================================
// Shore field
// ============================================================================

void WaterEnvironment::PublishShore(std::shared_ptr<const PBRWaterModel::ShoreField> field)
{
	shore.store(std::move(field), std::memory_order_release);
}

void WaterEnvironment::UpdateShore(float playerX, float playerY, const PBRWaterModel::ShoreParams& waves, std::shared_ptr<const PBRWaterModel::FetchField> fetchField, float fetchScale, bool enabled)
{
	if (!enabled) {
		if (shore.load(std::memory_order_acquire))
			PublishShore(nullptr);
		return;
	}
	// One build at a time; the next one starts from whatever is current when it finishes.
	if (shoreBuilding.load(std::memory_order_acquire))
		return;

	auto terrain = bathymetry.load(std::memory_order_acquire);
	auto levels = cellLevels.load(std::memory_order_acquire);
	if (!terrain) {
		if (shore.load(std::memory_order_acquire))
			PublishShore(nullptr);
		return;
	}

	const float half = 0.5f * ShoreTexels * ShoreStep;
	const bool moved = std::abs(playerX - shoreCentreX) > 0.25f * half || std::abs(playerY - shoreCentreY) > 0.25f * half;
	const bool newData = terrain != shoreBathymetry || levels != shoreLevels;
	// The travel times hold for the period they were traced for, the exposure for the swell's direction; a new field
	// takes over smoothly.
	auto turned = [](float a, float b, float degrees) { return std::abs(std::remainder(a - b, static_cast<float>(PBRWaterModel::TwoPi))) > degrees * std::numbers::pi_v<float> / 180.0f; };
	const bool newWaves = shoreTraced.omega <= 0.0f || std::abs(waves.omega / shoreTraced.omega - 1.0f) > 0.08f || turned(waves.swellDirection, shoreTraced.swellDirection, 8.0f);
	if (shore.load(std::memory_order_acquire) && !moved && !newData && !newWaves)
		return;

	shoreCentreX = playerX;
	shoreCentreY = playerY;
	shoreTraced = waves;
	shoreBathymetry = terrain;
	shoreLevels = levels;

	// Whole texels, so a rebuild never shifts the sampling grid.
	const float minX = std::floor((playerX - half) / ShoreStep) * ShoreStep;
	const float minY = std::floor((playerY - half) / ShoreStep) * ShoreStep;

	if (shoreWorker.joinable())
		shoreWorker.join();
	shoreBuilding.store(true, std::memory_order_release);
	const uint32_t epoch = shoreEpoch.load(std::memory_order_acquire);
	const float seaLevel = currentWorldSpace ? currentWorldSpace->GetDefaultWaterHeight() : std::numeric_limits<float>::quiet_NaN();
	shoreWorker = std::jthread([this, minX, minY, waves, fetchField, fetchScale, terrain, levels, seaLevel, epoch]() {
		PBRWaterModel::ShoreInputs inputs;
		inputs.width = ShoreTexels;
		inputs.height = ShoreTexels;
		inputs.minX = minX;
		inputs.minY = minY;
		inputs.step = ShoreStep;
		inputs.omega = waves.omega;
		inputs.swellDirection = waves.swellDirection;
		inputs.fetch = fetchField;
		inputs.fetchScale = fetchScale;
		inputs.seaLevel = seaLevel;
		const size_t count = static_cast<size_t>(ShoreTexels) * ShoreTexels;
		inputs.bed.assign(count, std::numeric_limits<float>::quiet_NaN());
		inputs.level.assign(count, std::numeric_limits<float>::quiet_NaN());

		// Still water level per cell: Unified Water's cache, else the sea's. A dry cell takes the highest level around it,
		// so the beach above a shoreline on a cell edge still knows the water below it.
		auto cellLevel = [&](int32_t cx, int32_t cy) { return levels ? levels->At(cx, cy) : seaLevel; };
		const int32_t cellMinX = static_cast<int32_t>(std::floor(minX / CellSize)) - 1;
		const int32_t cellMinY = static_cast<int32_t>(std::floor(minY / CellSize)) - 1;
		const int32_t cellCount = static_cast<int32_t>(std::ceil(ShoreTexels * ShoreStep / CellSize)) + 3;
		std::vector<float> cellLevelsLocal(static_cast<size_t>(cellCount) * cellCount, std::numeric_limits<float>::quiet_NaN());
		for (int32_t j = 0; j < cellCount; ++j) {
			for (int32_t i = 0; i < cellCount; ++i) {
				float level = cellLevel(cellMinX + i, cellMinY + j);
				if (!std::isfinite(level)) {
					for (int32_t dj = -1; dj <= 1; ++dj) {
						for (int32_t di = -1; di <= 1; ++di) {
							const float neighbour = cellLevel(cellMinX + i + di, cellMinY + j + dj);
							if (std::isfinite(neighbour) && (!std::isfinite(level) || neighbour > level))
								level = neighbour;
						}
					}
				}
				cellLevelsLocal[static_cast<size_t>(j) * cellCount + static_cast<size_t>(i)] = level;
			}
		}

		for (uint32_t y = 0; y < ShoreTexels && !shoreCancel.load(std::memory_order_relaxed); ++y) {
			const float wy = minY + static_cast<float>(y) * ShoreStep;
			const int32_t cy = static_cast<int32_t>(std::floor(wy / CellSize)) - cellMinY;
			for (uint32_t x = 0; x < ShoreTexels; ++x) {
				const float wx = minX + static_cast<float>(x) * ShoreStep;
				const size_t i = static_cast<size_t>(y) * ShoreTexels + x;
				if (terrain->Contains(wx, wy))
					inputs.bed[i] = terrain->Sample(wx, wy);
				const int32_t cx = static_cast<int32_t>(std::floor(wx / CellSize)) - cellMinX;
				if (cx >= 0 && cy >= 0 && cx < cellCount && cy < cellCount)
					inputs.level[i] = cellLevelsLocal[static_cast<size_t>(cy) * cellCount + static_cast<size_t>(cx)];
			}
		}

		std::shared_ptr<PBRWaterModel::ShoreField> field;
		if (!shoreCancel.load(std::memory_order_relaxed)) {
			try {
				field = PBRWaterModel::BuildShoreField(inputs, &shoreCancel);
			} catch (...) {
				field = nullptr;
			}
		}
		if (field && !shoreCancel.load(std::memory_order_relaxed) && epoch == shoreEpoch.load(std::memory_order_acquire))
			PublishShore(std::move(field));
		shoreBuilding.store(false, std::memory_order_release);
	});
}
