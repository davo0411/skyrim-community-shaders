#include "RippleSimulation.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float FixedStep = 1.0f / 60.0f;
	constexpr uint32_t MaxStepsPerFrame = 4;
}

void RippleSimulation::SetupResources()
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = GridSize;
	desc.Height = GridSize;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

	for (uint32_t i = 0; i < 2; ++i) {
		state[i] = std::make_unique<Texture2D>(desc, i == 0 ? "PBRWater::RippleState0" : "PBRWater::RippleState1");
		state[i]->CreateSRV(srvDesc);
		state[i]->CreateUAV(uavDesc);
	}
	previousFrame = std::make_unique<Texture2D>(desc, "PBRWater::RipplePreviousFrame");
	previousFrame->CreateSRV(srvDesc);
	previousFrame->CreateUAV(uavDesc);

	sourceBuffer = std::make_unique<StructuredBuffer>(StructuredBufferDesc<GpuSource>(MaxSources, true), MaxSources, "PBRWater::RippleSources");
	sourceBuffer->CreateSRV();
	simCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SimCB>(), "PBRWater::RippleSimCB");

	ClearShaderCache();
	Reset();
}

void RippleSimulation::ClearShaderCache()
{
	simCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\RippleSimCS.hlsl", {}, "cs_5_0")));
	ready = simCS && state[0] && state[1] && previousFrame;
}

void RippleSimulation::Reset()
{
	hasOrigin = false;
	accumulator = 0.0f;
	stepAlpha = previousStepAlpha = 0.0f;
	if (!state[0] || !state[1] || !previousFrame)
		return;
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto context = globals::d3d::context;
	context->ClearUnorderedAccessViewFloat(state[0]->uav.get(), zero);
	context->ClearUnorderedAccessViewFloat(state[1]->uav.get(), zero);
	context->ClearUnorderedAccessViewFloat(previousFrame->uav.get(), zero);
}

void RippleSimulation::SubmitSources(std::vector<Source>&& sources)
{
	std::scoped_lock lock(sourcesMutex);
	pendingSources = std::move(sources);
}

ID3D11ShaderResourceView* RippleSimulation::GetSRV() const
{
	return state[current] ? state[current]->srv.get() : nullptr;
}

ID3D11ShaderResourceView* RippleSimulation::GetPreviousSRV() const
{
	return previousFrame ? previousFrame->srv.get() : nullptr;
}

