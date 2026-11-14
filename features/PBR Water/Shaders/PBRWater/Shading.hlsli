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
//   Sun       : the waves shade themselves from the sun: each point marches towards it through a heightfield of the
//               displaced surface around the eye (PBRWater::UpdateSunMap). The water it crosses hides the sun's glint
//               and attenuates the light reaching the foam and the water below; the crest glow reads its thickness.
//   Wind      : micro-roughness from the local wind (Cox & Munk 1954): gusts sweep across the water as
//               darker, rougher patches (cat's paws), the lee of the upwind shore lies glassy and rain
//               roughens the surface.
//   Foam      : a coverage made where the waves break (PBRWater.hlsli) drawn with the baked foam texture
//               (PBRWater/Foam.hlsli).
// ============================================================================

namespace PBRWater
{
	Texture2D<float> WaveHeightfield : register(t27);  // the highest water surface around the eye, as depth against SunMap1 (PBRWater::UpdateSunMap)

	/// Height (absolute z) of the highest water surface over an absolute point; far below anything off the heightfield.
	float WaveHeightfieldTop(float2 absXY)
	{
		float2 uv = (absXY - SunMap0.xy) * SunMap0.z * float2(1.0, -1.0) + 0.5;
		if (any(uv != saturate(uv)))
			return -1e9;
		float depth = WaveHeightfield.SampleLevel(LinearClampSampler, uv, 0);
		return depth < 0.9999 ? SunMap1.x + (0.5 - depth) * 2.0 * SunMap1.y : -1e9;
	}

	/// The sunlight's way to a point of the surface through the waves around it.
	struct SunThroughWaves
	{
		float path;     // water it crosses (units)
		float visible;  // 0..1 how much of the sun is seen past the crests: its glint
		bool valid;     // the point is on the heightfield
	};

	/**
	 * Marches from a point of the water surface towards the sun through the heightfield of the displaced surface, adding up
	 * the stretches below the water: a crest between the point and the sun, the point's own wave on its far side from the
	 * sun, the lip curling over a breaker's face. Air between two waves counts for nothing. Jittered per pixel and frame:
	 * the temporal filter integrates the steps into a smooth path, so thin lips are not missed between them.
	 * @param positionWS camera-relative position of the surface
	 * @param toSun      direction towards the sun
	 * @param stillLevel camera-relative still water level here (SurfaceDisplacement::shore.w)
	 * @param jitter     0..1 per pixel and frame
	 */
	SunThroughWaves SunPathThroughWaves(float3 positionWS, float3 toSun, float stillLevel, float jitter)
	{
		SunThroughWaves o;
		o.path = 0.0;
		o.visible = 1.0;
		o.valid = false;
		if (SunMap0.w < 0.5 || toSun.z < 0.02)
			return o;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		// Fades out towards the heightfield's edge, past which the crests are missing.
		float2 edge = abs(absPos.xy - SunMap0.xy) * (2.0 * SunMap0.z);
		float fade = saturate((1.0 - max(edge.x, edge.y)) / 0.2);
		[branch] if (fade <= 0.0) return o;
		o.valid = true;

		// Nothing stands higher than the still water plus the largest displacement: the march ends there.
		float3 dir = normalize(toSun);
		float rise = stillLevel + FrameBuffer::CameraPosAdjust.z + DisplacementLimit() - absPos.z;
		float reach = min(max(rise, 0.0) / dir.z, 20.0 * UnitsPerMetre);
		static const uint Steps = 12;
		float stepLength = reach / Steps;
		// The heightfield's grid filters out waves shorter than its spacing, which the surface here still carries.
		static const float Bias = 0.04 * UnitsPerMetre;
		static const float Soft = 0.04 * UnitsPerMetre;
		float path = 0.0;
		[loop] for (uint i = 0; i < Steps; i++)
		{
			float t = (i + jitter) * stepLength;
			float3 p = absPos + dir * t;
			path += saturate((WaveHeightfieldTop(p.xy) - p.z - Bias) / Soft) * stepLength;
		}
		o.path = path * fade;
		// Any water at all in the way refracts the sun's direct beam away: its glint is gone past a few centimetres.
		o.visible = saturate(1.0 - o.path / (0.1 * UnitsPerMetre));
		return o;
	}

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
	float PointLightEmitterRadius() { return Tune5.x; }

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

