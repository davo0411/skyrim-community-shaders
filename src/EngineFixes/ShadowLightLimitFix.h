#pragma once

#include "Buffer.h"

/// Engine fix to raise Skyrim's hardcoded 4-shadow-casting-light limit.
///
/// ## Strategy: Hybrid Detour (Approach C3)
///
/// Instead of hooking the engine's accumulation loop directly, we let vanilla
/// handle its 4 shadow lights normally, then extend by calling Accumulate()
/// and Render() ourselves on the overflow lights. This leverages existing
/// Community Shaders hooks (Main_RenderShadowMaps, BSUtilityShader dispatch)
/// and avoids needing to find/patch the accumulation loop comparison.
///
/// ## Pipeline Integration Points
///
/// The engine's shadow pipeline runs in strict sequential phases:
///
///   Phase 1: ACCUMULATE ALL  → assigns maskIndex 0–3, exits at >= 4
///   Phase 2: RENDER ALL SHADOW MAPS → BSShadowLight::Render() per light
///   Phase 3: RENDER ALL SHADOW MASKS → BSUtilityShader writes to RGBA
///   Phase 4: LIGHTING → pixel shaders read shadow mask
///
/// We inject AFTER Phase 2 (using the existing Main_RenderShadowMaps hook)
/// to accumulate+render overflow lights, then intercept Phase 3 to redirect
/// their shadow mask output to our extended Texture2DArray.
///
/// ## Key Engine Details
///
///   - maskIndex == 255 (0xFF) is the engine sentinel for "not accumulated"
///   - LLF already filters these out: `if (light.shadowMaskIndex != 255)`
///   - activeShadowLights contains ALL shadow-capable lights (accumulated or not)
///   - shadowLightsAccum contains only those the engine chose to accumulate
///   - Accumulate() is a virtual: assigns maskIndex, sets up cameras/culling
///   - Render() uses the state from Accumulate() to generate depth maps
///
struct ShadowLightLimitFix : EngineFix
{
	static ShadowLightLimitFix* GetSingleton()
	{
		static ShadowLightLimitFix singleton;
		return &singleton;
	}

	std::string GetName() override { return "Shadow Light Limit Fix"; }

	void Install() override;

	/// Maximum simultaneous shadow-casting lights (including the sun).
	/// Default 8 = 1 directional + 7 point/spot.
	/// Vanilla limit is 4 = 1 directional + 3 point/spot.
	static constexpr uint32_t kMaxShadowLights = 8;
	static constexpr uint32_t kVanillaMaxShadowLights = 4;

	// =========================================================================
	// Extended Shadow Mask Resources
	// =========================================================================

	/// Texture2DArray: screenW × screenH × kMaxShadowLights slices.
	/// Each slice stores one shadow light's shadow factor (R8_UNORM).
	/// Slices 4+ are written directly by our extended shadow mask passes.
	winrt::com_ptr<ID3D11Texture2D> shadowMaskArray;
	winrt::com_ptr<ID3D11ShaderResourceView> shadowMaskArraySRV;
	winrt::com_ptr<ID3D11RenderTargetView> shadowMaskSliceRTVs[kMaxShadowLights];

	/// Saved render target to restore after an extended shadow mask pass.
	ID3D11RenderTargetView* savedRT = nullptr;
	ID3D11DepthStencilView* savedDSV = nullptr;

	bool resourcesSetup = false;

	// =========================================================================
	// Extended Shadow Light Tracking
	// =========================================================================

	struct ExtendedShadowLight
	{
		RE::BSShadowLight* light = nullptr;
		uint32_t assignedSlot = UINT32_MAX;
	};

	/// Lights that overflow vanilla's 4-slot limit this frame.
	/// Populated after Main_RenderShadowMaps, consumed during shadow mask pass.
	eastl::vector<ExtendedShadowLight> extendedLights;

	/// How many extended shadow lights were rendered this frame.
	uint32_t extendedShadowCount = 0;

	// =========================================================================
	// Shadow Mask Phase Tracking
	// =========================================================================

	/// Number of lights in shadowLightsAccum before we added our extended ones.
	/// Used to detect which shadow mask draws are for extended lights.
	uint32_t vanillaAccumCount = 0;

	/// Per-frame counter tracking which shadow mask draw call we're on
	/// (point/spot lights only — directional is always vanilla slot 0).
	uint32_t shadowMaskPointDrawIndex = 0;

	/// True if we're currently rendering to an extended slice (need to restore).
	bool currentlyRedirected = false;

	/// Pre-created blend state that writes to R channel only.
	/// Used when rendering shadow masks to our R8_UNORM extended slices.
	winrt::com_ptr<ID3D11BlendState> shadowMaskWriteBlendState;

	/// Saved blend state to restore after extended shadow mask pass.
	ID3D11BlendState* savedBlendState = nullptr;
	float savedBlendFactor[4] = {};
	UINT savedSampleMask = 0xFFFFFFFF;

	// =========================================================================
	// Methods
	// =========================================================================

	void SetupResources();
	void ReleaseResources();

	/// Called after vanilla Main_RenderShadowMaps completes.
	/// Scans activeShadowLights for overflow lights (maskIndex == 255),
	/// calls Accumulate() + Render() to generate their shadow maps.
	void AccumulateAndRenderExtendedLights();

	/// Called during the shadow mask phase (from State::Draw).
	/// For maskIndex >= 4, redirects the BSUtilityShader output to our
	/// Texture2DArray slices instead of the vanilla RGBA shadow mask.
	void BeginExtendedShadowMaskPass(uint32_t maskIndex);
	void EndExtendedShadowMaskPass(uint32_t maskIndex);

	/// Called from State::Draw for each shadow mask draw call.
	/// Detects whether the current draw is for an extended light and redirects RT.
	void HandleShadowMaskDraw(uint32_t pixelDescriptor);

	/// Restores RT/blend state if we redirected in a previous draw call.
	void RestorePreviousRedirect();

	/// Called at frame end (from State::Reset) to reset per-frame tracking.
	void ResetFrameState();

	/// Clears extended shadow mask slices to 1.0 (fully lit) before rendering.
	void ClearExtendedSlices();

	/// Binds the extended shadow mask SRV (t48) for lighting shaders.
	void BindExtendedShadowMaskSRV();

	/// Returns true if any extended shadow lights were rendered this frame.
	bool HasExtendedShadows() const { return extendedShadowCount > 0; }
};
