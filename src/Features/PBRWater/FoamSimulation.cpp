#include "FoamSimulation.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

namespace
{
	std::unique_ptr<Texture2D> CreateStateTexture(const char* name)
	{
		constexpr DXGI_FORMAT Format = DXGI_FORMAT_R16G16_FLOAT;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = FoamSimulation::GridSize;
		desc.Height = FoamSimulation::GridSize;
		desc.MipLevels = 1;
		desc.ArraySize = FoamSimulation::Levels;
		desc.Format = Format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = FoamSimulation::Levels;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = FoamSimulation::Levels;

		auto texture = std::make_unique<Texture2D>(desc, name);
		texture->CreateSRV(srvDesc);
		texture->CreateUAV(uavDesc);
		return texture;
	}
}

void FoamSimulation::SetupResources()
{
	state[0] = CreateStateTexture("PBRWater::Foam0");
	state[1] = CreateStateTexture("PBRWater::Foam1");
	simCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SimCB>(), "PBRWater::FoamCB");
	ClearShaderCache();
	Reset();
}

void FoamSimulation::ClearShaderCache()
{
	simCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\FoamSimCS.hlsl", {}, "cs_5_0")));
	ready = simCS && state[0] && state[1] && simCB;
	if (!simCS)
		logger::warn("[PBR Water] Foam shader failed to compile; the water draws only the foam breaking this frame");
}

void FoamSimulation::Reset()
{
	hasOrigin = false;
	auto context = globals::d3d::context;
	const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (auto& s : state) {
		if (s)
			context->ClearUnorderedAccessViewFloat(s->uav.get(), none);
	}
}

void FoamSimulation::GetLevel(uint32_t level, float& originX, float& originY, float& inverseExtent) const
{
	level = std::min(level, Levels - 1);
	originX = static_cast<float>(originTexel[level][0]) * texelSize[level];
	originY = static_cast<float>(originTexel[level][1]) * texelSize[level];
	inverseExtent = 1.0f / (texelSize[level] * GridSize);
}

void FoamSimulation::Update(float cameraX, float cameraY, float dt, const Settings& settings)
{
	if (!ready)
		return;
	auto context = globals::d3d::context;

	const float coarse = std::max(settings.extent, 4096.0f) / GridSize;
	const float sizes[Levels] = { coarse / LevelRatio, coarse };
	bool restart = !hasOrigin;
	for (uint32_t l = 0; l < Levels; ++l) {
		if (sizes[l] != texelSize[l])
			restart = true;
		texelSize[l] = sizes[l];
	}

	// Whole texels, so scrolling never resamples (and never smears) the foam.
	int64_t next[Levels][2];
	for (uint32_t l = 0; l < Levels; ++l) {
		next[l][0] = static_cast<int64_t>(std::floor(cameraX / texelSize[l])) - GridSize / 2;
		next[l][1] = static_cast<int64_t>(std::floor(cameraY / texelSize[l])) - GridSize / 2;
		if (!restart && (std::abs(next[l][0] - originTexel[l][0]) >= GridSize || std::abs(next[l][1] - originTexel[l][1]) >= GridSize))
			restart = true;
	}
	if (restart) {
		Reset();
		for (uint32_t l = 0; l < Levels; ++l) {
			originTexel[l][0] = next[l][0];
			originTexel[l][1] = next[l][1];
		}
		hasOrigin = true;
	}

	// A long hitch must not wipe the foam in one step.
	const float step = std::clamp(dt, 0.0f, 0.1f);
	SimCB cb{};
	for (uint32_t l = 0; l < Levels; ++l) {
		cb.shift[l * 2 + 0] = static_cast<int32_t>(next[l][0] - originTexel[l][0]);
		cb.shift[l * 2 + 1] = static_cast<int32_t>(next[l][1] - originTexel[l][1]);
		cb.origin[l * 2 + 0] = static_cast<float>(next[l][0]) * texelSize[l];
		cb.origin[l * 2 + 1] = static_cast<float>(next[l][1]) * texelSize[l];
	}
	cb.texel[0] = texelSize[0];
	cb.texel[1] = texelSize[1];
	cb.texel[2] = static_cast<float>(GridSize);
	cb.decay[0] = std::exp(-step / std::max(settings.lifetime, 0.05f));
	cb.decay[1] = std::exp(-step / std::max(settings.freshLifetime, 0.05f));
	cb.decay[2] = settings.waterLevel;
	simCB->Update(cb);

	const uint32_t target = current ^ 1;
	ID3D11ShaderResourceView* srv = state[current]->srv.get();
	ID3D11UnorderedAccessView* uav = state[target]->uav.get();
	ID3D11Buffer* cbs[1] = { simCB->CB() };
	context->CSSetShader(simCS.get(), nullptr, 0);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetConstantBuffers(0, 1, cbs);
	context->Dispatch(GridSize / 8, GridSize / 8, Levels);

	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	current = target;
	for (uint32_t l = 0; l < Levels; ++l) {
		originTexel[l][0] = next[l][0];
		originTexel[l][1] = next[l][1];
	}
}