void RippleSimulation::Update(float cameraX, float cameraY, float dt, const Settings& settings)
{
	if (!ready)
		return;

	const float newTexelSize = std::max(settings.extent, 256.0f) / GridSize;
	if (newTexelSize != texelSize) {
		// A different simulated area means a different grid; the old state no longer maps onto it.
		texelSize = newTexelSize;
		hasOrigin = false;
	}

	// What was displayed last frame becomes the motion-vector reference for this frame.
	globals::d3d::context->CopyResource(previousFrame->resource.get(), state[current]->resource.get());
	previousOriginTexelX = originTexelX;
	previousOriginTexelY = originTexelY;
	previousStepAlpha = stepAlpha;

	// Snap the grid to whole texels so scrolling never resamples (and never smears) the state.
	const int64_t newOriginX = static_cast<int64_t>(std::floor(cameraX / texelSize)) - GridSize / 2;
	const int64_t newOriginY = static_cast<int64_t>(std::floor(cameraY / texelSize)) - GridSize / 2;
	if (!hasOrigin || std::abs(newOriginX - originTexelX) >= GridSize || std::abs(newOriginY - originTexelY) >= GridSize) {
		// First frame or teleport: nothing of the old state is still in range.
		Reset();
		originTexelX = previousOriginTexelX = newOriginX;
		originTexelY = previousOriginTexelY = newOriginY;
		hasOrigin = true;
	}

	std::vector<Source> sources;
	{
		std::scoped_lock lock(sourcesMutex);
		sources = pendingSources;
	}

	// Sources are placed in the grid the next step writes, i.e. at the new origin.
	std::array<GpuSource, MaxSources> gpuSources{};
	uint32_t numSources = 0;
	const float originX = static_cast<float>(newOriginX) * texelSize;
	const float originY = static_cast<float>(newOriginY) * texelSize;
	for (const auto& s : sources) {
		if (numSources >= MaxSources)
			break;
		GpuSource& g = gpuSources[numSources];
		g.position = { (s.x - originX) / texelSize, (s.y - originY) / texelSize };
		if (g.position.x < -8.0f || g.position.y < -8.0f || g.position.x > GridSize + 8.0f || g.position.y > GridSize + 8.0f)
			continue;
		g.radius = std::max(s.radius / texelSize, 0.75f);
		g.depth = s.depth;
		g.silt = s.silt;
		if (g.depth <= 0.0f && g.silt <= 0.0f)
			continue;
		++numSources;
	}
	sourceBuffer->Update(gpuSources.data(), sizeof(gpuSources));

	// Fixed time step for a stable explicit integrator; C^2 = (c dt / dx)^2 must stay <= 0.5 in 2D.
	const float speedUnits = settings.waveSpeed * 70.0f;
	SimCB base{};
	base.gridSize = GridSize;
	base.waveSpeed2 = std::min(std::pow(speedUnits * FixedStep / texelSize, 2.0f), 0.45f);
	base.damping = std::pow(0.5f, FixedStep / std::max(settings.halfLife, 0.05f));
	base.foamDecay = std::pow(0.5f, FixedStep / std::max(settings.foamHalfLife, 0.05f));
	base.foamFromMotion = settings.foamFromMotion;
	base.siltDecay = std::pow(0.5f, FixedStep / std::max(settings.siltHalfLife, 0.5f));
	base.siltDiffusion = std::clamp(settings.siltSpread, 0.0f, 1.0f);
	// Kills grid-scale (checkerboard) noise within a few steps while barely touching real ripples.
	// Von Neumann analysis of the viscous leapfrog for the checkerboard mode (laplacian eigenvalue -8)
	// gives stability for nu <= (1 - 2 C^2) / 4; stay at 80% of that bound.
	base.viscosity = std::clamp(0.8f * (1.0f - 2.0f * base.waveSpeed2) / 4.0f, 0.0f, 0.12f);

	accumulator = std::min(accumulator + std::max(dt, 0.0f), FixedStep * MaxStepsPerFrame);
	bool first = true;
	while (accumulator >= FixedStep) {
		accumulator -= FixedStep;
		// The grid only scrolls together with a real step; between steps (high frame rates, pause)
		// it keeps its origin, so the stored state is never re-integrated without time passing.
		const int32_t shiftX = first ? static_cast<int32_t>(newOriginX - originTexelX) : 0;
		const int32_t shiftY = first ? static_cast<int32_t>(newOriginY - originTexelY) : 0;
		Step(shiftX, shiftY, numSources, base);
		first = false;
	}
	if (!first) {
		originTexelX = newOriginX;
		originTexelY = newOriginY;
	}

	// Shaders interpolate between the last two steps to the current time.
	stepAlpha = accumulator / FixedStep;
}

void RippleSimulation::Step(int32_t shiftX, int32_t shiftY, uint32_t numSources, const SimCB& base)
{
	auto context = globals::d3d::context;

	SimCB cb = base;
	cb.shiftX = shiftX;
	cb.shiftY = shiftY;
	cb.numSources = numSources;
	simCB->Update(cb);

	const uint32_t next = current ^ 1;
	ID3D11ShaderResourceView* srvs[2] = { state[current]->srv.get(), sourceBuffer->SRV(0) };
	ID3D11UnorderedAccessView* uav = state[next]->uav.get();
	ID3D11Buffer* cbs[1] = { simCB->CB() };

	context->CSSetShader(simCS.get(), nullptr, 0);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(0, 1, cbs);
	context->Dispatch(GridSize / 8, GridSize / 8, 1);

	ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetShaderResources(0, 2, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	current = next;
}
