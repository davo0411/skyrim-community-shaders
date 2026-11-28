// PBR Water: The deferred surface (PBRWater/Deferred.hlsli): the stencil pass's record, its tiles, the baked shore train
// and material, and the shading of the recorded pixels at full and half rate.

#include "Features/PBRWater.h"
#include "Features/PBRWater/EngineWater.h"
#include "Features/PBRWater/PBRWaterInternal.h"
#include "Features/PBRWater/Profiling.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricShadows.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/UI.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace PBRWaterDetail;

bool PBRWater::EnsureDeferredRecords()
{
	auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	if (!depth)
		return false;
	if (!deferredTilesCS) {
		deferredTilesCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredTilesCS.hlsl", {}, "cs_5_0")));
		if (!deferredTilesCS)
			return false;
	}
	D3D11_TEXTURE2D_DESC depthDesc{};
	depth->GetDesc(&depthDesc);
	if (deferredTags && deferredTags->desc.Width == depthDesc.Width && deferredTags->desc.Height == depthDesc.Height)
		return true;

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = depthDesc.Width;
	desc.Height = depthDesc.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	const float zero[4] = {};
	const std::tuple<std::unique_ptr<Texture2D>*, DXGI_FORMAT, const char*> targets[] = {
		{ &deferredTags, DXGI_FORMAT_R32_UINT, "PBRWater::DeferredTags" },
		{ &deferredWaves, DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::DeferredWaves" },
		{ &deferredTexCoords, DXGI_FORMAT_R32G32_FLOAT, "PBRWater::DeferredTexCoords" },
	};
	for (const auto& [target, format, name] : targets) {
		desc.Format = srvDesc.Format = rtvDesc.Format = format;
		*target = std::make_unique<Texture2D>(desc, name);
		(*target)->CreateSRV(srvDesc);
		(*target)->CreateRTV(rtvDesc);
		context->ClearRenderTargetView((*target)->rtv.get(), zero);
	}

	// The shore train per 2x2 pixels (DeferredShoreCS). Its shader failing to compile only costs the pixels the train.
	if (!deferredShoreCS)
		deferredShoreCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredShoreCS.hlsl", {}, "cs_5_0")));
	{
		D3D11_TEXTURE2D_DESC halfDesc = desc;
		halfDesc.Width = (desc.Width + 1) / 2;
		halfDesc.Height = (desc.Height + 1) / 2;
		halfDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
		halfDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SHADER_RESOURCE_VIEW_DESC halfSrv = srvDesc;
		halfSrv.Format = halfDesc.Format;
		D3D11_UNORDERED_ACCESS_VIEW_DESC halfUav{};
		halfUav.Format = halfDesc.Format;
		halfUav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		const char* names[3] = { "PBRWater::DeferredShore0", "PBRWater::DeferredShore1", "PBRWater::DeferredShore2" };
		for (uint32_t i = 0; i < 3; ++i) {
			deferredShore[i] = std::make_unique<Texture2D>(halfDesc, names[i]);
			deferredShore[i]->CreateSRV(halfSrv);
			deferredShore[i]->CreateUAV(halfUav);
		}
		deferredShoreFrame = UINT32_MAX;
	}

	// The surface's material (SurfaceMaterialCS): its shader failing to compile or the format not storing costs the
	// pixel shader the split, not the water.
	surfaceMaterial[0].reset();
	surfaceMaterial[1].reset();
	surfaceMaterialFrame = UINT32_MAX;
	surfaceMaterialFailed = true;
	if (!surfaceMaterialCS)
		surfaceMaterialCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SurfaceMaterialCS.hlsl", {}, "cs_5_0")));
	{
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{ DXGI_FORMAT_R32G32_UINT, 0 };
		const bool typedStore = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))) &&
		                        (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE);
		if (surfaceMaterialCS && typedStore) {
			D3D11_TEXTURE2D_DESC materialDesc = desc;
			materialDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			const std::pair<DXGI_FORMAT, const char*> materials[2] = {
				{ DXGI_FORMAT_R32G32B32A32_UINT, "PBRWater::Material A" },
				{ DXGI_FORMAT_R32G32_UINT, "PBRWater::Material B" },
			};
			const UINT zeros[4] = {};
			for (uint32_t i = 0; i < 2; ++i) {
				materialDesc.Format = materials[i].first;
				D3D11_SHADER_RESOURCE_VIEW_DESC materialSrv = srvDesc;
				materialSrv.Format = materialDesc.Format;
				D3D11_UNORDERED_ACCESS_VIEW_DESC materialUav{};
				materialUav.Format = materialDesc.Format;
				materialUav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				surfaceMaterial[i] = std::make_unique<Texture2D>(materialDesc, materials[i].second);
				surfaceMaterial[i]->CreateSRV(materialSrv);
				surfaceMaterial[i]->CreateUAV(materialUav);
				// Pixels no record covers keep this: the lighting's neighbours in a quad may read them.
				context->ClearUnorderedAccessViewUint(surfaceMaterial[i]->uav.get(), zeros);
			}
			surfaceMaterialFailed = false;
		}
	}

	// The distant tiles shaded at half rate (DeferredFar.hlsl): one pixel of every 2x2, and which one, at half size.
	{
		D3D11_TEXTURE2D_DESC farDesc = desc;
		farDesc.Width = (desc.Width + 1) / 2;
		farDesc.Height = (desc.Height + 1) / 2;
		farDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		D3D11_SHADER_RESOURCE_VIEW_DESC farSrv = srvDesc;
		farSrv.Format = farDesc.Format;
		D3D11_RENDER_TARGET_VIEW_DESC farRtv = rtvDesc;
		farRtv.Format = farDesc.Format;
		deferredFar = std::make_unique<Texture2D>(farDesc, "PBRWater::DeferredFar");
		deferredFar->CreateSRV(farSrv);
		deferredFar->CreateRTV(farRtv);
		deferredFarFrame = UINT32_MAX;
	}

	// The tile lists: DrawInstancedIndirect's arguments per list, and room for every tile of the screen per list.
	deferredMaxTiles = ((desc.Width + DeferredTile - 1) / DeferredTile) * ((desc.Height + DeferredTile - 1) / DeferredTile);
	D3D11_BUFFER_DESC argsDesc{};
	argsDesc.ByteWidth = (DeferredLists + 1) * 4 * sizeof(uint32_t);  // then the water tiles' Dispatch arguments
	argsDesc.Usage = D3D11_USAGE_DEFAULT;
	argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	deferredArgs = nullptr;
	deferredArgsUAV = nullptr;
	DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, nullptr, deferredArgs.put()));
	Util::SetResourceName(deferredArgs.get(), "PBRWater::DeferredTileArgs");
	D3D11_UNORDERED_ACCESS_VIEW_DESC argsUav{};
	argsUav.Format = DXGI_FORMAT_R32_TYPELESS;
	argsUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	argsUav.Buffer.NumElements = (DeferredLists + 1) * 4;
	argsUav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(deferredArgs.get(), &argsUav, deferredArgsUAV.put()));
	Util::SetResourceName(deferredArgsUAV.get(), "PBRWater::DeferredTileArgs UAV");

	D3D11_BUFFER_DESC tilesDesc{};
	tilesDesc.ByteWidth = (DeferredLists + 1) * deferredMaxTiles * sizeof(uint32_t);  // then every tile with water
	tilesDesc.Usage = D3D11_USAGE_DEFAULT;
	tilesDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	tilesDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	tilesDesc.StructureByteStride = sizeof(uint32_t);
	deferredTiles = std::make_unique<Buffer>(tilesDesc, nullptr, "PBRWater::DeferredTiles");
	D3D11_SHADER_RESOURCE_VIEW_DESC tilesSrv{};
	tilesSrv.Format = DXGI_FORMAT_UNKNOWN;
	tilesSrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	tilesSrv.Buffer.NumElements = (DeferredLists + 1) * deferredMaxTiles;
	deferredTiles->CreateSRV(tilesSrv);
	D3D11_UNORDERED_ACCESS_VIEW_DESC tilesUav{};
	tilesUav.Format = DXGI_FORMAT_UNKNOWN;
	tilesUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	tilesUav.Buffer.NumElements = (DeferredLists + 1) * deferredMaxTiles;
	deferredTiles->CreateUAV(tilesUav);

	deferredMeshes.clear();
	deferredMeshesFrame = UINT32_MAX;
	deferredTilesFrame = UINT32_MAX;
	return true;
}

