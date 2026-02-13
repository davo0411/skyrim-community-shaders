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
	/// Slices 0–3 are populated by copying from the vanilla RGBA shadow mask.
	/// Slices 4+ are written directly by our extended shadow mask passes.
	winrt::com_ptr<ID3D11Texture2D> shadowMaskArray;
	winrt::com_ptr<ID3D11ShaderResourceView> shadowMaskArraySRV;
	winrt::com_ptr<ID3D11RenderTargetView> shadowMaskSliceRTVs[kMaxShadowLights];

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
	// Methods
	// =========================================================================

	void SetupResources();
	void ReleaseResources();

	/// Called after vanilla Main_RenderShadowMaps completes.
	/// Scans activeShadowLights for overflow lights (maskIndex == 255),
	/// calls Accumulate() + Render() to generate their shadow maps.
	void AccumulateAndRenderExtendedLights();

	/// Called during the shadow mask phase.
	/// For maskIndex >= 4, redirects the BSUtilityShader output to our
	/// Texture2DArray slices instead of the vanilla RGBA shadow mask.
	void BeginExtendedShadowMaskPass(uint32_t maskIndex);
	void EndExtendedShadowMaskPass(uint32_t maskIndex);

	/// Clears extended shadow mask slices to 1.0 (fully lit) before rendering.
	void ClearExtendedSlices();

	/// Binds the extended shadow mask SRV (t48) for lighting shaders.
	void BindExtendedShadowMaskSRV();
};