	/// Size of the foam texture's tile (units): FoamPattern bakes PBRWaterFoam::BubblesPerTile large bubbles of 20 cm
	/// across it, with bubbles 2.7 times smaller in the walls between them.
	float FoamTile() { return Tune5.z * UnitsPerMetre * PBRWaterFoam::BubblesPerTile; }

	// ------------------------------------------------------------------------
	// Per-pixel surface
	// ------------------------------------------------------------------------
	struct SurfaceShading
	{
		float3 normal;
		float roughness;
		float fftFoam;       // 0..1 the open sea's foam here (FftFoam): drawn raw, never through the shore foam's pattern
		float foam;          // 0..1 the shore waves' white water at this pixel
		float swashSurface;  // the surf simulation's surface at this pixel (units above the still water; SurfSheet)
		float swashDepth;    // its depth (units; 0 where dry)
		float swashFoam;     // 0..1 the white water it carries
		float simShare;      // 0..1 share of the surf simulation in the surface here (ComposeSurf)
		float3 waveInfo;     // the shore wave here, for the debug view
		float3 waveNormal;   // the waves' own normal, without the ripples and detail maps (the foam's projection)
		float breakerFoam;   // 0..1 the breaking wave's own white water (BreakerFoam), with the surf's leftover foam on faces that are not flat: drawn raw, like fftFoam
		float surfFoam;      // 0..1 the foam the surf left on the water here (ShoreFoamAt)
		float clearFace;     // 0..1 a breaker's face about to plunge: clear water, no leftover foam (ShoreClearFace)
		float bore;          // 0..1 the wave has flattened into a bore: its foam takes the texture's pattern
		float crest;         // units above the still water the shore wave's crest stands here (ShoreTrain::crest)
		float surf;          // 0..1 real breakers run onto this shore: the water's edge carries foam
		float beach;         // 0..1 waves break on this shore (no surf or swash on rock faces and cliffs)
		float train;         // 0..1 share of the shore wave train (debug)
		float room;          // 0..1 room the shore field gives the train here (debug)
		float exposure;      // 0..1 exposure to the swell (debug)
		float levelError;    // the shore field's still water level minus this water's (units; < -1e8 outside the field) (debug)
		float breakStage;    // 0..1 how far the shore waves here have got with breaking (stirs up the bottom)
		float depth;
		float height;     // water surface above the still water at this pixel (units): the waves or the surf simulation (far below the ground where that is dry)
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
	/// The debug view of one shore wave.
	static const uint DebugShoreWave = 13;
	/// The debug view of the waves' sun shadows (Water.hlsl).
	static const uint DebugWaveSun = 14;

