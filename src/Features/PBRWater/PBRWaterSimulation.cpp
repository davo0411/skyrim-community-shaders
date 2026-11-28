// PBR Water: The per-frame compute passes that evaluate the waves outside the draws: the projected grids, the shore's
// simulations, the spray, the surface noise, and their shared constants and bindings.

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

void PBRWater::UpdateTessellation()
{
	if (!gpuBuffer || !settings.EnableTessellation || !globals::shaderCache->IsEnabled()) {
		PBRWaterProfiling::Skipped("PBRWater::ProjectedGrid");
		return;
	}
	// The grids' cells follow the worldspace (Unified Water's coverage), their vertices this frame's view.
	auto cells = environment.GetCellLevels();
	if (cells != gridCells) {
		gridCells = cells;
		projectedGrid.SetCells(cells);
	}
	if (!projectedGrid.HasCells()) {
		PBRWaterProfiling::Skipped("PBRWater::ProjectedGrid");
		return;
	}
	PBRWaterProfiling::Scope pass("PBRWater::ProjectedGrid");

	// The vertices are evaluated as the grids' draws evaluate them: this frame's constants (a grid's vertex mode, so the
	// breakers curl), textures, and the surf's state. A grid has no border with the far tiles, so no fade at the loaded
	// cells' edge, and none with the distance short of its end: its own spacing filters the far waves (ProjectedGridCS).
	GpuData d = frameData;
	d.Tess0.x = 6.0f;
	d.Draw0.x = 0.0f;
	d.Grid0 = {};
	d.Params1.z = 0.5f * GridMaxDistance;
	d.Params1.w = GridMaxDistance;
	UploadWaterConstants(d);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	BindWaterCompute(gpuBuffer->CB(), srvs);

	ProjectedGrid::FrameParams gp;
	const auto& unjittered = globals::game::frameBufferCached.GetCameraViewProjUnjittered();
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			gp.viewProj[r][c] = unjittered.m[r][c];
	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	gp.cameraX = camera.x;
	gp.cameraY = camera.y;
	gp.cameraZ = camera.z;
	float scale[2] = { 1.0f, 1.0f };
	if (globals::features::upscaling.loaded) {
		scale[0] = globals::features::upscaling.resolutionScale.x;
		scale[1] = globals::features::upscaling.resolutionScale.y;
	}
	gp.viewportWidth = static_cast<float>(globals::game::graphicsState->screenWidth) * scale[0];
	gp.viewportHeight = static_cast<float>(globals::game::graphicsState->screenHeight) * scale[1];
	gp.cellPixels = settings.GridCellSize;
	gp.maxDistance = GridMaxDistance;
	projectedGrid.Build(gp, globals::state->frameCount);
	UnbindWaterCompute();
}

void PBRWater::UpdateShoreTextures()
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	const auto current = snap ? snap->shoreField : nullptr;
	const auto previous = snap ? snap->shorePrevious : nullptr;
	if (current != shoreTextures.field) {
		if (previous && previous == shoreTextures.field)
			previousShoreTextures = std::move(shoreTextures);
		shoreTextures = UploadShoreField(current);
	}
	if (previous != previousShoreTextures.field)
		previousShoreTextures = UploadShoreField(previous);
}

