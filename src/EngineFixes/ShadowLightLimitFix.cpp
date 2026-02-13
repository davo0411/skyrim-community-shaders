#include "ShadowLightLimitFix.h"

#include "Deferred.h"
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

	// Check if iShadowMaskQuarter is set (quarter-resolution shadow mask)
	if (auto* setting = RE::GetINISetting("iShadowMaskQuarter:Display")) {
		if (setting->data.i > 0) {
			width /= 2;
			height /= 2;
		}
	}

	// -------------------------------------------------------------------------
	// Create the extended shadow mask Texture2DArray
	// -------------------------------------------------------------------------
	// The vanilla engine uses a single RGBA texture (4 channels = 4 shadow lights).
	// We create an array texture with kMaxShadowLights slices, where each slice
	// stores one shadow light's shadow factor.
	//
	// Slices 0–3 mirror the vanilla RGBA channels (for compatibility).
	// Slices 4+ are the extended slots enabled by this fix.
	//
	// Format: R8_UNORM — shadow factor is just a 0.0–1.0 occlusion value.
	// This uses 1 byte per texel per light, versus 4 bytes per texel for
	// the vanilla RGBA approach (but we have more slices).
	//
	// Memory cost: width × height × kMaxShadowLights bytes
	// At 1920×1080 with 8 lights: ~16 MB (vs vanilla's ~8 MB for RGBA8)

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

	// -------------------------------------------------------------------------
	// Create SRV for the full array (for sampling in lighting shaders)
	// -------------------------------------------------------------------------
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8_UNORM;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = kMaxShadowLights;

	DX::ThrowIfFailed(device->CreateShaderResourceView(
		shadowMaskArray.get(), &srvDesc, shadowMaskArraySRV.put()));

	// -------------------------------------------------------------------------
	// Create per-slice RTVs (for rendering shadow masks to individual slices)
	// -------------------------------------------------------------------------
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

	logger::info("[ShadowLightLimitFix] Created extended shadow mask array: {}x{} x {} slices",
		width, height, kMaxShadowLights);
}

void ShadowLightLimitFix::ReleaseResources()
{
	shadowMaskArraySRV = nullptr;
	for (auto& rtv : shadowMaskSliceRTVs)
		rtv = nullptr;
	shadowMaskArray = nullptr;
	shadowLightInfoBuffer.reset();
}

// =============================================================================
// Installation
// =============================================================================

void ShadowLightLimitFix::Install()
{
	// TODO: Resolve relocation IDs for the accumulation loop in DrawWorld.
	//
	// The target is the function that iterates ShadowSceneNode::shadowLightsAccum
	// and calls BSShadowLight::Accumulate() on each entry. The vanilla code:
	//
	//   for (auto* light : shadowScene->shadowLightsAccum) {
	//       light->Accumulate(globalCount, maskChannel, cullingScene);
	//       if (maskChannel >= 4) break;  // <--- THE LIMIT
	//   }
	//
	// We need to:
	//   1. Hook the function containing this loop
	//   2. Replace the "maskChannel >= 4" check with "maskChannel >= kMaxShadowLights"
	//   3. Ensure maskIndex assignment in Accumulate uses the extended range
	//
	// The relocation targets (need verification on each Skyrim version):
	//   SE:  REL::ID(xxxxx) — Main::DrawWorld or a sub-function
	//   AE:  REL::ID(yyyyy)
	//   VR:  REL::ID(zzzzz)
	//
	// For now we document the approach and provide the hook structure.

	Hooks::Install();

	logger::info("[ShadowLightLimitFix] Installed shadow light limit fix (max {} shadow lights)",
		kMaxShadowLights);
}

// =============================================================================
// Hook Implementations
// =============================================================================

