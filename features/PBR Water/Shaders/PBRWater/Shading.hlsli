#ifndef __PBR_WATER_SHADING_HLSLI__
#define __PBR_WATER_SHADING_HLSLI__

#include "Common/BRDF.hlsli"
#include "PBRWater/Optics.hlsli"

// ============================================================================
// PBR Water - pixel shading helpers.
//
//   Surface   : FFT wave normals, trilinear / anisotropically filtered over the pixel footprint; the
//               slope variance the filter averages away is recovered from mip-filtered second moments
//               (LEAN mapping, Olano & Baker 2010; Bruneton et al. 2010) and folded into the GGX
//               roughness, so distant water keeps its glitter energy instead of aliasing.
//   Interface : exact dielectric Fresnel (air/water IOR 1.333), including total internal
//               reflection when seen from below (Snell's window).
//   Volume    : Beer-Lambert transmittance along the refracted path, with extinction from the water
//               form's shallow colour and visibility plus spatially varying sediment, light reaching
//               the bottom attenuated by K_d, and in-scattering towards the body colour (Optics.hlsli).
//   Scatter   : sunlight transmitted through thin wave crests (Optics.hlsli).
//   Wind      : micro-roughness from the local wind (Cox & Munk 1954): gusts sweep across the water as
//               darker, rougher patches (cat's paws), the lee of the upwind shore lies glassy and rain
//               roughens the surface.
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
		return FoamLacePattern(p / scale, coverage, footprint / scale, time);
	}

	/**
	 * Whitecaps: the foam a breaking crest leaves behind. Where it lies and how large it is comes from the
	 * breaking itself (`coverage`); the pattern only gives it texture: fresh, dense foam torn into streaks
	 * along the wind (anisotropic, domain-warped fBm), thinning towards the edge of the patch.
	 * @param p        Lagrangian absolute position (drift already applied)
	 * @param coverage 0..1 breaking intensity: the crest folding now and the trail it leaves
	 */
	float Whitecaps(float2 p, float coverage, float footprint, float time)
	{
		float patch = smoothstep(0.15, 0.6, coverage);
		if (patch <= 0.002)
			return 0.0;
		float scale = max(Foam2.y, 1.0);
		float2 wind = Params1.xy;
		float2 q = float2(dot(p, wind), dot(p, float2(-wind.y, wind.x))) / scale;
		q.x /= max(Foam2.z, 1.0);  // streaks: longer along the wind than across it

		float2 warp = float2(Fbm(q * 0.7 + float2(time * 0.05, 0.0)), Fbm(q * 0.7 + 17.3 - float2(0.0, time * 0.04))) - 0.5;
		float n = Fbm(q * 1.3 + warp * 1.4);
		n = 0.65 * n + 0.35 * Fbm(q * 4.1 + warp * 2.0 + 5.7);

		// Dense in the middle of the patch, broken into streaks towards its edge.
		float pixelCells = footprint / scale;
		float feather = 0.04 + pixelCells * 1.5;
		float threshold = 1.0 - patch * 0.95;
		float foam = smoothstep(threshold - feather, threshold + feather, n + 0.25 * patch);
		// Far away the streaks are sub-pixel: converge to the patch.
		return lerp(foam, patch * 0.8, saturate(pixelCells * 3.0 - 0.5));
	}

	/**
	 * Bubbles: discrete specks of air carried just under the surface. Each grid cell holds one bubble
	 * that drifts on its own path, rises, and pops after a random lifetime, so the layer is always
	 * moving. `density` is how aerated the water is (foam, wakes, breaking waves). Only the specks are
	 * drawn: no milky layer tints the water, and specks too small to see fade out instead of averaging into
	 * a white haze.
	 * @return speck coverage
	 */
	float Bubbles(float2 p, float density, float footprint, float time)
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
		return saturate(specks) * saturate(1.5 - pixelCells * 2.0);
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

	// ------------------------------------------------------------------------
	// Per-pixel surface
	// ------------------------------------------------------------------------
	struct SurfaceShading
	{
		float3 normal;
		float roughness;
		float jacobian;
		float crestFoam;   // crest foam coverage including its decaying trail
		float shoreBreak;  // a shore wave breaking here
		float shoreMask;   // inner surf zone
		float shoreCrest;
		float beach;        // 0..1 waves break on this shore (no surf or swash on rock faces and cliffs)
		float shoreDelay;   // shoaling delay of the ray through here (s, debug)
		float swashFoam;    // foam riding the swash over the beach
		float2 foamAdvect;  // how far the swash has carried the foam pattern up the beach (units)
		float rippleFoam;
		float depth;
		float shoreDistance;  // horizontal distance to the waterline of the bed (units), large when unknown
		float fetchMetres;
		float footprint;
		float turbidity;       // column sediment, filled in by the water column shading
		float windSpeed;       // local wind at the surface (m/s)
		float windrows;        // foam gathered into streaks along the wind
		float3 dominantWaves;  // x height, yz slope of the dominant waves (sun shadows)
		float dominantK;       // their wavenumber (1/unit)
		float2 waveDir;        // the direction they travel in (wind, or towards the beach)
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

		WaveContext ctx = BuildWaveContext(waveParam.xyz, waveParam.w);
		WaveResult waves = EvaluateWavesFiltered(waveParam.xyz, ctx);
		float3 ripple = RippleSlopeFoam(waveParam.xyz, ctx.depth);

		float2 absXY = waveParam.xy + FrameBuffer::CameraPosAdjust.xy;
		float fetchMetres = SampleFetchMetres(absXY);
		float wind = LocalWindSpeed(absXY, fetchMetres);
		// The vanilla normal maps are the wind's ripples: calm patches turn glassy, gusts ripple the water.
		// River currents and rain ripple it whatever the wind.
		float flow = saturate((1.0 - waveParam.w) / max(Foam1.w, 0.01));
		float windrows = WindrowPattern(absXY, ctx.peakOmega);
		float slick = WindrowSlick(windrows);
		// The normal maps keep most of their strength on top of the FFT waves: they carry the capillary texture
		// and, on rivers, the flowmap normals, so the change from flowing to open water stays visible.
		float windDetail = lerp(0.75, 1.3, saturate(wind / 8.0));
		float detailScale = max(windDetail * (1.0 - 0.6 * slick), max(flow, Surface0.w));

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

		// Whitecaps: the dominant waves' crests breaking now and the trail they leave (thinning over the trail
		// time and drifting with the surface current), kept per cascade in the assemble pass.
		o.crestFoam = saturate(waves.crestTrail);

		o.shoreBreak = waves.shoreBreak;
		o.shoreMask = waves.shoreMask;
		o.shoreCrest = waves.shoreCrest;
		o.beach = ctx.beach;
		o.shoreDelay = ctx.shore.delay * ctx.shore.weight;

		// The swash over the beach: dense foam rides the front of each uprush, and drains away as lace on the
		// backwash. What it leaves on the sand is drawn with the terrain (Lighting), as the water is gone.
		o.swashFoam = 0.0;
		o.foamAdvect = 0.0;
		if (ctx.beach > 0.0) {
			float4 swash = SwashState(absXY, false);
			float cover = swash.x - ctx.elevation;  // how far under the run-up level the bed lies (units)
			// A band a few metres behind the tip, whatever the beach slope.
			float band = max(3.0 * UnitsPerMetre * ctx.shore.slope, 3.0);
			float front = smoothstep(-2.0, 2.0, cover) * smoothstep(band, 0.0, cover);
			float rising = saturate(swash.y / 15.0);
			o.swashFoam = ctx.beach * front * lerp(0.35, 1.0, rising);
			// The foam pattern rides up and down the beach with the swash: the tip's distance from the still waterline.
			o.foamAdvect = ctx.shore.dir * (swash.x / max(ctx.shore.slope, 0.02)) * ctx.beach;
		}
		o.rippleFoam = ripple.z;
		o.depth = ctx.depth;
		// Depth over the bed's slope: how far the shoreline is, so the contact band has the same width on a gentle
		// beach and at a cliff foot.
		o.shoreDistance = ctx.hasTerrain > 0.5 ? ctx.depth / max(length(ctx.terrainGrad), 0.02) : 1e6;
		o.fetchMetres = fetchMetres;
		o.footprint = footprint;
		o.turbidity = 0.0;
		o.dominantWaves = waves.dominant;
		o.dominantK = waves.dominantK;
		o.waveDir = SafeNormalize(float3(lerp(Params1.xy, ctx.shore.dir, ctx.beach), 0.0), float3(Params1.xy, 0.0)).xy;
		return o;
	}

	/**
	 * Sun shadows the waves cast on each other, from the dominant waves at this pixel alone (no texture reads).
	 * Along the sun's azimuth they are locally a sinusoid A cos(k x + phi), fitted to their height h and slope h' here:
	 * A = |(h, h' / k)|, phi = atan2(-h' / k, h). The ray to the sun rises at tan(e) = m A k; the next crest stands
	 * highest above it at cos(u) = sqrt(1 - m^2), sin(u) = -m, and shadows this point when
	 *     sqrt(1 - m^2) - cos(phi) - m (u - phi) > 0.
	 * Nothing is shadowed once the sun is higher than the steepest slope (m >= 1), and where the waves are filtered
	 * away (far off, or calm) A shrinks and the shadows go with it.
	 */
	float WaveSunShadow(SurfaceShading s, float3 toSun)
	{
		float shadow = 1.0;
		float horizontal = length(toSun.xy);
		[branch] if (toSun.z > 0.0 && horizontal > 1e-3 && s.dominantK > 1e-5)
		{
			float2 sunDir = toSun.xy / horizontal;
			// Along the sun's azimuth the waves are as much longer as they cross it at an angle.
			float k = s.dominantK * max(abs(dot(s.waveDir, sunDir)), 0.3);
			float h = s.dominantWaves.x;
			float slope = dot(s.dominantWaves.yz, sunDir) / k;
			float amplitude = sqrt(h * h + slope * slope);
			float m = toSun.z / horizontal / max(amplitude * k, 1e-6);
			if (m < 1.0) {
				float phi = atan2(-slope, h);
				float ahead = frac((-asin(m) - phi) / 6.2831853) * 6.2831853;     // phase to the next crest's highest point
				float clearance = m * ahead + h / amplitude - sqrt(1.0 - m * m);  // ray above that crest, in units of A
				shadow = saturate(0.5 + clearance / 0.3);
			}
		}
		return shadow;
	}

	/// Foam coverage from all sources. `contact` is 0..1 how close the waterline of something rising out of the
	/// water is (shores, rocks, piers, wading legs ...); 0 when unknown.
	/// x surface foam (contact band, breaking shore waves, swash, windrows), y whitecaps (folding crests),
	/// z aeration from splashes and wakes: churned water is full of bubbles but leaves little foam.
	float3 FoamCoverage(SurfaceShading s, float contact)
	{
		// A band along the waterline of rocks, piers and legs. On a beach the shore's foam comes from the waves
		// themselves (breaking crests, the swash), so the waterline is not ringed with standing foam.
		float band = contact * (1.0 - s.beach);
		// Breaking shore waves: dense foam on the crests that break, a lace of old foam between them in the surf zone.
		float breaking = (s.shoreBreak + 0.3 * s.shoreMask) * Shore1.x + s.swashFoam * Shore1.y;
		float aeration = saturate(s.rippleFoam * Foam1.z * 3.0);
		float surface = saturate((band + breaking + s.windrows) * Foam0.z);
		// The amount already sets how much of the sea breaks; this only switches it off at zero.
		float whitecap = saturate(s.crestFoam) * saturate(Foam2.x * 4.0);
		return float3(surface, whitecap, aeration);
	}

	float3 DebugView(uint mode, SurfaceShading s, float foam, float4 waveState)
	{
		switch (mode) {
		case 1:
			return s.normal * 0.5 + 0.5;
		case 2:
			return foam.xxx;
		case 3:
			// Depth (red, 10 m), beach where waves break and run up (green), shoaling delay of the wave rays (blue, 30 s).
			return float3(saturate(s.depth / (10.0 * UnitsPerMetre)), s.beach, saturate(s.shoreDelay / 30.0));
		case 4:
			return float3(saturate(1.0 - s.jacobian), saturate(-s.jacobian), s.shoreBreak);
		case 5:
			return s.roughness.xxx;
		case 6:
			// Height in units of Hs / 2: crests red, troughs green.
			return float3(saturate(2.0 * waveState.x / max(Optics0.w, 1.0)) * 0.5 + 0.5, saturate(-2.0 * waveState.x / max(Optics0.w, 1.0)), 0.0);
		case 7:
			{
				// Fetch on a log scale: blue 100 m (pond, river), green ~3 km (bay, harbour), red 100 km+ (open sea).
				float t = saturate((log10(max(s.fetchMetres, 10.0)) - 2.0) / 3.0);
				return lerp(lerp(float3(0.0, 0.2, 1.0), float3(0.0, 1.0, 0.2), saturate(t * 2.0)), float3(1.0, 0.1, 0.0), saturate(t * 2.0 - 1.0));
			}
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
