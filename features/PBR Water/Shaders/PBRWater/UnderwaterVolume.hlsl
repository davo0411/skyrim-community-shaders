// ============================================================================
// PBR Water - the water around the camera under the surface (PBRWater::DrawUnderwaterVolume), in place of the game's
// underwater fog (PBRWater zeroes each water form's underwater fog amount), with its light shafts.
//
// Drawn once over the screen before the water's own passes while the camera is under water. What every pixel shows lies
// some distance through the water: its light reaches the camera attenuated per channel (Beer-Lambert, T = exp(-sigma d),
// with the surface's extinction: Optics.hlsli Extinction, pure water's absorption its floor), and the water along the
// way scatters light in its own colour. Dual-source blending: SV_Target0 is added, SV_Target1 multiplies what is there.
// The surface seen from below draws over its own pixels afterwards with the same water (Water.hlsl, UNDERWATER).
//
// Light shafts (Shafts.x steps): the in-scattering is marched along the view ray, the part of it under the surface, with
// the steps spread exponentially (dense near the camera, where the shafts are sharp) and jittered per pixel and frame
// (the temporal filter adds them up). At each step the sun's light is what crossed the water down to it along the
// refracted sun direction, shadowed by what stands between it and the sun (ShadowSampling::GetSurfaceShadow: terrain,
// clouds, the shadow map), focused or spread by the waves it came through (the surface's curvature where that ray left
// it, a lens of focal length ~ 1 / ((1 - 1/n) curvature), blurred with depth as the water scatters it), and scattered
// towards the camera with a forward-peaked phase (Henyey-Greenstein). The sky's light adds without shafts.
// After HPWater's water volumetrics (AshenOneArt/HPWater, MPL-2.0), reimplemented here: exponential steps, IGN jitter,
// depth-driven blur of the focusing.
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
	float4 Extinction;  // xyz 1/unit, w the camera's depth under the surface (units)
	float4 Body;        // xyz the water's colour (linear, the water form's deep colour), w unused
	float4 Viewport;    // xy the target's size (pixels), z the farthest the water is crossed (units), w the surface's height above the camera (units)
	float4 Shafts;      // x steps (0: no shafts, the in-scattering in closed form), y phase anisotropy g, z wave focusing strength, w unused
};

Texture2D<float> Depth : register(t0);

struct VSOutput
{
	float4 position : SV_Position;
};

VSOutput VS(uint id : SV_VertexID)
{
	VSOutput o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

struct PSOutput
{
	float4 added : SV_Target0;
	float4 multiplied : SV_Target1;
};

/// How much the waves focus the sunlight reaching camera-relative `p`, `depth` units under the surface, along `toSun`
/// (refracted): the surface's curvature where that ray left it, from the cascades' compression (with choppy waves the
/// horizontal displacement's divergence is the height's curvature over the band's wavenumber, times the choppiness).
float WaveFocus(float3 p, float depth, float3 toSun)
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
	// A crest (curvature < 0) is a converging lens below it.
	float lens = 1.0 + Shafts.z * depth * (1.0 - 1.0 / PBRWater::WaterIOR) * curvature;
	return clamp(1.0 / max(lens, 0.25), 0.25, 4.0);
}

PSOutput PS(VSOutput input)
{
	float raw = Depth.Load(int3(input.position.xy, 0));
	float2 uv = input.position.xy / Viewport.xy;
	float4 p = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), raw, 1.0));
	float3 scene = p.xyz / p.w;
	float sceneDistance = min(length(scene), Viewport.z);
	float3 viewDirection = scene / max(length(scene), 1e-4);

	float3 sun = Color::DirectionalLight(SharedData::DirLightColor.xyz);
	float3 sky = Color::Ambient(max(0.0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0))));
	float3 transmittance;
	float3 scattered;

	[branch] if (Shafts.x < 0.5)
	{
		// No shafts: the light at the camera's depth, scattered evenly along the whole path.
		transmittance = exp(-Extinction.xyz * sceneDistance);
		float3 light = (sun * saturate(SharedData::DirLightDirection.z) + sky) * exp(-Extinction.xyz * Extinction.w);
		scattered = Body.xyz * light * (1.0 - transmittance);
	}
	else
	{
		// The ray is in the water up to the scene or to the surface above, whichever comes first (the surface from below
		// draws over the pixels it covers afterwards).
		float level = Viewport.w;
		float inWater = viewDirection.z > 1e-4 ? min(sceneDistance, level / viewDirection.z) : sceneDistance;
		float3 toSun = PBRWater::RefractedSunDirection(SharedData::DirLightDirection.xyz);
		float phase = PBRWater::PhaseHG(dot(viewDirection, toSun), Shafts.y);
		float sunAbove = saturate(SharedData::DirLightDirection.z * 4.0);  // the sun below the horizon lights no shafts

		// Exponential steps: t(u) = inWater (e^(k u) - 1) / (e^k - 1), u in 0..1.
		static const float K = 2.0;
		const float norm = 1.0 / (exp(K) - 1.0);
		uint steps = (uint)Shafts.x;
		float jitter = Random::InterleavedGradientNoise(input.position.xy, SharedData::FrameCount);
		transmittance = 1.0;
		scattered = 0.0;
		float previous = 0.0;
		[loop] for (uint i = 0; i < steps; i++)
		{
			float t1 = inWater * (exp(K * (i + 1.0) / steps) - 1.0) * norm;
			float t = inWater * (exp(K * (i + jitter) / steps) - 1.0) * norm;
			float segment = t1 - previous;
			previous = t1;
			float3 s = viewDirection * t;
			float depth = max(level - s.z, 0.0);
			float shadow = ShadowSampling::GetSurfaceShadow(s);
			float3 sunHere = sun * sunAbove * exp(-Extinction.xyz * depth / max(toSun.z, 0.2)) * shadow * WaveFocus(s, depth, toSun) * phase;
			float3 skyHere = sky * exp(-Extinction.xyz * depth);
			float3 segmentTransmittance = exp(-Extinction.xyz * segment);
			scattered += transmittance * Body.xyz * (sunHere + skyHere) * (1.0 - segmentTransmittance);
			transmittance *= segmentTransmittance;
		}
	}

	PSOutput o;
	o.added = float4(scattered * SharedData::PreExposure, 0.0);
	o.multiplied = float4(transmittance, 1.0);
	return o;
}
