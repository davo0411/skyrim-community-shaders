#include "TerrainBlending.h"

#include "Deferred.h"
#include "Globals.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.terrain_blending."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainBlending::Settings,
	Enabled)

void TerrainBlending::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Terrain Blending"), (bool*)&settings.Enabled);

	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable seamless blending between terrain and objects."));
	}
}

void TerrainBlending::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainBlending::SaveSettings(json& o_json)
{
	o_json = settings;
}

ID3D11PixelShader* TerrainBlending::GetOffsetDepthPixelShader()
{
	if (!offsetDepthPixelShader && !offsetDepthPixelShaderFailed) {
		logger::debug("Compiling OffsetDepthPS.hlsl");
		offsetDepthPixelShader = (ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\OffsetDepthPS.hlsl", {}, "ps_5_0");
		offsetDepthPixelShaderFailed = offsetDepthPixelShader == nullptr;
		if (offsetDepthPixelShaderFailed)
			logger::warn("[Terrain Blending] OffsetDepthPS.hlsl failed to compile, terrain is rendered without blending");
	}
	return offsetDepthPixelShader;
}

bool TerrainBlending::IsNearTerrain(const RE::BSRenderPass* a_pass)
{
	if (!a_pass->geometry)
		return true;

	// Without the offset shader the prepass cannot build the blend depths, so treat all terrain as far.
	if (!GetOffsetDepthPixelShader())
		return false;

	// (distance - radius) <= 1024  <=>  distance <= radius + 1024, compared squared to avoid a sqrt per pass
	const auto& bound = a_pass->geometry->worldBound;
	const float cullDistance = bound.radius + 1024.0f;
	return cullDistance > 0.0f && bound.center.GetSquaredDistance(eyePosition) <= cullDistance * cullDistance;
}

void TerrainBlending::RestoreGamePixelShader()
{
	auto* custom = globals::state->customPixelShader;
	auto* current = custom ? custom : *globals::game::currentPixelShader;
	globals::d3d::context->PSSetShader(current ? reinterpret_cast<ID3D11PixelShader*>(current->shader) : nullptr, nullptr, 0);
}

void TerrainBlending::BindTerrainMask()
{
	ID3D11ShaderResourceView* srv = mainDepthCopy->srv.get();
	globals::d3d::context->PSSetShaderResources(55, 1, &srv);
}

ID3D11ComputeShader* TerrainBlending::GetDepthBlendShader()
{
	if (!depthBlendShader) {
		logger::debug("Compiling DepthBlend.hlsl");
		depthBlendShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\DepthBlend.hlsl", {}, "cs_5_0");
	}
	return depthBlendShader;
}

void TerrainBlending::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &terrainDepth.texture));
		Util::SetResourceName(terrainDepth.texture, "TerrainBlending::TerrainDepth");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(terrainDepth.texture, &srvDesc, &terrainDepth.depthSRV));
		Util::SetResourceName(terrainDepth.depthSRV, "TerrainBlending::TerrainDepth SRV");

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(terrainDepth.texture, &dsvDesc, &terrainDepth.views[0]));
		Util::SetResourceName(terrainDepth.views[0], "TerrainBlending::TerrainDepth DSV");
	}

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		blendedDepthTexture = new Texture2D(texDesc, "TerrainBlending::BlendedDepth");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		uavDesc.Format = texDesc.Format;
		blendedDepthTexture->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		blendedDepthTexture16 = new Texture2D(texDesc, "TerrainBlending::BlendedDepth16");
		blendedDepthTexture16->CreateSRV(srvDesc);
		blendedDepthTexture16->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		mainDepthCopy = new Texture2D(texDesc, "TerrainBlending::MainDepthCopy");
		mainDepthCopy->CreateSRV(srvDesc);
		mainDepthCopy->CreateUAV(uavDesc);
		// Far-plane depth until the first prepass writes it, so an early landscape draw reads "nothing in front"
		const float farDepth[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		globals::d3d::context->ClearUnorderedAccessViewFloat(mainDepthCopy->uav.get(), farDepth);

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		depthSRVBackup = mainDepth.depthSRV;

		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		prepassSRVBackup = zPrepassCopy.depthSRV;
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, &terrainDepthStencilState));
		Util::SetResourceName(terrainDepthStencilState, "TerrainBlending::DepthStencilState");
	}
}