	/**
	 * Combines the analytic wave normal, the interactive ripples and the vanilla detail normal.
	 * @param waveParam  xyz undisplaced camera-relative position, w flow damping
	 * @param flowVelocity the river current here (units/s, interpolated from the vertices; FlowVelocity)
	 * @param gust       wind gust factor (WindGust, interpolated from the vertices)
	 * @param detailNormal vanilla normal-map normal (z up)
	 * @param detailVariance slope variance the mip filter averaged out of the vanilla normal maps (Toksvig), at their
	 *                       blended amplitude
	 */
	SurfaceShading ShadeSurface(float4 waveParam, float2 flowVelocity, float gust, float3 detailNormal, float detailVariance)
	{
		SurfaceShading o;

		float2 dpdx = ddx_fine(waveParam.xy);
		float2 dpdy = ddy_fine(waveParam.xy);
		float footprint = max(length(dpdx), length(dpdy));

		float2 absXY = waveParam.xy + FrameBuffer::CameraPosAdjust.xy;
		o.surfFoam = ShoreFoamAt(absXY);

		WaveContext ctx = BuildWaveContext(waveParam.xyz, waveParam.w);
		ctx.flow = flowVelocity;
		WaveResult waves = EvaluateWavesFiltered(waveParam.xyz, ctx, o.surfFoam);
		float2 ripple = RippleSlope(waveParam.xyz, ctx.depth);
		float wind = LocalWindSpeed(gust, ctx.fetchMetres);
		// The vanilla normal maps are the wind's ripples: calm patches turn glassy, gusts ripple the water.
		// River currents and rain ripple it whatever the wind.
		float flow = saturate((1.0 - waveParam.w) / max(Foam1.w, 0.01));
		float windrows = WindrowPattern(absXY, ctx.peakOmega);
		float slick = WindrowSlick(windrows);
		// The normal maps keep most of their strength on top of the FFT waves: they carry the capillary texture
		// and, on rivers, the flowmap normals, so the change from flowing to open water stays visible.
		float windDetail = lerp(Tune4.z, Tune4.w, saturate(wind / 8.0));
		float detailScale = max(windDetail * (1.0 - 0.6 * slick), max(flow, Surface0.w));
		// A breaker's face drawn up to plunge is stretched glassy: the wind's ripples on it are pulled out along the face.
		detailScale *= 1.0 - 0.5 * waves.clearFace;
		o.clearFace = waves.clearFace;

		// Add surface slopes: waves + ripples + vanilla detail (partial-derivative blending).
		float2 slope = -waves.normal.xy / max(waves.normal.z, 0.05) + ripple;
		float2 detailSlope = detailNormal.xy / max(detailNormal.z, 0.05) * detailScale;
		slope += detailSlope;
		o.normal = normalize(float3(-slope, 1.0));
		// Slopes (a height field) cannot hold a surface turned vertical or past it: a breaker's face, and the lip of a curl whose
		// underside faces down. There the ripples and the detail maps tilt the waves' own normal within its tangent plane
		// instead. Both forms give the waves' own normal exactly, so blending them by how steep the surface is leaves no seam.
		// (Blended by the curl's weight, which falls off sharply at the lip's edge, the two were averaged across a normal
		// facing down and drew a line of normals along the crest.)
		float steep = smoothstep(0.35, 0.1, waves.normal.z);
		[branch] if (steep > 0.0)
		{
			// The detail's slopes laid onto the face: what runs along the crest tilts the normal along the crest, what runs
			// along the way the face looks tilts it up or down the face. (Projected into the tangent plane as a horizontal
			// tilt, all but the along-crest part was lost on a face near vertical, and breakers' faces shaded flat.)
			float2 facing = SafeNormalize(float3(waves.normal.xy, 0.0), float3(1.0, 0.0, 0.0)).xy;
			float3 alongCrest = float3(-facing.y, facing.x, 0.0);
			float3 upFace = SafeNormalize(cross(waves.normal, alongCrest), float3(0.0, 0.0, 1.0));
			float2 detail = ripple + detailSlope;
			float3 tilted = SafeNormalize(waves.normal - alongCrest * dot(detail, alongCrest.xy) - upFace * dot(detail, facing), waves.normal);
			o.normal = SafeNormalize(lerp(o.normal, tilted, steep), tilted);
		}

		// Specular anti-aliasing: the normal varies within the pixel, so widen the lobe by that variance. Without it
		// a mirror-like sun glint lands on single pixels at ~1e4x the sky radiance and blooms into blobs.
		//  - waves: the slope variance their mip filtering removed (LEAN), from the cascades and the shore train;
		//  - detail maps: Toksvig (2005) from the shortening of their mip-filtered normals (Water.hlsl).
		// All of it comes from filtered textures, none from screen derivatives: those are shared by 2x2 pixel quads
		// and turned the roughness into blocks (the ripples are magnified, never sub-pixel, so they need no term).
		float windVariance = 0.5 * (WindMeanSquareSlope(wind) * (1.0 - 0.8 * slick) + RainMeanSquareSlope());
		float mapVariance = detailVariance * detailScale * detailScale;
		o.roughness = CombinedRoughness(Light0.x + waves.fftFoam * Tune2.w, waves.slopeVariance + windVariance + mapVariance);
		o.windSpeed = wind;
		o.fftFoam = waves.fftFoam;

		// Foam (PBRWater.hlsli): the shore waves carry theirs; the open sea's is o.fftFoam.
		// The breakers' white water, the crest of a curling lip included: breaking starts at the lip's tip. (Thinning it by the
		// lip's stretch, from when its pattern was drawn at the rest position and drawn out along the lip, cut a dark line
		// along every curling crest.)
		o.foam = waves.shoreFoam;
		// The surf simulation per pixel (ComposeSurf): interpolated from the vertices it is linear across each triangle, so
		// the sheet ended in straight lines along the triangle edges.
		o.swashSurface = waves.sheetSurface;
		o.swashDepth = waves.sheetDepth;
		o.swashFoam = waves.sheetFoam;
		o.simShare = waves.surf;
		o.beach = ctx.beach;
		o.surf = ctx.beach * BreakerStrength(ctx);
		o.train = ctx.train;
		o.room = ctx.shore.room;
		o.exposure = ctx.shore.exposure;
		o.waveInfo = 0.0;
		[branch] if ((uint)Draw0.z == DebugShoreWave && ctx.train > 0.0)
		{
			ShoreWave wave = ShoreWaveAt(absXY, CoastPoint(absXY, ctx.shore), ctx, Shore2.x);
			o.waveInfo = float3(wave.phase, wave.stage, wave.height / 4.0) * ctx.train;
		}
		o.levelError = 0.0;
		[branch] if ((uint)Draw0.z == DebugShoreLevel)
			o.levelError = ShoreLevel(absXY) - (waveParam.z + FrameBuffer::CameraPosAdjust.z);
		o.breakStage = waves.breakStage;
		o.depth = ctx.depth;
		o.height = waves.displacement.z;
		o.elevation = ctx.hasTerrain > 0.5 ? ctx.elevation : -1e6;
		o.bedSlope = length(ctx.terrainGrad);
		o.fetchMetres = ctx.fetchMetres;
		o.footprint = footprint;
		o.turbidity = 0.0;
		o.waveNormal = waves.normal;
		o.breakerFoam = waves.breakerFoam;
		o.bore = waves.bore;
		o.crest = waves.crest;
		o.waveDir = SafeNormalize(float3(lerp(Params1.xy, ctx.shore.direction, ctx.train), 0.0), float3(Params1.xy, 0.0)).xy;
		return o;
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
			// Cascade foam: the open sea's (red), the shore breakers' (green; BreakerFoam) and the shore waves' break stage (blue).
			return float3(s.fftFoam, s.breakerFoam, s.breakStage);
		case 5:
			return s.roughness.xxx;
		case 6:
			// Height against the still water, signed and in metres: red 0.5 at the still water, 0 at -4 m, 1 at +4 m. Green the
			// metres again as contour lines, blue how far the shore waves here have got with breaking.
			{
				float metres = waveHeight * MetresPerUnit;
				return float3(saturate(0.5 + metres / 8.0), frac(metres), s.breakStage);
			}
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
		case DebugShoreWave:
			// The shore wave here: where on it this point is (red, 0 ahead of the wave to 1 behind), how far through its life
			// (green, 0.5 reaching its limit) and its height (blue, 4 m).
			return s.waveInfo;
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
