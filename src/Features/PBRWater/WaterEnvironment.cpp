#include "WaterEnvironment.h"

#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	constexpr float CellSize = static_cast<float>(PBRWaterModel::CellSize);
	constexpr float MaxFetchCells = 160.0f;  // ~9.6 km marched; a ray that gets this far reaches the open sea
	constexpr float OpenSeaKm = 500.0f;      // fully developed: no fetch limit for any wind
	constexpr float StepCells = 0.5f;
	constexpr uint32_t BathymetryDownsample = 2;
	/// Exact terrain is carried this many 128-unit texels (half a cell) past the cells it is known for, fading into the
	/// coarse heightmap.
	constexpr int FeatherTexels = 16;

	float Smooth01(float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/**
	 * Blends exact terrain heights into the coarse heightmap where the exact ones end, so the bed - and every depth the
	 * waves take from it - has no step there. The coarse BC4 heightmap is metres off: a hard switch put a straight cliff
	 * in the depth along the edge of the exact data, and the waves' height, breaking and arrival times jumped along it.
	 * Exact values are carried outwards ring by ring for `radius` texels (each the mean of its already known neighbours)
	 * and faded into the coarse heights over that distance.
	 * @param bed    exact heights, NaN where unknown; replaced by the blended bed (NaN only where neither is known)
	 * @param coarse coarse heights of the same texels (NaN where unknown)
	 */
	void FeatherBed(std::vector<float>& bed, const std::vector<float>& coarse, uint32_t w, uint32_t h, int radius)
	{
		const size_t n = bed.size();
		std::vector<float> carried(bed);
		std::vector<int> ring(n), nextRing(n);
		for (size_t i = 0; i < n; ++i)
			ring[i] = std::isfinite(bed[i]) ? 0 : -1;
		for (int k = 1; k <= radius; ++k) {
			nextRing = ring;
			bool any = false;
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (ring[i] >= 0)
						continue;
					float sum = 0.0f;
					int count = 0;
					auto take = [&](size_t j) {
						if (ring[j] >= 0) {
							sum += carried[j];
							++count;
						}
					};
					if (x > 0)
						take(i - 1);
					if (x + 1 < w)
						take(i + 1);
					if (y > 0)
						take(i - w);
					if (y + 1 < h)
						take(i + w);
					if (count) {
						carried[i] = sum / static_cast<float>(count);
						nextRing[i] = k;
						any = true;
					}
				}
			}
			ring.swap(nextRing);
			if (!any)
				break;
		}
		for (size_t i = 0; i < n; ++i) {
			if (ring[i] == 0)
				continue;
			if (ring[i] < 0) {
				bed[i] = coarse[i];
				continue;
			}
			const float weight = 1.0f - Smooth01(static_cast<float>(ring[i]) / static_cast<float>(radius + 1));
			bed[i] = std::isfinite(coarse[i]) ? coarse[i] + (carried[i] - coarse[i]) * weight : carried[i];
		}
	}

	struct HeightmapRequest
	{
		std::filesystem::path path;
		float3 pos0;
		float3 pos1;
		float2 heightRange;  ///< pos0.z..pos1.z: what the normalised texels decode to
	};

	std::optional<HeightmapRequest> FindHeightmap(const RE::TESWorldSpace* worldSpace)
	{
		auto& terrainShadows = globals::features::terrainShadows;
		if (!terrainShadows.loaded || !worldSpace)
			return std::nullopt;

		// Same resolution rule as TerrainShadows::LoadHeightmap: child worldspaces share their parent's land.
		while (worldSpace->parentWorld && worldSpace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
			worldSpace = worldSpace->parentWorld;

		const char* editorID = worldSpace->GetFormEditorID();
		if (!editorID)
			return std::nullopt;
		const auto it = terrainShadows.heightmaps.find(editorID);
		if (it == terrainShadows.heightmaps.end())
			return std::nullopt;

		HeightmapRequest request;
		request.path = std::filesystem::path(it->second.dir) / it->second.filename;
		request.pos0 = it->second.pos0;
		request.pos1 = it->second.pos1;
		request.heightRange = { it->second.pos0.z, it->second.pos1.z };
		return request;
	}

	std::shared_ptr<PBRWaterModel::Bathymetry> LoadBathymetry(const HeightmapRequest& request, std::stop_token stop)
	{
		DirectX::ScratchImage image;
		if (FAILED(DirectX::LoadFromDDSFile(request.path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image))) {
			logger::warn("[PBR Water] Could not load heightmap {} for bathymetry", request.path.string());
			return nullptr;
		}

		DirectX::ScratchImage converted;
		const auto& meta = image.GetMetadata();
		HRESULT hr = DirectX::IsCompressed(meta.format) ?
		                 DirectX::Decompress(*image.GetImage(0, 0, 0), DXGI_FORMAT_R32_FLOAT, converted) :
		                 DirectX::Convert(*image.GetImage(0, 0, 0), DXGI_FORMAT_R32_FLOAT, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted);
		if (FAILED(hr)) {
			logger::warn("[PBR Water] Could not convert heightmap {} ({:X})", request.path.string(), static_cast<uint32_t>(hr));
			return nullptr;
		}

		const auto* src = converted.GetImage(0, 0, 0);
		auto bathymetry = std::make_shared<PBRWaterModel::Bathymetry>();
		bathymetry->width = static_cast<uint32_t>(src->width / BathymetryDownsample);
		bathymetry->height = static_cast<uint32_t>(src->height / BathymetryDownsample);
		if (bathymetry->width < 2 || bathymetry->height < 2)
			return nullptr;

		// Texel (i, j) centre sits at pos0 + (pos1 - pos0) * (i, j + 1) / size (see TerrainShadows::GetCommonBufferData).
		const float texelX = (request.pos1.x - request.pos0.x) / static_cast<float>(src->width);
		const float texelY = (request.pos1.y - request.pos0.y) / static_cast<float>(src->height);
		bathymetry->minX = request.pos0.x;
		bathymetry->minY = request.pos0.y + texelY;
		bathymetry->stepX = texelX * BathymetryDownsample;
		bathymetry->stepY = texelY * BathymetryDownsample;

		bathymetry->heights.resize(static_cast<size_t>(bathymetry->width) * bathymetry->height);
		for (uint32_t y = 0; y < bathymetry->height; ++y) {
			if (stop.stop_requested())
				return nullptr;
			const auto* row = reinterpret_cast<const float*>(src->pixels + static_cast<size_t>(y * BathymetryDownsample) * src->rowPitch);
			for (uint32_t x = 0; x < bathymetry->width; ++x) {
				const float normalised = row[x * BathymetryDownsample];
				bathymetry->heights[static_cast<size_t>(y) * bathymetry->width + x] = request.heightRange.x + (request.heightRange.y - request.heightRange.x) * normalised;
			}
		}
		return bathymetry;
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

float LandCache::Sample(float x, float y) const
{
	const int32_t cellX = static_cast<int32_t>(std::floor(x / CellSize));
	const int32_t cellY = static_cast<int32_t>(std::floor(y / CellSize));
	const auto it = cells.find(Key(cellX, cellY));
	if (it == cells.end())
		return std::numeric_limits<float>::quiet_NaN();
	const auto& heights = *it->second;
	const float spacing = CellSize / QuadsPerCell;
	const float fx = std::clamp((x - cellX * CellSize) / spacing, 0.0f, static_cast<float>(QuadsPerCell));
	const float fy = std::clamp((y - cellY * CellSize) / spacing, 0.0f, static_cast<float>(QuadsPerCell));
	const uint32_t x0 = std::min(static_cast<uint32_t>(fx), static_cast<uint32_t>(QuadsPerCell - 1));
	const uint32_t y0 = std::min(static_cast<uint32_t>(fy), static_cast<uint32_t>(QuadsPerCell - 1));
	const float tx = fx - static_cast<float>(x0);
	const float ty = fy - static_cast<float>(y0);
	const float* row0 = &heights[static_cast<size_t>(y0) * CellVertices];
	const float* row1 = row0 + CellVertices;
	const float a = row0[x0] + (row0[x0 + 1] - row0[x0]) * tx;
	const float b = row1[x0] + (row1[x0 + 1] - row1[x0]) * tx;
	return a + (b - a) * ty;
}

float LandCache::Level(int32_t cellX, int32_t cellY) const
{
	const auto it = levels.find(Key(cellX, cellY));
	return it != levels.end() ? it->second : std::numeric_limits<float>::quiet_NaN();
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

void WaterEnvironment::UpdateLoadedLand(bool exterior)
{
	struct LoadedCell
	{
		int32_t x, y;
		const RE::TESObjectLAND::LoadedLandData* data;
		float waterLevel;
	};
	std::vector<LoadedCell> cells;
	uint64_t signature = 1469598103934665603ull;
	if (auto tes = RE::TES::GetSingleton(); tes && exterior) {
		tes->ForEachCell([&](RE::TESObjectCELL* cell) {
			if (!cell || !cell->IsExteriorCell() || !cell->IsAttached())
				return;
			const auto* coords = cell->GetCoordinates();
			const auto* landRecord = cell->GetRuntimeData().cellLand;
			if (!coords || !landRecord || !landRecord->loadedData)
				return;
			const float waterLevel = cell->GetExteriorWaterHeight();
			cells.push_back({ coords->cellX, coords->cellY, landRecord->loadedData, waterLevel > -1.0e6f ? waterLevel : std::numeric_limits<float>::quiet_NaN() });
			signature = (signature ^ (static_cast<uint64_t>(static_cast<uint32_t>(coords->cellX)) << 32 | static_cast<uint32_t>(coords->cellY))) * 1099511628211ull;
			signature = (signature ^ reinterpret_cast<uintptr_t>(landRecord->loadedData)) * 1099511628211ull;
		});
	}
	// The coarse heightmap fills the cells never loaded; it is built after the worldspace loads, so its arrival refreshes the
	// grid too (until then those cells read as deep water).
	signature = (signature ^ reinterpret_cast<uintptr_t>(bathymetry.load(std::memory_order_acquire).get())) * 1099511628211ull;
	if (signature == landSignature)
		return;
	landSignature = signature;

	if (cells.empty()) {
		land.store(nullptr, std::memory_order_release);
		landGeneration.fetch_add(1, std::memory_order_acq_rel);
		return;
	}

	// Remember every loaded cell's exact heights and water level for as long as the player stays in the worldspace.
	// Quadrants: 0 south-west, 1 south-east, 2 north-west, 3 north-east; 17 x 17 vertices each, row-major from the south.
	// The streamed heights are relative to a per-cell base; heightExtents holds the cell's absolute min / max, which
	// recovers that base.
	constexpr int32_t QuadsPerCell = LandCache::QuadsPerCell;
	constexpr uint32_t CellVertices = LandCache::CellVertices;
	constexpr float VertexSpacing = CellSize / QuadsPerCell;
	const auto previousCache = landCache.load(std::memory_order_acquire);
	auto cache = previousCache ? std::make_shared<LandCache>(*previousCache) : std::make_shared<LandCache>();
	bool added = false;
	for (const auto& c : cells) {
		auto heights = std::make_shared<std::vector<float>>(static_cast<size_t>(CellVertices) * CellVertices, std::numeric_limits<float>::quiet_NaN());
		const auto [minH, maxH] = std::minmax_element(&c.data->heights[0][0], &c.data->heights[0][0] + 4 * 289);
		const float base = 0.5f * ((c.data->heightExtents.x - *minH) + (c.data->heightExtents.y - *maxH));
		for (uint32_t quad = 0; quad < 4; ++quad) {
			const uint32_t qx = (quad & 1) * 16;
			const uint32_t qy = (quad >> 1) * 16;
			for (uint32_t vy = 0; vy < 17; ++vy) {
				for (uint32_t vx = 0; vx < 17; ++vx) {
					const float h = c.data->heights[quad][vy * 17 + vx];
					if (std::isfinite(h))
						(*heights)[static_cast<size_t>(qy + vy) * CellVertices + qx + vx] = base + h;
				}
			}
		}
		const uint64_t key = LandCache::Key(c.x, c.y);
		added = added || !cache->cells.contains(key);
		cache->cells[key] = std::move(heights);
		cache->levels[key] = c.waterLevel;
	}
	if (added)
		cache->version = (previousCache ? previousCache->version : 0) + 1;
	landCache.store(cache, std::memory_order_release);

	// The grid the shaders and gameplay sample: 11 x 11 cells around the loaded block, exact wherever a cell was ever
	// loaded, feathered into the coarse heightmap elsewhere.
	constexpr int32_t WindowRadius = 5;
	int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
	for (const auto& c : cells) {
		minX = std::min(minX, c.x);
		minY = std::min(minY, c.y);
		maxX = std::max(maxX, c.x);
		maxY = std::max(maxY, c.y);
	}
	const int32_t centreX = (minX + maxX) / 2;
	const int32_t centreY = (minY + maxY) / 2;
	const int32_t windowX = centreX - WindowRadius;
	const int32_t windowY = centreY - WindowRadius;
	const int32_t windowCells = 2 * WindowRadius + 1;

	auto grid = std::make_shared<PBRWaterModel::Bathymetry>();
	grid->width = static_cast<uint32_t>(windowCells * QuadsPerCell + 1);
	grid->height = grid->width;
	grid->minX = windowX * CellSize;
	grid->minY = windowY * CellSize;
	grid->stepX = grid->stepY = VertexSpacing;
	const size_t count = static_cast<size_t>(grid->width) * grid->height;
	grid->heights.assign(count, std::numeric_limits<float>::quiet_NaN());
	std::vector<float> coarseHeights(count, std::numeric_limits<float>::quiet_NaN());
	const auto coarse = bathymetry.load(std::memory_order_acquire);
	for (uint32_t y = 0; y < grid->height; ++y) {
		for (uint32_t x = 0; x < grid->width; ++x) {
			const float wx = grid->minX + x * VertexSpacing;
			const float wy = grid->minY + y * VertexSpacing;
			if (coarse && coarse->Valid())
				coarseHeights[static_cast<size_t>(y) * grid->width + x] = coarse->Sample(wx, wy);
		}
	}
	for (int32_t cy = 0; cy < windowCells; ++cy) {
		for (int32_t cx = 0; cx < windowCells; ++cx) {
			const auto it = cache->cells.find(LandCache::Key(windowX + cx, windowY + cy));
			if (it == cache->cells.end())
				continue;
			const auto& heights = *it->second;
			for (uint32_t vy = 0; vy < CellVertices; ++vy) {
				for (uint32_t vx = 0; vx < CellVertices; ++vx) {
					const float h = heights[static_cast<size_t>(vy) * CellVertices + vx];
					if (std::isfinite(h))
						grid->heights[static_cast<size_t>(cy * QuadsPerCell + vy) * grid->width + cx * QuadsPerCell + vx] = h;
				}
			}
		}
	}
	FeatherBed(grid->heights, coarseHeights, grid->width, grid->height, FeatherTexels);
	for (auto& h : grid->heights) {
		if (!std::isfinite(h))
			h = -1.0e5f;  // neither known: deep water
	}

	const auto [lo, hi] = std::minmax_element(grid->heights.begin(), grid->heights.end());
	logger::info("[PBR Water] Terrain grid {}x{} cells at ({}, {}), {} cells cached, z {:.0f}..{:.0f}", windowCells, windowCells, windowX, windowY, cache->cells.size(), *lo, *hi);
	land.store(std::move(grid), std::memory_order_release);
	landGeneration.fetch_add(1, std::memory_order_acq_rel);
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
	bathymetry.store(nullptr, std::memory_order_release);
	cellLevels.store(nullptr, std::memory_order_release);
	// Cells are per worldspace.
	landCache.store(nullptr, std::memory_order_release);
	land.store(nullptr, std::memory_order_release);
	landSignature = 0;
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
	auto heightmap = FindHeightmap(worldSpace);

	if (!hasCoverage && !heightmap)
		return;

	building.store(true, std::memory_order_release);
	worker = std::jthread([this, coverage, hasCoverage, heightmap](std::stop_token stop) {
		if (hasCoverage && !stop.stop_requested()) {
			WaterCoverageView view{ coverage->minX, coverage->minY, coverage->width, coverage->height, coverage->water.data() };
			auto baked = BakeFetch(view);
			if (!stop.stop_requested()) {
				fetch.store(std::move(baked), std::memory_order_release);
				fetchGeneration.fetch_add(1, std::memory_order_acq_rel);
				logger::info("[PBR Water] Fetch field baked ({}x{} cells)", view.width, view.height);
			}
		}
		if (heightmap && !stop.stop_requested()) {
			if (auto loaded = LoadBathymetry(*heightmap, stop); loaded && !stop.stop_requested()) {
				logger::info("[PBR Water] Bathymetry loaded ({}x{})", loaded->width, loaded->height);
				bathymetry.store(std::move(loaded), std::memory_order_release);
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

	auto cache = landCache.load(std::memory_order_acquire);
	auto coarse = bathymetry.load(std::memory_order_acquire);
	auto levels = cellLevels.load(std::memory_order_acquire);
	if ((!cache && !coarse) || (!levels && !cache)) {
		if (shore.load(std::memory_order_acquire))
			PublishShore(nullptr);
		return;
	}

	const float half = 0.5f * ShoreTexels * ShoreStep;
	const bool moved = std::abs(playerX - shoreCentreX) > 0.25f * half || std::abs(playerY - shoreCentreY) > 0.25f * half;
	// Newly loaded cells refine the bed: picked up, but not more often than every 10 s (each field takes over over a few
	// seconds, and walking along a coast loads a cell every few seconds).
	const uint32_t landVersion = cache ? cache->version : 0;
	const auto now = std::chrono::steady_clock::now();
	const bool newLand = landVersion != shoreLandVersion && now - shoreBuiltAt > std::chrono::seconds(10);
	const bool newData = newLand || coarse != shoreCoarse || levels != shoreLevels;
	// The arrival times hold for the period and directions they were traced for; a new field takes over smoothly.
	auto turned = [](float a, float b, float degrees) { return std::abs(std::remainder(a - b, static_cast<float>(PBRWaterModel::TwoPi))) > degrees * std::numbers::pi_v<float> / 180.0f; };
	bool newWaves = shoreTraced.omega <= 0.0f || std::abs(waves.omega / shoreTraced.omega - 1.0f) > 0.08f || turned(waves.swellDirection, shoreTraced.swellDirection, 8.0f);
	for (uint32_t c = 0; c < PBRWaterModel::ShoreComponents; ++c)
		newWaves = newWaves || turned(waves.componentDirection[c], shoreTraced.componentDirection[c], 4.0f);
	if (shore.load(std::memory_order_acquire) && !moved && !newData && !newWaves)
		return;

	shoreCentreX = playerX;
	shoreCentreY = playerY;
	shoreTraced = waves;
	shoreLandVersion = landVersion;
	shoreBuiltAt = now;
	shoreCoarse = coarse;
	shoreLevels = levels;

	// Whole texels, so a rebuild never shifts the sampling grid.
	const float minX = std::floor((playerX - half) / ShoreStep) * ShoreStep;
	const float minY = std::floor((playerY - half) / ShoreStep) * ShoreStep;

	if (shoreWorker.joinable())
		shoreWorker.join();
	shoreBuilding.store(true, std::memory_order_release);
	const uint32_t epoch = shoreEpoch.load(std::memory_order_acquire);
	const float seaLevel = currentWorldSpace ? currentWorldSpace->GetDefaultWaterHeight() : std::numeric_limits<float>::quiet_NaN();
	shoreWorker = std::jthread([this, minX, minY, waves, fetchField, fetchScale, cache, coarse, levels, seaLevel, epoch]() {
		PBRWaterModel::ShoreInputs inputs;
		inputs.width = ShoreTexels;
		inputs.height = ShoreTexels;
		inputs.minX = minX;
		inputs.minY = minY;
		inputs.step = ShoreStep;
		inputs.omega = waves.componentOmega;
		inputs.direction = waves.componentDirection;
		inputs.swellDirection = waves.swellDirection;
		inputs.fetch = fetchField;
		inputs.fetchScale = fetchScale;
		inputs.seaLevel = seaLevel;
		const size_t count = static_cast<size_t>(ShoreTexels) * ShoreTexels;
		inputs.bed.assign(count, std::numeric_limits<float>::quiet_NaN());
		inputs.level.assign(count, std::numeric_limits<float>::quiet_NaN());
		std::vector<float> coarseBed(count, std::numeric_limits<float>::quiet_NaN());

		// Still water level per cell: Unified Water's cache, else the loaded cells. A dry cell takes the highest
		// level around it, so the beach above a shoreline on a cell edge still knows the water below it.
		auto cellLevel = [&](int32_t cx, int32_t cy) {
			float level = levels ? levels->At(cx, cy) : std::numeric_limits<float>::quiet_NaN();
			if (!std::isfinite(level) && cache)
				level = cache->Level(cx, cy);
			return level;
		};
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
				inputs.bed[i] = cache ? cache->Sample(wx, wy) : std::numeric_limits<float>::quiet_NaN();
				if (coarse && coarse->Contains(wx, wy))
					coarseBed[i] = coarse->Sample(wx, wy);
				const int32_t cx = static_cast<int32_t>(std::floor(wx / CellSize)) - cellMinX;
				if (cx >= 0 && cy >= 0 && cx < cellCount && cy < cellCount)
					inputs.level[i] = cellLevelsLocal[static_cast<size_t>(cy) * cellCount + static_cast<size_t>(cx)];
			}
		}

		FeatherBed(inputs.bed, coarseBed, ShoreTexels, ShoreTexels, FeatherTexels);

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
