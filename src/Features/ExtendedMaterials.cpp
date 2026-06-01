#include "ExtendedMaterials.h"

#include <algorithm>

#include "Deferred.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ExtendedMaterials::Settings,
	EnableComplexMaterial,
	EnableParallax,
	EnableTerrain,
	EnableHeightBlending,
	EnableShadows,
	EnableParallaxWarpingFix,
	EnableSilhouette,
	SilhouetteScale)

void ExtendedMaterials::DataLoaded()
{
	if (&settings.EnableTerrain) {
		if (auto bLandSpecular = globals::game::iniSettingCollection->GetSetting("bLandSpecular:Landscape"); bLandSpecular) {
			if (!bLandSpecular->data.b) {
				logger::info("[CPM] Changing bLandSpecular from {} to {} to support Terrain Parallax", bLandSpecular->data.b, true);
				bLandSpecular->data.b = true;
			}
		}
	}
}

void ExtendedMaterials::DrawSettings()
{
	if (ImGui::TreeNodeEx("Complex Material", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Complex Material", (bool*)&settings.EnableComplexMaterial);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Enables support for the Complex Material specification which makes use of the environment mask. "
				"This includes parallax, as well as more realistic metals and specular reflections. "
				"May lead to some warped textures on modded content which have an invalid alpha channel in their environment mask. ");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Parallax", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Enable Parallax", (bool*)&settings.EnableParallax)) {
			// SilhouetteActive() gates SSDM in the composite compile; stale SSDM + no MainCopy caused grey.
			globals::deferred->ClearShaderCache();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables parallax on standard meshes made for parallax.");
		}

		if (ImGui::Checkbox("Enable Legacy Terrain", (bool*)&settings.EnableTerrain)) {
			if (settings.EnableTerrain) {
				DataLoaded();
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Enables terrain parallax using the alpha channel of each landscape texture. "
				"Therefore, all landscape textures must support parallax for this effect to work properly. ");
		}
		ImGui::Checkbox("Enable Terrain Height Blending", (bool*)&settings.EnableHeightBlending);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables landscape texture blending based on parallax. ");
		}
		ImGui::Checkbox("Enable Parallax Warping Fix", (bool*)&settings.EnableParallaxWarpingFix);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables a fix reducing parallax scale on curved and smooth normal triangles.");
		}

		ImGui::Spacing();

		if (ImGui::Checkbox("Enable Silhouette Extrusion (SSDM)", (bool*)&settings.EnableSilhouette)) {
			// DeferredCompositeCS is built with/without the SSDM define from this flag; a mismatch reads
			// null SRVs (corrupt frame) until the cache is cleared, so force a rebuild on toggle.
			globals::deferred->ClearShaderCache();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Adds screen-space silhouette depth to mesh/terrain SIDES on top of texture-space parallax (Lobel SSDM). "
				"The interior keeps standard parallax occlusion mapping; only the displaced edges are extruded, "
				"so there is no screen-space swimming on flat surfaces.");
		}
		if (settings.EnableSilhouette) {
			ImGui::SliderFloat("Silhouette Depth (x authored)", &settings.SilhouetteScale, 0.0f, 4.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Multiplies the apparent depth of the extruded silhouette. 1 matches the parallax height.");
			}
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Approximate Soft Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Shadows", (bool*)&settings.EnableShadows);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Enables cheap soft shadows when using parallax. "
				"This applies to all directional and point lights. ");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ExtendedMaterials::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ExtendedMaterials::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ExtendedMaterials::RestoreDefaultSettings()
{
	settings = {};
	globals::deferred->ClearShaderCache();
}

bool ExtendedMaterials::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
		return true;
	default:
		return false;
	}
}

