#include "ProjectedGrid.h"
#include "WaterEnvironment.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace
{
	/// How far past the screen's edges a grid reaches (NDC), so waves pulled sideways or down never uncover the edge.
	constexpr float MarginX = 0.15f;
	constexpr float MarginY = 0.15f;
	/// Margin around a level's projected cell bounds (NDC).
	constexpr float BoundsPad = 0.05f;
	/// Within this of a level a mesh or a cell stands at it (units).
	constexpr float LevelTolerance = 4.0f;
	constexpr float CellUnits = 4096.0f;
	constexpr UINT VertexStride = 72;  ///< PBRWaterTess::Vertex
	/// Patch vertices: (Subdivision + 1)^2 a patch.
	constexpr UINT PatchVertexCount = (ProjectedGrid::Subdivision + 1) * (ProjectedGrid::Subdivision + 1);
	/// The args buffer: per level the patches' draw arguments (16 bytes), then per level their dispatch arguments (12 bytes).
	constexpr UINT ArgsBytes = ProjectedGrid::MaxLevels * 16 + ProjectedGrid::MaxLevels * 12;

	/// ProjectedGridCS.hlsl ProjectedGridCB.
	struct alignas(16) GridCB
	{
		float clipToWorld[4][4];
		uint32_t pass[4];
	};

	void Transform(const float m[4][4], const float v[4], float out[4])
	{
		for (int r = 0; r < 4; ++r)
			out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2] + m[r][3] * v[3];
	}

	/// The inverse of a matrix in either convention (the inverse of the transpose is the transpose of the inverse).
	bool Invert(const float m[4][4], float out[4][4])
	{
		DirectX::XMFLOAT4X4 in;
		std::memcpy(&in, m, sizeof(in));
		DirectX::XMVECTOR determinant;
		const DirectX::XMMATRIX inverse = DirectX::XMMatrixInverse(&determinant, DirectX::XMLoadFloat4x4(&in));
		if (std::abs(DirectX::XMVectorGetX(determinant)) < 1e-30f)
			return false;
		DirectX::XMFLOAT4X4 result;
		DirectX::XMStoreFloat4x4(&result, inverse);
		std::memcpy(out, &result, sizeof(result));
		return true;
	}

	std::unique_ptr<Buffer> CreateStructured(UINT stride, UINT count, bool srv, const char* name)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = stride * count;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;
		auto buffer = std::make_unique<Buffer>(desc, nullptr, name);
		if (srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC s{};
			s.Format = DXGI_FORMAT_UNKNOWN;
			s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			s.Buffer.NumElements = count;
			buffer->CreateSRV(s);
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
		u.Format = DXGI_FORMAT_UNKNOWN;
		u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		u.Buffer.NumElements = count;
		buffer->CreateUAV(u);
		return buffer;
	}

	bool CreateRaw(UINT bytes, UINT misc, const char* name, winrt::com_ptr<ID3D11Buffer>& buffer, winrt::com_ptr<ID3D11UnorderedAccessView>& uav)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | misc;
		auto device = globals::d3d::device;
		if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
			return false;
		Util::SetResourceName(buffer.get(), name);
		D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
		u.Format = DXGI_FORMAT_R32_TYPELESS;
		u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		u.Buffer.NumElements = bytes / 4;
		u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(buffer.get(), &u, uav.put())))
			return false;
		Util::SetResourceName(uav.get(), name);
		return true;
	}

	/// A grid's NDC rectangle and size before the budget.
	struct Layout
	{
		uint32_t levelIndex = 0;
		float range[4]{};
		float relative = 0.0f;
		float columns = 0.0f;
		float rows = 0.0f;
	};
}

void ProjectedGrid::Setup()
{
	CompileShaders();
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GridCB>(), "PBRWater::ProjectedGridCB");
	CreateBuffers();
}

