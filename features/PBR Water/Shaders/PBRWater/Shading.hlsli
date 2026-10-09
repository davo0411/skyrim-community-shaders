#ifndef __PBR_WATER_SHADING_HLSLI__
#define __PBR_WATER_SHADING_HLSLI__

#include "Common/BRDF.hlsli"
#include "PBRWater/Optics.hlsli"

// ============================================================================
// PBR Water - pixel shading helpers.
//
//   Surface   : analytic wave normals evaluated per pixel; wave components smaller than the pixel
//               footprint are removed and their slope variance is folded into the GGX roughness
//               (in the spirit of LEAN mapping, Olano & Baker 2010), so distant water keeps its
//               glitter energy instead of aliasing.
//   Interface : exact dielectric Fresnel (air/water IOR 1.333), including total internal
//               reflection when seen from below (Snell's window).
//   Volume    : Beer-Lambert transmittance along the refracted path, with extinction from the water
//               form's shallow colour and visibility plus spatially varying sediment, light reaching
//               the bottom attenuated by K_d, and in-scattering towards the body colour (Optics.hlsli).
//   Scatter   : sunlight transmitted through thin wave crests (Optics.hlsli).
//   Wind      : micro-roughness from the local wind (Cox & Munk 1954): gusts sweep across the water as
//               darker, rougher patches (cat's paws), the lee of the upwind shore lies glassy and rain
//               roughens the surface.
//   Glow      : opt-in bioluminescence where the water is churned, visible only in the dark.
//   Foam      : procedural cellular foam advected with the waves, thresholded by a physically
//               motivated coverage (shore depth, wave folding, breaking, wakes, whitecaps).
// ============================================================================

namespace PBRWater
{
	/// Perceptual roughness from the base roughness plus filtered-out slope variance.
	float CombinedRoughness(float baseRoughness, float slopeVariance)
	{
		float alpha = baseRoughness * baseRoughness;
		alpha = sqrt(alpha * alpha + 2.0 * slopeVariance);
		return saturate(sqrt(alpha));
	}

	// ------------------------------------------------------------------------
	// Wind on the surface
	// ------------------------------------------------------------------------

	/**
	 * Wind speed (m/s) just above the water at absolute position `absXY`. Gusts are patches of stronger
	 * and weaker wind carried downwind at the wind speed, stretched along the wind; close to the upwind
	 * shore (short fetch) the water lies in the lee, where the wind has not yet reached the surface.
	 */
	float LocalWindSpeed(float2 absXY, float fetchMetres)
	{
		float U = Params0.y;
		float2 along = Params1.xy;
		float2 across = float2(-along.y, along.x);
		float scale = max(Surface0.z, 100.0);
		float t = ClarityTime();
		float2 q = float2(dot(absXY, along) / (scale * 1.8), dot(absXY, across) / scale);
		q.x -= t * U * UnitsPerMetre / (scale * 1.8);
		float gusts = smoothstep(0.25, 0.75, Fbm2(q + float2(t * 0.01, 3.7)));
		float gust = lerp(1.0, 0.35 + 1.3 * gusts, saturate(Surface0.y));
		float shelter = sqrt(saturate(fetchMetres / 150.0));
		return U * gust * lerp(0.25, 1.0, shelter);
	}

	/// Mean square slope of the ripples too small for the waves and normal maps: the capillary share of the
	/// Cox & Munk (1954) clean-surface fit, mss = 0.003 + 5.08e-3 U (up-wind 3.16e-3 U, cross-wind 0.003 + 1.92e-3 U).
	float WindMeanSquareSlope(float windSpeed)
	{
		return Surface0.x * 0.25 * (0.003 + 5.08e-3 * max(windSpeed, 0.0));
	}

	/// Rain drops ring and crown the surface.
	float RainMeanSquareSlope()
	{
		return 0.015 * saturate(Surface0.w);
	}

