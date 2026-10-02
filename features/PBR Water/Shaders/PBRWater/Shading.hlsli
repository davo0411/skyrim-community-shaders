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
	//
	// Foam is a coverage field (how much of the surface is aerated) turned into a pattern with a
	// soft threshold, as in Crest (wave-harmonic/crest, MIT) and Sea of Thieves: as coverage drops
	// the bubbles in the foam grow and merge, so dense foam thins into lace and then into specks
	// instead of fading uniformly. The pattern is procedural: round bubbles of random size on a
	// jittered grid, with walls bent by a domain warp so no straight cell edges survive.
	// ------------------------------------------------------------------------

	// Hash without Sine, Dave Hoskins (MIT): stable across GPUs, unlike sin-based hashes.
	float Hash12(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * 0.1031);
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	float2 Hash22(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.xx + p3.yz) * p3.zy);
	}

	float ValueNoise(float2 p)
	{
		float2 i = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash12(i), Hash12(i + float2(1, 0)), u.x),
			lerp(Hash12(i + float2(0, 1)), Hash12(i + float2(1, 1)), u.x), u.y);
	}

	/// Three-octave fBm with rotated octaves (no grid-aligned artefacts), in [0, 1].
	float Fbm(float2 p)
	{
		const float2x2 rotation = float2x2(0.8, 0.6, -0.6, 0.8);
		float sum = 0.0;
		float amplitude = 0.5;
		[unroll] for (int i = 0; i < 3; i++)
		{
			sum += amplitude * ValueNoise(p);
			p = mul(rotation, p) * 2.03 + 7.1;
			amplitude *= 0.5;
		}
		return sum / 0.875;
	}

	/// Distance to the nearest bubble centre, normalised by that bubble's radius and merged with a
	/// smooth minimum: 0 at a bubble centre, ~1 at its rim, with rounded walls between bubbles.
	float BubbleField(float2 p)
	{
		float2 cell = floor(p);
		float2 f = frac(p);
		float result = 0.0;
		float weightSum = 0.0;
		[unroll] for (int y = -1; y <= 1; y++)
		{
			[unroll] for (int x = -1; x <= 1; x++)
			{
				float2 o = float2(x, y);
				float2 centre = o + 0.15 + 0.7 * Hash22(cell + o);
				float radius = 0.55 + 0.45 * Hash12(cell + o + 17.17);
				float d = length(centre - f) / radius;
				// Exponential smooth minimum: neighbouring bubbles merge softly instead of meeting at an edge.
				float w = exp2(-8.0 * d);
				result += w * d;
				weightSum += w;
			}
		}
		return result / max(weightSum, 1e-6);
	}

	/**
	 * Surface foam in [0, 1] for a local coverage.
	 * @param p         Lagrangian (undisplaced) absolute position, so foam rides the wave orbits
	 * @param coverage  fraction of the surface that is foam, 0..1
	 * @param footprint world size of a pixel (anti-aliasing)
	 * @param time      animation time (bubbles slowly churn and pop)
	 */
	float FoamLace(float2 p, float coverage, float footprint, float time)
	{
		float scale = max(Foam0.w, 1.0);
		float2 q = p / scale;

		// Large-scale density variation: foam gathers in clumps and streaks.
		float clumps = Fbm(q * 0.21 + float2(time * 0.011, -time * 0.007));
		coverage = saturate(coverage * (0.45 + 1.1 * clumps));
		if (coverage <= 0.002)
			return 0.0;

		// Domain warp bends the bubble walls into organic, curved filaments.
		float2 warp = float2(Fbm(q * 0.6 + time * 0.03), Fbm(q * 0.6 + 31.7 - time * 0.025)) - 0.5;
		float2 w = q + warp * 1.6;

		float large = BubbleField(w);
		float small = BubbleField(w * 2.7 + 9.3);

		// Bubbles grow as coverage falls; at zero coverage they swallow everything.
		float radius = lerp(1.45, 0.05, sqrt(coverage));
		float pixelCells = footprint / scale;
		float feather = 0.12 + pixelCells * 2.0;
		float lace = smoothstep(radius - feather, radius + feather, large);
		lace *= smoothstep(radius * 0.85 - feather * 2.7, radius * 0.85 + feather * 2.7, small);

		// Far away the pattern is sub-pixel: converge to its average instead of shimmering.
		float distanceBlend = saturate(pixelCells * 4.0 - 0.5);
		return lerp(lace, coverage * coverage, distanceBlend);
	}

	/// Soft, out-of-focus aeration under the foam (bubbles carried below the surface).
	float FoamBubbles(float2 p, float coverage, float time)
	{
		float2 q = p / max(Foam0.w * 1.7, 1.0);
		return saturate(coverage * 1.5) * (0.4 + 0.6 * Fbm(q + time * 0.02));
	}

	/// Crest foam coverage from surface folding (Jacobian below the threshold) and the wind's
	/// whitecap coverage (Monahan & O'Muircheartaigh 1980).
	float CrestCoverage(float jacobian)
	{
		return saturate((Foam0.y - jacobian) * 2.5) * saturate(Params2.y * 8.0 + 0.25);
	}

	// ------------------------------------------------------------------------
	// Per-pixel surface
	// ------------------------------------------------------------------------
	struct SurfaceShading
	{
		float3 normal;
		float roughness;
		float jacobian;
		float crestFoam;  // crest foam coverage including its decaying trail
		float shoreBreak;
		float shoreMask;
		float shoreCrest;
		float rippleFoam;
		float depth;
		float fetchMetres;
		float footprint;
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
		float3 ripple = RippleSlopeFoam(waveParam.xyz, ctx.depth);

		// Add surface slopes: waves + ripples + vanilla detail (partial-derivative blending).
		float2 slope = -waves.normal.xy / max(waves.normal.z, 0.05);
		slope += ripple.xy;
		slope += detailNormal.xy / max(detailNormal.z, 0.05);
		o.normal = normalize(float3(-slope, 1.0));

		o.roughness = CombinedRoughness(Light0.x, waves.slopeVariance);
		o.jacobian = waves.jacobian;

		// Crest foam with a trail: where this water was folding a moment ago, thinner the older it is.
		o.crestFoam = CrestCoverage(waves.jacobian);
		[branch] if (Foam1.x > 0.0 && Foam0.z > 0.0)
		{
			float older = CrestCoverage(WaveJacobian(waveParam.xyz, ctx, Foam1.x * 0.4, footprint));
			float oldest = CrestCoverage(WaveJacobian(waveParam.xyz, ctx, Foam1.x, footprint));
			o.crestFoam = max(o.crestFoam, max(older * 0.6, oldest * 0.3));
		}

		o.shoreBreak = waves.shoreBreak;
		o.shoreMask = waves.shoreMask;
		o.shoreCrest = waves.shoreCrest;
		o.rippleFoam = ripple.z;
		o.depth = ctx.depth;
		o.fetchMetres = SampleFetchMetres(waveParam.xy + FrameBuffer::CameraPosAdjust.xy);
		o.footprint = footprint;
		return o;
	}

	/// Foam coverage from all sources. `waterThickness` is the vertical water depth to the scene behind
	/// the surface (shores, rocks, piers, wading legs ...); pass a large value when unknown.
	float FoamCoverage(SurfaceShading s, float waterThickness)
	{
		// A thin band where the water meets anything, surging as each shore wave runs up.
		float contact = exp(-waterThickness / max(Foam0.x, 1.0));
		float surge = 0.6 + 0.4 * s.shoreCrest;
		float breaking = s.shoreBreak * Shore1.y * (0.5 + 0.5 * s.shoreCrest);
		return saturate((contact * surge + s.crestFoam + breaking + s.rippleFoam * Foam1.z) * Foam0.z);
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
		case 7:
			// Fetch on a log scale: black ~10 m (pond), white ~100 km (open sea).
			return saturate(log10(max(s.fetchMetres, 10.0)) / 4.0 - 0.25).xxx;
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
