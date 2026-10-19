#include "ShoreSimulation.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

namespace
{
	std::unique_ptr<Texture2D> CreateStateTexture(DXGI_FORMAT format, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = ShoreSimulation::GridSize;
		desc.Height = ShoreSimulation::GridSize;
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
	simCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SimCB>(), "PBRWater::SwashCB");
	ClearShaderCache();
	Reset();
}

void ShoreSimulation::ClearShaderCache()
{
	simCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\ShoreSimCS.hlsl", {}, "cs_5_0")));
	ready = simCS && wet[0] && wet[1] && simCB;
	if (!simCS)
		logger::warn("[PBR Water] Swash shader failed to compile; the swash leaves no wet sand behind");
}

void ShoreSimulation::Reset()
{
	hasOrigin = false;
	auto context = globals::d3d::context;
	const float dry[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < 2; ++i) {
		if (wet[i])
			context->ClearUnorderedAccessViewFloat(wet[i]->uav.get(), dry);
	}
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