	/**
	 * Windrows: Langmuir circulation sweeps whatever floats into lines along the wind, spaced at about
	 * twice the dominant wavelength (Craik & Leibovich 1976). Returns 0..1 how close `absXY` is to a line;
	 * the lines meander and break up along their length. From a fresh breeze on they carry foam; in light
	 * winds they collect natural surface films instead, which damp the capillary ripples into glassy slicks.
	 */
	float WindrowPattern(float2 absXY, float peakOmega)
	{
		if (Surface1.x <= 0.0 || Params0.y < 1.5)
			return 0.0;
		float wavelength = 6.2831853 * Gravity() / max(peakOmega * peakOmega, 1e-4);
		float spacing = max(2.0 * wavelength, 3.0 * UnitsPerMetre);
		float2 along = Params1.xy;
		float2 across = float2(-along.y, along.x);
		// Rows drift slowly downwind with the surface water.
		float v = (dot(absXY, along) - ClarityTime() * 0.15 * UnitsPerMetre) / (spacing * 6.0);
		float u = dot(absXY, across) / spacing;
		u += (ValueNoise(float2(v * 2.0, u * 0.3)) - 0.5) * 0.8;
		float row = 1.0 - abs(frac(u) * 2.0 - 1.0);
		row *= row;
		row *= row;
		row *= row;
		float breakup = smoothstep(0.3, 0.8, ValueNoise(float2(floor(u) * 7.31, v * 3.0)));
		return row * breakup;
	}

	/// Foam gathered into the windrows, well marked from a fresh breeze on.
	float WindrowFoam(float pattern)
	{
		return pattern * saturate((Params0.y - 5.0) / 10.0) * Surface1.x * saturate(Params2.y * 10.0 + 0.3);
	}

	/// Slicks: in light winds (~2-7 m/s) the windrows hold surface films that damp the ripples (0..1).
	float WindrowSlick(float pattern)
	{
		return pattern * saturate((Params0.y - 1.5) / 2.0) * saturate((7.0 - Params0.y) / 3.0) * saturate(Surface1.x);
	}

	/// Angular radius of the sun disc (rad).
	static const float SunAngularRadius = 0.00465;
	/// Physical radius given to point lights (units, ~a torch flame): Skyrim lights have no emitter size.
	static const float PointLightEmitterRadius = 8.0;

	/// Angular radius (tangent) of a spherical emitter at `distance`; never more than 45 degrees.
	float EmitterAngularRadius(float radius, float distance)
	{
		return radius / max(distance, radius);
	}

	/**
	 * GGX specular lobe for a light of finite angular size, Fresnel included. `toEye`/`toLight` point
	 * away from the surface.
	 *
	 * A light of angular radius theta reflected in a mirror covers a cone of half vectors of radius
	 * theta / (2 sqrt(N.V)), so its spread adds to the microfacet variance (the sphere-light idea of
	 * Karis 2013, "Real Shading in Unreal Engine 4"). Treating the sun as a true point instead makes
	 * GGX divide by alpha^2 -> 0 on calm water and by N.V -> 0 at grazing angles, which overflows to
	 * inf and turns into NaN further down the frame. With the source size the lobe peak is bounded
	 * by 1 / (pi theta^2), the radiance of the source itself, and every denominator stays positive.
	 */
	float SpecularGGX(float3 N, float3 toEye, float3 toLight, float roughness, float eta, float sourceAngularRadius)
	{
		float NdotL = saturate(dot(N, toLight));
		float NdotV = saturate(abs(dot(N, toEye)) + 1e-4);
		float3 H = SafeNormalize(toEye + toLight, N);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(toEye, H));

		float alpha = roughness * roughness;
		float a2 = alpha * alpha + sourceAngularRadius * sourceAngularRadius / (4.0 * NdotV);
		float d = NdotH * NdotH * (a2 - 1.0) + 1.0;  // >= a2 > 0
		float D = a2 / (Math::PI * d * d);

		float Vis = BRDF::Vis_SmithJointApprox(roughness, NdotV, NdotL);
		return D * Vis * FresnelDielectric(VdotH, eta) * NdotL;
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

