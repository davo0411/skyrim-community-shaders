#include "ShadowLightLimitFix.h"

#include "ShaderCache.h"
#include "State.h"

// =============================================================================
// Resource Setup
// =============================================================================

void ShadowLightLimitFix::SetupResources()
{
	auto device = globals::d3d::device;
	auto state = globals::state;

	auto screenSize = state->screenSize;
	uint32_t width = static_cast<uint32_t>(screenSize.x);
	uint32_t height = static_cast<uint32_t>(screenSize.y);

	// Respect iShadowMaskQuarter (quarter-resolution shadow mask)
	if (auto* setting = RE::GetINISetting("iShadowMaskQuarter:Display")) {
		if (setting->data.i > 0) {
			width /= 2;
			height /= 2;
		}
	}

	// Create extended shadow mask Texture2DArray
	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = kMaxShadowLights;
	texDesc.Format = DXGI_FORMAT_R8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = 0;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, shadowMaskArray.put()));

	// SRV for the full array (t48 in lighting shaders)
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = kMaxShadowLights;

	DX::ThrowIfFailed(device->CreateShaderResourceView(
		shadowMaskArray.get(), &srvDesc, shadowMaskArraySRV.put()));

	// Per-slice RTVs
	for (uint32_t i = 0; i < kMaxShadowLights; i++) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = DXGI_FORMAT_R8_UNORM;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Texture2DArray.MipSlice = 0;
		rtvDesc.Texture2DArray.FirstArraySlice = i;
		rtvDesc.Texture2DArray.ArraySize = 1;

		DX::ThrowIfFailed(device->CreateRenderTargetView(
			shadowMaskArray.get(), &rtvDesc, shadowMaskSliceRTVs[i].put()));
	}

	// Pre-create blend state for writing to R8_UNORM extended slices.
	// The BSUtilityShader shadow mask techniques write psout.Color.xyzw = value,
	// but we only want to capture the R channel into our R8 texture.
	D3D11_BLEND_DESC blendDesc{};
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.IndependentBlendEnable = FALSE;
	blendDesc.RenderTarget[0].BlendEnable = FALSE;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;

	DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, shadowMaskWriteBlendState.put()));

	resourcesSetup = true;

	logger::info("[ShadowLightLimitFix] Created extended shadow mask: {}x{} x {} slices",
		width, height, kMaxShadowLights);
}

void ShadowLightLimitFix::ReleaseResources()
{
	shadowMaskArraySRV = nullptr;
	for (auto& rtv : shadowMaskSliceRTVs)
		rtv = nullptr;
	shadowMaskArray = nullptr;
	shadowMaskWriteBlendState = nullptr;
	resourcesSetup = false;
}

// =============================================================================
// Core Logic: Approach C3 — Hybrid Detour
// =============================================================================

void ShadowLightLimitFix::AccumulateAndRenderExtendedLights()
{
	if (!resourcesSetup)
		return;

	auto smState = globals::game::smState;
	if (!smState)
		return;

	auto* ssn = smState->shadowSceneNode[0];
	if (!ssn)
		return;

	auto& shadowLights = ssn->GetRuntimeData().activeShadowLights;

	extendedLights.clear();
	extendedShadowCount = 0;

	// Record how many lights the engine accumulated (before our additions).
	// Phase 3 will iterate shadowLightsAccum — if Accumulate() adds to it,
	// we need to know which entries are ours so we can redirect their output.
	vanillaAccumCount = static_cast<uint32_t>(ssn->GetRuntimeData().shadowLightsAccum.size());

	uint32_t extendedChannel = kVanillaMaxShadowLights;

	for (auto& lightPtr : shadowLights) {
		if (extendedChannel >= kMaxShadowLights)
			break;

		auto* shadowLight = lightPtr.get();
		if (!shadowLight)
			continue;

		GET_INSTANCE_MEMBER(maskIndex, shadowLight);

		if (maskIndex != 255)
			continue;  // Already accumulated by vanilla (maskIndex 0–3)

		// Use the light's scene graph index for correct culling scene
		GET_INSTANCE_MEMBER(sceneGraphIndex, shadowLight);
		auto* lightSSN = smState->shadowSceneNode[sceneGraphIndex];
		if (!lightSSN)
			lightSSN = ssn;

		uint32_t globalCount = extendedChannel;
		RE::NiPointer<RE::NiAVObject> scene(lightSSN);

		shadowLight->Accumulate(globalCount, extendedChannel, scene);
		shadowLight->Render();

		extendedLights.push_back({ shadowLight, extendedChannel - 1 });
		extendedShadowCount++;

		logger::trace("[ShadowLightLimitFix] Extended shadow light: slot {}", extendedChannel - 1);
	}

	if (extendedShadowCount > 0) {
		ClearExtendedSlices();

		logger::trace("[ShadowLightLimitFix] Accumulated {} extended shadow lights this frame",
			extendedShadowCount);
	}
}

