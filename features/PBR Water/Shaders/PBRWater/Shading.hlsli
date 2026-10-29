#ifndef __PBR_WATER_SHADING_HLSLI__
#define __PBR_WATER_SHADING_HLSLI__

#include "Common/BRDF.hlsli"
#include "PBRWater/Foam.hlsli"
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
//   Foam      : a coverage from every source (Jacobian whitecaps, breaking shore waves, the swash front, the
//               water's edge, wakes) drawn with the baked foam texture (PBRWater/Foam.hlsli).
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
	// The foam's coverage (PBRWater.hlsli: made where waves break, kept by FoamSimCS) is drawn by PBRWaterFoam
	// (Foam.hlsli) with the baked foam texture, thresholded at the coverage (Crest's black-point fade) and churned by
	// Fluid Flux's flow advection.
	// ------------------------------------------------------------------------

	/// Size of the foam texture's tile (units): FoamPattern bakes PBRWaterFoam::BubblesPerTile large bubbles of 20 cm
	/// across it, with bubbles 2.7 times smaller in the walls between them.
	static const float FoamTile = 0.2 * UnitsPerMetre * PBRWaterFoam::BubblesPerTile;
	/// Fluid Flux's advection: the pattern slides 30 cm downwind and back every five seconds, so it churns in place.
	static const float FoamAdvectionRate = 0.2;  // cycles per second
	static const float FoamAdvectionSlide = 0.3 * UnitsPerMetre;
	/// Depth the bubble specks are seen at under the surface (units), for their parallax.
	static const float BubbleDepth = 0.3 * UnitsPerMetre;

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
		float foamEigen;   // smallest eigenvalue of the foam Jacobian (debug)
		float whitecap;    // 0..1 a whitecap breaking at this pixel now (debug)
		float foam;        // foam thickness: what broke lately (FoamSimCS) and what is breaking at this pixel; 1 covers the surface
		float swashTip;    // the swash tip at this pixel (units above the still water; SwashTip)
		float swashLens;   // its lens coefficient (1/unit; SwashTop)
		float foamKept;  // the simulation's part of it: what the swash left where the water has drained
		float freshness;   // 0..1 the foam is being made now (brighter; aerates the water below)
		float breaking;    // 0..1 a shore wave breaking here now (stirs up the bottom)
		float beach;       // 0..1 waves break on this shore (no surf or swash on rock faces and cliffs)
		float train;       // 0..1 share of the shore wave train (debug)
		float room;        // 0..1 room the shore field gives the train here (debug)
		float exposure;    // 0..1 exposure to the swell (debug)
		float levelError;  // the shore field's still water level minus this water's (units; < -1e8 outside the field) (debug)
		float breakStage;  // 0..1 how far the shore waves here have got with breaking (debug)
		float rippleFoam;  // air churned in by bodies moving through the water
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
	/// The debug view of the foam simulation.
	static const uint DebugFoamSimulation = 13;

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
		o.foamEigen = waves.foamEigen;
		o.whitecap = Whitecap(waves);

		// Foam (PBRWater.hlsli): what broke lately and has not thinned out yet (FoamSimCS), and what breaks at this pixel
		// now, which keeps the crest line as sharp as the pixels. Foam is only made on water: the shore train runs on up
		// the beach, but breaks only where its surface or the swash sheet covers the bed.
		float2 kept = SampleFoamSim(absXY);
		float2 made = BreakingFoam(waves, ctx);
		// The swash tip per pixel: interpolated from the vertices it is linear across each triangle, so the sheet and its
		// foam ended in straight lines along the triangle edges. The front's foam is made here too: the simulation's
		// texels (~half a metre) stepped and diamonded its leading edge.
		// Also where only the vertices around see the swash (steep rocks and banks where it does not break): keeping the
		// interpolated tip there put its triangles back into the sheet. SwashTip fades out with the beach, so this is smooth.
		o.swashTip = shoreState.x;
		o.swashLens = shoreState.y;
		[branch] if (ctx.beach > 0.0 || shoreState.x != 0.0)
		{
			float2 swashTip = SwashTip(waveParam.xyz, ctx, false);
			o.swashTip = swashTip.x;
			o.swashLens = swashTip.y;
		}
		if (ctx.hasTerrain > 0.5)
			made *= smoothstep(0.0, 0.03 * UnitsPerMetre, max(waves.displacement.z, SwashTop(ctx.elevation, o.swashTip, o.swashLens)) - ctx.elevation);
		o.foam = max(kept.x, made.x);
		o.foamKept = kept.x;
		o.freshness = max(kept.y, made.y);
		o.breaking = saturate(waves.whitewater);
		o.beach = ctx.beach;
		o.train = ctx.train;
		o.room = ctx.shore.room;
		o.exposure = ctx.shore.exposure;
		o.levelError = 0.0;
		[branch] if ((uint)Draw0.z == DebugShoreLevel)
			o.levelError = ShoreLevel(absXY) - (waveParam.z + FrameBuffer::CameraPosAdjust.z);
		o.breakStage = waves.breakStage;

		o.rippleFoam = ripple.z;
		o.depth = ctx.depth;
		o.height = waves.displacement.z;
		o.elevation = ctx.hasTerrain > 0.5 ? ctx.elevation : -1e6;
		o.bedSlope = length(ctx.terrainGrad);
		o.fetchMetres = ctx.fetchMetres;
		o.footprint = footprint;
		o.turbidity = 0.0;
		o.waveDir = SafeNormalize(float3(lerp(Params1.xy, ctx.shore.direction, ctx.train), 0.0), float3(Params1.xy, 0.0)).xy;
		return o;
	}

	/// x foam coverage, y aeration: the air the breaking crests and anything moving through the water churn in (bubbles).
	/// The churn saturates softly, so a wake fades out at its rim instead of ending in a circle.
	float2 FoamCoverage(SurfaceShading s)
	{
		// Even the densest whitecap keeps its holes and bubbles: at full coverage the black point turned it into a
		// featureless white sheet (thicker foam lasts longer instead).
		return float2(0.88 * saturate(s.foam), s.freshness);
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
			// Foam Jacobian: its compression (red), the crests breaking now (green) and the shore waves' whitewater (blue).
			return float3(saturate(1.0 - s.foamEigen), s.whitecap, s.breaking);
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
		case DebugFoamSimulation:
			// The foam: its coverage (red), how fresh it is (green) and how much thicker than covering it is (blue).
			return float3(saturate(s.foam), s.freshness, saturate((s.foam - 1.0) * 0.5));
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