	/// Distance to the nearest bubble centre, normalised by that bubble's radius and merged with a
	/// smooth minimum: 0 at a bubble centre, ~1 at its rim, with rounded walls between bubbles.
	float BubbleField(float2 p, float time)
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
				// Each bubble drifts on its own small orbit and breathes, so the lace keeps churning.
				float2 h = Hash22(cell + o);
				float phase = Hash12(cell + o + 5.31) * 6.2831853;
				float speed = 0.6 + 0.8 * h.x;
				float2 orbit = float2(sin(time * speed + phase), cos(time * speed * 0.83 + phase * 1.7));
				float2 centre = o + 0.5 + (h - 0.5) * 0.6 + orbit * 0.18;
				float radius = (0.55 + 0.45 * Hash12(cell + o + 17.17)) * (0.9 + 0.1 * sin(time * 1.3 * speed + phase));
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

		// Domain warp bends the bubble walls into organic, curved filaments; it flows over time.
		float2 warp = float2(Fbm(q * 0.6 + float2(time * 0.17, time * 0.05)), Fbm(q * 0.6 + 31.7 - float2(time * 0.06, time * 0.15))) - 0.5;
		float2 w = q + warp * 1.6;

		float large = BubbleField(w, time);
		float small = BubbleField(w * 2.7 + 9.3, time * 1.7);

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

	/**
	 * Whitecaps: the foam a breaking crest leaves behind. Unlike the lacy, settled surface foam it is
	 * fresh, dense and bright, torn into streaks along the wind, so it uses its own pattern: anisotropic
	 * fBm stretched along the wind direction, domain-warped, with a hard coverage threshold.
	 * @param p        Lagrangian absolute position (drift already applied)
	 * @param coverage 0..1 whitecap coverage
	 */
	float Whitecaps(float2 p, float coverage, float footprint, float time)
	{
		if (coverage <= 0.002)
			return 0.0;
		float scale = max(Foam2.y, 1.0);
		float2 wind = Params1.xy;
		float2 q = float2(dot(p, wind), dot(p, float2(-wind.y, wind.x))) / scale;
		q.x /= max(Foam2.z, 1.0);  // streaks: longer along the wind than across it

		float2 warp = float2(Fbm(q * 0.7 + float2(time * 0.05, 0.0)), Fbm(q * 0.7 + 17.3 - float2(0.0, time * 0.04))) - 0.5;
		float n = Fbm(q * 1.3 + warp * 1.4);
		n = 0.65 * n + 0.35 * Fbm(q * 4.1 + warp * 2.0 + 5.7);

		float pixelCells = footprint / scale;
		float feather = 0.04 + pixelCells * 1.5;
		float threshold = 1.0 - sqrt(saturate(coverage)) * 0.85;
		float foam = smoothstep(threshold - feather, threshold + feather, n);
		// Far away the streaks are sub-pixel: converge to their mean coverage.
		return lerp(foam, coverage, saturate(pixelCells * 3.0 - 0.5));
	}

	/**
	 * Bubbles: discrete specks of air carried just under the surface. Each grid cell holds one bubble
	 * that drifts on its own path, rises, and pops after a random lifetime, so the layer is always
	 * moving. `density` is how aerated the water is (foam, wakes, breaking waves).
	 * @return x speck coverage, y soft milky aeration
	 */
	float2 Bubbles(float2 p, float density, float footprint, float time)
	{
		density = saturate(density * Foam2.w);
		if (density <= 0.002)
			return 0.0;
		float cellSize = max(Foam0.w * 0.18, 4.0);
		float2 q = p / cellSize;
		float2 cell = floor(q);
		float specks = 0.0;
		[unroll] for (int y = -1; y <= 1; y++)
		{
			[unroll] for (int x = -1; x <= 1; x++)
			{
				float2 c = cell + float2(x, y);
				float2 h = Hash22(c);
				float rate = 0.25 + 0.5 * Hash12(c + 3.7);
				float life = frac(time * rate + h.x);  // 0 born .. 1 popped
				float2 wander = float2(sin(time * (0.7 + h.y) + h.x * 6.28), cos(time * (0.9 + h.x) + h.y * 6.28)) * 0.25;
				float2 centre = c + 0.5 + (h - 0.5) * 0.7 + wander;
				float radius = (0.08 + 0.14 * Hash12(c + 9.1)) * sin(life * 3.14159);
				float present = step(Hash12(c + 1.3), density);  // fewer bubbles in less aerated water
				float d = length(q - centre);
				specks += present * smoothstep(radius, radius * 0.4, d);
			}
		}
		float pixelCells = footprint / cellSize;
		specks = lerp(saturate(specks), density * 0.15, saturate(pixelCells * 2.0 - 0.5));
		float milk = density * (0.4 + 0.6 * Fbm(p / (cellSize * 6.0) + float2(time * 0.09, -time * 0.07)));
		return float2(specks, milk);
	}