PBRWater::ShoreTextures PBRWater::UploadShoreField(std::shared_ptr<const PBRWaterModel::ShoreField> field)
{
	ShoreTextures result;
	if (!field || !field->Valid())
		return result;

	const size_t count = static_cast<size_t>(field->width) * field->height;
	// Full precision for the travel times and distances: they become the waves' phase (omega tau, tens of radians) and the
	// nearest point of the coast, where half floats would step them visibly. The slownesses (0.05-1 s/m) and the rest
	// are fine as halves. Packed by the worker that built the field (ShoreField::PackForGpu): a field from elsewhere is
	// packed here.
	std::shared_ptr<PBRWaterModel::ShoreField> packed;
	const PBRWaterModel::ShoreField* source = field.get();
	if (field->gpuTimes.size() != count * 4 || field->gpuRays.size() != count * 4 || field->gpuRoom.size() != count * 2) {
		packed = std::make_shared<PBRWaterModel::ShoreField>(*field);
		packed->PackForGpu();
		source = packed.get();
	}
	const float* times = source->gpuTimes.data();
	const uint16_t* rays = source->gpuRays.data();
	const uint16_t* room = source->gpuRoom.data();

	auto create = [&](DXGI_FORMAT format, const void* data, UINT pitch, const char* name) -> std::unique_ptr<Texture2D> {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = field->width;
		desc.Height = field->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{ data, pitch, 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
			logger::warn("[PBR Water] Failed to create the {} texture", name);
			return nullptr;
		}
		auto texture2D = std::make_unique<Texture2D>(texture.detach(), name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		texture2D->CreateSRV(srvDesc);
		return texture2D;
	};
	result.times = create(DXGI_FORMAT_R32G32B32A32_FLOAT, times, static_cast<UINT>(field->width * 4 * sizeof(float)), "PBRWater::ShoreTimes");
	result.rays = create(DXGI_FORMAT_R16G16B16A16_FLOAT, rays, static_cast<UINT>(field->width * 4 * sizeof(uint16_t)), "PBRWater::ShoreRays");
	result.room = create(DXGI_FORMAT_R16G16_FLOAT, room, static_cast<UINT>(field->width * 2 * sizeof(uint16_t)), "PBRWater::ShoreRoom");
	if (result.times && result.rays && result.room)
		result.field = std::move(field);
	else
		result = {};
	return result;
}

void PBRWater::UpdateSwash(float dt)
{
	if (!gpuBuffer || frameData.Shore0.x < 0.5f) {
		frameData.Swash0 = {};
		frameData.Foam2 = {};
		PBRWaterProfiling::Skipped("PBRWater::ShoreFoam");
		PBRWaterProfiling::Skipped("PBRWater::SwashStep");
		PBRWaterProfiling::Skipped("PBRWater::SwashOut");
		PBRWaterProfiling::Skipped("PBRWater::ShoreWetness");
		return;
	}

	if (swashResetRequested.exchange(false, std::memory_order_acq_rel))
		swash.Reset();

	// The swash steps with this frame's wave constants and textures, but for its own outputs (the wet grid t93 and the
	// surf's t97), which the simulations bind as they write them.
	auto context = globals::d3d::context;
	GpuData d = frameData;
	UploadWaterConstants(d);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	srvs[13] = nullptr;
	srvs[17] = nullptr;
	BindWaterCompute(gpuBuffer->CB(), srvs);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	ShoreSimulation::Settings swashSettings;
	// The swash runs wherever the waves are still at full height (about a metre per texel at the default fade).
	swashSettings.extent = std::clamp(2.0f * settings.DisplacementFadeStart, 8960.0f, 32768.0f);
	// The surf simulation first: the wet level follows it. Off, the train runs to the waterline (Foam2.w = 0).
	// Asleep while no breaking beach is near the camera (MainThreadUpdate): the surf's grids then sit still.
	const bool surfWanted = settings.SurfQuality > 0;
	const bool surf = surfWanted && surfNearby.load(std::memory_order_acquire);
	if (surf) {
		if (surfAsleep)
			swash.ResetFoam();
		surfAsleep = false;
		swash.SetQuality(static_cast<uint32_t>(settings.SurfQuality - 1));
		swash.UpdateFoam(cameraPos.x, cameraPos.y, dt);
	} else {
		surfAsleep = surfWanted;
		PBRWaterProfiling::Skipped("PBRWater::ShoreFoam");
		PBRWaterProfiling::Skipped("PBRWater::SwashStep");
		PBRWaterProfiling::Skipped("PBRWater::SwashOut");
	}
	const bool foamReady = surf && swash.IsFoamReady();
	d.Foam2 = { swash.GetFoamOriginX(), swash.GetFoamOriginY(), 1.0f / swash.GetFoamExtent(), foamReady ? 1.0f : 0.0f };
	d.Shore0.z = foamReady ? swash.GetFoamBlendCode() : 1.0f;
	d.Shore2.z = swash.GetFoamShiftCode();
	UploadWaterConstants(d);
	ID3D11ShaderResourceView* swashSrv = d.Foam2.w > 0.5f ? swash.GetFoamSRV() : nullptr;
	context->CSSetShaderResources(WaterSRVBase + 17, 1, &swashSrv);
	// The wet grid: how far up the shore it is wet, and the water's height for the lit objects' caustics. Without Wet
	// Shores nothing reads the wetness, and the caustics follow the still water.
	if (settings.WetShores)
		swash.Update(cameraPos.x, cameraPos.y, dt, swashSettings);
	else
		PBRWaterProfiling::Skipped("PBRWater::ShoreWetness");

	UnbindWaterCompute();

	// The draws read the wet state the simulation just wrote.
	frameData.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), settings.WetShores && swash.IsReady() ? 1.0f : 0.0f };
	frameData.Foam2 = d.Foam2;
	frameData.Shore0.z = d.Shore0.z;
	frameData.Shore2.z = d.Shore2.z;

	// Lit terrain and objects read the wet sand, the foam on it and the water height from it.
	ID3D11ShaderResourceView* wetSrv = frameData.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	context->PSSetShaderResources(SwashWetSlot, 1, &wetSrv);
}