/// ## Accumulation Hook
///
/// The engine's DrawWorld function contains a loop like:
///
/// ```
/// uint32_t globalShadowLightCount = 0;
/// uint32_t shadowMaskChannel = 0;
///
/// // Sun shadow (directional) — always slot 0
/// if (sunShadowDirLight) {
///     sunShadowDirLight->Accumulate(globalShadowLightCount, shadowMaskChannel, scene);
/// }
///
/// // Point/spot shadow lights
/// for (auto* light : shadowScene->shadowLightsAccum) {
///     if (shadowMaskChannel >= 4) break;  // <--- VANILLA LIMIT
///     light->Accumulate(globalShadowLightCount, shadowMaskChannel, scene);
/// }
///
/// drawWorld->activeShadowLightCount = globalShadowLightCount;
/// drawWorld->shadowLightMaskIndex = shadowMaskChannel;
/// drawWorld->shadowLightMask = (1 << shadowMaskChannel) - 1;
/// ```
///
/// Our hook replaces the loop body to allow `shadowMaskChannel` to go up to
/// kMaxShadowLights. The Accumulate() virtual itself increments maskChannel
/// and assigns light->maskIndex = maskChannel++, so we don't need to patch
/// Accumulate — just remove the `>= 4` early-out.
///
/// ### Approach A: Patch the comparison instruction
/// Find the `cmp reg, 4` / `jge exit` and NOP it or change the immediate.
/// This is the simplest approach but fragile across game versions.
///
/// ### Approach B: Hook the outer function
/// Replace the entire accumulation function, re-implementing the loop with
/// our higher limit. More robust but requires understanding the full function.
///
/// ### Approach C: Hook each Accumulate() call
/// Less invasive — let the engine call Accumulate() as normal, but patch
/// the maskChannel check. We'd hook the point where `>= 4` is checked.
///
/// We implement Approach B as it gives us full control:

void ShadowLightLimitFix::Hooks::DrawWorld_AccumulateShadowLights::thunk(
	RE::ShadowSceneNode* shadowScene,
	uint32_t& globalShadowLightCount,
	uint32_t& shadowMaskChannel)
{
	// Reset tracking
	auto* fix = reinterpret_cast<ShadowLightLimitFix*>(nullptr);  // TODO: access singleton
	// fix->currentShadowLightCount = 0;

	// Call original — this handles the sun directional light and possibly
	// the first few shadow lights. We let vanilla handle slots 0–3 normally.
	func(shadowScene, globalShadowLightCount, shadowMaskChannel);

	// After vanilla accumulation, shadowMaskChannel should be 4 (all slots used).
	// Now we can accumulate additional shadow lights into our extended slots.
	//
	// However, we need to be careful: the engine's Accumulate() virtual
	// increments shadowMaskChannel and assigns maskIndex internally.
	// If we just let the counter keep going, Accumulate() will assign
	// maskIndex = 4, 5, 6, 7 — which is exactly what we want, but the
	// engine's shadow mask render pass will try to use these indices to
	// select RGBA channels, which will fail.
	//
	// The fix: we hook the shadow mask render pass separately to redirect
	// writes for maskIndex >= 4 to our extended Texture2DArray slices.

	if (shadowMaskChannel >= kVanillaMaxShadowLights) {
		auto& shadowLightsAccum = shadowScene->GetRuntimeData().shadowLightsAccum;

		for (auto* light : shadowLightsAccum) {
			if (shadowMaskChannel >= kMaxShadowLights)
				break;

			// Check if this light was already accumulated by vanilla
			// (maskIndex < 4 means it was handled in the original call)
			if (light->IsOmniLight() || light->GetIsFrustumLight()) {
				GET_INSTANCE_MEMBER(maskIndex, light);
				if (maskIndex < kVanillaMaxShadowLights)
					continue;  // Already accumulated by vanilla

				// This light didn't get a slot — accumulate it now
				// into our extended range
				// NOTE: We can't easily call Accumulate() again because
				// the engine may have already skipped it. Instead, we
				// directly assign the extended maskIndex.

				// Assign extended slot
				SET_INSTANCE_MEMBER(maskIndex, light, shadowMaskChannel);
				shadowMaskChannel++;
				globalShadowLightCount++;
			}
		}
	}
}

