#include "ShoreSimulation.h"
#include "Profiling.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

namespace
{
	std::unique_ptr<Texture2D> CreateStateTexture(DXGI_FORMAT format, const char* name, uint32_t size = ShoreSimulation::GridSize)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = size;
		desc.Height = size;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

		auto texture = std::make_unique<Texture2D>(desc, name);
		texture->CreateSRV(srvDesc);
		texture->CreateUAV(uavDesc);
		return texture;
	}
}

void ShoreSimulation::SetupResources()
{
	wet[0] = CreateStateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::SwashWet0");
	wet[1] = CreateStateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::SwashWet1");
	foam[0] = CreateStateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::ShoreFoam0", FoamGridSize);
	foam[1] = CreateStateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::ShoreFoam1", FoamGridSize);
	stage = CreateStateTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, "PBRWater::SwashStage", FoamGridSize);
	prep = CreateStateTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, "PBRWater::SwashPrep", FoamGridSize);
	surf = CreateStateTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, "PBRWater::Surf", FoamGridSize);
	surfMoved = CreateStateTexture(DXGI_FORMAT_R32G32B32A32_FLOAT, "PBRWater::SurfMoved", FoamGridSize);
	spraySource = CreateStateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::SpraySource", FoamGridSize);
	simCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SimCB>(), "PBRWater::SwashCB");
	SetupTileResources();
	ClearShaderCache();
	Reset();
}

void ShoreSimulation::ClearShaderCache()
{
	simCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\ShoreSimCS.hlsl", {}, "cs_5_0")));
	ready = simCS && wet[0] && wet[1] && simCB;
	if (!simCS)
		logger::warn("[PBR Water] Shore wetness shader failed to compile; the water leaves no wet sand behind");
	foamCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\ShoreFoamCS.hlsl", {}, "cs_5_0")));
	momentumCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashStepCS.hlsl", {}, "cs_5_0")));
	continuityCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashStepCS.hlsl", { { "SWASH_CONTINUITY", "" } }, "cs_5_0")));
	outCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashOutCS.hlsl", {}, "cs_5_0")));
	foamReady = foamCS && momentumCS && continuityCS && outCS && foam[0] && foam[1] && stage && prep && surf && surfMoved && spraySource && simCB;
	tilesMarkCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashTilesCS.hlsl", { { "SWASH_TILES_MARK", "" } }, "cs_5_0")));
	tilesListCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashTilesCS.hlsl", { { "SWASH_TILES_LIST", "" } }, "cs_5_0")));
	momentumTilesCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashStepCS.hlsl", { { "SWASH_TILES", "" } }, "cs_5_0")));
	continuityTilesCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SwashStepCS.hlsl", { { "SWASH_CONTINUITY", "" }, { "SWASH_TILES", "" } }, "cs_5_0")));
	// Without them every step covers the whole grid.
	tilesReady = tilesMarkCS && tilesListCS && momentumTilesCS && continuityTilesCS && busyTiles && tileListSRV && tileListUAV && tileArgs && tilesCB;
	if (!foamReady)
		logger::warn("[PBR Water] Shore foam or surf simulation shaders failed to compile; the shore waves end at the waterline and leave no foam behind");
}

