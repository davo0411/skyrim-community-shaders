// PBR Water: The water around the camera below the surface (UnderwaterVolume.hlsl) in place of the game's underwater fog.

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

void PBRWater::CompileUnderwater()
{
	// The light shafts' shadows: the shadow features that are loaded (ShadowSampling::GetSurfaceShadow). Every part of the
	// volume includes UnderwaterLight.hlsli, so all are compiled with them.
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::terrainShadows.loaded)
		defines.push_back({ "TERRAIN_SHADOWS", "" });
	if (globals::features::cloudShadows.loaded)
		defines.push_back({ "CLOUD_SHADOWS", "" });
	if (globals::features::volumetricShadows.loaded)
		defines.push_back({ "VOLUMETRIC_SHADOWS", "" });
	const wchar_t* volume = L"Data\\Shaders\\PBRWater\\UnderwaterVolume.hlsl";
	const wchar_t* shaftsCS = L"Data\\Shaders\\PBRWater\\UnderwaterShaftsCS.hlsl";
	underwaterVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(volume, defines, "vs_5_0", "VS")));
	underwaterPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(volume, defines, "ps_5_0", "PS")));
	underwaterMarchPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(volume, defines, "ps_5_0", "MarchPS")));
	auto withDefine = [&](const char* name) {
		auto d = defines;
		d.push_back({ name, "" });
		return d;
	};
	underwaterTemporalCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(shaftsCS, withDefine("TEMPORAL"), "cs_5_0")));
	underwaterBlurCS[0].attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(shaftsCS, withDefine("HORIZONTAL"), "cs_5_0")));
	underwaterBlurCS[1].attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(shaftsCS, defines, "cs_5_0")));
}

