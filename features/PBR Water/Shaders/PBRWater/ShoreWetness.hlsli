#ifndef __PBR_WATER_SHORE_WETNESS_HLSLI__
#define __PBR_WATER_SHORE_WETNESS_HLSLI__

// ============================================================================
// PBR Water - the shore as the waves leave it, for the lit terrain and objects (Lighting.hlsl).
//
// ShoreSimCS keeps, on a camera-centred grid, the highest water each point saw lately (draining slowly as the
// sand dries), the foam the backwash left on the sand and the height of the water surface (waves and swash).
// Here the lit surfaces read it: whatever the waves reached is wet (Wetness Effects), the foam lies on the sand
// in rafts that break up as their bubbles burst, and the caustics and the underwater wetness follow the waves
// instead of the flat still water level.
// ============================================================================

#include "PBRWater/Noise.hlsli"

namespace PBRWaterShore
{
	Texture2D<float4> SwashWetTexture : register(t123);  // x wet level, y foam the backwash left, z its deposit, w water surface (units above the still water)

	struct Shore
	{
		bool valid;
		float weight;    ///< 0..1 fades out towards the edge of the grid
		float wetLevel;  ///< highest water lately, draining as the sand dries (units above the still water)
		float foam;      ///< 0..1 foam the backwash left here
		float surface;   ///< water surface above the still water (units)
	};

	float4 BilinearLoad(float2 uv)
	{
		uint w, h;
		SwashWetTexture.GetDimensions(w, h);
		float2 p = uv * float2(w, h) - 0.5;
		int2 i0 = int2(floor(p));
		float2 f = p - i0;
		int2 hi = int2(w, h) - 1;
		int2 a = clamp(i0, 0, hi);
		int2 b = clamp(i0 + 1, 0, hi);
		return lerp(lerp(SwashWetTexture.Load(int3(a.x, a.y, 0)), SwashWetTexture.Load(int3(b.x, a.y, 0)), f.x),
			lerp(SwashWetTexture.Load(int3(a.x, b.y, 0)), SwashWetTexture.Load(int3(b.x, b.y, 0)), f.x), f.y);
	}

	/**
	 * The shore water at camera-relative `positionWS`.
	 * @param stillWater the still water level here (camera-relative, SharedData::GetWaterData)
	 */
	Shore Sample(float3 positionWS, float stillWater)
	{
		Shore s;
		s.valid = false;
		s.weight = 0.0;
		s.wetLevel = 0.0;
		s.foam = 0.0;
		s.surface = 0.0;

		const float4 grid = SharedData::pbrWaterSettings.SwashGrid;
		float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - grid.xy) * grid.z;
		[branch] if (grid.w > 0.5 && stillWater > -1e6 && all(uv > 0.0) && all(uv < 1.0))
		{
			float4 wet = BilinearLoad(uv);
			// The grid scrolls with the camera: its edge fades out so newly covered ground never pops.
			float2 e = saturate(min(uv, 1.0 - uv) * 20.0);
			s.valid = true;
			s.weight = e.x * e.y;
			s.wetLevel = wet.x;
			s.foam = wet.y * s.weight;
			s.surface = wet.w * s.weight;
		}
		return s;
	}

	/// The wetness the waves leave above the water surface, 0..1, fading over `range` units above the highest water.
	float Wetness(Shore s, float positionZ, float stillWater, float range)
	{
		float above = positionZ - (stillWater + s.wetLevel);
		float wet = saturate(1.0 - above / max(range, 1.0)) * s.weight;
		return s.valid && SharedData::pbrWaterSettings.Wetness.x > 0.5 ? wet : 0.0;
	}

	/// Albedo of sea foam (texture value): fresh, dense foam reflects ~55% of visible light (Whitlock et al. 1982).
	static const float3 FoamAlbedo = 0.8;

	/**
	 * The foam lying on an upward-facing surface, 0..1: the rafts of bubbles the backwash leaves, breaking up as they age
	 * (PBRWater::BeachFoamPattern).
	 * @param footprint world size of a pixel (anti-aliasing)
	 */
	float Foam(Shore s, float3 positionWS, float normalZ, float footprint)
	{
		float amount = s.foam * SharedData::pbrWaterSettings.Wetness.y * saturate(normalZ * 2.5 - 1.0);
		float foam = 0.0;
		[branch] if (amount > 0.002)
		{
			float scale = max(SharedData::pbrWaterSettings.Wetness.z, 1.0);
			float2 q = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy) / scale;
			foam = PBRWater::BeachFoamPattern(q, amount, footprint / scale);
		}
		return foam;
	}
}

#endif  // __PBR_WATER_SHORE_WETNESS_HLSLI__