uint32_t PBRWater::SprayPoolSize() const
{
	constexpr uint32_t sizes[3] = { 16384, 65536, 131072 };
	return sizes[std::clamp(settings.SprayQuality, 0, 2)];
}

void PBRWater::UpdateSpray(float dt)
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!gpuBuffer || !settings.EnableSpray || !spray.IsReady() || !snap || !snap->exterior || !globals::shaderCache->IsEnabled()) {
		PBRWaterProfiling::Skipped("PBRWater::SprayEmit");
		PBRWaterProfiling::Skipped("PBRWater::SpraySim");
		return;
	}

	// The emit pass evaluates the waves as the draws do: this frame's constants, textures and the surf's state.
	const GpuData& d = frameData;
	UploadWaterConstants(d);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	BindWaterCompute(gpuBuffer->CB(), srvs);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	SprayParticles::FrameInput input;
	input.dt = dt;
	const float windSpeed = d.Params0.y;
	input.windSpeed = windSpeed;
	// The wind the spray meets a metre or so over the water, not the 10 m wind the weather gives: the log profile over a
	// rough sea, U(z) / U(10 m) = ln(z / z0) / ln(10 m / z0) with z0 ~ 0.2 mm (Charnock), about 0.8 at 1 m, and the spray
	// thrown up in the lee of its own crest meets less still.
	constexpr float NearSurfaceWind = 0.6f;
	input.windVelocity = { d.Params1.x * windSpeed * NearSurfaceWind * UnitsPerMetre, d.Params1.y * windSpeed * NearSurfaceWind * UnitsPerMetre };
	input.gravity = d.Params0.w;
	input.cameraX = cameraPos.x;
	input.cameraY = cameraPos.y;
	input.cellSize = CellSize;
	input.cellX = std::floor(cameraPos.x / CellSize);
	input.cellY = std::floor(cameraPos.y / CellSize);
	OccluderCB levels{};
	FillOccluderLevels(levels, input.cellX, input.cellY);
	for (size_t i = 0; i < input.cellLevels.size(); ++i)
		input.cellLevels[i] = levels.cellLevels[i].x;

	if (d.Foam2.w > 0.5f && swash.GetSpraySourceSRV()) {
		input.shoreSource = swash.GetSpraySourceSRV();
		input.shoreStage = swash.GetStageSRV();
		input.shorePrep = swash.GetPrepSRV();
		input.shoreOriginX = swash.GetFoamOriginX();
		input.shoreOriginY = swash.GetFoamOriginY();
		input.shoreTexelSize = swash.GetFoamTexelSize();
		input.shoreGridSize = swash.GetFoamGridSize();
	}

	SprayParticles::Settings spraySettings;
	spraySettings.emitters = &settings.SprayEmitters;
	spraySettings.amount = settings.SprayAmount;
	spraySettings.range = settings.SprayRange;
	spraySettings.poolSize = SprayPoolSize();
	spray.Update(input, spraySettings);
	UnbindWaterCompute();
}

void PBRWater::DrawSpray()
{
	if (!loaded || !settings.EnableSpray || !spray.IsReady() || !globals::shaderCache->IsEnabled())
		return;
	auto* state = globals::state;
	if (sprayDrawFrame == state->frameCount)
		return;
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior)
		return;
	// From under the water the surface hides it.
	if (const auto* water = RE::TESWaterSystem::GetSingleton(); water && water->playerUnderwater)
		return;
	sprayDrawFrame = state->frameCount;
	auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	spray.Draw(depth);
}

