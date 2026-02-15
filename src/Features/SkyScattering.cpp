#include "SkyScattering.h"

#include "Globals.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkyScattering::Settings,
	Enabled,
	Opacity,
	NumLayers,
	CloudShadowStrength,
	ScatterTint,
	ScatterAmount,
	SilverIntensity,
	SilverSpread,
	AmbientDarkening)

void SkyScattering::DrawSettings()
{
	ImGui::Checkbox("Enable", (bool*)&settings.Enabled);

	ImGui::SeparatorText("Ground Shadows");

	ImGui::SliderFloat("Shadow Opacity", &settings.Opacity, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Opacity of cloud shadows cast on the ground.");
	}

	int numLayers = static_cast<int>(settings.NumLayers);
	if (ImGui::SliderInt("Cloud Layers", &numLayers, 1, static_cast<int>(MAX_CLOUD_LAYERS))) {
		settings.NumLayers = static_cast<uint32_t>(numLayers);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Number of cloud layers for volumetric scattering.\nMore layers = more accurate but higher GPU cost.");
	}

	ImGui::SeparatorText("Cloud Visuals");

	ImGui::SliderFloat("Inter-Cloud Shadowing", &settings.CloudShadowStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("How much upper clouds darken lower clouds.\nMakes clouds appear more volumetric and layered.");
	}

	ImGui::ColorEdit3("Scatter Tint", &settings.ScatterTint.x);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Color tint applied to light scattering through clouds.\nWarm tones simulate realistic forward scattering.");
	}

	ImGui::SliderFloat("Scatter Amount", &settings.ScatterAmount, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Strength of forward scattering color effect.");
	}

	ImGui::SliderFloat("Silver Lining", &settings.SilverIntensity, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Brightness of cloud edges when backlit by the sun.\nCreates the classic 'silver lining' effect.");
	}

	ImGui::SliderFloat("Silver Spread", &settings.SilverSpread, 0.01f, 0.5f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("How wide the silver lining effect spreads around cloud edges.");
	}

	ImGui::SliderFloat("Ambient Darkening", &settings.AmbientDarkening, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("How much thick cloud regions darken from reduced ambient light.\nAdds depth and volume to dense clouds.");
	}
}

void SkyScattering::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SkyScattering::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkyScattering::RestoreDefaultSettings()
{
	settings = {};
}

void SkyScattering::CheckResourcesSide(int side)
{
	static Util::FrameChecker frame_checker[6];
	if (!frame_checker[side].IsNewFrame())
		return;

	auto context = globals::d3d::context;

	// Clear all layers for this face
	float black[4] = { 0, 0, 0, 0 };
	for (uint32_t layer = 0; layer < settings.NumLayers; ++layer) {
		context->ClearRenderTargetView(layerRTVs[layer][side], black);
	}

	currentCloudLayer = 0;
}

void SkyScattering::SkyShaderHacks()
{
	if (overrideSky) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;

		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		// Determine which cubemap face we're rendering
		ID3D11RenderTargetView* rtvs[4];
		ID3D11DepthStencilView* dsv;
		context->OMGetRenderTargets(3, rtvs, &dsv);

		int side = -1;
		for (int i = 0; i < 6; ++i) {
			if (rtvs[0] == reflections.cubeSideRTV[i]) {
				side = i;
				break;
			}
		}

		if (side == -1) {
			for (int i = 0; i < 3; ++i) {
				if (rtvs[i])
					rtvs[i]->Release();
			}
			if (dsv)
				dsv->Release();
			return;
		}

		CheckResourcesSide(side);

		uint32_t layerIdx = std::min(currentCloudLayer, settings.NumLayers - 1);

		// Bind the current layer's RTV as render target 3
		rtvs[3] = layerRTVs[layerIdx][side];
		context->OMSetRenderTargets(4, rtvs, nullptr);

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		UINT sampleMask = 0xffffffff;
		context->OMSetBlendState(scatterBlendState, blendFactor, sampleMask);

		// Bind the layer above (if any) as SRV so clouds can be shadowed by layers above them
		if (layerIdx > 0) {
			// Copy the previous layer to the copy texture for safe SRV binding
			// We use the full array copy approach for simplicity
			context->CopyResource(texCubemapArrayCopy->resource.get(), texCubemapArray->resource.get());

			ID3D11ShaderResourceView* srv = texCubemapArrayCopy->srv.get();
			context->PSSetShaderResources(26, 1, &srv);
		}

		auto cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		context->PSSetShaderResources(17, 1, &cubemapDepth.depthSRV);

		// Release COM objects to prevent memory leaks
		for (int i = 0; i < 3; ++i) {
			if (rtvs[i])
				rtvs[i]->Release();
		}
		if (dsv)
			dsv->Release();

		overrideSky = false;
	}
}