void PBRWater::DrawUnderwaterVolume()
{
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!settings.UnderwaterVolume || !underwaterVS || !underwaterPS || !underwaterBlend || !waterSystem || !waterSystem->playerUnderwater) {
		underwaterHistoryFrame = UINT32_MAX;
		return;
	}
	auto* form = waterSystem->currentWaterType;
	auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!form || !depth.depthSRV)
		return;
	auto context = globals::d3d::context;
	BoundTargets bound(context);
	if (!bound.rtv[0])
		return;
	const uint32_t frame = globals::state->frameCount;

	// The water form's optics as the surface works them out (Optics.hlsli Extinction, GetWaterOptics): its shallow colour
	// is the tint the light takes over its visibility, pure water's absorption the floor; the scattering is pure water's
	// own (Morel 1974) and the particles' share of what the form adds, spectrally flat. The volume scatters the albedo
	// sigma_s / sigma_t of what it takes.
	auto linear = [](uint8_t c) { return std::pow(c / 255.0f, 2.2f); };
	const auto& shallow = form->data.shallowWaterColor;
	float tint[3] = { linear(shallow.red), linear(shallow.green), linear(shallow.blue) };
	const float tintMax = std::max({ tint[0], tint[1], tint[2], 1e-4f });
	const float visibility = std::max(form->data.aboveWaterFogDistFar * settings.Visibility, 1.0f);
	constexpr float pureAbsorption[3] = { 0.35f, 0.06f, 0.012f };       // 1/m (Optics.hlsli PureWaterAbsorption)
	constexpr float pureScattering[3] = { 0.0010f, 0.0019f, 0.0045f };  // 1/m (Optics.hlsli PureWaterScattering)
	float extinction[3], excess[3];
	for (int i = 0; i < 3; ++i) {
		extinction[i] = std::max((std::log(20.0f) - std::log(std::max(tint[i] / tintMax, 0.02f))) / visibility, (pureAbsorption[i] + pureScattering[i]) / UnitsPerMetre);
		excess[i] = std::max(extinction[i] - (pureAbsorption[i] + pureScattering[i]) / UnitsPerMetre, 0.0f);
	}
	const float particles = 0.5f * std::min({ excess[0], excess[1], excess[2] });
	float albedo[3];
	for (int i = 0; i < 3; ++i)
		albedo[i] = std::clamp((pureScattering[i] / UnitsPerMetre + particles) / std::max(extinction[i], 1e-8f), 0.0f, 1.0f);

	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	D3D11_VIEWPORT viewport{};
	UINT viewports = 1;
	context->RSGetViewports(&viewports, &viewport);
	const uint32_t width = std::max(static_cast<uint32_t>(viewport.Width), 1u);
	const uint32_t height = std::max(static_cast<uint32_t>(viewport.Height), 1u);
	const uint32_t halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
	const bool shafts = settings.LightShaftSamples > 0 && underwaterMarchPS && underwaterTemporalCS && underwaterBlurCS[0] && underwaterBlurCS[1];

	// The shafts' half-resolution targets: the march, two histories, the blur's middle.
	if (shafts && (!underwaterShaftTargets[0] || underwaterShaftTargets[0]->desc.Width != halfWidth || underwaterShaftTargets[0]->desc.Height != halfHeight)) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = halfWidth;
		desc.Height = halfHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		const char* names[4] = { "PBRWater::UnderwaterShaftMarch", "PBRWater::UnderwaterShaftHistory0", "PBRWater::UnderwaterShaftHistory1", "PBRWater::UnderwaterShaftBlur" };
		for (uint32_t i = 0; i < 4; ++i) {
			underwaterShaftTargets[i] = std::make_unique<Texture2D>(desc, names[i]);
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			underwaterShaftTargets[i]->CreateSRV(srv);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			underwaterShaftTargets[i]->CreateUAV(uav);
			D3D11_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = desc.Format;
			rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			underwaterShaftTargets[i]->CreateRTV(rtv);
		}
		underwaterHistoryFrame = UINT32_MAX;
	}
	const bool historyValid = underwaterHistoryFrame + 1 == frame;
	const uint32_t current = frame & 1;  // the history written this frame (1, 2), the other read

	const float cameraDepth = std::max(waterSystem->underwaterHeight - eye.z, 0.0f);
	float4 data[5] = {
		{ extinction[0], extinction[1], extinction[2], cameraDepth },
		{ albedo[0], albedo[1], albedo[2], 0.0f },
		{ static_cast<float>(width), static_cast<float>(height), 200.0f * UnitsPerMetre, cameraDepth },
		{ shafts ? static_cast<float>(settings.LightShaftSamples) : 0.0f, 0.8f, settings.LightShaftFocus, historyValid ? 1.0f : 0.0f },
		{ static_cast<float>(halfWidth), static_cast<float>(halfHeight), (frame & 1) * 0.5f, ((frame >> 1) & 1) * 0.5f },
	};
	underwaterCB->Update(data);

	// What the draws change, put back after them: the engine binds its state lazily and expects it unchanged.
	winrt::com_ptr<ID3D11VertexShader> savedVS;
	winrt::com_ptr<ID3D11PixelShader> savedPS;
	winrt::com_ptr<ID3D11InputLayout> savedLayout;
	winrt::com_ptr<ID3D11BlendState> savedBlend;
	winrt::com_ptr<ID3D11Buffer> savedCb, savedFrameCb;
	winrt::com_ptr<ID3D11ShaderResourceView> savedSrvs[2];
	float blendFactor[4];
	UINT sampleMask;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	context->VSGetShader(savedVS.put(), nullptr, nullptr);
	context->PSGetShader(savedPS.put(), nullptr, nullptr);
	context->IAGetInputLayout(savedLayout.put());
	context->IAGetPrimitiveTopology(&topology);
	context->OMGetBlendState(savedBlend.put(), blendFactor, &sampleMask);
	context->PSGetConstantBuffers(0, 1, savedCb.put());
	context->PSGetConstantBuffers(12, 1, savedFrameCb.put());
	{
		ID3D11ShaderResourceView* views[2] = {};
		context->PSGetShaderResources(0, 2, views);
		savedSrvs[0].attach(views[0]);
		savedSrvs[1].attach(views[1]);
	}

	// Shared by the passes: the volume's constants, the frame's, PBR Water's (the waves the shafts read) and its views.
	ID3D11Buffer* cbs[1] = { underwaterCB->CB() };
	ID3D11Buffer* frameCb[1] = { *globals::game::perFrame.get() };
	ID3D11Buffer* waterCb = flatDrawBuffer->CB();
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->PSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(12, 1, frameCb);
	context->PSSetConstantBuffers(7, 1, &waterCb);
	context->PSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
	context->PSSetSamplers(WaterSamplerBase, 2, samplers);
	boundPixelCb = nullptr;
	drawResourcesFrame = UINT32_MAX;
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(underwaterVS.get(), nullptr, 0);
	ID3D11ShaderResourceView* depthView = depth.depthSRV;

	if (shafts) {
		PBRWaterProfiling::Scope scope("PBRWater::Underwater Shafts");
		// The march, at half resolution (a pixel shader: the shadow maps are bound to the pixel stage).
		ID3D11RenderTargetView* marchTarget = underwaterShaftTargets[0]->rtv.get();
		context->OMSetRenderTargets(1, &marchTarget, nullptr);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		D3D11_VIEWPORT halfViewport{ 0.0f, 0.0f, static_cast<float>(halfWidth), static_cast<float>(halfHeight), 0.0f, 1.0f };
		context->RSSetViewports(1, &halfViewport);
		context->PSSetShader(underwaterMarchPS.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &depthView);
		context->Draw(3, 0);
		context->RSSetViewports(1, &viewport);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		// Temporal, then the separable blur (compute: no shadows needed).
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetConstantBuffers(12, 1, frameCb);
		context->CSSetSamplers(WaterSamplerBase, 2, samplers);
		auto dispatch = [&](ID3D11ComputeShader* shader, std::initializer_list<ID3D11ShaderResourceView*> inputs, ID3D11UnorderedAccessView* output, uint32_t x, uint32_t y) {
			ID3D11ShaderResourceView* views[2] = {};
			uint32_t n = 0;
			for (auto* view : inputs)
				views[n++] = view;
			context->CSSetShaderResources(0, 2, views);
			context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			context->CSSetShader(shader, nullptr, 0);
			context->Dispatch(x, y, 1);
			ID3D11ShaderResourceView* none[2] = {};
			ID3D11UnorderedAccessView* noUav = nullptr;
			context->CSSetShaderResources(0, 2, none);
			context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
		};
		auto* history = underwaterShaftTargets[1 + current].get();
		auto* previous = underwaterShaftTargets[2 - current].get();
		dispatch(underwaterTemporalCS.get(), { underwaterShaftTargets[0]->srv.get(), previous->srv.get() }, history->uav.get(), (halfWidth + 7) / 8, (halfHeight + 7) / 8);
		// 256-wide groups of which 232 texels are written (the 12-texel window on either side): Effects 11's layout.
		dispatch(underwaterBlurCS[0].get(), { history->srv.get() }, underwaterShaftTargets[3]->uav.get(), (halfWidth + 231) / 232, halfHeight);
		dispatch(underwaterBlurCS[1].get(), { underwaterShaftTargets[3]->srv.get() }, underwaterShaftTargets[0]->uav.get(), halfWidth, (halfHeight + 231) / 232);
		context->CSSetShader(nullptr, nullptr, 0);
		underwaterHistoryFrame = frame;
	} else {
		underwaterHistoryFrame = UINT32_MAX;
	}

	// The composite over the scene. The depth buffer is bound for writing: the colour target alone for this draw.
	context->OMSetRenderTargets(1, bound.rtv, nullptr);
	context->OMSetBlendState(underwaterBlend.get(), nullptr, 0xFFFFFFFF);
	context->PSSetShader(underwaterPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* views[2] = { depthView, shafts ? underwaterShaftTargets[0]->srv.get() : nullptr };
	context->PSSetShaderResources(0, 2, views);
	{
		PBRWaterProfiling::Scope scope("PBRWater::Underwater Volume");
		context->Draw(3, 0);
	}

	ID3D11ShaderResourceView* restoredSrvs[2] = { savedSrvs[0].get(), savedSrvs[1].get() };
	context->PSSetShaderResources(0, 2, restoredSrvs);
	ID3D11Buffer* restoredCb = savedCb.get();
	context->PSSetConstantBuffers(0, 1, &restoredCb);
	ID3D11Buffer* restoredFrameCb = savedFrameCb.get();
	context->PSSetConstantBuffers(12, 1, &restoredFrameCb);
	context->VSSetShader(savedVS.get(), nullptr, 0);
	context->PSSetShader(savedPS.get(), nullptr, 0);
	context->IASetInputLayout(savedLayout.get());
	context->IASetPrimitiveTopology(topology);
	context->OMSetBlendState(savedBlend.get(), blendFactor, sampleMask);
	context->OMSetRenderTargets(bound.count, bound.rtv, bound.dsv);
}

void PBRWater::ApplyUnderwaterFogOverride()
{
	auto* dataHandler = RE::TESDataHandler::GetSingleton();
	if (!dataHandler)
		return;
	const bool replace = loaded && settings.UnderwaterVolume && underwaterVS && underwaterPS && underwaterBlend;
	if (replace) {
		for (auto* form : dataHandler->GetFormArray<RE::TESWaterForm>()) {
			if (form && originalUnderwaterFog.try_emplace(form, form->data.underwaterFogAmount).second)
				form->data.underwaterFogAmount = 0.0f;
		}
	} else if (!originalUnderwaterFog.empty()) {
		for (auto& [form, amount] : originalUnderwaterFog)
			form->data.underwaterFogAmount = amount;
		originalUnderwaterFog.clear();
	}
}