void TerrainBlending::PostPostLoad()
{
	Hooks::Install();
}

void TerrainBlending::DataLoaded()
{
	auto bEnableLandFade = RE::GetINISetting("bEnableLandFade:Display");
	bEnableLandFade->data.b = false;
}

void TerrainBlending::TerrainShaderHacks()
{
	if (renderTerrainDepth) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;
		if (renderAltTerrain) {
			// Main depth: terrain pushed back by a fixed linear distance, written per pixel
			auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			ID3D11Buffer* sharedData = globals::state->sharedDataCB->CB();
			context->PSSetConstantBuffers(5, 1, &sharedData);
			context->PSSetShader(GetOffsetDepthPixelShader(), nullptr, 0);
		} else {
			// Terrain depth: real terrain surface, drawn with the game's own shaders
			auto dsv = terrainDepth.views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			RestoreGamePixelShader();
		}
		renderAltTerrain = !renderAltTerrain;
	}
}

void TerrainBlending::ResetDepth()
{
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Reset Depth");
	auto context = globals::d3d::context;

	auto dsv = terrainDepth.views[0];
	globals::profiler->BeginPass("TerrainBlending::ClearDepth");
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0u);
	globals::profiler->EndPass();
}

void TerrainBlending::ResetTerrainDepth()
{
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Reset Terrain Depth");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Terrain Blending - Reset Terrain Depth");

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	// The game skips redundant shader binds, so put its pixel shader back before the next non-terrain draw
	RestoreGamePixelShader();

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void TerrainBlending::BlendPrepassDepths()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Blend Prepass Depths");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("Terrain Blending - Blend Prepass Depths");

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Depth Blend CS");

		ID3D11ShaderResourceView* views[2] = { depthSRVBackup, terrainDepth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3] = { blendedDepthTexture->uav.get(), blendedDepthTexture16->uav.get(), mainDepthCopy->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDepthBlendShader(), nullptr, 0);

		globals::profiler->BeginPass("TerrainBlending::DepthBlend");
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		globals::profiler->EndPass();
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[3] = { nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	auto stateUpdateFlags = globals::game::stateUpdateFlags;
	stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
}

void TerrainBlending::ClearShaderCache()
{
	if (offsetDepthPixelShader) {
		offsetDepthPixelShader->Release();
		offsetDepthPixelShader = nullptr;
	}
	offsetDepthPixelShaderFailed = false;
	if (depthBlendShader) {
		depthBlendShader->Release();
		depthBlendShader = nullptr;
	}
}

void TerrainBlending::Hooks::Main_RenderDepth::thunk(bool a1, bool a2)
{
	ZoneScopedS(8);

	auto& singleton = globals::features::terrainBlending;
	auto shaderCache = globals::shaderCache;
	auto renderer = globals::game::renderer;

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	globals::game::graphicsState->SetCameraData(RE::Main::WorldRootCamera(), 1);

	singleton.eyePosition = Util::GetEyePosition();

	if (shaderCache->IsEnabled() && singleton.settings.Enabled) {
		mainDepth.depthSRV = singleton.blendedDepthTexture->srv.get();
		zPrepassCopy.depthSRV = singleton.blendedDepthTexture->srv.get();

		singleton.renderDepth = true;
		singleton.ResetDepth();

		{
			ZoneScopedN("Terrain Depth - Game Render");
			func(a1, a2);
		}

		singleton.renderDepth = false;

		if (singleton.renderTerrainDepth) {
			singleton.renderTerrainDepth = false;
			singleton.ResetTerrainDepth();
			globals::profiler->EndPass();
		}

		singleton.BlendPrepassDepths();
	} else {
		mainDepth.depthSRV = singleton.depthSRVBackup;
		zPrepassCopy.depthSRV = singleton.prepassSRVBackup;

		{
			ZoneScopedN("Terrain Depth - Game Render");
			func(a1, a2);
		}
	}
}

