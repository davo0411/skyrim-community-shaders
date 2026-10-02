#ifndef __PBR_WATER_SHADING_HLSLI__
#define __PBR_WATER_SHADING_HLSLI__

#include "Common/BRDF.hlsli"
#include "PBRWater/PBRWater.hlsli"

// ============================================================================
// PBR Water - pixel shading helpers.
//
//   Surface   : analytic wave normals evaluated per pixel; wave components smaller than the pixel
//               footprint are removed and their slope variance is folded into the GGX roughness
//               (in the spirit of LEAN mapping, Olano & Baker 2010), so distant water keeps its
//               glitter energy instead of aliasing.
//   Interface : exact dielectric Fresnel (air/water IOR 1.333), including total internal
//               reflection when seen from below (Snell's window).
//   Volume    : Beer-Lambert transmittance whose extinction is derived from the water form's own
//               vanilla shallow colour and visibility distance, plus single-scatter in-scattering.
//   Scatter   : wave subsurface term from Atlas (Pleasant & Ross, GDC 2019).
//   Foam      : procedural cellular foam advected with the waves, thresholded by a physically
//               motivated coverage (shore depth, wave folding, breaking, wakes, whitecaps).
// ============================================================================

namespace PBRWater
{
	static const float WaterIOR = 1.333;

	/// Exact unpolarised Fresnel reflectance for a dielectric. `eta` = n_incident / n_transmitted.
	/// Returns 1 under total internal reflection.
	float FresnelDielectric(float cosI, float eta)
	{
		cosI = saturate(cosI);
		float sinT2 = eta * eta * (1.0 - cosI * cosI);
		if (sinT2 >= 1.0)
			return 1.0;
		float cosT = sqrt(1.0 - sinT2);
		float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
		float rp = (cosI - eta * cosT) / (cosI + eta * cosT);
		return 0.5 * (rs * rs + rp * rp);
	}

	/// Perceptual roughness from the base roughness plus filtered-out slope variance.
	float CombinedRoughness(float baseRoughness, float slopeVariance)
	{
		float alpha = baseRoughness * baseRoughness;
		alpha = sqrt(alpha * alpha + 2.0 * slopeVariance);
		return saturate(sqrt(alpha));
	}