void SkyScattering::ModifySky(RE::BSRenderPass* Pass)
{
	auto shadowState = globals::game::shadowState;

	GET_INSTANCE_MEMBER(cubeMapRenderTarget, shadowState);

	if (cubeMapRenderTarget != RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)
		return;

	auto skyProperty = static_cast<const RE::BSSkyShaderProperty*>(Pass->shaderProperty);

	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS) {
		overrideSky = true;
	}
}

void SkyScattering::AdvanceCloudLayer()
{
	if (currentCloudLayer < settings.NumLayers - 1) {
		auto context = globals::d3d::context;

		// Copy current array state so the next layer can sample layers above it
		context->CopyResource(texCubemapArrayCopy->resource.get(), texCubemapArray->resource.get());

		currentCloudLayer++;
	}
}

void SkyScattering::ReflectionsPrepass()
{
	Util::FrameChecker frameChecker;
	if (frameChecker.IsNewFrame()) {
		if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
			!globals::game::sky->currentClimate)
			return;

		auto context = globals::d3d::context;

		// Copy the last layer (cloud shadow result) for use in lighting
		context->CopyResource(texCubemapArrayCopy->resource.get(), texCubemapArray->resource.get());

		// Bind the final layer's SRV for cloud shadow sampling
		ID3D11ShaderResourceView* srv = texCubemapArrayCopy->srv.get();
		context->PSSetShaderResources(26, 1, &srv);
		context->CSSetShaderResources(26, 1, &srv);
	}
}

void SkyScattering::EarlyPrepass()
{
	if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
		!globals::game::sky->currentClimate)
		return;

	auto context = globals::d3d::context;

	ID3D11ShaderResourceView* srv = texCubemapArray->srv.get();
	context->PSSetShaderResources(26, 1, &srv);
	context->CSSetShaderResources(26, 1, &srv);
}

void SkyScattering::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		reflections.texture->GetDesc(&texDesc);

		// Create cubemap array: 6 faces * NumLayers
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.ArraySize = 6 * MAX_CLOUD_LAYERS;
		texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
		srvDesc.TextureCubeArray.MostDetailedMip = 0;
		srvDesc.TextureCubeArray.MipLevels = 1;
		srvDesc.TextureCubeArray.First2DArrayFace = 0;
		srvDesc.TextureCubeArray.NumCubes = MAX_CLOUD_LAYERS;

		texCubemapArray = new Texture2D(texDesc);
		texCubemapArray->CreateSRV(srvDesc);

		texCubemapArrayCopy = new Texture2D(texDesc);
		texCubemapArrayCopy->CreateSRV(srvDesc);

		// Create per-layer per-face RTVs
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Texture2DArray.MipSlice = 0;
		rtvDesc.Texture2DArray.ArraySize = 1;

		for (uint32_t layer = 0; layer < MAX_CLOUD_LAYERS; ++layer) {
			for (int face = 0; face < 6; ++face) {
				rtvDesc.Texture2DArray.FirstArraySlice = layer * 6 + face;
				DX::ThrowIfFailed(device->CreateRenderTargetView(texCubemapArray->resource.get(), &rtvDesc, &layerRTVs[layer][face]));
				DX::ThrowIfFailed(device->CreateRenderTargetView(texCubemapArrayCopy->resource.get(), &rtvDesc, &layerCopyRTVs[layer][face]));
			}
		}
	}

	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;

		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &scatterBlendState));
	}
}

void SkyScattering::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& feature = globals::features::skyScattering;
	feature.ModifySky(Pass);
	func(This, Pass, RenderFlags);
	feature.SkyShaderHacks();
}
