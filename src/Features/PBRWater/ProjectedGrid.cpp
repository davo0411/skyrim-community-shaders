#include "ProjectedGrid.h"
#include "WaterEnvironment.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace
{
	/// Most vertices the grid holds (72 bytes each): about 12 x 12 px cells at 4K, finer below.
	constexpr uint32_t MaxVertices = 300000;
	/// How far past the screen's edges the grid reaches (NDC), so waves pulled sideways or down never uncover the edge.
	constexpr float MarginX = 0.15f;
	constexpr float MarginY = 0.15f;
	/// Within this of the sea's level a cell holds the sea (units; cell levels are exact, the meshes' bounds are not).
	constexpr float LevelTolerance = 2.0f;

	/// ProjectedGridCS.hlsl ProjectedGridCB.
	struct alignas(16) GridCB
	{
		float clipToWorld[4][4];
		uint32_t size[4];
		float range[4];
		float plane[4];
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
}

void ProjectedGrid::Setup()
{
	gridCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\ProjectedGridCS.hlsl", {}, "cs_5_0")));
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GridCB>(), "PBRWater::ProjectedGridCB");

	// Water.hlsl reads it as the tessellator's vertices (PBRWaterTess::Vertex, 72 bytes).
	constexpr UINT stride = 72;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = stride * MaxVertices;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = stride;
	vertices = std::make_unique<Buffer>(desc, nullptr, "PBRWater::ProjectedGridVertices");
	D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_UNKNOWN;
	srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv.Buffer.NumElements = MaxVertices;
	vertices->CreateSRV(srv);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = MaxVertices;
	vertices->CreateUAV(uav);
	capacity = MaxVertices;

	if (!gridCS)
		logger::warn("[PBR Water] The projected grid's shader failed to compile; the sea is drawn from its meshes");
}