void ExtendedMaterials::SetupResources()
{
	auto device = globals::d3d::device;
	auto renderer = globals::game::renderer;
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC mainDesc{};
	mainTex.texture->GetDesc(&mainDesc);

	uint w = mainDesc.Width;
	uint h = mainDesc.Height;

	{
		D3D11_TEXTURE2D_DESC texDesc = {
			.Width = w,
			.Height = h,
			.MipLevels = SSDM_MIP_LEVELS,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		texDisplacement = eastl::make_unique<Texture2D>(texDesc);
		texDisplacement->CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC{
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = SSDM_MIP_LEVELS } });

		CD3D11_RENDER_TARGET_VIEW_DESC rtvDesc(D3D11_RTV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32G32B32A32_FLOAT, 0);
		DX::ThrowIfFailed(device->CreateRenderTargetView(texDisplacement->resource.get(), &rtvDesc, rtvDisplacement.put()));

		for (int i = 0; i < SSDM_MIP_LEVELS; ++i) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUav = {
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = (UINT)i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texDisplacement->resource.get(), &mipUav, uavDisplacement[i].put()));

			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrv = {
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = (UINT)i, .MipLevels = 1 },
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(texDisplacement->resource.get(), &mipSrv, srvDisplacementMip[i].put()));
		}
	}

	{
		D3D11_TEXTURE2D_DESC ssdmDesc = {
			.Width = w,
			.Height = h,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		texSSDM = eastl::make_unique<Texture2D>(ssdmDesc);
		texSSDM->CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC{
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		texSSDM->CreateUAV(D3D11_UNORDERED_ACCESS_VIEW_DESC{
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 } });
	}

	cbufSSDMSolve = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc(sizeof(SSDMSolveCB), true));

	ClearShaderCache();
}

void ExtendedMaterials::ClearShaderCache()
{
	ssdmBuildPyramidCS = nullptr;
	ssdmSolveCS = nullptr;
}

void ExtendedMaterials::RegisterDisplacementRT()
{
	if (!texDisplacement || !rtvDisplacement)
		return;
	auto renderer = globals::game::renderer;
	auto& rt = renderer->GetRuntimeData().renderTargets[SSDM_DISPLACEMENT];
	rt.texture = texDisplacement->resource.get();
	rt.SRV = texDisplacement->srv.get();
	rt.RTV = rtvDisplacement.get();
}

ID3D11ShaderResourceView* ExtendedMaterials::GetSSDMOffsetSRV() const
{
	return (texSSDM && SilhouetteActive()) ? texSSDM->srv.get() : nullptr;
}

ID3D11ShaderResourceView* ExtendedMaterials::GetDisplacementSeedSRV() const
{
	return (texDisplacement && SilhouetteActive()) ? texDisplacement->srv.get() : nullptr;
}

void ExtendedMaterials::ClearDisplacementTexture()
{
	if (!rtvDisplacement)
		return;
	const float clearColor[4] = { 0, 0, 0, 0 };
	globals::d3d::context->ClearRenderTargetView(rtvDisplacement.get(), clearColor);
}

void ExtendedMaterials::CompileSSDMComputeShadersIfNeeded()
{
	const std::vector<std::pair<const char*, const char*>> defines{};

	if (!ssdmBuildPyramidCS || !ssdmSolveCS) {
		ssdmBuildPyramidCS = nullptr;
		ssdmSolveCS = nullptr;

		winrt::com_ptr<ID3D11ComputeShader> pyramid;
		winrt::com_ptr<ID3D11ComputeShader> solve;

		if (auto* raw = Util::CompileShader(L"Data\\Shaders\\ExtendedMaterials\\SSDMBuildPyramidCS.hlsl", defines, "cs_5_0")) {
			pyramid.attach(reinterpret_cast<ID3D11ComputeShader*>(raw));
			Util::SetResourceName(pyramid.get(), "SSDMBuildPyramidCS");
		} else {
			logger::error("[ExtendedMaterials] Failed to compile SSDMBuildPyramidCS.hlsl");
		}
		if (auto* raw = Util::CompileShader(L"Data\\Shaders\\ExtendedMaterials\\SSDMSolveCS.hlsl", defines, "cs_5_0")) {
			solve.attach(reinterpret_cast<ID3D11ComputeShader*>(raw));
			Util::SetResourceName(solve.get(), "SSDMSolveCS");
		} else {
			logger::error("[ExtendedMaterials] Failed to compile SSDMSolveCS.hlsl");
		}

		if (pyramid && solve) {
			ssdmBuildPyramidCS = std::move(pyramid);
			ssdmSolveCS = std::move(solve);
		}
	}
}