void PBRWater::DrawRecordedStencil()
{
	auto context = globals::d3d::context;
	const uint32_t frame = globals::state->frameCount;
	BoundTargets bound(context);
	// The record's targets stay bound across the stencil batch (EndDeferredBatch puts the engine's back): the engine
	// binds its own again only when it changes targets.
	const bool ours = deferredTags && bound.rtv[2] == deferredTags->rtv.get();
	const char* reason = nullptr;
	if (!bound.dsv)
		reason = "the stencil pass has no depth target";
	else if (!ours && (bound.rtv[2] || bound.rtv[3] || bound.rtv[4]))
		reason = "the stencil pass already uses render targets 2-4";
	else if (!cachedDrawGeometry)
		reason = "no geometry for the draw";
	else if (!EnsureDeferredRecords())
		reason = "the record could not be created (no depth buffer, or DeferredTilesCS did not compile)";
	else if (deferredMeshesFrame == frame && deferredMeshes.size() >= DeferredMeshes)
		reason = "more grids than the record holds (32)";
	if (reason) {
		DeferredFallback(reason);
		DrawGrid(GridMeshStart - cachedDrawStart);
		return;
	}
	if (deferredMeshesFrame != frame) {
		// No mesh anywhere until this frame's stencil pass records one.
		deferredMeshes.clear();
		deferredMeshesFrame = frame;
		const float zero[4] = {};
		context->ClearRenderTargetView(deferredTags->rtv.get(), zero);
	}
	deferredMeshes.push_back(DeferredMesh{ cachedDrawStart });

	auto& batch = deferredBatch;
	if (!batch.recording) {
		batch.recording = true;
		batch.recordTargets = 0;
		batch.recordBlend = nullptr;
	}
	// The grids' texture coordinates are recorded too (target 4).
	const UINT needed = 5;
	if (!ours || batch.recordTargets != needed) {
		ID3D11RenderTargetView* recorded[5] = { bound.rtv[0], bound.rtv[1], deferredTags->rtv.get(), deferredWaves->rtv.get(), deferredTexCoords->rtv.get() };
		context->OMSetRenderTargets(needed, recorded, bound.dsv);
		batch.recordTargets = needed;
	}
	// The engine's blend state need not write targets 2-4 (a zero write mask drops the record without a word): its own for
	// targets 0 and 1, the record's written whole. Set again only when the engine has set its own since.
	{
		winrt::com_ptr<ID3D11BlendState> blend;
		float factor[4] = {};
		UINT mask = 0xFFFFFFFF;
		context->OMGetBlendState(blend.put(), factor, &mask);
		if (blend.get() != batch.recordBlend || !batch.recordBlend) {
			batch.engineBlend = blend;
			std::copy_n(factor, 4, batch.blendFactor);
			batch.sampleMask = mask;
			batch.recordBlend = RecordBlendState(blend.get());
			context->OMSetBlendState(batch.recordBlend, factor, mask);
		}
	}

	DeferredDrawCB data;
	data.tag[0] = static_cast<uint32_t>(deferredMeshes.size());
	deferredCB->Update(data);
	ID3D11Buffer* drawCb = deferredCB->CB();
	context->PSSetConstantBuffers(13, 1, &drawCb);
	DrawGrid(GridMeshStart - cachedDrawStart);
	// The engine's stencil draws in between write the record too, with no tag: where their water stands nearer, none of
	// the grids owns the pixel.
	ID3D11Buffer* zeroCb = deferredZeroCB->CB();
	context->PSSetConstantBuffers(13, 1, &zeroCb);
	++drawStats.deferredRecorded;
}