	/**
	 * World-space xy gradient of a per-pixel scalar from its screen derivatives and those of the
	 * world position (inverse of the 2x2 screen-to-world Jacobian). Lets the procedural foam bump
	 * the surface normal without evaluating its pattern several times.
	 */
	float2 WorldGradient(float value, float2 positionWS)
	{
		float2 dpdx = ddx(positionWS);
		float2 dpdy = ddy(positionWS);
		float dfdx = ddx(value);
		float dfdy = ddy(value);
		float det = dpdx.x * dpdy.y - dpdx.y * dpdy.x;
		det = abs(det) > 1e-6 ? det : 1e-6;
		return float2(dfdx * dpdy.y - dfdy * dpdx.y, dpdx.x * dfdy - dpdy.x * dfdx) / det;
	}

	/// Normal of the foam layer: the water normal it rests on, bumped by the foam's own relief.
	float3 FoamNormal(float3 waterNormal, float foam, float2 positionWS)
	{
		float2 g = WorldGradient(foam, positionWS) * (0.04 * UnitsPerMetre);  // foam relief ~4 cm
		g = clamp(g, -1.5, 1.5);
		return normalize(waterNormal - float3(g, 0.0));
	}

	/// Crest foam coverage from surface folding (Jacobian below the threshold) and the wind's
	/// whitecap coverage (Monahan & O'Muircheartaigh 1980).
	float CrestCoverage(float jacobian)
	{
		// Folding decides where crests break; the wind's whitecap fraction (Monahan) decides how much
		// of that actually turns white, so light winds give sparse caps and only gales foam over.
		return saturate((Foam0.y - jacobian) * 3.0) * saturate(Params2.y * 6.0 + 0.03);
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
		float turbidity;  // column sediment, filled in by the water column shading
		float windSpeed;  // local wind at the surface (m/s)
		float windrows;   // foam gathered into streaks along the wind
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
		WaveResult waves = EvaluateWaves(waveParam.xyz, ctx, footprint);
		float3 ripple = RippleSlopeFoam(waveParam.xyz, ctx.depth);

		float2 absXY = waveParam.xy + FrameBuffer::CameraPosAdjust.xy;
		float fetchMetres = SampleFetchMetres(absXY);
		float wind = LocalWindSpeed(absXY, fetchMetres);
		// The vanilla normal maps are the wind's ripples: calm patches turn glassy, gusts ripple the water.
		// River currents and rain ripple it whatever the wind.
		float flow = saturate((1.0 - waveParam.w) / max(Foam1.w, 0.01));
		float windrows = WindrowPattern(absXY, ctx.peakOmega);
		float slick = WindrowSlick(windrows);
		float detailScale = max(lerp(0.3, 1.25, saturate(wind / 8.0)) * (1.0 - 0.6 * slick), max(flow, Surface0.w));

		// Add surface slopes: waves + ripples + vanilla detail (partial-derivative blending).
		float2 slope = -waves.normal.xy / max(waves.normal.z, 0.05);
		slope += ripple.xy;
		slope += detailNormal.xy / max(detailNormal.z, 0.05) * detailScale;
		o.normal = normalize(float3(-slope, 1.0));

		// Geometric specular anti-aliasing (Kaplanyan 2016, Tokuyoshi 2019): the normal varies within the
		// pixel (wave curvature, ripples, detail maps), so widen the lobe by that variance. Without it a
		// mirror-like sun glint lands on single pixels at ~1e4x the sky radiance and blooms into blobs.
		float3 dndx = ddx(o.normal);
		float3 dndy = ddy(o.normal);
		float normalVariance = 0.25 * (dot(dndx, dndx) + dot(dndy, dndy));
		float windVariance = 0.5 * (WindMeanSquareSlope(wind) * (1.0 - 0.8 * slick) + RainMeanSquareSlope());
		o.roughness = CombinedRoughness(Light0.x, waves.slopeVariance + windVariance + min(normalVariance, 0.09));
		o.windSpeed = wind;
		o.windrows = WindrowFoam(windrows);
		o.jacobian = waves.jacobian;

		// Crest foam with a trail: where this water was folding a moment ago, thinner the older it is.
		o.crestFoam = CrestCoverage(waves.jacobian);
		if (Foam1.x > 0.0) {
			float older = CrestCoverage(waves.pastJacobian.x);
			float oldest = CrestCoverage(waves.pastJacobian.y);
			o.crestFoam = max(o.crestFoam, max(older * 0.6, oldest * 0.3));
		}

		o.shoreBreak = waves.shoreBreak;
		o.shoreMask = waves.shoreMask;
		o.shoreCrest = waves.shoreCrest;
		o.rippleFoam = ripple.z;
		o.depth = ctx.depth;
		o.fetchMetres = fetchMetres;
		o.footprint = footprint;
		o.turbidity = 0.0;
		return o;
	}