void ExtendedMaterials::DrawSSDM()
{
	if (!SilhouetteActive())
		return;
	if (!texDisplacement || !texSSDM || !cbufSSDMSolve)
		return;

	CompileSSDMComputeShadersIfNeeded();
	if (!ssdmBuildPyramidCS || !ssdmSolveCS)
		return;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "SSDM");

	auto context = globals::d3d::context;
	auto* deferred = Deferred::GetSingleton();
	if (!deferred || !deferred->pointSampler)
		return;

	const UINT bufW = texDisplacement->desc.Width;
	const UINT bufH = texDisplacement->desc.Height;
	const float2 dynRes = Util::ConvertToDynamic(float2{ static_cast<float>(bufW), static_cast<float>(bufH) });
	const UINT surfaceW = std::min(bufW, std::max(1u, static_cast<UINT>(std::ceil(static_cast<float>(dynRes.x) - 1e-4f))));
	const UINT surfaceH = std::min(bufH, std::max(1u, static_cast<UINT>(std::ceil(static_cast<float>(dynRes.y) - 1e-4f))));

	const Util::DispatchCount solveDispatch = Util::GetScreenDispatchCount(true);

	const float z[4] = { 0.f, 0.f, 0.f, 0.f };
	context->ClearUnorderedAccessViewFloat(texSSDM->uav.get(), z);

	for (int dstMip = 1; dstMip < SSDM_MIP_LEVELS; ++dstMip) {
		const int srcMip = dstMip - 1;
		ID3D11ShaderResourceView* srcSrv = srvDisplacementMip[srcMip].get();
		context->CSSetShaderResources(0, 1, &srcSrv);
		ID3D11UnorderedAccessView* dstUav = uavDisplacement[dstMip].get();
		context->CSSetUnorderedAccessViews(0, 1, &dstUav, nullptr);
		context->CSSetShader(ssdmBuildPyramidCS.get(), nullptr, 0);
		const UINT mw = std::max(1u, bufW >> dstMip);
		const UINT mh = std::max(1u, bufH >> dstMip);
		context->Dispatch((mw + 7u) / 8u, (mh + 7u) / 8u, 1);
	}

	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

	SSDMSolveCB solveData{};
	solveData.surfaceWidth = static_cast<float>(surfaceW);
	solveData.surfaceHeight = static_cast<float>(surfaceH);
	solveData.bufferWidth = static_cast<float>(bufW);
	solveData.bufferHeight = static_cast<float>(bufH);
	solveData.rcpBufferWidth = bufW ? 1.0f / static_cast<float>(bufW) : 0.0f;
	solveData.rcpBufferHeight = bufH ? 1.0f / static_cast<float>(bufH) : 0.0f;
	solveData.maxStepUv = std::min(0.25f, 32.0f / static_cast<float>(std::max(bufW, bufH)));  // ~32 px/step cap; 0.045 stalled the solve entirely
	solveData.damping = 0.65f;
	solveData.numMips = SSDM_MIP_LEVELS;
	solveData.numIters = 12;
	solveData.pad0 = solveData.pad1 = solveData.pad2 = solveData.pad3 = solveData.pad4 = solveData.pad5 = 0;
	cbufSSDMSolve->Update(solveData);

	ID3D11Buffer* cbSolve = cbufSSDMSolve->CB();
	context->CSSetConstantBuffers(0, 1, &cbSolve);
	ID3D11ShaderResourceView* duvSRV = texDisplacement->srv.get();
	context->CSSetShaderResources(0, 1, &duvSRV);
	ID3D11SamplerState* solveSamplers[] = { deferred->pointSampler };
	context->CSSetSamplers(0, 1, solveSamplers);
	{
		ID3D11UnorderedAccessView* uavSolve = texSSDM->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uavSolve, nullptr);
	}
	context->CSSetShader(ssdmSolveCS.get(), nullptr, 0);
	context->Dispatch(solveDispatch.x, solveDispatch.y, 1);

	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11SamplerState* nullSamps[] = { nullptr };
	context->CSSetSamplers(0, 1, nullSamps);
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
}