void ShoreSimulation::SetupTileResources()
{
	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = TileCount;
	desc.Height = TileCount;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	busyTiles = std::make_unique<Texture2D>(desc, "PBRWater::SwashBusyTiles");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	busyTiles->CreateSRV(srvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	busyTiles->CreateUAV(uavDesc);

	D3D11_BUFFER_DESC listDesc{};
	listDesc.ByteWidth = TileCount * TileCount * sizeof(uint32_t);
	listDesc.Usage = D3D11_USAGE_DEFAULT;
	listDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	listDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	listDesc.StructureByteStride = sizeof(uint32_t);
	if (FAILED(device->CreateBuffer(&listDesc, nullptr, tileList.put())))
		return;
	Util::SetResourceName(tileList.get(), "PBRWater::SwashTileList");
	D3D11_SHADER_RESOURCE_VIEW_DESC listSrv{};
	listSrv.Format = DXGI_FORMAT_UNKNOWN;
	listSrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	listSrv.Buffer.NumElements = TileCount * TileCount;
	if (FAILED(device->CreateShaderResourceView(tileList.get(), &listSrv, tileListSRV.put())))
		return;
	Util::SetResourceName(tileListSRV.get(), "PBRWater::SwashTileList SRV");
	D3D11_UNORDERED_ACCESS_VIEW_DESC listUav{};
	listUav.Format = DXGI_FORMAT_UNKNOWN;
	listUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	listUav.Buffer.NumElements = TileCount * TileCount;
	listUav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
	if (FAILED(device->CreateUnorderedAccessView(tileList.get(), &listUav, tileListUAV.put())))
		return;
	Util::SetResourceName(tileListUAV.get(), "PBRWater::SwashTileList UAV");

	// ThreadGroupCountX is the list's count (CopyStructureCount); Y and Z stay 1.
	const uint32_t args[3] = { 0, 1, 1 };
	D3D11_BUFFER_DESC argsDesc{};
	argsDesc.ByteWidth = sizeof(args);
	argsDesc.Usage = D3D11_USAGE_DEFAULT;
	argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
	if (FAILED(device->CreateBuffer(&argsDesc, &init, tileArgs.put())))
		return;
	Util::SetResourceName(tileArgs.get(), "PBRWater::SwashTileArgs");

	tilesCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TilesCB>(), "PBRWater::SwashTilesCB");
}