void ProjectedGrid::CompileShaders()
{
	auto compile = [](const char* entry) {
		winrt::com_ptr<ID3D11ComputeShader> cs;
		cs.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\ProjectedGridCS.hlsl", {}, "cs_5_0", entry)));
		return cs;
	};
	gridCS = compile("GridCS");
	flagCS = compile("FlagCS");
	argsCS = compile("ArgsCS");
	patchCS = compile("PatchCS");
	if (!gridCS || !flagCS || !argsCS || !patchCS) {
		gridCS = nullptr;
		logger::warn("[PBR Water] The projected grid's shaders failed to compile; the water is drawn from its meshes");
	}
}

void ProjectedGrid::CreateBuffers()
{
	D3D11_BUFFER_DESC levelsDesc{};
	levelsDesc.ByteWidth = sizeof(GpuLevel) * MaxLevels;
	levelsDesc.Usage = D3D11_USAGE_DEFAULT;
	levelsDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	levelsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	levelsDesc.StructureByteStride = sizeof(GpuLevel);
	levelsBuffer = std::make_unique<Buffer>(levelsDesc, nullptr, "PBRWater::ProjectedGridLevels");
	D3D11_SHADER_RESOURCE_VIEW_DESC levelsView{};
	levelsView.Format = DXGI_FORMAT_UNKNOWN;
	levelsView.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	levelsView.Buffer.NumElements = MaxLevels;
	levelsBuffer->CreateSRV(levelsView);

	vertices = CreateStructured(VertexStride, MaxVertices, true, "PBRWater::ProjectedGridVertices");
	flags = CreateStructured(sizeof(uint32_t), MaxVertices, false, "PBRWater::ProjectedGridFlags");
	patches = CreateStructured(sizeof(uint32_t), MaxPatches, false, "PBRWater::ProjectedGridPatches");
	patchVertices = CreateStructured(VertexStride, MaxPatches * PatchVertexCount, true, "PBRWater::ProjectedGridPatchVertices");
	bool ok = CreateRaw(MaxLevels * 4, 0, "PBRWater::ProjectedGridCounters", counters, countersUAV);
	ok = ok && CreateRaw(ArgsBytes, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, "PBRWater::ProjectedGridArgs", args, argsUAV);

	if (!ok) {
		gridCS = nullptr;
		logger::warn("[PBR Water] The projected grid's buffers failed; the water is drawn from its meshes");
	}
}

void ProjectedGrid::SetCells(const std::shared_ptr<const CellLevels>& cells)
{
	cellsTexture = nullptr;
	cellsSRV = nullptr;
	levels.clear();
	excluded.clear();
	built.clear();
	builtFrame = UINT32_MAX;
	if (!cells || !cells->width || !cells->height || cells->level.size() != size_t(cells->width) * cells->height)
		return;

	// Every level's cells and bounds: the sea is the level most cells share, the lakes the others.
	std::map<int64_t, Level> found;
	std::vector<float> texels(cells->level.size(), -1e9f);
	for (uint32_t y = 0; y < cells->height; ++y) {
		for (uint32_t x = 0; x < cells->width; ++x) {
			const float z = cells->level[size_t(y) * cells->width + x];
			if (!std::isfinite(z))
				continue;
			texels[size_t(y) * cells->width + x] = z;
			auto& level = found[std::llround(z)];
			const float x0 = (cells->minX + static_cast<int32_t>(x)) * CellUnits;
			const float y0 = (cells->minY + static_cast<int32_t>(y)) * CellUnits;
			if (!level.cells) {
				level.z = static_cast<float>(std::llround(z));
				level.minX = x0;
				level.minY = y0;
				level.maxX = x0 + CellUnits;
				level.maxY = y0 + CellUnits;
			}
			level.minX = std::min(level.minX, x0);
			level.minY = std::min(level.minY, y0);
			level.maxX = std::max(level.maxX, x0 + CellUnits);
			level.maxY = std::max(level.maxY, y0 + CellUnits);
			++level.cells;
		}
	}
	if (found.empty())
		return;
	for (auto& [key, level] : found)
		levels.push_back(level);
	seaIndex = static_cast<uint32_t>(std::max_element(levels.begin(), levels.end(), [](const Level& a, const Level& b) { return a.cells < b.cells; }) - levels.begin());

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = cells->width;
	desc.Height = cells->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;  // rivers take their cells out (ExcludeRiver)
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{ texels.data(), cells->width * static_cast<UINT>(sizeof(float)), 0 };
	auto device = globals::d3d::device;
	if (FAILED(device->CreateTexture2D(&desc, &data, cellsTexture.put())) ||
		FAILED(device->CreateShaderResourceView(cellsTexture.get(), nullptr, cellsSRV.put()))) {
		cellsTexture = nullptr;
		cellsSRV = nullptr;
		levels.clear();
		return;
	}
	Util::SetResourceName(cellsTexture.get(), "PBRWater::ProjectedGridCells");
	Util::SetResourceName(cellsSRV.get(), "PBRWater::ProjectedGridCells SRV");
	cellsMinX = cells->minX;
	cellsMinY = cells->minY;
	cellsWidth = cells->width;
	cellsHeight = cells->height;
	cellsMapping = { static_cast<float>(cells->minX), static_cast<float>(cells->minY), static_cast<float>(cells->width), static_cast<float>(cells->height) };
	logger::info("[PBR Water] Projected grids: {} water levels in {}x{} cells, the sea at {:.0f}", levels.size(), cells->width, cells->height, SeaLevel());
}

