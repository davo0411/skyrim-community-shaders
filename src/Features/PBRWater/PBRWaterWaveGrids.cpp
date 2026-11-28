// PBR Water: The displaced wave grids drawn outside the main view: the Skylighting occluder and the wave heightfield the
// sun shadows march through.

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

void PBRWater::CompileOccluder()
{
	occluderVS = nullptr;
	if (auto* vs = reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SkylightingOccluderVS.hlsl", {}, "vs_5_0")))
		occluderVS.attach(vs);
	else
		logger::warn("[PBR Water] Skylighting occluder failed to compile; the waves do not occlude the sky");
}

void PBRWater::DrawSkylightingOccluder(Texture2D& occlusion, const REX::W32::XMFLOAT4X4& occlusionViewProj)
{
	PBRWaterProfiling::SkipUnlessRun row("PBRWater::SkylightingOccluder");
	ID3D11DepthStencilView* dsv = occlusion.dsv.get();
	if (!loaded || !settings.EnableSkyOcclusion || !dsv || !occluderVS || !occluderIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer || !globals::shaderCache->IsEnabled())
		return;
	// Calm water is the flat plane the probes are lifted above anyway: only waves need drawing.
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	row.ran = true;
	PBRWaterProfiling::Scope pass("PBRWater::SkylightingOccluder");

	OccluderCB cb{};
	cb.occlusionViewProj = occlusionViewProj;
	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	cb.eye = { eye.x, eye.y, eye.z, OccluderExtent };
	cb.grid = { static_cast<float>(OccluderQuads), OccluderExtent / OccluderQuads, eyeCellX, eyeCellY };
	if (!FillOccluderLevels(cb, eyeCellX, eyeCellY))
		return;
	DrawWaveGrid(dsv, occlusion.desc.Width, occlusion.desc.Height, cb, occluderIndices.get(), DXGI_FORMAT_R16_UINT, OccluderQuads * OccluderQuads * 6);
	occluderFrame = globals::state->frameCount;
}

bool PBRWater::FillOccluderLevels(OccluderCB& cb, float eyeCellX, float eyeCellY) const
{
	// Called by up to three passes a frame around the same cell: the 25 lookups are done once.
	const uint32_t frame = globals::state->frameCount;
	if (levelCache.frame == frame && levelCache.cellX == eyeCellX && levelCache.cellY == eyeCellY) {
		std::copy(std::begin(levelCache.levels), std::end(levelCache.levels), std::begin(cb.cellLevels));
		return levelCache.anyWater;
	}
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return false;
	// The still water of the cells around the eye, as SharedData::GetWaterData sees them.
	const auto eye = Util::GetEyePosition();
	bool anyWater = false;
	for (int32_t j = -2; j <= 2; ++j) {
		for (int32_t i = -2; i <= 2; ++i) {
			float level = -1.0e9f;
			const RE::NiPoint3 centre{ (eyeCellX + static_cast<float>(i) + 0.5f) * CellSize, (eyeCellY + static_cast<float>(j) + 0.5f) * CellSize, eye.z };
			if (auto* cell = tes->GetCell(centre)) {
				if (const float height = cell->GetExteriorWaterHeight(); height > -1.0e6f) {
					level = height;
					anyWater = true;
				}
			}
			cb.cellLevels[(i + 2) + 5 * (j + 2)] = { level, 0.0f, 0.0f, 0.0f };
		}
	}
	std::copy(std::begin(cb.cellLevels), std::end(cb.cellLevels), std::begin(levelCache.levels));
	levelCache.frame = frame;
	levelCache.cellX = eyeCellX;
	levelCache.cellY = eyeCellY;
	levelCache.anyWater = anyWater;
	return anyWater;
}

void PBRWater::DrawWaveGrid(ID3D11DepthStencilView* dsv, UINT width, UINT height, const OccluderCB& cb, ID3D11Buffer* indices, DXGI_FORMAT indexFormat, UINT indexCount)
{
	occluderCB->Update(cb);
	const GpuData& d = frameData;
	UploadWaterConstants(d);

	auto context = globals::d3d::context;

	// Save what this draw changes.
	winrt::com_ptr<ID3D11InputLayout> savedLayout;
	D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
	winrt::com_ptr<ID3D11Buffer> savedIndices;
	DXGI_FORMAT savedIndexFormat{};
	UINT savedIndexOffset = 0;
	context->IAGetInputLayout(savedLayout.put());
	context->IAGetPrimitiveTopology(&savedTopology);
	context->IAGetIndexBuffer(savedIndices.put(), &savedIndexFormat, &savedIndexOffset);
	winrt::com_ptr<ID3D11VertexShader> savedVS;
	winrt::com_ptr<ID3D11HullShader> savedHS;
	winrt::com_ptr<ID3D11DomainShader> savedDS;
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	winrt::com_ptr<ID3D11PixelShader> savedPS;
	context->VSGetShader(savedVS.put(), nullptr, nullptr);
	context->HSGetShader(savedHS.put(), nullptr, nullptr);
	context->DSGetShader(savedDS.put(), nullptr, nullptr);
	context->GSGetShader(savedGS.put(), nullptr, nullptr);
	context->PSGetShader(savedPS.put(), nullptr, nullptr);
	ID3D11Buffer* savedCB0 = nullptr;
	ID3D11Buffer* savedCB7 = nullptr;
	ID3D11Buffer* savedCB12 = nullptr;
	context->VSGetConstantBuffers(0, 1, &savedCB0);
	context->VSGetConstantBuffers(7, 1, &savedCB7);
	context->VSGetConstantBuffers(12, 1, &savedCB12);
	ID3D11ShaderResourceView* savedSRVs[WaterSRVCount] = {};
	context->VSGetShaderResources(WaterSRVBase, WaterSRVCount, savedSRVs);
	ID3D11SamplerState* savedSamplers[2] = {};
	context->VSGetSamplers(WaterSamplerBase, 2, savedSamplers);
	winrt::com_ptr<ID3D11RasterizerState> savedRasterizer;
	context->RSGetState(savedRasterizer.put());
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	context->RSGetViewports(&viewportCount, savedViewports);
	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepthState;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepthState.put(), &savedStencilRef);

	// Depth only, with the caller's transform.
	context->OMSetRenderTargets(0, nullptr, dsv);
	context->OMSetDepthStencilState(occluderDepthState.get(), 0);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(occluderRasterizer.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetIndexBuffer(indices, indexFormat, 0);
	context->VSSetShader(occluderVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* cb0 = occluderCB->CB();
	ID3D11Buffer* cb7 = gpuBuffer->CB();
	ID3D11Buffer* cb12 = *globals::game::perFrame.get();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->VSSetConstantBuffers(7, 1, &cb7);
	context->VSSetConstantBuffers(12, 1, &cb12);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, srvs);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->VSSetSamplers(WaterSamplerBase, 2, samplers);

	context->DrawIndexed(indexCount, 0, 0);
	drawResourcesFrame = UINT32_MAX;

	// Restore.
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	context->OMSetDepthStencilState(savedDepthState.get(), savedStencilRef);
	context->RSSetViewports(viewportCount, savedViewports);
	context->RSSetState(savedRasterizer.get());
	context->IASetInputLayout(savedLayout.get());
	context->IASetPrimitiveTopology(savedTopology);
	context->IASetIndexBuffer(savedIndices.get(), savedIndexFormat, savedIndexOffset);
	context->VSSetShader(savedVS.get(), nullptr, 0);
	context->HSSetShader(savedHS.get(), nullptr, 0);
	context->DSSetShader(savedDS.get(), nullptr, 0);
	context->GSSetShader(savedGS.get(), nullptr, 0);
	context->PSSetShader(savedPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &savedCB0);
	context->VSSetConstantBuffers(7, 1, &savedCB7);
	context->VSSetConstantBuffers(12, 1, &savedCB12);
	context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, savedSRVs);
	context->VSSetSamplers(WaterSamplerBase, 2, savedSamplers);
	auto release = [](auto*& p) {
		if (p)
			p->Release();
		p = nullptr;
	};
	release(savedCB0);
	release(savedCB7);
	release(savedCB12);
	for (auto*& srv : savedSRVs)
		release(srv);
	for (auto*& sampler : savedSamplers)
		release(sampler);
	for (auto*& rtv : savedRTVs)
		release(rtv);
	release(savedDSV);
}