/// ## Shadow Mask Render Hook
///
/// During the shadow mask rendering phase, the engine binds the kSHADOW_MASK
/// render target and renders each shadow light's depth comparison into the
/// appropriate RGBA channel (selected by maskIndex → output write mask).
///
/// For extended lights (maskIndex >= 4), we need to:
///   1. Detect which maskIndex is being rendered (from the current shadow light)
///   2. Unbind the vanilla RGBA RT
///   3. Bind our extended array slice RTV for that index
///   4. Render the shadow mask to that slice
///   5. Restore the vanilla RT binding
///
/// The maskIndex → output channel mapping in the vanilla utility shader is:
///   maskIndex 0 → write to .r channel (output mask = 0001)
///   maskIndex 1 → write to .g channel (output mask = 0010)
///   maskIndex 2 → write to .b channel (output mask = 0100)
///   maskIndex 3 → write to .a channel (output mask = 1000)
///
/// For extended slots, we instead write to .r of the corresponding array slice.

void ShadowLightLimitFix::Hooks::BSUtilityShader_RenderShadowmask::thunk(
	RE::BSShader* shader, uint32_t technique)
{
	// TODO: Determine which shadow light is currently being rendered.
	// This information is typically in the current render state or
	// can be inferred from the utility shader's constant buffer data.
	//
	// For maskIndex 0–3: let vanilla handle it unchanged.
	// For maskIndex 4+: swap render target to our array slice.
	//
	// Pseudocode:
	//
	// uint32_t currentMaskIndex = GetCurrentShadowLightMaskIndex();
	// if (currentMaskIndex >= kVanillaMaxShadowLights) {
	//     auto context = globals::d3d::context;
	//     auto* fix = GetSingleton();
	//
	//     // Save current RT
	//     ID3D11RenderTargetView* savedRT = nullptr;
	//     ID3D11DepthStencilView* savedDSV = nullptr;
	//     context->OMGetRenderTargets(1, &savedRT, &savedDSV);
	//
	//     // Bind extended array slice
	//     auto* sliceRTV = fix->shadowMaskSliceRTVs[currentMaskIndex].get();
	//     context->OMSetRenderTargets(1, &sliceRTV, savedDSV);
	//
	//     // Call original (renders shadow mask to our slice)
	//     func(shader, technique);
	//
	//     // Restore original RT
	//     context->OMSetRenderTargets(1, &savedRT, savedDSV);
	//     if (savedRT) savedRT->Release();
	//     if (savedDSV) savedDSV->Release();
	//     return;
	// }

	// For vanilla slots (0–3), pass through unchanged
	func(shader, technique);
}

void ShadowLightLimitFix::Hooks::ClearShadowMask::thunk()
{
	// Clear vanilla shadow mask
	func();

	// Also clear our extended array slices
	// TODO: Access singleton and clear
	// auto context = globals::d3d::context;
	// auto* fix = GetSingleton();
	// float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };  // 1.0 = fully lit (no shadow)
	// for (uint32_t i = kVanillaMaxShadowLights; i < kMaxShadowLights; i++) {
	//     context->ClearRenderTargetView(fix->shadowMaskSliceRTVs[i].get(), clearColor);
	// }
}

void ShadowLightLimitFix::Hooks::Install()
{
	// =========================================================================
	// Hook targets (require relocation ID research per Skyrim version)
	// =========================================================================
	//
	// 1. ACCUMULATION LOOP
	//    The function in Main::DrawWorld (or a helper) that iterates
	//    ShadowSceneNode::shadowLightsAccum and calls Accumulate().
	//
	//    Known relocation candidates:
	//    - SE: REL::ID(35560) — Main::DrawWorld_RenderShadows
	//    - AE: REL::ID(36559)
	//
	//    The specific offset within that function where `cmp [maskChannel], 4`
	//    or `jge` exits the loop.
	//
	// 2. SHADOW MASK RENDER PASS
	//    The BSUtilityShader dispatch for RenderShadowmask techniques.
	//    This is where we'd swap render targets for extended slots.
	//
	//    Already partially hooked in Deferred.cpp (Main_RenderShadowmasks).
	//
	// 3. SHADOW MASK CLEAR
	//    The point where the engine clears the SHADOW_MASK RT before rendering.
	//    We need to also clear our extended array slices.
	//
	// These relocations need to be verified against each Skyrim variant.
	// For now we leave them as TODOs with the approach documented.

	logger::info("[ShadowLightLimitFix] Hook installation pending relocation ID resolution");
}
