#ifndef __PBR_WATER_UNDERWATER_HLSLI__
#define __PBR_WATER_UNDERWATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "PBRWater/Optics.hlsli"

// ============================================================================
// PBR Water - underwater view.
//
// The water body is rendered by Exponential Height Fog: while the eye is below the displaced surface,
// PBR Water hands it the water as its medium (extinction, scattering albedo and phase, light attenuated
// with depth, caustic light shafts), so the froxel volume with its shadows and local lights, and every
// shader that applies height fog, sees the water.
//
// What only the water knows stays here: the meniscus where the surface crosses the lens (from the Crest
// Ocean System, wave-harmonic/crest, MIT License, Copyright (c) 2019 Wave Harmonic and contributors:
// UnderwaterMeniscus.shader) and the reflectance of the surface seen from below.
// ============================================================================

namespace PBRWater
{
	/// The camera is within reach of the water surface or below it (meniscus, surface from below).
	bool UnderwaterActive() { return Underwater0.x > 0.5; }

	/// Flat water height at the camera, camera-relative.
	float UnderwaterPlaneZ() { return Underwater0.y - FrameBuffer::CameraPosAdjust.z; }

	/**
	 * Colour multiplier for a pixel of the final image: a thin, slightly blue-grey line where the displaced
	 * surface crosses the lens, a couple of pixels wide whatever the resolution.
	 */
	float3 UnderwaterMeniscus(float2 uv)
	{
		float2 ndc = float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
		float4 nearH = mul(FrameBuffer::CameraViewProjInverse, float4(ndc, 0.0, 1.0));
		float3 nearPos = nearH.xyz / nearH.w;
		float signedHeight = SurfaceHeightAt(nearPos.xy, UnderwaterPlaneZ(), 2) - nearPos.z;
		float pixels = abs(signedHeight) / max(fwidth(signedHeight), 1e-5);
		float width = 2.5 * max(SharedData::BufferDim.y / 1080.0, 0.5);
		// Only while the waves can reach the lens (z: their reach, 0 when the camera is far from the surface).
		float alpha = Underwater0.z > 0.0 ? sqrt(smoothstep(width, 0.0, pixels)) * saturate(Underwater3.x) : 0.0;
		return lerp(1.0, 1.3 * float3(0.37, 0.4, 0.5), alpha);
	}

	/**
	 * Reflectance of the surface seen from below (water to air), averaged over the facets within a pixel.
	 * Outside Snell's window total internal reflection mirrors the water; the switch is abrupt for a single
	 * facet, so on wave slopes it flickers between window and mirror as dark and bright stripes. Rough or
	 * distant water spreads the facet angles, which widens the transition.
	 */
	float FresnelFromBelow(float cosI, float roughness)
	{
		float spread = 0.04 + 0.6 * saturate(roughness);
		return (FresnelDielectric(cosI - spread, WaterIOR) + FresnelDielectric(cosI, WaterIOR) + FresnelDielectric(cosI + spread, WaterIOR)) / 3.0;
	}
}

#endif  // __PBR_WATER_UNDERWATER_HLSLI__