void TerrainBlending::Hooks::BSBatchRenderer__RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	auto& singleton = globals::features::terrainBlending;
	auto shaderCache = globals::shaderCache;

	if (shaderCache->IsEnabled() && singleton.settings.Enabled) {
		if (singleton.renderDepth) {
			// Entering or exiting terrain depth section
			bool inTerrain = a_pass->shaderProperty && a_pass->shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape) && singleton.IsNearTerrain(a_pass);

			// One profiler pass spans each contiguous run of terrain draws instead of one timer per draw.
			if (singleton.renderTerrainDepth != inTerrain) {
				if (inTerrain) {
					globals::profiler->BeginPass("TerrainBlending::TerrainDepth");
				} else {
					singleton.ResetTerrainDepth();
					globals::profiler->EndPass();
				}
				singleton.renderTerrainDepth = inTerrain;
			}

			if (inTerrain)
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);  // Run terrain twice
		} else if (globals::state->inWorld) {
			if (auto shaderProperty = a_pass->shaderProperty) {
				if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
					if (shaderProperty->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
						// Only near terrain was drawn into the terrain depth, so only it can blend over objects.
						// Far terrain matches the prepass exactly: draw it in place, opaque, with the game's depth-equal test.
						if (singleton.IsNearTerrain(a_pass)) {
							RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
							singleton.terrainRenderPasses.push_back(call);
							return;
						}
						singleton.BindTerrainMask();
					}

					// Detect meshes which should not get terrain blending using an unused flag (kNoTransparencyMultiSample)
					if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kNoTransparencyMultiSample)) {
						RenderPass call{ a_pass, a_technique, a_alphaTest, a_renderFlags };
						singleton.renderPasses.push_back(call);
						return;
					}
				}
			}
		}
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void TerrainBlending::RenderTerrainBlendingPasses()
{
	ZoneScoped;

	if (!settings.Enabled) {
		renderDepth = false;
		renderTerrainDepth = false;
		renderAltTerrain = false;
		terrainRenderPasses.clear();
		renderPasses.clear();
		auto renderer = globals::game::renderer;
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		mainDepth.depthSRV = depthSRVBackup;
		zPrepassCopy.depthSRV = prepassSRVBackup;
		return;
	}

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;
	auto shadowState = globals::game::shadowState;
	auto stateUpdateFlags = globals::game::stateUpdateFlags;

	BindTerrainMask();

	const uint64_t terrainPassCount = static_cast<uint64_t>(terrainRenderPasses.size());
	const uint64_t noBlendPassCount = static_cast<uint64_t>(renderPasses.size());

	if (terrainPassCount != 0 || noBlendPassCount != 0) {
		TracyD3D11Zone(globals::state->tracyCtx, "Terrain Blending - Render Passes");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("Terrain Blending - Render Passes");

		auto& alphaBlendMode = shadowState->GetRuntimeData().alphaBlendMode;
		auto& alphaBlendWriteMode = shadowState->GetRuntimeData().alphaBlendWriteMode;
		auto& depthStencilDepthMode = shadowState->GetRuntimeData().depthStencilDepthMode;

		// Reset alpha write and enable alpha blending
		alphaBlendWriteMode = 1;
		alphaBlendMode = 1;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Enable rendering for depth below the surface
		context->OMSetDepthStencilState(terrainDepthStencilState, 0xFF);

		globals::profiler->BeginPass("TerrainBlending::TerrainPasses");
		for (auto& renderPass : terrainRenderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);
		globals::profiler->EndPass();

		// Reset alpha blending
		alphaBlendMode = 0;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

		// Reset depth testing
		depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kTestEqual;
		stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);

		globals::profiler->BeginPass("TerrainBlending::NoBlendPasses");
		for (auto& renderPass : renderPasses)
			Hooks::BSBatchRenderer__RenderPassImmediately::func(renderPass.a_pass, renderPass.a_technique, renderPass.a_alphaTest, renderPass.a_renderFlags);
		globals::profiler->EndPass();

		terrainRenderPasses.clear();
		renderPasses.clear();

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	mainDepth.depthSRV = depthSRVBackup;
	// Point the prepass copy back at its own texture: Deferred copies the final depth into it,
	// and everything after this pass (water included) should read that rather than blended prepass depth.
	auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	zPrepassCopy.depthSRV = prepassSRVBackup;
}
#undef I18N_KEY_PREFIX