int ProjectedGrid::FindLevel(float z) const
{
	for (size_t i = 0; i < levels.size(); ++i) {
		if (std::abs(levels[i].z - z) < LevelTolerance)
			return static_cast<int>(i);
	}
	return -1;
}

void ProjectedGrid::Want(float z, uint32_t frame)
{
	if (const int index = FindLevel(z); index >= 0)
		levels[index].wantedFrame = frame;
}

void ProjectedGrid::ExcludeRiver(float centerX, float centerY, float radius, float z)
{
	if (!cellsTexture || FindLevel(z) < 0)
		return;
	// The cells whose centres the mesh covers (its bound's inscribed square: a cell mesh's own cell).
	const float half = radius * 0.7071f;
	const int32_t x0 = static_cast<int32_t>(std::floor((centerX - half) / CellUnits + 0.5f));
	const int32_t x1 = static_cast<int32_t>(std::floor((centerX + half) / CellUnits - 0.5f));
	const int32_t y0 = static_cast<int32_t>(std::floor((centerY - half) / CellUnits + 0.5f));
	const int32_t y1 = static_cast<int32_t>(std::floor((centerY + half) / CellUnits - 0.5f));
	auto context = globals::d3d::context;
	for (int32_t y = y0; y <= y1; ++y) {
		for (int32_t x = x0; x <= x1; ++x) {
			const int32_t tx = x - cellsMinX, ty = y - cellsMinY;
			if (tx < 0 || ty < 0 || tx >= static_cast<int32_t>(cellsWidth) || ty >= static_cast<int32_t>(cellsHeight))
				continue;
			const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y);
			if (!excluded.insert(key).second)
				continue;
			const float none = -1e9f;
			const D3D11_BOX box{ static_cast<UINT>(tx), static_cast<UINT>(ty), 0, static_cast<UINT>(tx) + 1, static_cast<UINT>(ty) + 1, 1 };
			context->UpdateSubresource(cellsTexture.get(), 0, &box, &none, sizeof(float), sizeof(float));
		}
	}
}

void ProjectedGrid::SetTexCoords(float z, const float4& u, const float4& v)
{
	if (const int index = FindLevel(z); index >= 0) {
		levels[index].texCoordU = u;
		levels[index].texCoordV = v;
		levels[index].hasTexCoords = true;
	}
}

