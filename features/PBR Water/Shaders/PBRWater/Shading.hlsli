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
//               motivated coverage (breaking shore waves, the swash front, wakes).
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
	 * Wind speed (m/s) just above the water, with the gust factor `gust` there (WindGust). Close to the upwind shore
	 * (short fetch) the water lies in the lee, where the wind has not yet reached the surface.
	 */
	float LocalWindSpeed(float gust, float fetchMetres)
	{
		float shelter = sqrt(saturate(fetchMetres / 150.0));
		return Params0.y * gust * lerp(0.25, 1.0, shelter);
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
	 * the lines meander and break up along their length. In light winds they collect natural surface films,
	 * which damp the capillary ripples into glassy slicks.
	 */
	float WindrowPattern(float2 absXY, float peakOmega)
	{
		if (Surface1.x <= 0.0 || Params0.y < 1.5)
			return 0.0;
		float wavelength = Math::TAU * Gravity() / max(peakOmega * peakOmega, 1e-4);
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
	/// Largest foam pattern cell (units); the foam mixes sizes from it down to a tenth of it.
	static const float FoamCellCoarse = 0.2 * UnitsPerMetre;

	/// Size (units) of foam pattern cell `level`, 0 (0.2 m) .. 3 (0.02 m), geometric.
	float FoamCellSize(float level)
	{
		return FoamCellCoarse * exp2(-1.1073 * level);  // 10^(-level / 3)
	}

	float FoamLace(float2 p, float coverage, float footprint, float time)
	{
		// FoamLacePattern's clumps raise the coverage by at most 1.55x and draw nothing below 0.002: most of the water has
		// no foam, and skips the pattern's noise altogether.
		[branch] if (coverage * 1.55 <= 0.002) return 0.0;
		// Patches of foam of four sizes, 0.2 m cells down to 0.02 m, drifting into each other, so the foam is never one
		// stamp repeated across every breaker. Each pixel blends the two neighbouring sizes, each only evaluated where it
		// shows.
		float level = saturate(Fbm2(p / (11.0 * FoamCellCoarse) + 3.1) * 1.3 - 0.15) * 2.999;
		float index = floor(level);
		float t = smoothstep(0.3, 0.7, level - index);
		float a = 0.0, b = 0.0;
		[branch] if (t < 0.999)
		{
			float size = FoamCellSize(index);
			a = FoamLacePattern(p / size + index * 17.3, coverage, footprint / size, time);
		}
		[branch] if (t > 0.001)
		{
			float size = FoamCellSize(index + 1.0);
			b = FoamLacePattern(p / size + (index + 1.0) * 17.3, coverage, footprint / size, time);
		}
		return lerp(a, b, t);
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
		density = saturate(density * Foam0.y);
		if (density <= 0.002)
			return 0.0;
		const float cellSize = 4.0;
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
				float2 wander = float2(sin(time * (0.7 + h.y) + h.x * Math::TAU), cos(time * (0.9 + h.x) + h.y * Math::TAU)) * 0.25;
				float2 centre = c + 0.5 + (h - 0.5) * 0.7 + wander;
				// Fewer and smaller bubbles in less aerated water, so a bubbly patch thins out at its edge.
				float present = smoothstep(0.0, 0.15, density - Hash12(c + 1.3));
				float radius = (0.08 + 0.14 * Hash12(c + 9.1)) * sin(life * Math::PI) * lerp(0.5, 1.0, present);
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
		float2 dpdx = ddx_fine(positionWS);
		float2 dpdy = ddy_fine(positionWS);
		float dfdx = ddx_fine(value);
		float dfdy = ddy_fine(value);
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
		float shoreBreak;   // a shore wave breaking here
		float beach;        // 0..1 waves break on this shore (no surf or swash on rock faces and cliffs)
		float train;        // 0..1 share of the shore wave train (debug)
		float room;         // 0..1 room the shore field gives the train here (debug)
		float exposure;     // 0..1 exposure to the swell (debug)
		float levelError;   // the shore field's still water level minus this water's (units; < -1e8 outside the field) (debug)
		float breakStage;   // 0..1 how far the shore waves here have got with breaking (debug)
		float swashFoam;    // foam riding the front of the swash up the beach
		float edgeFoam;     // foam lingering in the thin water at the water's edge
		float residueFoam;  // foam the drained swash left on the sand (shown where the ground is dry)
		float rippleFoam;   // air churned in by bodies moving through the water
		float depth;
		float height;     // water surface above the still water at this pixel (units): waves, without the swash
		float elevation;  // bed above the still water at this pixel (units, -1e6 unknown)
		float bedSlope;   // |grad| of the bed
		float fetchMetres;
		float footprint;
		float turbidity;  // column sediment, filled in by the water column shading
		float windSpeed;  // local wind at the surface (m/s)
		float2 waveDir;   // the direction the waves travel in (wind, or towards the beach)
	};

	/// The debug view (Settings::DebugView) that compares the shore field's water level with this water's.
	static const uint DebugShoreLevel = 12;

	/**
	 * Combines the analytic wave normal, the interactive ripples and the vanilla detail normal.
	 * @param waveParam  xyz undisplaced camera-relative position, w flow damping
	 * @param shoreState the vertex shader's swash (SurfaceDisplacement::shore): x tip, y swash mark
	 * @param gust       wind gust factor (WindGust, interpolated from the vertices)
	 * @param detailNormal vanilla normal-map normal (z up)
	 * @param detailVariance slope variance the mip filter averaged out of the vanilla normal maps (Toksvig), at their
	 *                       blended amplitude
	 */
	SurfaceShading ShadeSurface(float4 waveParam, float4 shoreState, float gust, float3 detailNormal, float detailVariance)
	{
		SurfaceShading o;

		float2 dpdx = ddx_fine(waveParam.xy);
		float2 dpdy = ddy_fine(waveParam.xy);
		float footprint = max(length(dpdx), length(dpdy));

		WaveContext ctx = BuildWaveContext(waveParam.xyz, waveParam.w);
		WaveResult waves = EvaluateWavesFiltered(waveParam.xyz, ctx);
		float3 ripple = RippleSlopeFoam(waveParam.xyz, ctx.depth);

		float2 absXY = waveParam.xy + FrameBuffer::CameraPosAdjust.xy;
		float wind = LocalWindSpeed(gust, ctx.fetchMetres);
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
		float2 slope = -waves.normal.xy / max(waves.normal.z, 0.05) + ripple.xy;
		slope += detailNormal.xy / max(detailNormal.z, 0.05) * detailScale;
		o.normal = normalize(float3(-slope, 1.0));

		// Specular anti-aliasing: the normal varies within the pixel, so widen the lobe by that variance. Without it
		// a mirror-like sun glint lands on single pixels at ~1e4x the sky radiance and blooms into blobs.
		//  - waves: the slope variance their mip filtering removed (LEAN), from the cascades and the shore train;
		//  - detail maps: Toksvig (2005) from the shortening of their mip-filtered normals (Water.hlsl).
		// All of it comes from filtered textures, none from screen derivatives: those are shared by 2x2 pixel quads
		// and turned the roughness into blocks (the ripples are magnified, never sub-pixel, so they need no term).
		float windVariance = 0.5 * (WindMeanSquareSlope(wind) * (1.0 - 0.8 * slick) + RainMeanSquareSlope());
		float mapVariance = detailVariance * detailScale * detailScale;
		o.roughness = CombinedRoughness(Light0.x, waves.slopeVariance + windVariance + mapVariance);
		o.windSpeed = wind;
		o.jacobian = waves.jacobian;

		o.shoreBreak = waves.shoreBreak;
		o.beach = ctx.beach;
		o.train = ctx.train;
		o.room = ctx.shore.room;
		o.exposure = ctx.shore.exposure;
		o.levelError = 0.0;
		[branch] if ((uint)Draw0.z == DebugShoreLevel)
			o.levelError = ShoreLevel(absXY) - (waveParam.z + FrameBuffer::CameraPosAdjust.z);
		o.breakStage = waves.breakStage;

		// The swash over the beach: foam rides the front of each uprush. What it leaves on the sand is drawn with the
		// terrain (Lighting), as the water is gone. The tip comes from the vertices (it varies smoothly), the bed per
		// pixel. The foam band moves with the tip; the pattern in it stays put and churns (carrying it with the tip would
		// shear it wherever the run-up varies along the shore).
		o.swashFoam = 0.0;
		if (ctx.beach > 0.0) {
			float cover = shoreState.x - ctx.elevation;  // how far under the tip level the bed lies (units)
			// A band a few metres behind the tip, whatever the beach slope, its leading edge feathered over a metre.
			float band = max(3.0 * UnitsPerMetre * ctx.shore.slope, 3.0);
			float lead = max(0.5 * UnitsPerMetre * ctx.shore.slope, 1.0);
			float front = smoothstep(-lead, lead, cover) * smoothstep(band, 0.0, cover);
			o.swashFoam = ctx.beach * front * 0.75 * BreakerFoam(ctx);
		}
		// The foam the drained crests left on the sand up to the swash mark, until the next uprush covers it.
		o.residueFoam = 0.0;
		[branch] if (ctx.beach > 0.0 && ctx.hasTerrain > 0.5 && ctx.elevation < shoreState.y)
			o.residueFoam = SwashCrests(waveParam.xyz, ctx, false, ctx.elevation).z * BreakerFoam(ctx);
		o.rippleFoam = ripple.z;

		// Foam lingering at the water's edge, in the last few centimetres of water over the bed - from the terrain heights,
		// never the depth buffer (that put foam in the shape of whatever stood in the water). A band of about the same
		// width on a gentle beach and a steep bank (its depth follows the bed's slope); always there a little, and fresh
		// where a wave has just broken or run up, so on a stream or a pond it is a faint rim and in the surf a wide one.
		o.edgeFoam = 0.0;
		[branch] if (ctx.hasTerrain > 0.5 && Foam0.x > 0.0)
		{
			float thickness = max(waves.displacement.z, SwashTop(ctx.elevation, shoreState.x)) - ctx.elevation;
			float activity = saturate(max(o.shoreBreak, o.swashFoam) * 2.0);
			float width = (0.4 + 1.2 * activity) * UnitsPerMetre;  // horizontal
			float depthWidth = clamp(width * length(ctx.terrainGrad), 0.03 * UnitsPerMetre, 0.3 * UnitsPerMetre);
			// Both edges feathered: in from the waterline over a few centimetres, out into the deeper water over the band.
			float band = smoothstep(0.0, 0.04 * UnitsPerMetre, thickness) * (1.0 - smoothstep(0.2 * depthWidth, depthWidth, thickness));
			o.edgeFoam = band * lerp(0.4, 1.0, activity) * Foam0.x;
		}
		o.depth = ctx.depth;
		o.height = waves.displacement.z;
		o.elevation = ctx.hasTerrain > 0.5 ? ctx.elevation : -1e6;
		o.bedSlope = length(ctx.terrainGrad);
		o.fetchMetres = ctx.fetchMetres;
		o.footprint = footprint;
		o.turbidity = 0.0;
		float2 shoreDir = SafeNormalize(float3(ctx.shore.slownessX.x, ctx.shore.slownessY.x, 0.0), float3(Params1.xy, 0.0)).xy;
		o.waveDir = SafeNormalize(float3(lerp(Params1.xy, shoreDir, ctx.train), 0.0), float3(Params1.xy, 0.0)).xy;
		return o;
	}

	/// Foam coverage: x surface foam on the breaking shore waves, the front of the swash and the water's edge, y aeration:
	/// the air the breakers and anything moving through the water churn in (bubbles). Only where a wave is breaking now
	/// (the surf zone between the breakers is not blanketed) and along the edge. The sources overlap (a breaking crest
	/// runs into the edge band): the densest one counts, never their sum, which doubled the foam where they met. The
	/// calm edge rim makes no bubbles (it drew a ring of them a set distance off every shore), and the churn saturates
	/// softly so a wake fades out at its rim instead of ending in a circle.
	float2 FoamCoverage(SurfaceShading s)
	{
		float breaking = saturate(max(s.shoreBreak * Shore1.x, s.swashFoam * Shore1.y) * Foam0.z);
		float churn = 1.0 - exp(-2.0 * s.rippleFoam * Foam1.z);
		return float2(max(breaking, saturate(s.edgeFoam * Foam0.z)), max(breaking, churn));
	}

	float3 DebugView(uint mode, SurfaceShading s, float foam, float waveHeight)
	{
		switch (mode) {
		case 1:
			return s.normal * 0.5 + 0.5;
		case 2:
			return foam.xxx;
		case 3:
			// Depth (red, 10 m), beach where waves break and run up (green), break stage of the shore waves (blue).
			return float3(saturate(s.depth / (10.0 * UnitsPerMetre)), s.beach, s.breakStage);
		case 4:
			return float3(saturate(1.0 - s.jacobian), saturate(-s.jacobian), s.shoreBreak);
		case 5:
			return s.roughness.xxx;
		case 6:
			// Height in units of Hs / 2: crests red, troughs green.
			return float3(saturate(2.0 * waveHeight / max(Optics0.w, 1.0)) * 0.5 + 0.5, saturate(-2.0 * waveHeight / max(Optics0.w, 1.0)), 0.0);
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
			// Local wind: black calm, white 15 m/s.
			return saturate(s.windSpeed / 15.0).xxx;
		case 10:
			// Shore field: share of the shore wave train (red), room for it (green), exposure to the swell (blue).
			return float3(s.train, s.room, s.exposure);
		case 11:
			// Direction the waves travel in (red east, green north).
			return float3(s.waveDir * 0.5 + 0.5, 0.0);
		case DebugShoreLevel:
			// The shore field's water level against this water's: green within 0.5 m, red where the field's is lower (it
			// sees this water as land), blue where higher; black outside the field.
			if (s.levelError < -1e8)
				return 0.0;
			return float3(saturate(-s.levelError / 140.0 - 0.25), 1.0 - saturate(abs(s.levelError) / 35.0), saturate(s.levelError / 140.0 - 0.25));
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