void PBRWater::EndDeferredBatch()
{
	auto& batch = deferredBatch;
	if (!batch.recording && !batch.shading)
		return;
	auto context = globals::d3d::context;
	if (batch.recording) {
		BoundTargets bound(context);
		if (deferredTags && bound.rtv[2] == deferredTags->rtv.get())
			context->OMSetRenderTargets(2, bound.rtv, bound.dsv);
		winrt::com_ptr<ID3D11BlendState> blend;
		float factor[4] = {};
		UINT mask = 0xFFFFFFFF;
		context->OMGetBlendState(blend.put(), factor, &mask);
		if (blend.get() == batch.recordBlend)
			context->OMSetBlendState(batch.engineBlend.get(), batch.blendFactor, batch.sampleMask);
		ID3D11Buffer* nullCb = nullptr;
		context->PSSetConstantBuffers(13, 1, &nullCb);
		// The tiles are listed by the first surface draw (DrawDeferredSurface): sorting them by distance reads the depth,
		// which the stencil pass still has bound for writing here.
		batch.recording = false;
		batch.recordBlend = nullptr;
		batch.engineBlend = nullptr;
	}
	if (batch.shading) {
		ID3D11ShaderResourceView* nullSrvs[6] = {};
		context->PSSetShaderResources(38, 5, nullSrvs);
		context->PSSetShaderResources(46, 3, nullSrvs);
		context->VSSetShaderResources(101, 1, nullSrvs);
		ID3D11Buffer* nullCbs[3] = {};
		context->PSSetConstantBuffers(8, 3, nullCbs);
		context->PSSetConstantBuffers(13, 1, nullCbs);
		context->VSSetConstantBuffers(13, 1, nullCbs);
		ID3D11Buffer* cb = flatDrawBuffer->CB();
		context->VSSetConstantBuffers(7, 1, &cb);
		context->PSSetConstantBuffers(7, 1, &cb);
		BoundTargets bound(context);
		if (bound.dsv && bound.dsv == batch.readOnly && batch.engineDSV)
			context->OMSetRenderTargets(bound.count, bound.rtv, batch.engineDSV.get());
		batch.shading = false;
		batch.readOnly = nullptr;
		batch.engineDSV = nullptr;
	}
}