void PBRWater::UpdateSunMap(bool waves)
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == sunMapFrame)
		return;
	sunMapFrame = frame;
	frameData.SunMap0.w = 0.0f;
	PBRWaterProfiling::SkipUnlessRun row("PBRWater::WaveHeightfield");
	// Calm water casts no shadow on itself: the heightfield is only drawn where waves stand.
	ID3D11DepthStencilView* dsv = sunMapTexture ? sunMapTexture->dsv.get() : nullptr;
	if (!waves || !dsv || !occluderVS || !sunMapIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer)
		return;
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	row.ran = true;
	PBRWaterProfiling::Scope pass("PBRWater::WaveHeightfield");

	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	constexpr float spacing = SunMapExtent / SunMapQuads;
	OccluderCB cb{};
	cb.eye = { eye.x, eye.y, eye.z, SunMapExtent };
	cb.grid = { static_cast<float>(SunMapQuads), spacing, eyeCellX, eyeCellY };
	if (!FillOccluderLevels(cb, eyeCellX, eyeCellY))
		return;
	// Seen from straight above. The grid is centred on the eye snapped to its spacing (SkylightingOccluderVS); heights are
	// stored as depth against the still water of the eye's cell, the highest surface nearest, so LESS_EQUAL keeps the
	// top of a curling lip.
	const float centreX = std::floor(eye.x / spacing) * spacing;
	const float centreY = std::floor(eye.y / spacing) * spacing;
	const float eyeLevel = cb.cellLevels[12].x;
	const float reference = eyeLevel > -1.0e8f ? eyeLevel : eye.z;
	const float scale = 2.0f / SunMapExtent;
	REX::W32::XMFLOAT4X4 m{};
	m.m[0][0] = scale;
	m.m[0][3] = -scale * (centreX - eye.x);
	m.m[1][1] = scale;
	m.m[1][3] = -scale * (centreY - eye.y);
	m.m[2][2] = -0.5f / SunMapRange;
	m.m[2][3] = 0.5f + (reference - eye.z) * 0.5f / SunMapRange;
	m.m[3][3] = 1.0f;
	cb.occlusionViewProj = m;

	auto context = globals::d3d::context;
	// Out of the pixel stage before it is drawn into.
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->PSSetShaderResources(WaveHeightfieldSlot, 1, &nullSrv);
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	DrawWaveGrid(dsv, SunMapSize, SunMapSize, cb, sunMapIndices.get(), DXGI_FORMAT_R32_UINT, SunMapQuads * SunMapQuads * 6);

	frameData.SunMap0 = { centreX, centreY, 1.0f / SunMapExtent, 1.0f };
	frameData.SunMap1 = { reference, SunMapRange, 0.0f, 0.0f };
}