	/// Foam coverage from all sources. `waterThickness` is the vertical water depth to the scene behind
	/// the surface (shores, rocks, piers, wading legs ...); pass a large value when unknown.
	/// x surface foam (contact band, breaking shore waves, wakes), y whitecaps (folding crests).
	float2 FoamCoverage(SurfaceShading s, float waterThickness)
	{
		// A thin band where the water meets anything, surging as each shore wave runs up.
		float contact = exp(-waterThickness / max(Foam0.x, 1.0));
		float surge = 0.6 + 0.4 * s.shoreCrest;
		// Breaking foam rides the crests that are breaking, with only a thin residue between them.
		float breaking = s.shoreBreak * Shore1.y * (0.1 + 0.9 * s.shoreCrest * s.shoreCrest);
		float surface = saturate((contact * surge + breaking + s.rippleFoam * Foam1.z + s.windrows) * Foam0.z);
		float whitecap = saturate(s.crestFoam * Foam2.x);
		return float2(surface, whitecap);
	}

	/**
	 * Bioluminescence (opt-in): dinoflagellates flash blue-green when the water around them is sheared, so
	 * breaking waves, wakes, splashes and the swash glow at night. `agitation` is the churned share of the
	 * surface; the light is only visible in the dark. Returns emitted colour in the shading space.
	 */
	float3 Bioluminescence(float agitation, float2 absXY, float time, float darkness)
	{
		if (Surface1.y <= 0.0 || darkness <= 0.0 || agitation <= 0.0)
			return 0.0;
		// Individual flashes: sub-metre sparks that light up and fade within a second or so.
		float2 p = absXY * MetresPerUnit * 3.0;
		float sparks = smoothstep(0.55, 0.95, ValueNoise(p + float2(time * 1.7, -time * 1.3)));
		sparks = max(sparks, 0.6 * smoothstep(0.6, 0.95, ValueNoise(p * 2.3 - float2(time * 2.9, time * 0.7) + 11.0)));
		return Color::Water(Surface2.xyz) * (Surface1.y * darkness * saturate(agitation) * (0.35 + 1.65 * sparks));
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
		case 8:
			// Water clarity: blue clear, brown murky.
			return lerp(float3(0.05, 0.25, 0.6), float3(0.55, 0.35, 0.1), saturate(s.turbidity / 2.0)) * (0.4 + 0.6 * saturate(s.turbidity));
		case 9:
			// Local wind: black calm, white 15 m/s; foam streaks in red.
			return float3(max(saturate(s.windSpeed / 15.0), saturate(s.windrows * 2.0)), saturate(s.windSpeed / 15.0).xx);
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