bool ShoreSimulation::ListActiveTiles(uint32_t steps)
{
	if (!tilesReady)
		return false;
	auto context = globals::d3d::context;
	PBRWaterProfiling::Scope pass("PBRWater::SwashTiles");

	TilesCB cb{};
	cb.gridSize = FoamGridSize;
	cb.tileCount = TileCount;
	// Water moves a cell a step at most: the frame's remaining steps cannot carry it further than this from a busy tile.
	cb.radius = static_cast<int32_t>((steps - 1 + 7) / 8);
	tilesCB->Update(cb);
	ID3D11Buffer* cbs[1] = { tilesCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);

	const UINT zero[4] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(busyTiles->uav.get(), zero);
	ID3D11ShaderResourceView* nullSrvs[2] = {};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	{
		ID3D11ShaderResourceView* srvs[2] = { surf->srv.get(), prep->srv.get() };
		ID3D11UnorderedAccessView* uav = busyTiles->uav.get();
		context->CSSetShader(tilesMarkCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(FoamGridSize / 8, FoamGridSize / 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShaderResources(0, 2, nullSrvs);
	}
	{
		ID3D11ShaderResourceView* srv = busyTiles->srv.get();
		ID3D11UnorderedAccessView* uav = tileListUAV.get();
		const UINT reset = 0;
		context->CSSetShader(tilesListCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, &reset);
		context->Dispatch((TileCount + 7) / 8, (TileCount + 7) / 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShaderResources(0, 1, nullSrvs);
	}
	context->CopyStructureCount(tileArgs.get(), 0, tileListUAV.get());

	// The steps' own constants again.
	cbs[0] = simCB->CB();
	context->CSSetConstantBuffers(0, 1, cbs);
	return true;
}

void ShoreSimulation::Reset()
{
	hasOrigin = false;
	auto context = globals::d3d::context;
	const float dry[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < 2; ++i) {
		if (wet[i])
			context->ClearUnorderedAccessViewFloat(wet[i]->uav.get(), dry);
		if (foam[i])
			context->ClearUnorderedAccessViewFloat(foam[i]->uav.get(), dry);
	}
	for (auto* texture : { surf.get(), surfMoved.get() }) {
		if (texture)
			context->ClearUnorderedAccessViewFloat(texture->uav.get(), dry);
	}
	foamHasOrigin = false;
}

void ShoreSimulation::Update(float cameraX, float cameraY, float dt, const Settings& settings)
{
	if (!ready)
		return;
	auto context = globals::d3d::context;

	const float newTexelSize = std::max(settings.extent, 1024.0f) / GridSize;
	if (newTexelSize != texelSize) {
		texelSize = newTexelSize;
		hasOrigin = false;
	}

	// Whole texels, so scrolling never resamples (and never smears) the state.
	const int64_t newOriginX = static_cast<int64_t>(std::floor(cameraX / texelSize)) - GridSize / 2;
	const int64_t newOriginY = static_cast<int64_t>(std::floor(cameraY / texelSize)) - GridSize / 2;
	if (!hasOrigin || std::abs(newOriginX - originTexelX) >= GridSize || std::abs(newOriginY - originTexelY) >= GridSize) {
		Reset();
		originTexelX = newOriginX;
		originTexelY = newOriginY;
		hasOrigin = true;
	}

	SimCB cb{};
	cb.shiftX = static_cast<int32_t>(newOriginX - originTexelX);
	cb.shiftY = static_cast<int32_t>(newOriginY - originTexelY);
	cb.gridSize = GridSize;
	// A long hitch must not dry the sand in one step.
	cb.dt = std::clamp(dt, 0.0f, 0.1f);
	cb.originX = static_cast<float>(newOriginX) * texelSize;
	cb.originY = static_cast<float>(newOriginY) * texelSize;
	cb.texelSize = texelSize;
	cb.dryRate = std::max(settings.dryRate, 0.0f);
	simCB->Update(cb);

	PBRWaterProfiling::Scope pass("PBRWater::ShoreWetness");
	const uint32_t next = current ^ 1;
	ID3D11ShaderResourceView* srv = wet[current]->srv.get();
	ID3D11UnorderedAccessView* uav = wet[next]->uav.get();
	ID3D11Buffer* cbs[1] = { simCB->CB() };
	context->CSSetShader(simCS.get(), nullptr, 0);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(0, 1, cbs);
	context->Dispatch(GridSize / 8, GridSize / 8, 1);

	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	current = next;
	originTexelX = newOriginX;
	originTexelY = newOriginY;
}

void ShoreSimulation::UpdateFoam(float cameraX, float cameraY, float dt)
{
	if (!foamReady)
		return;
	auto context = globals::d3d::context;

	// Whole texels, so scrolling never resamples (and never smears) the foam and the surf.
	const int64_t newOriginX = static_cast<int64_t>(std::floor(cameraX / FoamTexelSize)) - FoamGridSize / 2;
	const int64_t newOriginY = static_cast<int64_t>(std::floor(cameraY / FoamTexelSize)) - FoamGridSize / 2;
	bool fresh = false;
	if (!foamHasOrigin || std::abs(newOriginX - foamOriginX) >= FoamGridSize || std::abs(newOriginY - foamOriginY) >= FoamGridSize) {
		const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (auto& texture : foam)
			context->ClearUnorderedAccessViewFloat(texture->uav.get(), clear);
		// The surf scrolls in with the water at rest (the first step shifts the old state out of the grid).
		context->ClearUnorderedAccessViewFloat(surf->uav.get(), clear);
		foamOriginX = newOriginX;
		foamOriginY = newOriginY;
		foamHasOrigin = true;
		fresh = true;
	}

	SimCB cb{};
	cb.shiftX = static_cast<int32_t>(newOriginX - foamOriginX);
	cb.shiftY = static_cast<int32_t>(newOriginY - foamOriginY);
	cb.gridSize = FoamGridSize;
	// A long hitch must not carry the foam off in one step.
	cb.dt = std::clamp(dt, 0.0f, 0.1f);
	cb.originX = static_cast<float>(newOriginX) * FoamTexelSize;
	cb.originY = static_cast<float>(newOriginY) * FoamTexelSize;
	cb.texelSize = FoamTexelSize;
	simCB->Update(cb);

	ID3D11Buffer* cbs[1] = { simCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	const UINT groups = FoamGridSize / 8;
	ID3D11ShaderResourceView* nullSrvs[3] = {};
	ID3D11UnorderedAccessView* nullUavs[3] = {};

	// The surf's foam, and what the waves bring the surf simulation this frame.
	{
		PBRWaterProfiling::Scope pass("PBRWater::ShoreFoam");
		ID3D11ShaderResourceView* srv = foam[foamCurrent]->srv.get();
		ID3D11UnorderedAccessView* uavs[3] = { stage->uav.get(), prep->uav.get(), spraySource->uav.get() };
		context->CSSetShader(foamCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->Dispatch(groups, groups, 1);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	}

	// The surf simulation: explicit steps within the shallow water's stability limit. Each is two passes: the face
	// velocities under the old depths, then the depths with the new velocities (SwashStepCS). The scheme is stable to a
	// Courant number of about 0.7 in two dimensions: bores and uprushes of up to 9 m/s over a couple of metres of water
	// (waves at 4.5 m/s), across 17 cm cells, stay under 0.35 at 4 ms, and no face moves water more than a quarter of a
	// cell, which keeps every depth positive. The first pass of the frame takes the grid's scroll; a fresh grid starts
	// with the water at rest.
	{
		PBRWaterProfiling::Scope pass("PBRWater::SwashStep");
		constexpr float maxStep = 0.004f;  // s
		// The surf keeps the waves' time down to 10 fps, as the foam does: held to fewer steps a frame it ran slower than
		// the waves feeding it at low frame rates, and each uprush followed its bore late.
		const float stepDt = std::clamp(dt, 0.0f, 0.1f);
		const uint32_t steps = std::max(1u, static_cast<uint32_t>(std::ceil(stepDt / maxStep)));
		// The first step covers the whole grid (it takes the scroll); the rest only the tiles with anything to move.
		bool tiled = false;
		for (uint32_t i = 0; i < steps; ++i) {
			if (i == 1)
				tiled = ListActiveTiles(steps);
			SimCB stepCb = cb;
			stepCb.dt = stepDt / static_cast<float>(steps);
			if (i > 0)
				stepCb.shiftX = stepCb.shiftY = 0;
			else if (fresh)
				stepCb.shiftX = static_cast<int32_t>(FoamGridSize) * 4;
			simCB->Update(stepCb);
			ID3D11ShaderResourceView* tiles = tiled ? tileListSRV.get() : nullptr;
			auto dispatch = [&]() {
				if (tiled)
					context->DispatchIndirect(tileArgs.get(), 0);
				else
					context->Dispatch(groups, groups, 1);
			};
			{
				ID3D11ShaderResourceView* srvs[4] = { surf->srv.get(), prep->srv.get(), stage->srv.get(), tiles };
				ID3D11UnorderedAccessView* uav = surfMoved->uav.get();
				context->CSSetShader(tiled ? momentumTilesCS.get() : momentumCS.get(), nullptr, 0);
				context->CSSetShaderResources(0, 4, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				dispatch();
				context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
			// The second pass reads the first's output where it stands: no scroll.
			if (stepCb.shiftX != 0 || stepCb.shiftY != 0) {
				stepCb.shiftX = stepCb.shiftY = 0;
				simCB->Update(stepCb);
			}
			{
				ID3D11ShaderResourceView* srvs[4] = { surfMoved->srv.get(), prep->srv.get(), stage->srv.get(), tiles };
				ID3D11UnorderedAccessView* uav = surf->uav.get();
				context->CSSetShader(tiled ? continuityTilesCS.get() : continuityCS.get(), nullptr, 0);
				context->CSSetShaderResources(0, 4, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				dispatch();
				context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
		}
		if (tiled) {
			ID3D11ShaderResourceView* nullSrv = nullptr;
			context->CSSetShaderResources(3, 1, &nullSrv);
		}
		lastSwashSteps = steps;
	}

	// Packed for the draws.
	{
		PBRWaterProfiling::Scope pass("PBRWater::SwashOut");
		const uint32_t next = foamCurrent ^ 1;
		ID3D11ShaderResourceView* srvs[3] = { stage->srv.get(), surf->srv.get(), prep->srv.get() };
		ID3D11UnorderedAccessView* uav = foam[next]->uav.get();
		context->CSSetShader(outCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch(groups, groups, 1);
		foamCurrent = next;
	}

	ID3D11Buffer* nullCb = nullptr;
	context->CSSetShaderResources(0, 3, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	foamOriginX = newOriginX;
	foamOriginY = newOriginY;
}