	/// GGX specular lobe for a punctual light, Fresnel included. `toEye`/`toLight` point away from the surface.
	float SpecularGGX(float3 N, float3 toEye, float3 toLight, float roughness, float eta)
	{
		float3 H = normalize(toEye + toLight);
		float NdotL = saturate(dot(N, toLight));
		float NdotV = saturate(abs(dot(N, toEye)) + 1e-4);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(toEye, H));
		float D = BRDF::D_GGX(roughness, NdotH);
		float Vis = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		return D * Vis * FresnelDielectric(VdotH, eta) * NdotL;
	}

	/// Per-channel extinction (1/unit). The vanilla shallow colour is read as the tint light picks up
	/// over the water form's visibility distance; at that distance 5% of the light remains.
	float3 Extinction(float3 shallowColorLinear, float visibilityUnits)
	{
		float3 tint = saturate(shallowColorLinear / max(max(shallowColorLinear.r, max(shallowColorLinear.g, shallowColorLinear.b)), 1e-4));
		tint = max(tint, 0.02);
		return (log(20.0) - log(tint)) / max(visibilityUnits, 1.0);
	}

	/// Wave subsurface scattering (Atlas, GDC 2019): light entering the back of a wave and leaving
	/// towards the viewer, strongest at crests seen against the sun.
	float SubsurfaceScatter(float3 N, float3 toEye, float3 toLight, float crestHeight)
	{
		float towardsSun = pow(saturate(dot(-toEye, toLight)), 4.0);
		float backLit = pow(saturate(0.5 - 0.5 * dot(toLight, N)), 3.0);
		float facing = pow(saturate(dot(toEye, N)), 2.0);
		return crestHeight * towardsSun * backLit * 4.0 + facing * 0.15;
	}

	// ------------------------------------------------------------------------
	// Foam
	// ------------------------------------------------------------------------
	float2 FoamHash2(float2 p)
	{
		p = float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)));
		return frac(sin(p) * 43758.5453);
	}

	/// Distance to the nearest cell border of a jittered grid: bright filaments around dark bubbles.
	float CellularFoam(float2 p, float time)
	{
		float2 cell = floor(p);
		float2 f = frac(p);
		float d1 = 8.0, d2 = 8.0;
		[unroll] for (int y = -1; y <= 1; y++)
		{
			[unroll] for (int x = -1; x <= 1; x++)
			{
				float2 o = float2(x, y);
				float2 h = FoamHash2(cell + o);
				h = 0.5 + 0.4 * sin(time * 0.6 + Math::TAU * h);
				float d = length(o + h - f);
				if (d < d1) {
					d2 = d1;
					d1 = d;
				} else if (d < d2) {
					d2 = d;
				}
			}
		}
		return saturate(1.0 - (d2 - d1) * 2.5);
	}

	/// Foam pattern in [0, 1]. `p` is a Lagrangian (undisplaced) position, so the foam rides the wave
	/// orbits instead of sliding over the surface; the cells themselves slowly churn over time.
	float FoamPattern(float2 p, float time)
	{
		float scale = max(Foam0.w, 1.0);
		float a = CellularFoam(p / scale, time);
		float b = CellularFoam(p / (scale * 0.37) + 17.3, time * 1.3);
		return saturate(a * 0.65 + b * 0.45);
	}

	/// Turns a coverage in [0, 1] into a foam mask using the pattern as a threshold, so sparse foam
	/// breaks into filaments and dense foam becomes solid.
	float FoamMask(float coverage, float pattern)
	{
		coverage = saturate(coverage * Foam0.z);
		return saturate((pattern - (1.0 - coverage)) * 4.0) * saturate(coverage * 3.0);
	}

	// ------------------------------------------------------------------------
	// Per-pixel surface
	// ------------------------------------------------------------------------
	struct SurfaceShading
	{
		float3 normal;
		float roughness;
		float jacobian;
		float shoreBreak;
		float shoreMask;
		float rippleFoam;
		float depth;
	};

	/**
	 * Combines the analytic wave normal, the interactive ripples and the vanilla detail normal.
	 * @param waveParam  xyz undisplaced camera-relative position, w flow damping
	 * @param detailNormal vanilla normal-map normal (z up)
	 */
	SurfaceShading ShadeSurface(float4 waveParam, float3 detailNormal)
	{
		SurfaceShading o;

		float2 dpdx = ddx(waveParam.xy);
		float2 dpdy = ddy(waveParam.xy);
		float footprint = max(length(dpdx), length(dpdy));

		WaveContext ctx = BuildWaveContext(waveParam.xyz, waveParam.w, 0.0);
		WaveResult waves = EvaluateWaves(waveParam.xyz, ctx, false, footprint);
		float3 ripple = RippleSlopeFoam(waveParam.xyz);

		// Add surface slopes: waves + ripples + vanilla detail (partial-derivative blending).
		float2 slope = -waves.normal.xy / max(waves.normal.z, 0.05);
		slope += ripple.xy;
		slope += detailNormal.xy / max(detailNormal.z, 0.05);
		o.normal = normalize(float3(-slope, 1.0));

		o.roughness = CombinedRoughness(Light0.x, waves.slopeVariance);
		o.jacobian = waves.jacobian;
		o.shoreBreak = waves.shoreBreak;
		o.shoreMask = waves.shoreMask;
		o.rippleFoam = ripple.z;
		o.depth = ctx.depth;
		return o;
	}

	/// Foam coverage from all sources. `waterThickness` is the vertical water depth to the scene behind
	/// the surface (shores, rocks, piers, wading legs ...); pass a large value when unknown.
	float FoamCoverage(SurfaceShading s, float waterThickness)
	{
		float shore = 1.0 - saturate(waterThickness / max(Foam0.x, 1.0));
		shore *= shore;
		float crest = saturate((Foam0.y - s.jacobian) * 2.0) * saturate(Params2.y * 8.0 + 0.25);
		float breaking = s.shoreBreak * Shore1.y;
		return saturate(shore + crest + breaking + s.rippleFoam * Foam1.z);
	}

	float3 DebugView(uint mode, SurfaceShading s, float foam, float4 waveState)
	{
		switch (mode) {
		case 1:
			return s.normal * 0.5 + 0.5;
		case 2:
			return foam.xxx;
		case 3:
			return float3(saturate(s.depth / (10.0 * UnitsPerMetre)), s.shoreMask, 0.0);
		case 4:
			return float3(saturate(1.0 - s.jacobian), saturate(-s.jacobian), s.shoreBreak);
		case 5:
			return s.roughness.xxx;
		case 6:
			return float3(saturate(waveState.x / max(Params2.z, 1.0)) * 0.5 + 0.5, saturate(-waveState.x / max(Params2.z, 1.0)), 0.0);
		default:
			return 0.0;
		}
	}

	/// Anti-aliased triangle edges from the geometry shader barycentrics.
	float WireframeMask(float3 barycentric)
	{
		float3 d = fwidth(barycentric);
		float3 a = smoothstep(0.0, d * 1.5, barycentric);
		return 1.0 - min(a.x, min(a.y, a.z));
	}
}

#endif  // __PBR_WATER_SHADING_HLSLI__
