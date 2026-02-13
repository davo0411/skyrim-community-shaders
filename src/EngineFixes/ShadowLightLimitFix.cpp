#include "ShadowLightLimitFix.h"

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
	//
	// Format: R8_UNORM — shadow factor is a scalar 0.0–1.0 occlusion value.
	// Memory: width × height × kMaxShadowLights bytes
	//   At 1920×1080 × 8 slices = ~16 MB

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

	// SRV for the full array (sampling in lighting shaders at t48)
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = kMaxShadowLights;

	DX::ThrowIfFailed(device->CreateShaderResourceView(
		shadowMaskArray.get(), &srvDesc, shadowMaskArraySRV.put()));

	// Per-slice RTVs (for writing shadow mask to individual slices)
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

	logger::info("[ShadowLightLimitFix] Created extended shadow mask: {}x{} x {} slices",
		width, height, kMaxShadowLights);
}

void ShadowLightLimitFix::ReleaseResources()
{
	shadowMaskArraySRV = nullptr;
	for (auto& rtv : shadowMaskSliceRTVs)
		rtv = nullptr;
	shadowMaskArray = nullptr;
}

// =============================================================================
// Core Logic: Approach C3 — Hybrid Detour
// =============================================================================

void ShadowLightLimitFix::AccumulateAndRenderExtendedLights()
{
	// Called after vanilla Main_RenderShadowMaps completes.
	// At this point, the engine has:
	//   - Accumulated 4 shadow lights (maskIndex 0–3 assigned)
	//   - Rendered their shadow depth maps
	//   - Lights with maskIndex == 255 were skipped
	//
	// We now process the overflow lights.

	auto smState = globals::game::smState;
	auto* ssn = smState->shadowSceneNode[0];
	if (!ssn)
		return;

	auto& shadowLights = ssn->GetRuntimeData().activeShadowLights;

	extendedLights.clear();
	extendedShadowCount = 0;

	uint32_t extendedChannel = kVanillaMaxShadowLights;

	for (auto& lightPtr : shadowLights) {
		if (extendedChannel >= kMaxShadowLights)
			break;

		auto* bsLight = lightPtr.get();
		if (!bsLight || !bsLight->IsShadowLight())
			continue;

		auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
		GET_INSTANCE_MEMBER(maskIndex, shadowLight);

		if (maskIndex != 255)
			continue;  // Already accumulated by vanilla (maskIndex 0–3)

		// This light overflowed the vanilla limit.
		//
		// Strategy:
		//   1. Call Accumulate() to set up internal state (cameras, culling, descriptors)
		//   2. Call Render() to generate the shadow depth map
		//
		// Accumulate() will do: this->maskIndex = extendedChannel++
		// Render() uses the camera/culling state set by Accumulate()
		//
		// Risk note: Accumulate() may have internal assumptions about maskChannel < 4.
		// For point lights (BSShadowParabolicLight), this should be safe because:
		//   - maskIndex is just stored as a uint32, not used to index focusShadowmapDescriptors[4]
		//     (that array is for cascade splits, only used by directional lights)
		//   - The shadowmap descriptor setup uses shadowmapDescriptors (a BSTArray, dynamic)

		uint32_t dummyGlobalCount = extendedChannel;
		RE::NiPointer<RE::NiAVObject> scene(ssn);

		shadowLight->Accumulate(dummyGlobalCount, extendedChannel, scene);
		shadowLight->Render();

		extendedLights.push_back({ shadowLight, extendedChannel - 1 });
		extendedShadowCount++;

		logger::trace("[ShadowLightLimitFix] Extended shadow light: slot {}", extendedChannel - 1);
	}

	if (extendedShadowCount > 0) {
		logger::trace("[ShadowLightLimitFix] Accumulated {} extended shadow lights this frame",
			extendedShadowCount);
	}
}

void ShadowLightLimitFix::ClearExtendedSlices()
{
	auto context = globals::d3d::context;
	float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // 1.0 = fully lit (no shadow)

	for (uint32_t i = kVanillaMaxShadowLights; i < kMaxShadowLights; i++) {
		context->ClearRenderTargetView(shadowMaskSliceRTVs[i].get(), clearColor);
	}
}

void ShadowLightLimitFix::BeginExtendedShadowMaskPass(uint32_t maskIndex)
{
	if (maskIndex < kVanillaMaxShadowLights)
		return;  // Vanilla handles slots 0–3

	auto context = globals::d3d::context;

	// Swap render target from the vanilla RGBA shadow mask to our extended array slice.
	// The BSUtilityShader will write its shadow comparison result to .r of our slice
	// instead of to an RGBA channel of the vanilla RT.
	auto* sliceRTV = shadowMaskSliceRTVs[maskIndex].get();

	// Get current DSV (depth stencil) — we need to keep it
	ID3D11RenderTargetView* currentRT = nullptr;
	ID3D11DepthStencilView* currentDSV = nullptr;
	context->OMGetRenderTargets(1, &currentRT, &currentDSV);

	context->OMSetRenderTargets(1, &sliceRTV, currentDSV);

	if (currentRT)
		currentRT->Release();
	if (currentDSV)
		currentDSV->Release();
}

void ShadowLightLimitFix::EndExtendedShadowMaskPass(uint32_t maskIndex)
{
	if (maskIndex < kVanillaMaxShadowLights)
		return;

	// RT will be restored by the engine's normal flow at the end of the shadowmask
	// phase — or we could save/restore here if needed for safety.
}

void ShadowLightLimitFix::BindExtendedShadowMaskSRV()
{
	auto context = globals::d3d::context;
	auto* srv = shadowMaskArraySRV.get();
	context->PSSetShaderResources(48, 1, &srv);  // t48 in lighting shaders
}

// =============================================================================
// Installation
// =============================================================================

void ShadowLightLimitFix::Install()
{
	// Approach C3 leverages EXISTING Community Shaders hooks:
	//
	// 1. Main_RenderShadowMaps (Deferred.cpp)
	//    Already hooked. We call AccumulateAndRenderExtendedLights() from its thunk.
	//
	// 2. BSUtilityShader dispatch (State.cpp, CopyShadowData path)
	//    Already hooked. We intercept shadowmask renders for maskIndex >= 4.
	//
	// 3. LLF Prepass (LightLimitFix.cpp)
	//    Already runs. We call BindExtendedShadowMaskSRV() there.
	//
	// NO NEW RELOCATION IDS NEEDED for the core integration.
	// The only new hooks are thin wrappers around existing hook points.

	logger::info("[ShadowLightLimitFix] Installed (max {} shadow lights, using Approach C3 hybrid detour)",
		kMaxShadowLights);
}