void ProjectedGrid::SetCells(const std::shared_ptr<const CellLevels>& cells)
{
	cellsTexture = nullptr;
	cellsSRV = nullptr;
	builtFrame = UINT32_MAX;
	if (!cells || !cells->width || !cells->height || cells->level.size() != size_t(cells->width) * cells->height)
		return;

	// The sea is the level most cells share: lakes and rivers stand at levels of their own, a few cells each.
	std::map<int64_t, uint32_t> counts;
	for (const float level : cells->level) {
		if (std::isfinite(level))
			++counts[std::llround(level)];
	}
	if (counts.empty())
		return;
	const auto sea = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
	seaLevel = static_cast<float>(sea->first);

	std::vector<float> texels(cells->level.size());
	for (size_t i = 0; i < texels.size(); ++i) {
		const float level = cells->level[i];
		texels[i] = std::isfinite(level) && std::abs(level - seaLevel) <= LevelTolerance ? level : -1e9f;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = cells->width;
	desc.Height = cells->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{ texels.data(), cells->width * static_cast<UINT>(sizeof(float)), 0 };
	auto device = globals::d3d::device;
	if (FAILED(device->CreateTexture2D(&desc, &data, cellsTexture.put())) ||
		FAILED(device->CreateShaderResourceView(cellsTexture.get(), nullptr, cellsSRV.put()))) {
		cellsTexture = nullptr;
		cellsSRV = nullptr;
		return;
	}
	Util::SetResourceName(cellsTexture.get(), "PBRWater::ProjectedGridCells");
	Util::SetResourceName(cellsSRV.get(), "PBRWater::ProjectedGridCells SRV");
	cellsMapping = { static_cast<float>(cells->minX), static_cast<float>(cells->minY), static_cast<float>(cells->width), static_cast<float>(cells->height) };
	logger::info("[PBR Water] Projected grid: sea at {:.0f} in {}x{} cells", seaLevel, cells->width, cells->height);
}

bool ProjectedGrid::Build(const FrameParams& p, uint32_t frame)
{
	builtFrame = UINT32_MAX;
	if (!gridCS || !constants || !vertices || !cellsSRV || p.viewportWidth < 1.0f || p.viewportHeight < 1.0f || p.maxDistance <= 0.0f)
		return false;

	// The sea's plane relative to the camera, never edge-on: a camera on the water sees it from just above.
	float level = seaLevel - p.cameraZ;
	if (std::abs(level) < 1.0f)
		level = level > 0.0f ? 1.0f : -1.0f;
	const bool above = level < 0.0f;

	float inverse[4][4];
	if (!Invert(p.viewProj, inverse))
		return false;

	// The rows run from the screen's edge to where the sea ends on screen: the ring of the plane at maxDistance straight
	// ahead (the horizon a hair beyond it). Looking straight down (or up from below) the sea fills the screen.
	const float centreNdc[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
	float centre[4];
	Transform(inverse, centreNdc, centre);
	const float dirX = centre[0] / centre[3], dirY = centre[1] / centre[3], dirZ = centre[2] / centre[3];
	const float flat = std::sqrt(dirX * dirX + dirY * dirY);
	float ring;
	if (flat < 1e-4f * std::abs(dirZ)) {
		if (dirZ * level <= 0.0f)
			return false;  // straight away from the plane
		ring = above ? 1e6f : -1e6f;
	} else {
		const float point[4] = { dirX / flat * p.maxDistance, dirY / flat * p.maxDistance, level, 1.0f };
		float clip[4];
		Transform(p.viewProj, point, clip);
		ring = clip[3] > 1e-6f ? clip[1] / clip[3] : (above ? -1e6f : 1e6f);
	}
	const float yMin = above ? -1.0f - MarginY : std::max(ring - 0.01f, -1.0f - MarginY);
	const float yMax = above ? std::min(ring + 0.01f, 1.0f + MarginY) : 1.0f + MarginY;
	if (yMax <= yMin)
		return false;

	// Cells of about cellPixels on screen, fewer when the budget would be exceeded.
	const float cell = std::max(p.cellPixels, 2.0f);
	float cols = std::ceil(p.viewportWidth * (1.0f + MarginX) / cell);
	float rowCount = std::ceil((yMax - yMin) * 0.5f * p.viewportHeight / cell);
	const float scale = std::sqrt((cols + 1.0f) * (rowCount + 1.0f) / static_cast<float>(capacity));
	if (scale > 1.0f) {
		cols = std::floor(cols / scale) - 1.0f;
		rowCount = std::floor(rowCount / scale) - 1.0f;
	}
	columns = static_cast<uint32_t>(std::clamp(cols, 1.0f, 4096.0f));
	rows = static_cast<uint32_t>(std::clamp(rowCount, 1.0f, 4096.0f));
	if ((columns + 1) * (rows + 1) > capacity)
		return false;

	GridCB data{};
	std::memcpy(data.clipToWorld, inverse, sizeof(inverse));
	data.size[0] = columns;
	data.size[1] = rows;
	data.range[0] = -1.0f - MarginX;
	data.range[1] = 1.0f + MarginX;
	data.range[2] = yMin;
	data.range[3] = yMax;
	data.plane[0] = level;
	data.plane[1] = p.maxDistance;
	constants->Update(data);

	auto context = globals::d3d::context;
	ID3D11Buffer* cb = constants->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11UnorderedAccessView* uav = vertices->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(gridCS.get(), nullptr, 0);
	context->Dispatch((columns + 1 + 7) / 8, (rows + 1 + 7) / 8, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	builtFrame = frame;
	return true;
}

void ProjectedGrid::Draw() const
{
	if (!vertices || !columns || !rows)
		return;
	auto context = globals::d3d::context;
	// In the tessellator's vertex slot (Water.hlsl TessellatedVertices): the grid's vertices are the same struct.
	ID3D11ShaderResourceView* srv = vertices->srv.get();
	context->VSSetShaderResources(109, 1, &srv);

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

	context->Draw(columns * rows * 6, 0);

	context->RSSetState(engineState.get());
	context->IASetPrimitiveTopology(topology);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->VSSetShaderResources(109, 1, &nullSrv);
}
