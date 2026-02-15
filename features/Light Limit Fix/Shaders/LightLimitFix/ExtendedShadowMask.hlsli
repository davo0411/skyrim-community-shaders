/// Extended Shadow Mask Sampling
///
/// This header provides functions to sample shadow data for lights with
/// maskIndex values beyond the vanilla 4-channel limit.
///
/// The vanilla system uses a single RGBA texture (TexShadowMaskSampler, t14)
/// where each channel stores one shadow light's shadow factor:
///   channel 0 (R) = directional light / shadow light 0
///   channel 1 (G) = shadow light 1
///   channel 2 (B) = shadow light 2
///   channel 3 (A) = shadow light 3
///
/// The extended system adds a Texture2DArray (t48) with up to N slices:
///   slice 0–3: mirrors the vanilla RGBA channels (for consistency)
///   slice 4+:  additional shadow-casting light channels
///
/// Usage:
///   float shadow = ExtendedShadowMask::GetShadow(screenPos, maskIndex);
///
/// This replaces the vanilla pattern of:
///   shadowColor[ShadowLightMaskSelect[lightIndex]]
/// with:
///   ExtendedShadowMask::GetShadow(screenPos, light.shadowLightIndex)

#ifndef __EXTENDED_SHADOW_MASK_HLSLI__
#define __EXTENDED_SHADOW_MASK_HLSLI__

namespace ExtendedShadowMask
{
	// Maximum number of shadow-casting lights supported.
	// Must match ShadowLightLimitFix::kMaxShadowLights in C++.
	static const uint kMaxShadowLights = 8;

	// The vanilla 4-channel shadow mask (always available, t14).
	// This is the engine's standard RGBA shadow mask texture.
	// We don't redeclare it here — it's already bound as TexShadowMaskSampler.

	// The extended shadow mask array texture (t48).
	// Each slice contains the shadow factor for one shadow-casting light.
	// Slice index == BSShadowLight::maskIndex.
	Texture2DArray<float> ExtendedShadowMaskArray : register(t48);

	/// Sample the shadow factor for a given shadow light.
	///
	/// For maskIndex 0–3, we read from the vanilla RGBA shadow mask
	/// to maintain perfect compatibility with all existing code paths.
	/// For maskIndex 4+, we read from the extended Texture2DArray.
	///
	/// @param screenPos  Screen-space position (xy pixels, from SV_Position)
	/// @param shadowMaskColor  The vanilla shadow mask sample (float4 RGBA from t14)
	/// @param maskIndex  The shadow light's assigned mask channel (0 to kMaxShadowLights-1)
	/// @return Shadow factor: 0.0 = fully shadowed, 1.0 = fully lit
	float GetShadow(float2 screenPos, float4 shadowMaskColor, uint maskIndex)
	{
		if (maskIndex < 4)
		{
			// Vanilla path: index into RGBA channels
			return shadowMaskColor[maskIndex];
		}
		else if (maskIndex < kMaxShadowLights)
		{
			// Extended path: sample from Texture2DArray slice.
			// Load(int4(x, y, arraySlice, mipLevel))
			return ExtendedShadowMaskArray.Load(int4(int2(screenPos), maskIndex, 0)).x;
		}
		else
		{
			// Invalid index: assume fully lit (no shadow)
			return 1.0;
		}
	}

	/// Overload for the common case in the LLF clustered light loop.
	/// Takes the light struct directly.
	float GetShadowForLight(float2 screenPos, float4 shadowMaskColor, uint shadowLightIndex)
	{
		return GetShadow(screenPos, shadowMaskColor, shadowLightIndex);
	}
}

#endif // __EXTENDED_SHADOW_MASK_HLSLI__
