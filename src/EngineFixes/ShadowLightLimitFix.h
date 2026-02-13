#pragma once

#include "Buffer.h"

/// Engine fix to raise Skyrim's hardcoded 4-shadow-casting-light limit.
///
/// ## Problem
/// The vanilla engine limits shadow-casting lights to 4 simultaneous casters:
///   - The shadow mask render target (`kSHADOW_MASK`) is a single RGBA texture
///   - Each RGBA channel stores the shadow factor for one `BSShadowLight`
///   - `BSShadowLight::maskIndex` (0–3) selects which channel a light writes to
///   - `DrawWorld::shadowLightMaskIndex` cycles 0→3 during accumulation
///   - The shader reads `shadowColor[ShadowLightMaskSelect[i]]` with i < 4
///
/// In practice: 1 directional (sun) + up to 3 point/spot shadow casters = 4 total.
/// Additional shadow-casting lights are silently discarded, causing visible
/// shadow pop-in/out as the camera moves.
///
/// ## Solution Architecture
///
/// We replace the single RGBA shadow mask with an **extended shadow mask** system:
///
/// ### Phase 1: Shadow Mask Array (this fix)
///   - Create a `Texture2DArray` shadow mask with N slices (default 8)
///   - Each slice is a single-channel (R32_FLOAT or R8_UNORM) texture
///   - Hook the engine's shadow mask render pass to write to array slices
///   - Hook accumulation to allow `maskIndex` values beyond 3
///   - Provide the array to the lighting shader as a new SRV
///
/// ### Phase 2: Shader Integration (LightLimitFix changes)
///   - Replace `shadowColor[light.shadowLightIndex]` with array texture lookup
///   - `ShadowMaskArray.Load(int3(screenPos.xy, light.shadowLightIndex))`
///   - Update `ShadowBitMask` from `uint` (4-bit) to support N bits
///
/// ### Phase 3: Engine Accumulation Override
///   - Hook `BSShadowLight::Accumulate()` (or the caller in DrawWorld)
///   - Replace the `shadowLightMaskIndex` 0–3 cycle with 0–(N-1)
///   - Patch the shadow mask render target binding during utility shader passes
///
/// ## Constraints & Considerations
///   - Each additional shadow light costs a full shadowmap render pass
///   - GPU memory: N × (screenW × screenH × bytesPerTexel) for shadow mask array
///   - The directional light (sun) always occupies slot 0
///   - Must remain compatible with VR (doubled viewports)
///   - Must work with Screen-Space Shadows (which only affects the directional channel)
///   - Must work with the Deferred `CopyShadowData` pipeline
///   - INI setting `iShadowMaskQuarter` affects resolution
///
struct ShadowLightLimitFix : EngineFix
{
	std::string GetName() override { return "Shadow Light Limit Fix"; }

	void Install() override;

	/// Maximum number of simultaneous shadow-casting lights.
	/// Default: 8 (1 directional + 7 point/spot).
	/// The engine's vanilla limit is 4 (RGBA channels).
	static constexpr uint32_t kMaxShadowLights = 8;

	/// The original engine limit, for reference and fallback.
	static constexpr uint32_t kVanillaMaxShadowLights = 4;

	// =========================================================================
	// Extended Shadow Mask Resources
	// =========================================================================

	/// The extended shadow mask texture array.
	/// Dimensions: screenWidth × screenHeight × kMaxShadowLights slices.
	/// Format: DXGI_FORMAT_R8_UNORM (1 byte per texel per shadow light).
	///
	/// This replaces the vanilla RGBA shadow mask render target for shadow
	/// light channels beyond the first 4.
	winrt::com_ptr<ID3D11Texture2D> shadowMaskArray;
	winrt::com_ptr<ID3D11ShaderResourceView> shadowMaskArraySRV;

	/// Per-slice render target views, one per shadow light channel.
	/// Used during the shadow mask render pass to write each light's
	/// shadow data to its own array slice.
	winrt::com_ptr<ID3D11RenderTargetView> shadowMaskSliceRTVs[kMaxShadowLights];

	/// Compute shader that composites the extended shadow mask array
	/// back into a format usable by the lighting shaders.
	ID3D11ComputeShader* compositeShadowMaskCS = nullptr;

	/// Structured buffer containing per-shadow-light metadata for the
	/// lighting shader (light index → array slice mapping, etc.)
	eastl::unique_ptr<Buffer> shadowLightInfoBuffer;

	// =========================================================================
	// Shadow Light Tracking
	// =========================================================================

	/// Per-frame tracking of which shadow lights are active and their
	/// assigned mask indices (now 0 to kMaxShadowLights-1).
	struct ShadowLightInfo
	{
		uint32_t maskIndex = UINT32_MAX;
		bool active = false;
		bool isDirectional = false;
	};

	ShadowLightInfo activeShadowLightInfo[kMaxShadowLights]{};
	uint32_t currentShadowLightCount = 0;

	// =========================================================================
	// Resource Management
	// =========================================================================

	void SetupResources();
	void ReleaseResources();

	// =========================================================================
	// Engine Hooks
	// =========================================================================

	struct Hooks
	{
		/// Hook the shadow light accumulation in DrawWorld to allow > 4 lights.
		///
		/// The vanilla engine calls each BSShadowLight::Accumulate() with
		/// references to `activeShadowLightCount` and `shadowLightMaskIndex`.
		/// We intercept the caller to:
		///   1. Allow maskIndex to go beyond 3
		///   2. Track which lights were assigned which slots
		///   3. Manage the extended shadow mask array slices
		///
		/// Target: The loop in Main::DrawWorld that iterates shadowLightsAccum
		/// and calls Accumulate() on each BSShadowLight.
		struct DrawWorld_AccumulateShadowLights
		{
			/// Replacement for the engine's shadow light accumulation loop.
			/// Allows up to kMaxShadowLights instead of 4.
			static void thunk(RE::ShadowSceneNode* shadowScene,
				uint32_t& globalShadowLightCount,
				uint32_t& shadowMaskChannel);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// Hook the utility shader's shadow mask render pass.
		///
		/// When the engine renders shadow masks (BSUtilityShader with
		/// RenderShadowmask/Spot/Pb/Dpb flags), it normally writes to the
		/// RGBA shadow mask RT. We intercept this to:
		///   1. For maskIndex 0–3: let the engine write to vanilla RGBA as normal
		///   2. For maskIndex 4+: redirect output to our array texture slices
		///
		/// This maintains backwards compatibility with any code that reads
		/// the vanilla shadow mask while extending capacity.
		struct BSUtilityShader_RenderShadowmask
		{
			static void thunk(RE::BSShader* shader, uint32_t technique);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// Hook the shadow mask clear to also clear extended array slices.
		struct ClearShadowMask
		{
			static void thunk();
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};
};
