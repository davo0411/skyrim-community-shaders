#ifndef __PBR_WATER_UNDERWATER_LIGHT_HLSLI__
#define __PBR_WATER_UNDERWATER_LIGHT_HLSLI__

// ============================================================================
// PBR Water - the light in the water around the camera under the surface: shared by the shafts' march
// (UnderwaterShaftsCS.hlsl) and the composite over the scene (UnderwaterVolume.hlsl). PBRWater::DrawUnderwaterVolume.
//
// What every pixel shows lies some distance through the water: its light reaches the camera attenuated per channel
// (Beer-Lambert with the water's extinction) and the water along the way scatters light towards the camera, a share
// sigma_s / sigma_t of what it takes (the single-scattering albedo, Body). The sun's share is marched along the view ray
// under the surface (Shafts.x steps): the sunlight that crossed the water down along the refracted sun direction,
// shadowed by what stands between it and the sun (ShadowSampling::GetSurfaceShadow), focused or spread by the waves where
// that ray left the surface (a crest is a converging lens of focal length ~ 1 / ((1 - 1/n) curvature)), blurred with
// depth, scattered towards the camera with a forward-peaked Henyey-Greenstein phase. The sky's light adds without shafts.
// Steps spread exponentially (dense near the camera) and jittered per pixel and frame.
// After HPWater's water volumetrics (AshenOneArt/HPWater, MPL-2.0), reimplemented here.
// ============================================================================

#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/Optics.hlsli"

// The shadow helpers sample with the includer's linear sampler.
#define LinearSampler PBRWater::LinearClampSampler
#include "Common/ShadowSampling.hlsli"

cbuffer UnderwaterCB : register(b0)
{
	float4 Extinction;  // xyz sigma_t (1/unit), w the camera's depth under the surface (units)
	float4 Body;        // xyz single-scattering albedo sigma_s / sigma_t, w unused
	float4 Viewport;    // xy the full-resolution target's size (pixels), z the farthest the water is crossed (units), w the surface's height above the camera (units)
	float4 Shafts;      // x steps (0: no shafts, the in-scattering in closed form), y phase anisotropy g, z wave focusing strength, w history valid (0/1)
	float4 Half;        // xy the shafts' target size (pixels, half resolution), zw this frame's sub-pixel of each 2x2 (0..1)
};

/// Camera-relative position of what full-resolution pixel `pixel` (centre) shows at device depth `raw`.
float3 UnderwaterScenePoint(float2 pixel, float raw)
{
	float2 uv = pixel / Viewport.xy;
	float4 p = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), raw, 1.0));
	return p.xyz / p.w;
}

/// How much the waves focus the sunlight reaching camera-relative `p`, `depth` units under the surface, along `toSun`
/// (refracted): the surface's curvature where that ray left it, from the cascades' compression (with choppy waves the
/// horizontal displacement's divergence is the height's curvature over the band's wavenumber, times the choppiness).
float UnderwaterWaveFocus(float3 p, float depth, float3 toSun)
{
	[branch] if (!PBRWater::OceanActive() || Shafts.z <= 0.0)
		return 1.0;
	float3 exitPoint = p + toSun * (depth / max(toSun.z, 0.2));
	float curvature = 0.0;
	[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
	{
		// Scattering blurs the pattern with depth: a coarser mip the deeper the light has come.
		float mip = clamp(log2(1.0 + 0.1 * depth / max(PBRWater::Cascade0[c].w, 1e-3)), 0.0, PBRWater::Params2.w);
		float4 d = PBRWater::OceanDerivatives.SampleLevel(PBRWater::OceanSampler, float3(PBRWater::CascadeUV(exitPoint.xy, c), c), mip);
		curvature -= (d.z + d.w) * PBRWater::Cascade1[c].z / max(PBRWater::Params2.x, 0.1);
	}
	float lens = 1.0 + Shafts.z * depth * (1.0 - 1.0 / PBRWater::WaterIOR) * curvature;
	return clamp(1.0 / max(lens, 0.25), 0.25, 4.0);
}

/// The sun's and the sky's light at the camera's depth (linear, unencoded).
void UnderwaterLights(out float3 sun, out float3 sky)
{
	sun = Color::DirectionalLight(SharedData::DirLightColor.xyz) * saturate(SharedData::DirLightDirection.z * 4.0);
	sky = Color::Ambient(max(0.0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0))));
}

/// The light the water scatters towards the camera along `viewDirection` over `distance` units (linear, unencoded), in
/// closed form: the light at the camera's depth everywhere, no shafts.
float3 UnderwaterScatteringClosed(float distance)
{
	float3 sun, sky;
	UnderwaterLights(sun, sky);
	float3 light = (sun * saturate(SharedData::DirLightDirection.z) + sky) * exp(-Extinction.xyz * Extinction.w);
	return Body.xyz * light * (1.0 - exp(-Extinction.xyz * distance));
}

/// The same, marched for the shafts (Shafts.x steps, `jitter` 0..1).
float3 UnderwaterScatteringMarched(float3 viewDirection, float distance, float jitter)
{
	float3 sun, sky;
	UnderwaterLights(sun, sky);
	float level = Viewport.w;
	// The ray is in the water up to what it shows or to the surface above, whichever comes first.
	float inWater = viewDirection.z > 1e-4 ? min(distance, level / viewDirection.z) : distance;
	float3 toSun = PBRWater::RefractedSunDirection(SharedData::DirLightDirection.xyz);
	float phase = PBRWater::PhaseHG(dot(viewDirection, toSun), Shafts.y);

	// Exponential steps: t(u) = inWater (e^(K u) - 1) / (e^K - 1), u in 0..1.
	static const float K = 2.0;
	const float norm = 1.0 / (exp(K) - 1.0);
	uint steps = (uint)Shafts.x;
	float3 transmittance = 1.0;
	float3 scattered = 0.0;
	float previous = 0.0;
	[loop] for (uint i = 0; i < steps; i++)
	{
		float end = inWater * (exp(K * (i + 1.0) / steps) - 1.0) * norm;
		float t = inWater * (exp(K * (i + jitter) / steps) - 1.0) * norm;
		float segment = end - previous;
		previous = end;
		float3 s = viewDirection * t;
		float depth = max(level - s.z, 0.0);
		float shadow = ShadowSampling::GetSurfaceShadow(s);
		float3 sunHere = sun * exp(-Extinction.xyz * depth / max(toSun.z, 0.2)) * shadow * UnderwaterWaveFocus(s, depth, toSun) * phase;
		float3 skyHere = sky * exp(-Extinction.xyz * depth);
		float3 segmentTransmittance = exp(-Extinction.xyz * segment);
		scattered += transmittance * Body.xyz * (sunHere + skyHere) * (1.0 - segmentTransmittance);
		transmittance *= segmentTransmittance;
	}
	return scattered;
}

#endif  // __PBR_WATER_UNDERWATER_LIGHT_HLSLI__