PBRWater::SharedSettings PBRWater::GetCommonBufferData() const
{
	SharedSettings data{};
	if (!loaded)
		return data;
	if (frameData.Swash0.w > 0.5f) {
		data.SwashGrid = frameData.Swash0;
		// y: the highest the water can reach above the still level (PBRWater::DisplacementLimit, mirrored), so lit pixels
		// above it skip the grid fetch.
		const auto& f = frameData;
		const float limit = 2.0f * std::max(f.Params2.z, 0.0f) +
		                    2.0f * (std::max(f.Shore1.z, 0.0f) + std::max(f.Shore1.w, 0.0f)) * std::max(1.0f, f.Tune4.y / 1.55f) +
		                    std::max(f.Shore0.w, 0.0f) + 4.0f * std::max(f.Ripple1.x, 0.0f) + 1.0f;
		data.Wetness = { settings.WetShores ? 1.0f : 0.0f, limit, 0.0f, 0.0f };
	}
	// Drawn this frame or the last (the reflection passes update the shared data before Skylighting's probes).
	const bool occluding = occluderFrame != UINT32_MAX && globals::state->frameCount - occluderFrame <= 1;
	data.Skylighting = { occluding ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	return data;
}

void PBRWater::CompileSurfaceNoise()
{
	noiseCS = nullptr;
	if (auto* cs = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SurfaceNoiseCS.hlsl", {}, "cs_5_0")))
		noiseCS.attach(cs);
	else
		logger::warn("[PBR Water] Surface noise shader failed to compile; the vertices evaluate the noise themselves");
}

void PBRWater::UpdateSurfaceNoise()
{
	if (!gpuBuffer || !noiseCS || !noiseTexture || frameData.Noise0.z <= 0.0f) {
		frameData.Noise0.w = 0.0f;
		PBRWaterProfiling::Skipped("PBRWater::SurfaceNoise");
		return;
	}
	PBRWaterProfiling::Scope pass("PBRWater::SurfaceNoise");
	auto context = globals::d3d::context;
	UploadWaterConstants(frameData);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	ID3D11UnorderedAccessView* uav = noiseTexture->uav.get();
	context->CSSetConstantBuffers(7, 1, cbs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(noiseCS.get(), nullptr, 0);
	context->Dispatch(SurfaceNoiseSize / 8, SurfaceNoiseSize / 8, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(7, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	// The draws read what was just baked.
	frameData.Noise0.w = 1.0f;
}

void PBRWater::GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const
{
	srvs[0] = settings.EnableRipples ? ripples.GetSRV() : nullptr;
	srvs[1] = fetchTexture ? fetchTexture->srv.get() : nullptr;
	srvs[2] = (d.Terrain1.z > 0.5f && terrainTexture) ? terrainTexture->srv.get() : nullptr;
	srvs[3] = settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr;
	srvs[4] = shoreProfileTexture ? shoreProfileTexture->srv.get() : nullptr;
	srvs[5] = ocean.GetDisplacementSRV();
	srvs[6] = ocean.GetDerivativesSRV();
	srvs[7] = ocean.GetSurfaceSRV();
	srvs[8] = ocean.GetPreviousDisplacementSRV();
	const bool shore = d.Shore0.x > 0.5f && shoreTextures.field;
	srvs[9] = shore ? shoreTextures.times->srv.get() : nullptr;
	srvs[10] = shore ? shoreTextures.rays->srv.get() : nullptr;
	srvs[11] = kelvinPattern.get();  // the Kelvin wake pattern (KelvinWakes); the FFT foam reaches the pixels through Surface.w
	srvs[12] = shore && d.Shore0.y < 1.0f && previousShoreTextures.field ? previousShoreTextures.times->srv.get() : nullptr;
	srvs[13] = shore && d.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	srvs[14] = shore ? shoreTextures.room->srv.get() : nullptr;
	srvs[15] = foamTexture ? foamTexture->srv.get() : nullptr;
	srvs[16] = d.Noise0.w > 0.5f && noiseTexture ? noiseTexture->srv.get() : nullptr;
	srvs[17] = shore && d.Foam2.w > 0.5f ? swash.GetFoamSRV() : nullptr;
}

void PBRWater::UploadWaterConstants(const GpuData& d)
{
	// The compute passes and the wave grids each take a variant of the frame's constants: only a change is sent.
	if (uploadedWaterValid && std::memcmp(&uploadedWaterData, &d, sizeof(GpuData)) == 0)
		return;
	gpuBuffer->Update(d);
	uploadedWaterData = d;
	uploadedWaterValid = true;
}

void PBRWater::BindWaterCompute(ID3D11Buffer* constants, ID3D11ShaderResourceView* const* srvs)
{
	auto context = globals::d3d::context;
	context->CSSetConstantBuffers(7, 1, &constants);
	ID3D11Buffer* frameBuffer = *globals::game::perFrame.get();
	context->CSSetConstantBuffers(12, 1, &frameBuffer);
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, srvs);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(WaterSamplerBase, 2, samplers);
}

void PBRWater::UnbindWaterCompute()
{
	auto context = globals::d3d::context;
	ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(7, 1, &nullCb);
	ID3D11SamplerState* nullSamplers[2] = {};
	context->CSSetSamplers(WaterSamplerBase, 2, nullSamplers);
}