void ShadowLightLimitFix::ClearExtendedSlices()
{
	if (!resourcesSetup)
		return;

	auto context = globals::d3d::context;
	float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

	for (uint32_t i = kVanillaMaxShadowLights; i < kMaxShadowLights; i++) {
		if (shadowMaskSliceRTVs[i])
			context->ClearRenderTargetView(shadowMaskSliceRTVs[i].get(), clearColor);
	}
}

// =============================================================================
// Shadow Mask Phase Interception
// =============================================================================

void ShadowLightLimitFix::HandleShadowMaskDraw(uint32_t pixelDescriptor)
{
	if (!resourcesSetup || !extendedShadowCount)
		return;

	using UFlags = SIE::ShaderCache::UtilityShaderFlags;

	// Restore state from the previous redirected draw (if any)
	RestorePreviousRedirect();

	// Only track point/spot shadow mask draws (not directional).
	// The directional light (RenderShadowmask, bit 21) always uses slot 0.
	// Point/spot lights use RenderShadowmaskSpot/Pb/Dpb (bits 22-24).
	static constexpr uint32_t kPointSpotMask =
		static_cast<uint32_t>(UFlags::RenderShadowmaskSpot) |
		static_cast<uint32_t>(UFlags::RenderShadowmaskPb) |
		static_cast<uint32_t>(UFlags::RenderShadowmaskDpb);

	if (!(pixelDescriptor & kPointSpotMask))
		return;

	// Each point/spot shadow mask draw corresponds to one light in shadowLightsAccum
	// (after the directional light at index 0).
	// Draws 0..(vanillaPointCount-1) are vanilla lights; subsequent draws are ours.
	uint32_t vanillaPointCount = (vanillaAccumCount > 0) ? vanillaAccumCount - 1 : 0;

	if (shadowMaskPointDrawIndex >= vanillaPointCount) {
		// This draw is for one of our extended lights
		uint32_t extIndex = shadowMaskPointDrawIndex - vanillaPointCount;
		if (extIndex < extendedLights.size()) {
			uint32_t slotIndex = extendedLights[extIndex].assignedSlot;
			BeginExtendedShadowMaskPass(slotIndex);
		}
	}

	shadowMaskPointDrawIndex++;
}

void ShadowLightLimitFix::RestorePreviousRedirect()
{
	if (!currentlyRedirected)
		return;

	auto context = globals::d3d::context;

	// Restore the original render target
	if (savedRT || savedDSV) {
		context->OMSetRenderTargets(1, &savedRT, savedDSV);
		if (savedRT) savedRT->Release();
		if (savedDSV) savedDSV->Release();
		savedRT = nullptr;
		savedDSV = nullptr;
	}

	// Restore the original blend state
	if (savedBlendState) {
		context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
		savedBlendState->Release();
		savedBlendState = nullptr;
	}

	currentlyRedirected = false;
}

void ShadowLightLimitFix::BeginExtendedShadowMaskPass(uint32_t maskIndex)
{
	if (!resourcesSetup)
		return;

	if (maskIndex < kVanillaMaxShadowLights || maskIndex >= kMaxShadowLights)
		return;

	auto context = globals::d3d::context;

	// Save current render target
	savedRT = nullptr;
	savedDSV = nullptr;
	context->OMGetRenderTargets(1, &savedRT, &savedDSV);

	// Redirect to our extended array slice
	auto* sliceRTV = shadowMaskSliceRTVs[maskIndex].get();
	context->OMSetRenderTargets(1, &sliceRTV, savedDSV);

	// Save and override blend state.
	// The engine's blend state for maskIndex >= 4 has an invalid write mask
	// (1 << maskIndex overflows the 4-bit RGBA write mask range).
	// We override to write to R channel of our R8_UNORM slice.
	savedBlendState = nullptr;
	context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);
	float defaultFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->OMSetBlendState(shadowMaskWriteBlendState.get(), defaultFactor, 0xFFFFFFFF);

	currentlyRedirected = true;
}

void ShadowLightLimitFix::EndExtendedShadowMaskPass(uint32_t maskIndex)
{
	RestorePreviousRedirect();
}

void ShadowLightLimitFix::ResetFrameState()
{
	// Ensure any lingering redirect is cleaned up
	RestorePreviousRedirect();

	shadowMaskPointDrawIndex = 0;
	extendedShadowCount = 0;
	extendedLights.clear();
	vanillaAccumCount = 0;
}

void ShadowLightLimitFix::BindExtendedShadowMaskSRV()
{
	if (!resourcesSetup || !extendedShadowCount)
		return;

	auto context = globals::d3d::context;
	auto* srv = shadowMaskArraySRV.get();
	context->PSSetShaderResources(48, 1, &srv);
}

// =============================================================================
// Installation
// =============================================================================

void ShadowLightLimitFix::Install()
{
	logger::info("[ShadowLightLimitFix] Installed (max {} shadow lights, using Approach C3 hybrid detour)",
		kMaxShadowLights);
}