ID3D11BlendState* PBRWater::RecordBlendState(ID3D11BlendState* engineState)
{
	auto& cached = recordBlendStates[engineState];
	if (!cached) {
		D3D11_BLEND_DESC desc{};
		if (engineState)
			engineState->GetDesc(&desc);
		else {
			desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		if (!desc.IndependentBlendEnable) {
			for (uint32_t i = 1; i < 8; ++i)
				desc.RenderTarget[i] = desc.RenderTarget[0];
			desc.IndependentBlendEnable = TRUE;
		}
		for (uint32_t i = 2; i < 5; ++i) {
			auto& target = desc.RenderTarget[i];
			target.BlendEnable = FALSE;
			target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
			target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		if (FAILED(globals::d3d::device->CreateBlendState(&desc, cached.put())))
			return engineState;
		Util::SetResourceName(cached.get(), "PBRWater::DeferredRecordBlend");
	}
	return cached.get();
}

void PBRWater::DeferredFallback(const char* reason)
{
	++drawStats.deferredForward;
	drawStats.deferredReason = reason;
}

void PBRWater::ClassifyDeferredTiles()
{
	auto context = globals::d3d::context;
	deferredTilesFrame = globals::state->frameCount;
	// No tiles for any mesh yet; DeferredTilesCS fills in the vertex counts as it lists them. Cleared on the GPU: a
	// CPU upload into a buffer the last frame's draws read can make the driver wait for them.
	const UINT zero[4] = {};
	context->ClearUnorderedAccessViewUint(deferredArgsUAV.get(), zero);

	// Distant tiles only where the surface pass can shade them at half rate.
	const bool distant = settings.DistantHalfRate && !deferredFarFailed && deferredFar;
	struct alignas(16) TilesCB
	{
		uint32_t width, height, maxTiles;
		float farDistance;
	} tilesData{ deferredTags->desc.Width, deferredTags->desc.Height, deferredMaxTiles,
		distant ? settings.DistantHalfRateDistance * static_cast<float>(PBRWaterModel::UnitsPerMetre) : 0.0f };
	if (!deferredTilesCB)
		deferredTilesCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TilesCB>(), "PBRWater::DeferredTilesCB");
	deferredTilesCB->Update(tilesData);

	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	ID3D11Buffer* savedCb = nullptr;
	context->CSGetConstantBuffers(0, 1, &savedCb);
	ID3D11Buffer* savedFrameCb = nullptr;
	context->CSGetConstantBuffers(12, 1, &savedFrameCb);
	ID3D11ShaderResourceView* savedSrvs[2] = {};
	context->CSGetShaderResources(0, 2, savedSrvs);
	ID3D11UnorderedAccessView* savedUavs[2] = {};
	context->CSGetUnorderedAccessViews(0, 2, savedUavs);

	ID3D11Buffer* cb = deferredTilesCB->CB();
	ID3D11Buffer* frameCb = *globals::game::perFrame.get();
	ID3D11ShaderResourceView* depthSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	ID3D11ShaderResourceView* srvs[2] = { deferredTags->srv.get(), depthSRV };
	ID3D11UnorderedAccessView* uavs[2] = { deferredArgsUAV.get(), deferredTiles->uav.get() };
	context->CSSetShader(deferredTilesCS.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetConstantBuffers(12, 1, &frameCb);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->Dispatch((tilesData.width + DeferredTile - 1) / DeferredTile, (tilesData.height + DeferredTile - 1) / DeferredTile, 1);

	context->CSSetShader(savedShader.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &savedCb);
	context->CSSetConstantBuffers(12, 1, &savedFrameCb);
	context->CSSetShaderResources(0, 2, savedSrvs);
	context->CSSetUnorderedAccessViews(0, 2, savedUavs, nullptr);
	if (savedCb)
		savedCb->Release();
	if (savedFrameCb)
		savedFrameCb->Release();
	for (auto* view : savedSrvs) {
		if (view)
			view->Release();
	}
	for (auto* uav : savedUavs) {
		if (uav)
			uav->Release();
	}
}

void PBRWater::BakeDeferredShore(ID3D11ShaderResourceView* depthSRV)
{
	PBRWaterProfiling::Scope pass("PBRWater::DeferredShore");
	auto context = globals::d3d::context;
	deferredShoreFrame = globals::state->frameCount;
	struct alignas(16) ShoreCB
	{
		uint32_t width, height, halfWidth, halfHeight;
	} shoreData{ deferredTags->desc.Width, deferredTags->desc.Height, deferredShore[0]->desc.Width, deferredShore[0]->desc.Height };
	if (!deferredShoreCB)
		deferredShoreCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShoreCB>(), "PBRWater::DeferredShoreCB");
	deferredShoreCB->Update(shoreData);

	// The deferred draws' constants and views (the curl's spacing follows the grid: Tess0), this pass's own.
	ID3D11Buffer* cbs[1] = { deferredShoreCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	BindWaterCompute(deferredDrawBuffer->CB(), drawSRVs);
	ID3D11ShaderResourceView* srvs[3] = { deferredTags->srv.get(), deferredWaves->srv.get(), depthSRV };
	context->CSSetShaderResources(0, 3, srvs);
	ID3D11UnorderedAccessView* uavs[3] = { deferredShore[0]->uav.get(), deferredShore[1]->uav.get(), deferredShore[2]->uav.get() };
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	context->CSSetShader(deferredShoreCS.get(), nullptr, 0);
	context->Dispatch((shoreData.halfWidth + 7) / 8, (shoreData.halfHeight + 7) / 8, 1);

	UnbindWaterCompute();
	ID3D11ShaderResourceView* nullSrvs[3] = {};
	context->CSSetShaderResources(0, 3, nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[3] = {};
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
}

bool PBRWater::BakeSurfaceMaterial(ID3D11ShaderResourceView* depthSRV)
{
	if (surfaceMaterialFailed || !surfaceMaterialCS || !surfaceMaterial[0] || !surfaceMaterial[1])
		return false;
	PBRWaterProfiling::Scope pass("PBRWater::SurfaceMaterial");
	auto context = globals::d3d::context;
	surfaceMaterialFrame = globals::state->frameCount;
	// The tiles the surface pass shades at half rate (ClassifyDeferredTiles' test): their material is filtered over two pixels.
	const bool distant = settings.DistantHalfRate && !deferredFarFailed && deferredFar;
	struct alignas(16) MaterialCB
	{
		uint32_t width, height, shoreBaked;
		float farDistance;
		uint32_t tileList[4];
	} materialData{ deferredTags->desc.Width, deferredTags->desc.Height, deferredShoreFrame == surfaceMaterialFrame ? 1u : 0u,
		distant ? settings.DistantHalfRateDistance * static_cast<float>(PBRWaterModel::UnitsPerMetre) : 0.0f, { DeferredLists * deferredMaxTiles, 0, 0, 0 } };
	if (!surfaceMaterialCB)
		surfaceMaterialCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<MaterialCB>(), "PBRWater::SurfaceMaterialCB");
	surfaceMaterialCB->Update(materialData);

	// As BakeDeferredShore: the deferred draws' constants and views, this pass's own, and the shore train it baked.
	ID3D11Buffer* cbs[1] = { surfaceMaterialCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	BindWaterCompute(deferredDrawBuffer->CB(), drawSRVs);
	ID3D11ShaderResourceView* srvs[4] = { deferredTags->srv.get(), deferredWaves->srv.get(), depthSRV, deferredTiles->srv.get() };
	context->CSSetShaderResources(0, 4, srvs);
	ID3D11ShaderResourceView* shoreSrvs[3] = {};
	if (materialData.shoreBaked) {
		for (uint32_t i = 0; i < 3; ++i)
			shoreSrvs[i] = deferredShore[i]->srv.get();
	}
	context->CSSetShaderResources(46, 3, shoreSrvs);
	ID3D11UnorderedAccessView* uavs[2] = { surfaceMaterial[0]->uav.get(), surfaceMaterial[1]->uav.get() };
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(surfaceMaterialCS.get(), nullptr, 0);
	// Only the tiles with water (DeferredTilesCS), one group each.
	context->DispatchIndirect(deferredArgs.get(), DeferredLists * 4 * sizeof(uint32_t));

	UnbindWaterCompute();
	ID3D11ShaderResourceView* nullSrvs[4] = {};
	context->CSSetShaderResources(0, 4, nullSrvs);
	context->CSSetShaderResources(46, 3, nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[2] = {};
	context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	return true;
}

ID3D11PixelShader* PBRWater::GetMaterialPixelShader()
{
	if (surfaceMaterialFailed || !cachedDrawShader || debugCompiled)
		return nullptr;
	const auto descriptor = cachedDrawPixelDescriptor | static_cast<uint32_t>(SC::WaterShaderFlags::DeferredMaterial);
	// null while it compiles, or when it failed: this draw's pixel shader evaluates the waves itself
	const auto* shader = globals::shaderCache->GetPixelShader(*cachedDrawShader, descriptor);
	return shader ? reinterpret_cast<ID3D11PixelShader*>(shader->shader) : nullptr;
}

bool PBRWater::DrawDeferredSurface()
{
	const uint32_t frame = globals::state->frameCount;
	if (deferredMeshesFrame != frame || !deferredTags) {
		DeferredFallback("the stencil pass recorded no mesh this frame");
		return false;
	}
	const auto mesh = std::find_if(deferredMeshes.begin(), deferredMeshes.end(), [&](const DeferredMesh& m) { return m.startIndex == cachedDrawStart; });
	if (mesh == deferredMeshes.end()) {
		DeferredFallback("the stencil pass did not record this mesh");
		return false;
	}
	const uint32_t slot = static_cast<uint32_t>(mesh - deferredMeshes.begin());

	auto context = globals::d3d::context;
	auto& batch = deferredBatch;
	BoundTargets bound(context);
	// The read-only depth and the record stay bound across the batch (EndDeferredBatch puts the engine's back); bound
	// again only when the engine has bound its own depth target since, which also drops the depth's view from the
	// pixel stage.
	if (!batch.shading || bound.dsv != batch.readOnly) {
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ID3D11DepthStencilView* readOnly = nullptr;
		ID3D11DepthStencilView* engineDSV = nullptr;
		for (uint32_t i = 0; i < std::size(depth.views); ++i) {
			if (bound.dsv && depth.views[i] == bound.dsv) {
				readOnly = depth.readOnlyViews[i];
				engineDSV = bound.dsv;
			}
			if (bound.dsv && depth.readOnlyViews[i] == bound.dsv)
				readOnly = bound.dsv;  // the engine bound it already
		}
		if (!readOnly || !depth.depthSRV) {
			DeferredFallback(!bound.dsv ? "the surface pass has no depth target" : "the surface pass's depth target is not the main depth buffer");
			return false;
		}
		if (readOnly != bound.dsv) {
			context->OMSetRenderTargets(bound.count, bound.rtv, readOnly);
			batch.engineDSV.copy_from(engineDSV);
		}
		// Listed once a frame, with the depth bound read-only (DeferredTilesCS reads it to find the distant tiles).
		if (deferredTilesFrame != frame)
			ClassifyDeferredTiles();
		if (deferredShoreFrame != frame && deferredShoreCS && frameData.Shore0.x > 0.5f)
			BakeDeferredShore(depth.depthSRV);
		ID3D11ShaderResourceView* srvs[5] = { deferredTags->srv.get(), deferredWaves->srv.get(), nullptr, deferredTexCoords->srv.get(), depth.depthSRV };
		context->PSSetShaderResources(38, 5, srvs);
		ID3D11ShaderResourceView* shoreSrvs[3] = {};
		if (deferredShoreFrame == frame) {
			for (uint32_t i = 0; i < 3; ++i)
				shoreSrvs[i] = deferredShore[i]->srv.get();
		}
		context->PSSetShaderResources(46, 3, shoreSrvs);
		ID3D11ShaderResourceView* tilesSrv = deferredTiles->srv.get();
		context->VSSetShaderResources(101, 1, &tilesSrv);
		batch.shading = true;
		batch.readOnly = readOnly;
	}

	// The pixel shader that lights from the surface's material, once it is compiled and the material is evaluated (the
	// first draw of the frame evaluates it, after the shore train and the stencil pass's last record): otherwise the draw's
	// own pixel shader evaluates the waves for itself.
	ID3D11PixelShader* materialPS = GetMaterialPixelShader();
	if (materialPS && surfaceMaterialFrame != frame) {
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		BakeSurfaceMaterial(depth.depthSRV);
	}
	if (surfaceMaterialFrame != frame)
		materialPS = nullptr;
	winrt::com_ptr<ID3D11PixelShader> enginePS;
	if (materialPS) {
		context->PSGetShader(enginePS.put(), nullptr, nullptr);
		context->PSSetShader(materialPS, nullptr, 0);
		ID3D11ShaderResourceView* materialSrvA = surfaceMaterial[0]->srv.get();
		ID3D11ShaderResourceView* materialSrvB = surfaceMaterial[1]->srv.get();
		context->PSSetShaderResources(SurfaceMaterialSlotA, 1, &materialSrvA);
		context->PSSetShaderResources(SurfaceMaterialSlotB, 1, &materialSrvB);
	}

	// The vertex program runs per pixel: the draw's vertex constants go to the pixel stage too (b8-b10).
	ID3D11Buffer* vertexCbs[3] = {};
	context->VSGetConstantBuffers(0, 3, vertexCbs);
	context->PSSetConstantBuffers(8, 3, vertexCbs);
	for (auto* buffer : vertexCbs) {
		if (buffer)
			buffer->Release();
	}

	D3D11_VIEWPORT viewport{};
	UINT viewports = 1;
	context->RSGetViewports(&viewports, &viewport);
	DeferredDrawCB data;
	data.tag[0] = slot + 1;
	data.tag[1] = deferredMaxTiles;
	data.tag[3] = deferredShoreFrame == frame ? 1u : 0u;
	data.viewport = { std::max(viewport.Width, 1.0f), std::max(viewport.Height, 1.0f), 0.0f, 0.0f };
	deferredCB->Update(data);
	ID3D11Buffer* drawCb = deferredCB->CB();
	context->VSSetConstantBuffers(13, 1, &drawCb);
	context->PSSetConstantBuffers(13, 1, &drawCb);
	ID3D11Buffer* tileCb = deferredDrawBuffer->CB();
	context->VSSetConstantBuffers(7, 1, &tileCb);
	context->PSSetConstantBuffers(7, 1, &tileCb);
	// The near tiles, then the distant ones: at half rate in the surface pass, at full rate in the point light passes
	// (and wherever the half-rate pass cannot run).
	data.mode[0] = slot;
	data.mode[1] = DeferredFull;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), slot * 4 * sizeof(uint32_t));
	if (!(cachedDrawTechnique == 0 && DrawDistantHalfRate(slot, data))) {
		data.mode[0] = DeferredMeshes + slot;
		deferredCB->Update(data);
		context->DrawInstancedIndirect(deferredArgs.get(), (DeferredMeshes + slot) * 4 * sizeof(uint32_t));
	}
	if (materialPS) {
		// The engine's pixel shader back for its next draw, and the material off the pixel stage before the next frame writes it.
		context->PSSetShader(enginePS.get(), nullptr, 0);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		context->PSSetShaderResources(SurfaceMaterialSlotA, 1, &nullSrv);
		context->PSSetShaderResources(SurfaceMaterialSlotB, 1, &nullSrv);
	}
	++drawStats.deferredShaded;
	return true;
}

bool PBRWater::DrawDistantHalfRate(uint32_t slot, DeferredDrawCB data)
{
	if (!settings.DistantHalfRate || deferredFarFailed || !deferredFar)
		return false;
	auto context = globals::d3d::context;
	BoundTargets bound(context);
	// Only the main view's surface: its targets are the record's size.
	if (bound.count == 0 || !bound.rtv[0])
		return false;
	{
		winrt::com_ptr<ID3D11Resource> resource;
		bound.rtv[0]->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (desc.Width != deferredTags->desc.Width || desc.Height != deferredTags->desc.Height)
			return false;
	}
	if (!deferredFarVS || !deferredFarPS) {
		deferredFarVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredFar.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
		deferredFarPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredFar.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!deferredFarVS || !deferredFarPS) {
			deferredFarFailed = true;  // from the next frame on the distant tiles are not listed apart
			return false;
		}
	}
	const uint32_t frame = globals::state->frameCount;
	if (deferredFarFrame != frame) {
		// Nothing shaded yet this frame: the upsampling reads a key of 0 where no mesh shaded.
		deferredFarFrame = frame;
		const float zero[4] = {};
		context->ClearRenderTargetView(deferredFar->rtv.get(), zero);
	}
	const uint32_t list = DeferredMeshes + slot;
	const UINT argsOffset = list * 4 * sizeof(uint32_t);

	// One pixel of every 2x2 into the half-size target, with the surface's own shaders: no depth (the record decides which
	// pixels are this mesh's water) and no blending (the key rides in alpha).
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&viewportCount, viewports);
	winrt::com_ptr<ID3D11BlendState> blend;
	float blendFactor[4] = {};
	UINT sampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(blend.put(), blendFactor, &sampleMask);

	ID3D11RenderTargetView* farRtv = deferredFar->rtv.get();
	context->OMSetRenderTargets(1, &farRtv, nullptr);
	const D3D11_VIEWPORT half{ 0.0f, 0.0f, static_cast<float>(deferredFar->desc.Width), static_cast<float>(deferredFar->desc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &half);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	data.viewport = { half.Width, half.Height, 0.0f, 0.0f };
	data.mode[0] = list;
	data.mode[1] = DeferredHalf;
	// A fixed sample: rotating it each frame moved every pixel's value (a 1-in-4 point sample of glints and foam, reweighted
	// by DeferredFar.hlsl) on a 4-frame cycle, which the temporal filter cannot hide.
	data.mode[2] = 0;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), argsOffset);

	context->OMSetRenderTargets(bound.count, bound.rtv, bound.dsv);
	context->RSSetViewports(viewportCount, viewports);
	context->OMSetBlendState(blend.get(), blendFactor, sampleMask);

	// The distant tiles at full size, with the surface draw's blend, depth and stencil state: each pixel from the samples
	// around it.
	winrt::com_ptr<ID3D11VertexShader> engineVS;
	winrt::com_ptr<ID3D11PixelShader> enginePS;
	context->VSGetShader(engineVS.put(), nullptr, nullptr);
	context->PSGetShader(enginePS.put(), nullptr, nullptr);
	context->VSSetShader(deferredFarVS.get(), nullptr, 0);
	context->PSSetShader(deferredFarPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* farSrv = deferredFar->srv.get();
	context->PSSetShaderResources(DeferredFarSlot, 1, &farSrv);
	data.viewport = { std::max(viewports[0].Width, 1.0f), std::max(viewports[0].Height, 1.0f), 0.0f, 0.0f };
	data.mode[1] = DeferredFull;
	data.mode[2] = 0;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), argsOffset);

	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->PSSetShaderResources(DeferredFarSlot, 1, &nullSrv);
	context->VSSetShader(engineVS.get(), nullptr, 0);
	context->PSSetShader(enginePS.get(), nullptr, 0);
	return true;
}
