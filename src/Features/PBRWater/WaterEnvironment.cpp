#include "WaterEnvironment.h"

#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
#include "Globals.h"

#include <DirectXTex.h>
#include <algorithm>
#include <cmath>

namespace
{
	constexpr float CellSize = 4096.0f;
	constexpr float MaxFetchCells = 160.0f;  // ~9.6 km; beyond that the sea is fully developed for gameplay winds
	constexpr float StepCells = 0.5f;
	constexpr uint32_t BathymetryDownsample = 2;

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

WaterEnvironment::~WaterEnvironment()
{
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
	field->openSeaKm = MaxFetchCells * CellSize * static_cast<float>(PBRWaterModel::MetresPerUnit) / 1000.0f;
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
				while (travelled < MaxFetchCells) {
					px += upwindX;
					py += upwindY;
					travelled += StepCells;
					if (!isWater(static_cast<int32_t>(std::floor(px)), static_cast<int32_t>(std::floor(py))))
						break;
				}
				// Half a cell of open water exists even in a single water cell.
				slice[static_cast<size_t>(y) * coverage.width + x] = std::max(travelled, 0.5f) * kmPerCell;
			}
		}
	}

	// A single upwind ray is far too sensitive: one headland or an offshore wind zeroes the sea. Waves
	// arrive from a spread of directions around the wind, so use the Shore Protection Manual (USACE 1984)
	// effective fetch, F = sum(F_i cos^2 a_i) / sum(cos a_i) over radials within 45 degrees of upwind.
	// Swell generated over the open sea also reaches any exposed water whatever the local wind, so no
	// cell gets less than a share of its longest radial.
	constexpr int SpreadSteps = 2;  // 2 x 22.5 degrees each side
	constexpr float SwellExposure = 0.4f;
	const float stepAngle = static_cast<float>(PBRWaterModel::TwoPi) / FetchField::Directions;
	for (size_t c = 0; c < cells; ++c) {
		if (!coverage.water[c])
			continue;
		float longest = 0.0f;
		for (uint32_t d = 0; d < FetchField::Directions; ++d)
			longest = std::max(longest, radial[d * cells + c]);
		for (uint32_t d = 0; d < FetchField::Directions; ++d) {
			float weighted = 0.0f;
			float weights = 0.0f;
			for (int k = -SpreadSteps; k <= SpreadSteps; ++k) {
				const uint32_t dk = (d + FetchField::Directions + k) % FetchField::Directions;
				const float cosA = std::cos(k * stepAngle);
				weighted += radial[dk * cells + c] * cosA * cosA;
				weights += cosA;
			}
			field->kilometres[d * cells + c] = std::max(weighted / weights, SwellExposure * longest);
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
			cells.push_back({ coords->cellX, coords->cellY, landRecord->loadedData });
			signature = (signature ^ (static_cast<uint64_t>(static_cast<uint32_t>(coords->cellX)) << 32 | static_cast<uint32_t>(coords->cellY))) * 1099511628211ull;
			signature = (signature ^ reinterpret_cast<uintptr_t>(landRecord->loadedData)) * 1099511628211ull;
		});
	}
	if (signature == landSignature)
		return;
	landSignature = signature;

	if (cells.empty()) {
		land.store(nullptr, std::memory_order_release);
		landGeneration.fetch_add(1, std::memory_order_acq_rel);
		return;
	}

	// 32 quads per cell (two 16-quad quadrants per side); neighbouring cells share their edge vertices.
	constexpr int32_t QuadsPerCell = 32;
	constexpr float VertexSpacing = CellSize / QuadsPerCell;
	int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
	for (const auto& c : cells) {
		minX = std::min(minX, c.x);
		minY = std::min(minY, c.y);
		maxX = std::max(maxX, c.x);
		maxY = std::max(maxY, c.y);
	}

	auto grid = std::make_shared<PBRWaterModel::Bathymetry>();
	grid->width = static_cast<uint32_t>((maxX - minX + 1) * QuadsPerCell + 1);
	grid->height = static_cast<uint32_t>((maxY - minY + 1) * QuadsPerCell + 1);
	grid->minX = minX * CellSize;
	grid->minY = minY * CellSize;
	grid->stepX = grid->stepY = VertexSpacing;

	// Cells inside the rectangle that are not loaded fall back to the coarse heightmap, else deep water.
	const auto coarse = bathymetry.load(std::memory_order_acquire);
	grid->heights.resize(static_cast<size_t>(grid->width) * grid->height);
	for (uint32_t y = 0; y < grid->height; ++y) {
		for (uint32_t x = 0; x < grid->width; ++x) {
			const float wx = grid->minX + x * VertexSpacing;
			const float wy = grid->minY + y * VertexSpacing;
			grid->heights[static_cast<size_t>(y) * grid->width + x] = (coarse && coarse->Valid()) ? coarse->Sample(wx, wy) : -1.0e5f;
		}
	}

	// Quadrants: 0 south-west, 1 south-east, 2 north-west, 3 north-east; 17 x 17 vertices each, row-major from the south.
	// The streamed heights are relative to a per-cell base; heightExtents holds the cell's absolute
	// min / max, which recovers that base.
	for (const auto& c : cells) {
		const auto [minH, maxH] = std::minmax_element(&c.data->heights[0][0], &c.data->heights[0][0] + 4 * 289);
		const float base = 0.5f * ((c.data->heightExtents.x - *minH) + (c.data->heightExtents.y - *maxH));
		const uint32_t baseX = static_cast<uint32_t>((c.x - minX) * QuadsPerCell);
		const uint32_t baseY = static_cast<uint32_t>((c.y - minY) * QuadsPerCell);
		for (uint32_t quad = 0; quad < 4; ++quad) {
			const uint32_t qx = baseX + (quad & 1) * 16;
			const uint32_t qy = baseY + (quad >> 1) * 16;
			for (uint32_t vy = 0; vy < 17; ++vy) {
				for (uint32_t vx = 0; vx < 17; ++vx) {
					const float h = c.data->heights[quad][vy * 17 + vx];
					if (std::isfinite(h))
						grid->heights[static_cast<size_t>(qy + vy) * grid->width + qx + vx] = base + h;
				}
			}
		}
	}

	const auto [lo, hi] = std::minmax_element(grid->heights.begin(), grid->heights.end());
	logger::info("[PBR Water] Loaded terrain grid {}x{} cells at ({}, {}), z {:.0f}..{:.0f}", maxX - minX + 1, maxY - minY + 1, minX, minY, *lo, *hi);
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