bool ProjectedGrid::HasTexCoords(float z) const
{
	const int index = FindLevel(z);
	return index < 0 || levels[index].hasTexCoords;
}

int ProjectedGrid::SlotOf(float z, uint32_t frame) const
{
	if (builtFrame != frame)
		return -1;
	for (size_t i = 0; i < built.size(); ++i) {
		if (std::abs(built[i].z - z) < LevelTolerance)
			return static_cast<int>(i);
	}
	return -1;
}

void ProjectedGrid::Build(const FrameParams& p, uint32_t frame)
{
	built.clear();
	builtFrame = UINT32_MAX;
	if (!gridCS || !cellsSRV || levels.empty() || p.viewportWidth < 1.0f || p.viewportHeight < 1.0f || p.maxDistance <= 0.0f)
		return;
	float inverse[4][4];
	if (!Invert(p.viewProj, inverse))
		return;

	// The camera's view direction, for where each plane's far ring meets the screen.
	const float centreNdc[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
	float centre[4];
	Transform(inverse, centreNdc, centre);
	const float dirX = centre[0] / centre[3], dirY = centre[1] / centre[3], dirZ = centre[2] / centre[3];
	const float flat = std::sqrt(dirX * dirX + dirY * dirY);

	// The levels whose meshes were drawn in the last few frames: the sea first, then the lakes nearest first (a level whose
	// water is nowhere in view costs nothing).
	auto wanted = [&](const Level& level) { return level.wantedFrame != 0 && level.wantedFrame + 2 >= frame; };
	std::vector<uint32_t> candidates;
	if (wanted(levels[seaIndex]))
		candidates.push_back(seaIndex);
	std::vector<std::pair<float, uint32_t>> lakes;
	for (uint32_t i = 0; i < levels.size(); ++i) {
		const auto& level = levels[i];
		if (i == seaIndex || !wanted(level))
			continue;
		const float dx = std::max({ level.minX - p.cameraX, 0.0f, p.cameraX - level.maxX });
		const float dy = std::max({ level.minY - p.cameraY, 0.0f, p.cameraY - level.maxY });
		lakes.emplace_back(dx * dx + dy * dy, i);
	}
	std::sort(lakes.begin(), lakes.end());
	for (const auto& [distance, index] : lakes) {
		if (candidates.size() >= MaxLevels)
			break;
		candidates.push_back(index);
	}

	std::vector<Layout> layouts;
	const float cell = std::max(p.cellPixels, 2.0f);
	for (const uint32_t index : candidates) {
		const auto& level = levels[index];
		// The plane relative to the camera, never edge-on: a camera on the water sees it from just above.
		float relative = level.z - p.cameraZ;
		if (std::abs(relative) < 1.0f)
			relative = relative > 0.0f ? 1.0f : -1.0f;
		const bool above = relative < 0.0f;

		// The rows run from the screen's edge to where the plane ends on screen: its ring at maxDistance straight ahead (the
		// horizon a hair beyond it). Looking straight down (or up from below) the plane fills the screen.
		float ring;
		if (flat < 1e-4f * std::abs(dirZ)) {
			if (dirZ * relative <= 0.0f)
				continue;
			ring = above ? 1e6f : -1e6f;
		} else {
			const float point[4] = { dirX / flat * p.maxDistance, dirY / flat * p.maxDistance, relative, 1.0f };
			float clip[4];
			Transform(p.viewProj, point, clip);
			ring = clip[3] > 1e-6f ? clip[1] / clip[3] : (above ? -1e6f : 1e6f);
		}
		Layout layout;
		layout.levelIndex = index;
		layout.relative = relative;
		layout.range[0] = -1.0f - MarginX;
		layout.range[1] = 1.0f + MarginX;
		layout.range[2] = above ? -1.0f - MarginY : std::max(ring - 0.01f, -1.0f - MarginY);
		layout.range[3] = above ? std::min(ring + 0.01f, 1.0f + MarginY) : 1.0f + MarginY;

		// The level's cells on screen: their bounds' corners, when all are in front of the camera.
		{
			const float corners[4][2] = { { level.minX, level.minY }, { level.maxX, level.minY }, { level.minX, level.maxY }, { level.maxX, level.maxY } };
			float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
			bool inFront = true;
			for (const auto& corner : corners) {
				const float point[4] = { corner[0] - p.cameraX, corner[1] - p.cameraY, relative, 1.0f };
				float clip[4];
				Transform(p.viewProj, point, clip);
				if (clip[3] < 1.0f) {
					inFront = false;
					break;
				}
				for (int k = 0; k < 2; ++k) {
					lo[k] = std::min(lo[k], clip[k] / clip[3]);
					hi[k] = std::max(hi[k], clip[k] / clip[3]);
				}
			}
			if (inFront) {
				layout.range[0] = std::max(layout.range[0], lo[0] - BoundsPad);
				layout.range[1] = std::min(layout.range[1], hi[0] + BoundsPad);
				layout.range[2] = std::max(layout.range[2], lo[1] - BoundsPad);
				layout.range[3] = std::min(layout.range[3], hi[1] + BoundsPad);
			}
		}
		if (layout.range[1] <= layout.range[0] || layout.range[3] <= layout.range[2])
			continue;
		layout.columns = std::ceil((layout.range[1] - layout.range[0]) * 0.5f * p.viewportWidth / cell);
		layout.rows = std::ceil((layout.range[3] - layout.range[2]) * 0.5f * p.viewportHeight / cell);
		layouts.push_back(layout);
	}
	if (layouts.empty())
		return;

	// Cells of about cellPixels on screen, coarser for all when the buffers would overflow.
	float need = 0.0f;
	for (const auto& layout : layouts)
		need += (layout.columns + 1.0f) * (layout.rows + 1.0f);
	const float scale = need > MaxVertices ? std::sqrt(need / MaxVertices) * 1.02f : 1.0f;

	std::array<GpuLevel, MaxLevels> gpu{};
	uint32_t vertexCursor = 0;
	uint32_t cellCursor = 0;
	const uint32_t patchesEach = MaxPatches / static_cast<uint32_t>(layouts.size());
	for (const auto& layout : layouts) {
		const uint32_t columns = static_cast<uint32_t>(std::clamp(std::floor(layout.columns / scale), 1.0f, 4096.0f));
		const uint32_t rows = static_cast<uint32_t>(std::clamp(std::floor(layout.rows / scale), 1.0f, 4096.0f));
		const uint32_t count = (columns + 1) * (rows + 1);
		if (vertexCursor + count > MaxVertices)
			break;
		const auto& level = levels[layout.levelIndex];
		const uint32_t slot = static_cast<uint32_t>(built.size());
		BuiltLevel b;
		b.z = level.z;
		b.columns = columns;
		b.rows = rows;
		b.firstVertex = vertexCursor;
		b.firstCell = cellCursor;
		b.firstPatch = slot * patchesEach;
		built.push_back(b);

		auto& g = gpu[slot];
		std::copy_n(layout.range, 4, g.range);
		g.plane[0] = layout.relative;
		g.plane[1] = p.maxDistance;
		g.layout[0] = columns;
		g.layout[1] = rows;
		g.layout[2] = vertexCursor;
		g.layout[3] = cellCursor;
		g.patches[0] = b.firstPatch;
		g.patches[1] = patchesEach;
		g.texCoordU = level.texCoordU;
		g.texCoordV = level.texCoordV;
		vertexCursor += count;
		cellCursor += columns * rows;
	}
	if (built.empty())
		return;

	auto context = globals::d3d::context;
	context->UpdateSubresource(levelsBuffer->resource.get(), 0, nullptr, gpu.data(), 0, 0);
	const UINT zeros[4] = {};
	context->ClearUnorderedAccessViewUint(countersUAV.get(), zeros);

	GridCB data{};
	std::memcpy(data.clipToWorld, inverse, sizeof(inverse));
	data.pass[1] = static_cast<uint32_t>(built.size());
	ID3D11Buffer* cb = constants->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* levelsView = levelsBuffer->srv.get();
	context->CSSetShaderResources(0, 1, &levelsView);
	ID3D11UnorderedAccessView* uavs[5] = { vertices->uav.get(), flags->uav.get(), patches->uav.get(), countersUAV.get(), patchVertices->uav.get() };
	context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);

	auto pass = [&](uint32_t slot) {
		data.pass[0] = slot;
		constants->Update(data);
	};
	context->CSSetShader(gridCS.get(), nullptr, 0);
	for (uint32_t slot = 0; slot < built.size(); ++slot) {
		pass(slot);
		context->Dispatch((built[slot].columns + 1 + 7) / 8, (built[slot].rows + 1 + 7) / 8, 1);
	}
	context->CSSetShader(flagCS.get(), nullptr, 0);
	for (uint32_t slot = 0; slot < built.size(); ++slot) {
		pass(slot);
		context->Dispatch((built[slot].columns + 7) / 8, (built[slot].rows + 7) / 8, 1);
	}
	// The patch counts into the arguments, then the arguments off the compute stage: the patches' dispatches read them.
	ID3D11UnorderedAccessView* argsView = argsUAV.get();
	context->CSSetUnorderedAccessViews(5, 1, &argsView, nullptr);
	context->CSSetShader(argsCS.get(), nullptr, 0);
	context->Dispatch(1, 1, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(5, 1, &nullUav, nullptr);
	context->CSSetShader(patchCS.get(), nullptr, 0);
	for (uint32_t slot = 0; slot < built.size(); ++slot) {
		pass(slot);
		context->DispatchIndirect(args.get(), MaxLevels * 16 + slot * 12);
	}

	ID3D11UnorderedAccessView* nullUavs[5] = {};
	context->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	builtFrame = frame;
}

void ProjectedGrid::Draw(uint32_t vertexCount, const ID3D11ShaderResourceView* vertexView, uint32_t argsOffset) const
{
	auto context = globals::d3d::context;
	// In the tessellator's vertex slot (Water.hlsl TessellatedVertices): the grid's vertices are the same struct.
	auto* view = const_cast<ID3D11ShaderResourceView*>(vertexView);
	context->VSSetShaderResources(109, 1, &view);

	// A triangle list, both faces: the grid's winding on screen flips with the side of the plane the camera is on.
	D3D11_PRIMITIVE_TOPOLOGY topology{};
	context->IAGetPrimitiveTopology(&topology);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	winrt::com_ptr<ID3D11RasterizerState> engineState;
	context->RSGetState(engineState.put());
	auto& state = twoSided[engineState.get()];
	if (!state) {
		D3D11_RASTERIZER_DESC desc{ D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
		if (engineState)
			engineState->GetDesc(&desc);
		desc.CullMode = D3D11_CULL_NONE;
		if (SUCCEEDED(globals::d3d::device->CreateRasterizerState(&desc, state.put())))
			Util::SetResourceName(state.get(), "PBRWater::ProjectedGridRasterizer");
	}
	if (state)
		context->RSSetState(state.get());

	if (vertexCount)
		context->Draw(vertexCount, 0);
	else
		context->DrawInstancedIndirect(args.get(), argsOffset);

	context->RSSetState(engineState.get());
	context->IASetPrimitiveTopology(topology);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->VSSetShaderResources(109, 1, &nullSrv);
}

void ProjectedGrid::DrawCells(uint32_t slot) const
{
	if (slot < built.size())
		Draw(built[slot].columns * built[slot].rows * 6, vertices->srv.get(), 0);
}

void ProjectedGrid::DrawPatches(uint32_t slot) const
{
	if (slot < built.size())
		Draw(0, patchVertices->srv.get(), slot * 16);
}
