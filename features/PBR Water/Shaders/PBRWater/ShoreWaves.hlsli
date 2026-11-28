#ifndef __PBR_WATER_SHORE_WAVES_HLSLI__
#define __PBR_WATER_SHORE_WAVES_HLSLI__

// PBR Water - the shore wave train, its breakers and their curl. Part of PBRWater.hlsli, included where it stands in
// the wave program; not included on its own.

namespace PBRWater
{
	// ------------------------------------------------------------------------
	// Shore wave train
	//
	// After Fluid Flux 2's coastline waves (Imaginary Blend; DaiZiLing's HLSL port, Flux2-Shoreline-Wave-HLSL). Near
	// the shore the sea is one train of its dominant waves running over a coastline map (ShoreField) that holds the
	// travel time tau from each point to the waterline (eikonal |grad tau| = 1 / c(h)): the crests slow, close up over
	// shoals, refract towards the depth contours and wrap into bays.
	//
	// The phase is cut into whole waves, each shaped by one lookup of the profile (ShoreProfile.h) by phase and stage.
	// The stage is the wave's height against the depth under its own crest, so a wave peaks as it shoals, pitches over
	// and collapses into a bore at its limit (McCowan 1894), on a bar as well as on the beach. The profile is flat
	// where one wave ends, so neighbouring waves can differ without a step.
	//
	// Mirrored on the CPU for swimming, buoyancy and floating objects (WaveModel.cpp ShoreTrain).
	// ------------------------------------------------------------------------

	/// A wave's water is carried forward by its wavelength times the profile, but by no more than this many wave heights:
	/// small waves over a long period compress, they do not fold. Mirrors PBRWaterModel::ShoreForwardHeights.
	static const float ShoreForwardHeights = 16.0;

	/// The nearest point of the waterline (absolute units): along the direction of travel, by the signed distance.
	float2 CoastPoint(float2 absXY, ShoreRay ray)
	{
		return absXY + ray.direction * ray.distance;
	}

	/**
	 * One wave's height as a factor on the sea's (mean ~1): a slow group envelope by the wave's number times a
	 * memoryless Rayleigh draw (Longuet-Higgins 1952, 1984). Waves of different heights break at different depths,
	 * which spreads the break points over the surf zone (Thornton and Guza 1983). The draw varies along the coast over
	 * a crest length (ShoreCrestLength; Baker et al. 2023, Nuss et al. 2023), so stretches of one crest break at
	 * different distances. Depends only on the wave's number and the waterline point. Mirrors PBRWaterModel::ShoreSet.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param index the wave's number (ShoreWaveAt)
	 */
	float ShoreSet(float2 coast, float index)
	{
		float crest = max(Tune4.y * Shore3.w, 1.0);
		float group = ValueNoise(coast / (3.0 * crest) + index * float2(0.13, 0.07));
		float2 seed = float2(Hash12(float2(index, 3.7)), Hash12(float2(index, 11.3))) * 61.0;
		// Value noise bunches around its middle: stretched towards a uniform draw before the Rayleigh transform
		// sqrt(-ln(1 - u)), whose mean over a uniform u is Gamma(3/2) = 0.8862.
		float u = clamp(0.5 + 1.7 * (ValueNoise(coast / crest + seed) - 0.5), 0.03, 0.97);
		float rayleigh = sqrt(-log(1.0 - u)) * 1.128379;
		return clamp(1.0 + Tune4.x * (lerp(0.6, 1.4, group) * rayleigh - 1.0), 0.1, ShoreSetCeiling);
	}

	/// Where the bore's front stands in its wave, in phase. Mirrors BoreFront in ShoreProfile.cpp.
	static const float ShoreBoreFront = 0.16;
	/// The phase of a wave at which the depth it answers to is read: where its crest stands as it breaks. Mirrors
	/// PBRWaterModel::ShoreCrestReference.
	static const float ShoreCrestReference = 0.32;
	/// The least a bore's front stands over the bed as it reaches the waterline (units): about u0^2 / 9g for uprushes
	/// of a few metres a second (Shen and Meyer 1963). Only seen where no surf simulation takes the wave over.
	static const float ShoreBoreFloor = 0.25 * UnitsPerMetre;
	/// Over this height above the still water (units) the shore waves end: up the beach the water is the surf
	/// simulation's (ComposeSurf).
	static const float ShoreTrainLanding = 0.1 * UnitsPerMetre;

	/**
	 * The phase of a wave's crest at stage `stage`: it moves forward as the wave shoals (0.5 to 0.32) and, as it
	 * breaks, runs ahead to where a plunger's lip lands and the bore's front forms (0.16). Mirrors Evaluate in
	 * ShoreProfile.cpp and PBRWaterModel's ShoreCrestPhase.
	 */
	float ShoreCrestPhase(float stage)
	{
		return 0.5 - 0.18 * smoothstep(0.0, 0.5, stage) - (0.32 - ShoreBoreFront) * smoothstep(0.50, 0.72, stage);
	}

	/**
	 * How far the crests run ahead of or behind a straight front along the coast (waves): long, slowly drifting bends,
	 * so stretches of beach break at different times. Depends on the waterline point and the clock only; each term
	 * turns over a whole number of times per wrap of the clock (1024 waves). Shore1.x scales it. Mirrors
	 * PBRWaterModel::ShoreWobble.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param clock the shore clock (rad)
	 */
	float ShoreWobble(float2 coast, float clock)
	{
		float2 q = coast / max(2.0 * Shore3.w, 1.0);
		static const float2 d0 = float2(1.0, 0.0);
		static const float2 d1 = float2(-0.504846, 0.863209);
		static const float2 d2 = float2(-0.400799, -0.916166);
		float w = 0.5 * sin(dot(q, d0) + clock * (37.0 / 1024.0)) +
		          0.3 * sin(1.73 * dot(q, d1) - clock * (61.0 / 1024.0)) +
		          0.2 * sin(2.91 * dot(q, d2) + clock * (89.0 / 1024.0));
		return Shore1.x * w;
	}

	/// The phase of the shore train (rad): each cycle is one wave, rising towards the sea. Mirrors the CPU's ShorePhase.
	float ShorePhase(ShoreRay ray, float2 coast, float clock)
	{
		return clock + ray.phaseLag + Math::TAU * (ray.along + ShoreWobble(coast, clock));
	}

	/// One lookup of the wave profile. Mirrors PBRWaterModel::ShoreProfileTable::At.
	float4 ShoreProfileAt(float phase, float stage, float slice)
	{
		static const float2 size = float2(256.0, 64.0);  // ShoreProfileTable::Phases, Stages
		float2 uv = (float2(phase, stage) * (size - 1.0) + 0.5) / size;
		return ShoreProfileTexture.SampleLevel(LinearClampSampler, float3(uv, slice), 0);
	}

	/**
	 * The phase step of the tent that low-passes the breaker's shape to sampling `scale` (units) on a wave of `wavelength`
	 * (units): taps a spacing apart zero the shape's components at the sampling's Nyquist rate. Seen at a grazing angle the
	 * grid's cells are long along the view; a crest sharper than one of them snaps from row to row and draws as a row of
	 * teeth along it. 0 where the table's own resolution is finer.
	 */
	float ShoreTentStep(float scale, float wavelength)
	{
		float h = min(scale / max(wavelength, 1e-3), 0.25);
		return h > 1.0 / 256.0 ? h : 0.0;
	}

	/// ShoreProfileAt low-passed over the phase step `h` (ShoreTentStep).
	float4 ShoreProfileFiltered(float phase, float stage, float h)
	{
		float4 shape = ShoreProfileAt(phase, stage, 0.0);
		[branch] if (h > 0.0)
			shape = 0.5 * shape + 0.25 * (ShoreProfileAt(phase + h, stage, 0.0) + ShoreProfileAt(phase - h, stage, 0.0));
		return shape;
	}

	/**
	 * Weggel's (1972) breaker index for a plane slope `slope` (tan beta), breaker height `heightMetres` and period
	 * `period` (s): gamma = b(m) - a(m) Hb / (g T^2), a = 43.75 (1 - e^-19m), b = 1.56 / (1 + e^-19.5m). 0.78 on a flat
	 * bed; fitted for slopes up to 1:10. Mirrors PBRWaterModel::ShoreBreakerIndex.
	 */
	float ShoreBreakerIndex(float slope, float heightMetres, float period)
	{
		float m = clamp(slope, 0.0, 0.1);
		float a = 43.75 * (1.0 - exp(-19.0 * m));
		float b = 1.56 / (1.0 + exp(-19.5 * m));
		return clamp(b - a * heightMetres / (GravityMetres * period * period), 0.6, 1.2);
	}

	/**
	 * A wave's stage (ShoreProfile.h) from `ratio`, its shoaled height over the breaker limit under its crest. Up to
	 * the limit the stage follows the ratio. Past it, breaking runs on the distance travelled since the break point in
	 * breaker heights, (1 - ratio^-4/5) / (gamma m) on a slope m (Green's law depth h_b ratio^-4/5). A plunger's lip
	 * lands after Galvin's (1969) x_p = (4.0 - 9.25 m) H_b and the tube is gone within about half a plunge length more
	 * (Svendsen 1984; Peregrine 1983); a spiller has no tube, so the plunge length shrinks with `plunging`. The bore
	 * then runs in to the waterline. Mirrors PBRWaterModel::ShoreBreakerStage.
	 * @param index the breaker index gamma (ShoreBreakerIndex)
	 * @param plunging 0..1 how much of a plunger the breaker is (ShoreBreakerPlunging)
	 */
	float ShoreBreakerStage(float ratio, float slope, float index, float plunging)
	{
		float stage = ratio / (1.0 + ratio);
		[branch] if (ratio > 1.0)
		{
			float m = clamp(slope, 0.01, 0.3);
			float depthShare = pow(abs(ratio), -0.8);  // h / h_b
			float travelled = (1.0 - depthShare) / (index * m);
			float plunge = max(4.0 - 9.25 * m, 2.0) * lerp(0.35, 1.0, plunging);
			float transition = 0.5 * plunge;
			// Where the bore has formed, as a share of the breaking depth, and the stage running to 1 at the waterline.
			float formed = 1.0 - (plunge + transition) * index * m;
			if (travelled < plunge)
				stage = 0.5 + 0.16 * travelled / plunge;
			else if (travelled < plunge + transition)
				stage = 0.66 + 0.14 * (travelled - plunge) / transition;
			else
				stage = 0.80 + 0.20 * saturate(1.0 - depthShare / max(formed, 1e-3));
		}
		return stage;
	}

	/**
	 * How much of a plunger a breaker is on a beach of surf similarity `iribarren` (Battjes 1974: spilling below 0.5,
	 * plunging above, surging beyond 3.3; collapsing from about 2). Centred on 0.5. Mirrors
	 * PBRWaterModel::ShoreBreakerPlunging.
	 */
	float ShoreBreakerPlunging(float iribarren)
	{
		return smoothstep(0.35, 0.6, iribarren) * (1.0 - smoothstep(2.0, 3.3, iribarren));
	}

	/// How far a full onshore (+1) or offshore (-1) wind moves the breaker type, as a share of the surf similarity. Mirrors PBRWaterModel::ShoreWindPlunge.
	static const float ShoreWindPlunge = 0.3;

	/**
	 * The surf similarity the breakers take their type from: the beach's, moved by the wind. Onshore wind pushes the
	 * crest over early into a spiller, offshore wind holds the face up into a plunger (Douglass 1990; Feddersen and
	 * Fincham 2023). Whether waves break here at all (WaveContext::beach) stays the beach's. Mirrors
	 * PBRWaterModel::ShoreBreakerIribarren.
	 */
	float ShoreBreakerIribarren(WaveContext ctx)
	{
		return ctx.iribarren * (1.0 - ShoreWindPlunge * ctx.onshoreWind);
	}

	/// The bore's crest as a share of the wave's height: the profile's collapse leaves 1 - 0.42 of it (ShoreProfile.cpp).
	static const float ShoreBoreCrest = 0.58;

	/**
	 * The share of the depth a broken wave's height saturates at in the inner surf on a bed of slope `slope` (tan
	 * beta): about 0.4 on a gentle beach to 0.6 on a steep one (Thornton and Guza 1983; Raubenheimer, Guza and Elgar
	 * 1996).
	 */
	float ShoreSaturatedShare(float slope)
	{
		return 0.4 + 0.2 * saturate((clamp(slope, 0.01, 0.3) - 0.02) / 0.08);
	}

	/**
	 * A breaking wave's height (m) once it is a bore, from its height `height` under the breaker limit (gamma h, gamma
	 * up to 1.2). The bore is lower (the profile's collapse leaves 0.58 of the height at its front), saturates at a
	 * share of the depth in the inner surf (ShoreSaturatedShare), and ends at the waterline with a front a share of the
	 * breaker's height that runs on as the swash (Shen and Meyer 1963; Peregrine and Williams 2001). Comes in with the
	 * bore (stage 0.66 to 0.80). Mirrors PBRWaterModel::ShoreBoreHeight.
	 * @param ratio shoaled height over the breaker limit (> 1: broken)
	 * @param depth still water depth under the crest (m)
	 */
	float ShoreBoreHeight(float height, float stage, float ratio, float slope, float index, float depth)
	{
		[branch] if (ratio > 1.0)
		{
			float breakerHeight = index * depth * pow(abs(ratio), 0.8);  // H_b: the limit at the break point's depth
			float saturated = ShoreSaturatedShare(slope) * depth;
			// The front is held up only while there is water for it: over the last few decimetres the bore collapses
			// into the swash.
			float front = max(saturated, 0.15 * breakerHeight * smoothstep(0.05, 0.4, depth)) / ShoreBoreCrest;
			height = lerp(height, front, smoothstep(0.66, 0.80, stage));
		}
		return height;
	}

	/// One wave of a train, as a point on it sees it.
	struct ShoreWave
	{
		float phase;        // where this point is on the wave: 0 the trough ahead of it, 1 the trough behind
		float index;        // the wave's number
		float stage;        // its life, 0 deep water, 0.5 reaching its limit, 1 the bore at the shore
		float height;       // trough to crest (m)
		float wavelength;   // here (units)
		float whelp;        // height (m) of the crests trailing the wave's front (ShoreWhelps); 0 where it has none
		float whelpLength;  // their wavelength (units)
	};

	/// Height (m) the sea's dominant waves arrive with from deep water, before shoaling. Mirrors the CPU's ShoreDeepHeight.
	float ShoreDeepHeight(WaveContext ctx)
	{
		return ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ctx.damping;
	}

	/**
	 * The whelps of an undular bore: a bore weak against its depth (Froude number below about 1.3) rides on a train of
	 * smaller crests a few depths apart that shrink away behind it (Favre 1935; Chanson 2009). Water deepening inshore
	 * of a bar frees the wave's harmonics the same way (Beji and Battjes 1993). Both live in the main wave's phase.
	 * Mirrors PBRWaterModel::ShoreWhelpAmplitude.
	 * @param wave the wave, with phase, stage and height set
	 * @param depthA the depth under the point the whelps trail (m)
	 * @param crestDepth the depth under the wave's crest (m)
	 * @return the height (m) of the first crest behind the front
	 */
	float ShoreWhelpAmplitude(ShoreWave wave, float depthA, float crestDepth)
	{
		float froude = ShoreBoreCrest * wave.height / depthA;
		float weakBore = (1.0 - smoothstep(0.25, 0.5, froude)) * smoothstep(0.7, 0.85, wave.stage);
		float barLee = smoothstep(1.0, 1.6, depthA / crestDepth) * smoothstep(0.25, 0.4, wave.stage);
		return Shore3.y * max(weakBore, barLee) * 0.3 * wave.height;
	}

	/**
	 * The whelps' height above the wave's profile and its rate of change with the phase (units of `amplitude`): crests
	 * of spacing `spacing` from the wave's crest (ShoreCrestPhase) back, dying away within a few spacings. Mirrors
	 * PBRWaterModel::ShoreWhelps.
	 * @param x the phase on the wave
	 * @param stage the wave's stage
	 * @param amplitude height of the first crest
	 * @param lambda the wave's wavelength (units)
	 * @param spacing the whelps' wavelength (units)
	 */
	float2 ShoreWhelps(float x, float stage, float amplitude, float lambda, float spacing)
	{
		float anchor = ShoreCrestPhase(stage);
		float2 o = 0.0;
		[branch] if (amplitude > 0.0 && x > anchor)
		{
			float c = lambda / spacing;  // whelps per phase
			float s = (x - anchor) * c;
			float e = exp(-s / 2.2);
			float t = saturate((x - 0.75) / 0.22);
			float tail = 1.0 - t * t * (3.0 - 2.0 * t);  // gone by the end of the wave, where the next begins
			float dTail = -6.0 * t * (1.0 - t) / 0.22;
			float b = 0.5 - 0.5 * cos(Math::TAU * s);
			float dB = Math::PI * c * sin(Math::TAU * s);
			o = amplitude * float2(e * tail * b, e * (dTail * b + tail * dB - c / 2.2 * tail * b));
		}
		return o;
	}

	/**
	 * The depth (m) a wave answers to when its crest stands at absolute `absXY` (ShoreField::crestDepth): averaged
	 * along the crest and never more than the least it crossed coming in, so breaking is never undone and a wave that
	 * broke on a bar crosses the deeper water behind it as a bore. Mirrors ShoreField::CrestDepthAt.
	 * @param fallback the depth to use outside the shore field (m)
	 */
	float ShoreCrestDepth(float2 absXY, float fallback)
	{
		float depth = fallback;
		float2 uv;
		[branch] if (ShoreFieldUV(absXY, uv))
			depth = ShoreRoomTexture.SampleLevel(LinearClampSampler, uv, 0).y;
		return depth;
	}

	/// The wave passing absolute `absXY` at `clock`.
	ShoreWave ShoreWaveAt(float2 absXY, float2 coast, WaveContext ctx, float clock)
	{
		ShoreRay ray = ctx.shore;
		ShoreWave o;
		float cycles = ShorePhase(ray, coast, clock) / Math::TAU;
		o.index = floor(cycles);
		o.phase = cycles - o.index;
		float omega = max(Shore4.x, 1e-3);
		o.wavelength = Math::TAU / (omega * max(ray.slowness, 1e-6));

		// Everything about the wave is decided where its crest stands as it breaks, and by a depth that only falls as
		// the wave comes in, so its stage only advances.
		float crestDepth = max(ShoreCrestDepth(absXY + ray.direction * ((o.phase - ShoreCrestReference) * o.wavelength), ctx.depth * MetresPerUnit), 0.05);
		float deep = ShoreDeepHeight(ctx) * ShoreSet(coast, o.index);
		float shoaled = deep * ShoalingCoefficient(omega, crestDepth);
		// An onshore wind makes the wave break earlier (a lower breaker index), an offshore wind later (ShoreWindBreaking).
		float index = ShoreBreakerIndex(ctx.shore.slope, min(shoaled, ShoreBreakerLimit * crestDepth), Math::TAU / omega) * (1.0 - ShoreWindBreaking * ctx.onshoreWind);
		float limit = index * crestDepth;
		o.height = min(min(shoaled, limit), ShoreSteepestWave * o.wavelength * MetresPerUnit);
		// Rock faces and cliffs: the waves surge against them and go back, they stay short of breaking.
		o.stage = min(ShoreBreakerStage(shoaled / limit, ctx.shore.slope, index, ShoreBreakerPlunging(ShoreBreakerIribarren(ctx))), lerp(0.42, 1.0, ctx.beach));
		o.height = ShoreBoreHeight(o.height, o.stage, shoaled / limit, ctx.shore.slope, index, crestDepth);

		// The whelps trail the crest: a weak bore or a bar's lee. The depth under it, from the wave's own phase, is the same
		// for every point of the wave.
		o.whelp = 0.0;
		o.whelpLength = o.wavelength;
		[branch] if (Shore3.y > 0.0 && o.stage > 0.25)
		{
			float anchorBed = TerrainHeight(absXY + ray.direction * ((o.phase - ShoreCrestPhase(o.stage)) * o.wavelength));
			float anchorDepth = max((ctx.waterZ - anchorBed) * MetresPerUnit, 0.05);
			o.whelp = ShoreWhelpAmplitude(o, anchorDepth, crestDepth);
			// Spaced at a few depths, from a crest in a few dozen to a couple of whelps per wave.
			o.whelpLength = clamp(8.0 * anchorDepth * UnitsPerMetre, o.wavelength / 24.0, o.wavelength / 2.5);
		}
		return o;
	}

	/**
	 * How real the shore waves' breaking is here, 0..1: only real breakers pitch over and entrain air. A trace from a
	 * couple of centimetres of significant height, full from 40 cm.
	 */
	float BreakerStrength(WaveContext ctx)
	{
		return smoothstep(0.02, 0.4, ctx.shoreHeight * ctx.damping * MetresPerUnit);
	}

	// ------------------------------------------------------------------------
	// The breaker's shape and its curl
	//
	// A wave is one hump through its life (ShoreShapeAt, ShoreHump; baked into the profile table by ShoreProfile.cpp):
	// it peaks as it shoals, its crest runs ahead as it breaks, and it becomes a bore with a steep front and a long
	// back.
	//
	// Where the mesh is fine enough the wave also curls over. The water above a pivot on the front face turns forward
	// about it, most at the crest, which becomes the lip's tip, and is thrown ahead into the trough (the shape of
	// Longuet-Higgins' overturning jet; the timing is Galvin's plunge distance, ShoreBreakerStage). The surface stays
	// one sheet, so the lip's underside faces down and the face is seen through the tube. Once the jet lands the tube's
	// roof falls onto the water below and slides back to its place in the bore; the splash-up (ShoreCurlSplash) rises
	// where the jet lands.
	//
	// The curl is kept apart from the surface it rides on (ShoreTrain::lip), which near the shore is the surf
	// simulation's. Geometry only: the CPU mirror rides the uncurled wave.
	// ------------------------------------------------------------------------
	static const float ShoreCurlAngle = 2.3;   // turn of the crest at full curl (rad)
	static const float ShoreCurlThrow = 1.0;   // how far the lip is thrown ahead of the face at full curl (wave heights)
	static const float ShoreCurlPivot = 0.45;  // the pivot's height on the front face (share of the crest's)
	/// How far behind the crest the turn runs out (wave heights): the top of the lip. Kept short so the back of the
	/// wave is not swung up about the pivot.
	static const float ShoreCurlLipLength = 0.6;
	/// Stages of the curl (ShoreBreakerStage): the lip turns over until it lands in the trough (Galvin's plunge distance),
	/// the tube's roof falls onto the water under it, and that water settles into the bore as it forms.
	static const float2 ShoreCurlOverturn = float2(0.44, 0.64);
	static const float2 ShoreCurlImpact = float2(0.62, 0.70);
	static const float2 ShoreCurlSettle = float2(0.68, 0.80);
	/// The stages the curl is drawn over: faded in as the wave steepens, and out once the lip has settled into the bore.
	static const float4 ShoreCurlWindow = float4(0.40, 0.46, 0.80, 0.84);
	/// The stages over which the wave turns into the bore (Evaluate in ShoreProfile.cpp).
	static const float2 ShoreBoreRise = float2(0.58, 0.80);
	/// The splash-up (Peregrine 1983; Bonmarin 1989): the jet drives the trough's water up ahead of the face as a
	/// forward- leaning sheet that can rise past the old crest, falls forward and throws up a weaker second round
	/// before the bore takes over. Per round: x its first and y its last stage, z how far ahead of the jet's landing it
	/// rises, w how far it travels as it flies (wave heights). Kept within about a wave height of the landing, where
	/// the bore's front forms.
	static const float4 ShoreSplashFirst = float4(0.63, 0.73, 0.0, 0.6);
	static const float4 ShoreSplashSecond = float4(0.71, 0.80, 0.6, 0.4);
	static const float2 ShoreSplashHeight = float2(0.7, 0.28);  // the rounds' peaks (wave heights, at full curl)
	static const float2 ShoreSplashWidth = float2(0.3, 0.6);    // a round's width as it rises and as it lands (wave heights)
	/// How far the sheet leans forward per its height. Much more and its front folds over itself at the peak.
	static const float ShoreSplashLean = 0.4;
	static const float ShoreSplashClumpLength = 2.0;  // the clumps it bursts up in along the crest (wave heights)

	/// Smoothstep as ShoreProfile.cpp's: (x - a) / (b - a), clamped and eased.
	float ShoreSmooth(float a, float b, float x)
	{
		float t = saturate((x - a) / (b - a));
		return t * t * (3.0 - 2.0 * t);
	}

	/// The hump of a wave at one stage of its life (Evaluate in ShoreProfile.cpp).
	struct ShoreShape
	{
		float crest;   // the crest's phase
		float front;   // the front face's width (phase)
		float back;    // the back's width while the wave is whole (phase)
		float power;   // the narrowing of the crest
		float lean;    // how far the crest's water is carried forward (share of `forward`)
		float height;  // the crest's height (share of the wave's): the bore is lower than the wave that broke
		float bore;    // 0..1 how far the hump has turned into the bore
	};

	/**
	 * The shape of a wave at stage `v`. Mirrors Evaluate in ShoreProfile.cpp.
	 * @param forwardScale the table's scale of this stage's forward displacement (ShoreProfileAt slice 1, z): 1 for the
	 *                     shape alone
	 */
	ShoreShape ShoreShapeAt(float v, float forwardScale)
	{
		ShoreShape f;
		float s = ShoreSmooth(0.0, 0.5, v);
		f.bore = ShoreSmooth(ShoreBoreRise.x, ShoreBoreRise.y, v);
		f.crest = ShoreCrestPhase(v);
		f.front = lerp(0.5 * (1.0 - s) + 0.075 * s, 0.105, f.bore);
		f.back = 0.5 * (1.0 - s) + 0.30 * s;
		f.power = 1.0 + 1.2 * s;
		f.lean = lerp(0.10 * ShoreSmooth(0.25, 0.55, v), 0.05, f.bore) * forwardScale;
		f.height = 1.0 - (1.0 - ShoreBoreCrest) * ShoreSmooth(0.58, 0.72, v);
		return f;
	}

	/// The hump's height at phase `x` as a share of its crest's: either face falls away from the crest, a raised cosine
	/// narrowed towards it while the wave is whole, a ramp ahead and a long back once it is a bore.
	float ShoreHump(float x, ShoreShape f)
	{
		float xi = x - f.crest;
		float hump;
		if (xi < 0.0) {
			float a = saturate(-xi / f.front);
			float wave = pow(abs(0.5 * (1.0 + cos(Math::PI * a))), f.power);
			hump = lerp(wave, 1.0 - a * a * (3.0 - 2.0 * a), f.bore);
		} else {
			float wave = pow(abs(0.5 * (1.0 + cos(Math::PI * saturate(xi / f.back)))), f.power);
			float bore = exp(-xi / 0.34) * (1.0 - ShoreSmooth(0.82, 1.0, x));
			hump = lerp(wave, bore, f.bore);
		}
		return hump;
	}

	/**
	 * The uncurled wave at phase `x`: where its water stands along the travel (its rest position is -x lambda) and how
	 * high above the trough (units). The same surface as the profile table's.
	 */
	float2 ShoreCurlBackground(float x, ShoreShape f, float lambda, float height, float forward)
	{
		float hump = ShoreHump(x, f);
		return float2(-x * lambda + f.lean * hump * forward, f.height * hump * height);
	}

	/// What a curling wave's shape needs that is the same all along it: worked out once (ShoreCurlSetup) and passed to
	/// every ShoreCurlLip of the wave.
	struct ShoreCurlFrame
	{
		ShoreShape shape;  // the hump the lip is turned out of
		float pivot;       // the pivot's phase on the front face
		float2 pivotAt;    // the pivot (along the travel, up; units)
		float turn;        // the crest's turn now (rad)
		float thrown;      // how far the crest is thrown ahead now (units)
		float impact;      // 0..1 the jet has landed: the roof falls onto the water under it
		float settle;      // 0..1 the fallen roof has slid back to its own place in the bore
		float3 splashA;    // the splash-up's first round: x where it stands along the travel, y its width, z its height (units)
		float3 splashB;    // and its second
	};

	/// The pivot's phase on the front face of `s`, where the whole wave stands ShoreCurlPivot of its crest high.
	float ShoreCurlPivotPhase(ShoreShape s)
	{
		return s.crest - acos(2.0 * pow(ShoreCurlPivot, 1.0 / s.power) - 1.0) / Math::PI * s.front;
	}

	/// One round of the splash-up at stage `v` (ShoreSplashFirst / Second), thrown up from `landing` (units along the
	/// travel): where it stands, its width and its height (units). It rises and falls as thrown water does, 4 t (1 - t).
	float3 ShoreSplashRound(float v, float4 timing, float peak, float landing, float height, float amount)
	{
		float t = saturate((v - timing.x) / (timing.y - timing.x));
		return float3(landing + (timing.z + timing.w * t) * height, lerp(ShoreSplashWidth.x, ShoreSplashWidth.y, t) * height,
			peak * amount * 4.0 * t * (1.0 - t) * height);
	}

	/**
	 * The frame of a curling wave of stage `v`, wavelength `lambda`, height `height` (units), forward reach `forward`
	 * (units) and its scale for this stage `forwardScale`, and curl strength `amount` (ShoreCurlWeight).
	 */
	ShoreCurlFrame ShoreCurlSetup(float v, float lambda, float height, float forward, float forwardScale, float amount)
	{
		ShoreCurlFrame f;
		f.shape = ShoreShapeAt(v, forwardScale);
		f.pivot = ShoreCurlPivotPhase(f.shape);
		f.pivotAt = ShoreCurlBackground(f.pivot, f.shape, lambda, height, forward);
		// The lip accelerates as it is thrown and hits the trough at full speed.
		float overturn = saturate((v - ShoreCurlOverturn.x) / (ShoreCurlOverturn.y - ShoreCurlOverturn.x));
		float life = overturn * overturn;
		float fullTurn = ShoreCurlAngle * min(amount, 1.0);
		f.turn = fullTurn * life;
		f.thrown = ShoreCurlThrow * amount * life * height;
		f.impact = ShoreSmooth(ShoreCurlImpact.x, ShoreCurlImpact.y, v);
		f.settle = ShoreSmooth(ShoreCurlSettle.x, ShoreCurlSettle.y, v);

		// Where the jet's tip comes down: the crest fully turned and thrown, on the wave as it stands when the lip
		// lands, so the splash rides with the wave rather than with the hump's crest.
		ShoreShape land = ShoreShapeAt(ShoreCurlOverturn.y, forwardScale);
		float2 landPivot = ShoreCurlBackground(ShoreCurlPivotPhase(land), land, lambda, height, forward);
		float2 d = ShoreCurlBackground(land.crest, land, lambda, height, forward) - landPivot;
		float landing = landPivot.x + d.x * cos(fullTurn) + d.y * sin(fullTurn) + ShoreCurlThrow * amount * height;
		f.splashA = ShoreSplashRound(v, ShoreSplashFirst, ShoreSplashHeight.x, landing, height, amount);
		f.splashB = ShoreSplashRound(v, ShoreSplashSecond, ShoreSplashHeight.y, landing, height, amount);
		return f;
	}

	/// The splash-up over the water at phase `x` that stands at `along` (units along the travel, ShoreCurlBackground x): x
	/// along the travel, y up (units). The sheet leans forward: its top is thrown ahead of its foot.
	float2 ShoreCurlSplash(float x, float along, ShoreCurlFrame f)
	{
		float a = (along - f.splashA.x) / max(f.splashA.y, 1e-3);
		float b = (along - f.splashB.x) / max(f.splashB.y, 1e-3);
		// Faded out in the trough ahead of the wave: past phase 0 the water belongs to the next wave.
		float up = (f.splashA.z * exp(-a * a) + f.splashB.z * exp(-b * b)) * ShoreSmooth(0.0, 0.04, x);
		return float2(ShoreSplashLean * up, up);
	}

	/// How big the splash-up is along the crest (0.4..1.4 of its mean): it bursts up in clumps a couple of wave heights
	/// long, different for every wave.
	float ShoreSplashClump(float2 absXY, float2 dir, float height, float index)
	{
		float along = dot(absXY, float2(-dir.y, dir.x)) / max(ShoreSplashClumpLength * height, 1.0);
		return lerp(0.4, 1.4, ValueNoise(float2(along, index * 7.31)));
	}

	/**
	 * The curl at phase `x`: how far the curled wave stands from ShoreCurlBackground's surface (units), x along the direction
	 * of travel, y up, with the splash-up thrown where its jet lands (ShoreCurlSplash). Zero where neither reaches.
	 * @param f       ShoreCurlSetup of the wave
	 * @param lambda  the wavelength (units)
	 * @param height  trough to crest (units)
	 * @param forward the table's forward reach (units)
	 */
	float2 ShoreCurlLip(float x, ShoreCurlFrame f, float lambda, float height, float forward)
	{
		// How much of the turn this point takes: none below the pivot, rising to all of it at the crest, the lip's tip, and
		// running out behind it over the top of the lip. Ahead of it, the splash-up thrown up where the jet lands.
		float2 shape = ShoreCurlBackground(x, f.shape, lambda, height, forward);
		float2 lip = ShoreCurlSplash(x, shape.x, f);
		float g = 0.0;
		if (x > f.shape.crest) {
			float behind = (x - f.shape.crest) * lambda / max(ShoreCurlLipLength * height, 1e-3);
			g = exp(-behind * behind);
		} else if (x > f.pivot) {
			float u = (x - f.pivot) / max(f.shape.crest - f.pivot, 1e-4);
			g = u * u;
		}
		[branch] if (g > 1e-3)
		{
			float t = f.turn * g;
			float ct = cos(t);
			float st = sin(t);
			float2 d = shape - f.pivotAt;
			float2 curled = f.pivotAt + float2(d.x * ct + d.y * st, d.y * ct - d.x * st);
			curled.x += f.thrown * g;
			[branch] if (f.impact > 0.0)
			{
				// Once the jet has landed the tube's roof falls onto the water under it (the point of that surface
				// below it, found in two steps since the surface leans forward), then slides back along it, under the
				// surface, to its own place in the bore.
				float under = saturate(-curled.x / max(lambda, 1e-3));
				[unroll] for (uint i = 0; i < 2; i++)
					under = saturate(under + (ShoreCurlBackground(under, f.shape, lambda, height, forward).x - curled.x) / max(lambda, 1e-3));
				curled = lerp(curled, ShoreCurlBackground(lerp(under, x, f.settle), f.shape, lambda, height, forward), f.impact);
			}
			lip += curled - shape;
		}
		return lip;
	}

	/**
	 * The projected grid's spacing (units) at camera-relative `positionWS` on its plane, or 1e6 where the draw is no grid's
	 * (Tess0.x: 3 the deferred surface, which shades only the grids, 6 and 7 a grid's cells and patches). A cell is Grid
	 * Cell Size (Tess0.y) pixels on screen: across the view that is its width at this distance, along it the width stretched
	 * by the grazing angle (ProjectedGridCS Evaluate takes the longer side). Where a breaker needs more than a cell gives,
	 * the cell is a patch Tess0.z cells across (ProjectedGridCS FlagCS, the same test). Refined from half the lip's length
	 * (lipLength / 50, where a coarse cell's curl starts to fade, ShoreCurlWeight), so the curl's strength is continuous
	 * across the threshold: at lipLength / 25 a coarse cell has none and a refined one all of it. Up to where even the
	 * refined cell is too coarse for any curl. A function of the position and the frame only, so the grid's vertices and
	 * the pixels agree on it.
	 */
	float GridSpacing(float3 positionWS)
	{
		float spacing = 1e6;
		[branch] if (abs(Tess0.x - 3.0) < 0.5 || Tess0.x > 5.5)
		{
			float distance = max(length(positionWS), 1.0);
			float across = Tess0.y * distance / max(Tess0.w, 1e-3);
			spacing = across / max(abs(positionWS.z) / distance, 0.01);
			float lip = max(Shore3.w, UnitsPerMetre) / 25.0;
			if (Tess0.z > 1.5 && spacing > 0.5 * lip && spacing < 2.0 * Tess0.z * lip)
				spacing /= Tess0.z;
		}
		return spacing;
	}

	/**
	 * Where a wave of `wavelength` (units) at stage `stage` takes the curl's shape at camera-relative `positionWS` on a
	 * beach of surf similarity `iribarren`. Returns the share of the curl (ShoreCurlLip) over the table's shape (0..1)
	 * and sets `amount` to the curl's strength (Shore3.z at most). Weaker plungers and meshes too coarse for the lip
	 * get a weaker curl, not a partial blend of the full one; the share only fades the lip in and out at the stage
	 * window's ends and where the mesh first resolves the face.
	 */
	float ShoreCurlWeight(float3 positionWS, float wavelength, float stage, float iribarren, out float amount)
	{
		float weight = 0.0;
		amount = 0.0;
		[branch] if (Shore3.z > 0.0 && stage > ShoreCurlWindow.x && stage < ShoreCurlWindow.w)
		{
			// Only plungers curl (Battjes 1974); spillers tumble down the face and the steepest beaches collapse or
			// surge. A wave that plunges at all throws its lip all the way over.
			float plunging = ShoreBreakerPlunging(iribarren);
			// The lip needs a few dozen vertices across its wave. Measured against the deep-water wavelength
			// (Shore3.w), which does not shrink in the shallows, so a wave keeps its curl all the way in. The grid
			// refines its cells by the same length (GridSpacing).
			float lipLength = max(wavelength, Shore3.w);
			float spacing = GridSpacing(positionWS);
			amount = Shore3.z * smoothstep(0.0, 0.3, plunging) * smoothstep(lipLength / 25.0, lipLength / 50.0, spacing);
			if (amount > 0.0)
				weight = smoothstep(lipLength / 12.0, lipLength / 25.0, spacing) * smoothstep(ShoreCurlWindow.x, ShoreCurlWindow.y, stage) *
				         (1.0 - smoothstep(ShoreCurlWindow.z, ShoreCurlWindow.w, stage));
		}
		return weight;
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units), without the lip
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float3 lip;                   // a breaker's overturning lip: its offset from the surface it rides on (units)
		float3 previousLip;           // the same last frame
		float2 lipSlope;              // the lip's share of the height gradient
		float3 lipCompression;        // and of the horizontal displacement gradient
		float foam;                   // 0..1 white water the waves carry here
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curl;                   // 0..1 how much of a breaker's curl is shown here: the surface may face down
		float crest;                  // units above the still water the crest of the shore wave here stands (its top)
		float bore;                   // 0..1 the wave has broken into a bore: its white water lies on a flattened surface (stage 0.7..0.85)
		float aeration;               // 0..1 air churned into the water here (ShoreAeration): the crushed tube, the splash-up and the roller
		float impact;                 // 0..1 the wave's jet has landed (stage past 0.6): its folding surface is white water from now on
		float clearFace;              // 0..1 the steepening face, the lip and the tube of a wave about to plunge: clear water (ShoreClearFace)
		float curlNeed;               // 0..1 a plunger in its curl's stages passes here: the curl shows where the vertices are fine enough (the grid refines here)
		// Set by ComposeSurf:
		float rawHeight;     // the train's own height here, before the surf simulation took over (units above the still water)
		float surf;          // 0..1 share of the surf simulation in the drawn surface here
		float sheetSurface;  // the simulation's surface here (units above the still water)
		float sheetDepth;    // its depth (units; 0 where dry or not drawn)
		float sheetFoam;     // 0..1 the white water it carries
		float seaHold;       // 0..1 the sea is held over a bed below the still water (SeaFloorHold): none where the simulation alone is the water
	};

	static const float ShoreRollerLumps = 0.12;  // the roller's churning lumps, in wave heights at full aeration

	/**
	 * Air in the breaking wave at phase `x` and stage `v`, 0..1, behind `front` (the foot of the front face). Clear
	 * before the jet lands (white only at the lip's crown, the profile's foam); then the collapsing tube and the
	 * splash-up turn the crushed region opaque (void fractions to 0.6 and 0.35: Na et al. 2016; Lubin et al. 2011).
	 * That becomes the roller on the bore's front, thinning behind it and weakening as the bore runs into the shallows.
	 */
	float ShoreAeration(float x, float v, float front)
	{
		// Behind the front: the whole crushed tube at first (to about the old crest), a roller a wave height long once
		// the bore has formed.
		float impact = ShoreSmooth(0.60, 0.68, v);
		float tail = lerp(0.28, 0.08, ShoreSmooth(0.70, 0.88, v));
		float ahead = ShoreSmooth(front - 0.04, front, x);
		float behind = x <= front ? 1.0 : exp(-(x - front) / tail);
		float roller = ahead * behind * (1.0 - ShoreSmooth(0.82, 1.0, x));
		return impact * roller * (1.0 - 0.5 * ShoreSmooth(0.88, 1.0, v));
	}

	/**
	 * The part of a wave about to plunge that is clear water, 0..1: its face from the trough up over the crest, the lip
	 * and the tube, from when it steepens until the jet lands. The sea's foam is drawn off it (BreakerFoam keeps the
	 * lip's crown).
	 */
	float ShoreClearFace(float x, float v)
	{
		float window = ShoreSmooth(0.38, 0.46, v) * (1.0 - ShoreSmooth(0.64, 0.72, v));
		// Counted from the foot of the front face, so the flat water in front of the plunger keeps its foam.
		ShoreShape shape = ShoreShapeAt(v, 1.0);
		float foot = shape.crest - shape.front;
		return window * ShoreSmooth(foot - 0.02, foot + 0.3 * shape.front, x) * (1.0 - ShoreSmooth(shape.crest + 0.04, shape.crest + 0.12, x));
	}

	/**
	 * The roller's churning lumps at `q` (the position in lump lengths), boiling at `clock`: x the height (-0.5..0.5),
	 * yz its gradient in lump lengths.
	 */
	float3 ShoreRollerChurn(float2 q, float clock)
	{
		float3 a = ValueNoiseGrad(q + float2(0.31, -0.17) * clock);
		float3 b = ValueNoiseGrad(q * 2.1 + float2(-0.23, 0.29) * clock + 5.3);
		return float3(0.65 * (a.x - 0.5) + 0.35 * (b.x - 0.5), 0.65 * a.yz + 0.35 * 2.1 * b.yz);
	}

	/**
	 * The shape of one wave of the train (ShoreWaves) at undisplaced camera-relative `positionWS`, added to `o`.
	 * @param wave   the wave passing here (ShoreWaveAt)
	 * @param height its height here (units)
	 * @param gate   the train's share here
	 * @param w      the same times the fade of waves the mesh or the pixel cannot carry
	 */
	void ShoreWaveShape(inout ShoreTrain o, float3 positionWS, float2 absXY, float2 coast, WaveContext ctx, ShoreWave wave, float height, float gate, float w, float scale, bool previous, bool shading)
	{
		float tent = ShoreTentStep(scale, wave.wavelength);
		float4 shape = ShoreProfileFiltered(wave.phase, wave.stage, tent);
		float forward = min(wave.wavelength, ShoreForwardHeights * height);
		float2 dir = ctx.shore.direction;
		float omega = max(Shore4.x, 1e-3);

		// The profile stands on its trough, so a wave of any height meets the next without a step. The mean level comes
		// from the depth here, not the wave's: about half the height while the wave is a sinusoid, less as it narrows.
		float localDepth = max(ctx.depth * MetresPerUnit, 0.05);
		float localShoaled = ShoreDeepHeight(ctx) * ShoalingCoefficient(omega, localDepth);
		float localLimit = ShoreBreakerLimit * localDepth;
		float meanLevel = min(localShoaled, localLimit) * lerp(0.5, 0.22, saturate(2.0 * localShoaled / (localShoaled + localLimit))) * UnitsPerMetre;

		// Along the travel and up (units), and their rates of change with the phase.
		float along = shape.x * forward;
		float up = shape.y * height;
		float dAlong = shape.z * forward;
		float dUp = shape.w * height;

		float lastPhase = wave.phase;
		if (previous) {
			float cycles = ShorePhase(ctx.shore, coast, Shore2.y) / Math::TAU;
			lastPhase = cycles - floor(cycles);
		}

		// The curl: the lip's offset from the table's surface, and its rates of change (central differences: they carry the
		// surface around the lip and under it). The lip's tip is sharper than the vertices or pixels can carry: low-passed
		// as the shape is (ShoreTentStep), its taps the differences' own.
		float2 lip = 0.0;
		float2 dLip = 0.0;
		float2 lastLip = 0.0;
		float curlAmount;
		float curl = ShoreCurlWeight(positionWS, wave.wavelength, wave.stage, ShoreBreakerIribarren(ctx), curlAmount);
		bool splashing = Shore3.z > 0.0 && wave.stage > ShoreSplashFirst.x && wave.stage < ShoreSplashSecond.y;
		// The profile's second slice: x the foam (shading), z the scale of this stage's forward displacement (the curl).
		float4 profileFoam = 0.0;
		[branch] if (shading || curl > 0.0 || splashing)
			profileFoam = ShoreProfileAt(wave.phase, wave.stage, 1.0);
		[branch] if (curl > 0.0)
		{
			float e = max(1.0 / 512.0, ctx.lipSpacing >= 0.0 ? ShoreTentStep(ctx.lipSpacing, wave.wavelength) : tent);
			ShoreCurlFrame frame = ShoreCurlSetup(wave.stage, wave.wavelength, height, forward, profileFoam.z, curlAmount);
			float clump = ShoreSplashClump(absXY, dir, height, wave.index);
			frame.splashA.z *= clump;
			frame.splashB.z *= clump;
			float2 c0 = ShoreCurlLip(wave.phase, frame, wave.wavelength, height, forward);
			float2 cA = ShoreCurlLip(wave.phase + e, frame, wave.wavelength, height, forward);
			float2 cB = ShoreCurlLip(wave.phase - e, frame, wave.wavelength, height, forward);
			lip = curl * (0.25 * (cA + cB) + 0.5 * c0);
			dLip = curl * (cA - cB) / (2.0 * e);
			if (previous) {
				float2 l0 = ShoreCurlLip(lastPhase, frame, wave.wavelength, height, forward);
				float2 lA = ShoreCurlLip(lastPhase + e, frame, wave.wavelength, height, forward);
				float2 lB = ShoreCurlLip(lastPhase - e, frame, wave.wavelength, height, forward);
				lastLip = curl * (0.25 * (lA + lB) + 0.5 * l0);
			}
			o.curl = curl * w;
		}
		// The undular bore's whelps ride on the profile; ones shorter than the vertex spacing or pixel fade out.
		float whelpAmplitude = wave.whelp * UnitsPerMetre * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / wave.whelpLength * scale);
		[branch] if (whelpAmplitude > 0.0)
		{
			float2 whelps = ShoreWhelps(wave.phase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength);
			up += whelps.x;
			dUp += whelps.y;
		}
		o.bore = smoothstep(0.70, 0.85, wave.stage) * gate;
		o.crest = (height - meanLevel) * w;

		// The splash-up is white water and throws spray (ShoreFoamCS, SprayEmitCS), also where the mesh is too coarse to raise
		// it (the surf's grid has no curl): its jet's strength without the curl's mesh fade.
		float splashWhite = 0.0;
		[branch] if (splashing)
		{
			float jet = Shore3.z * smoothstep(0.0, 0.3, ShoreBreakerPlunging(ShoreBreakerIribarren(ctx)));
			ShoreCurlFrame splash = ShoreCurlSetup(wave.stage, wave.wavelength, height, forward, profileFoam.z, jet);
			float up = ShoreCurlSplash(wave.phase, ShoreCurlBackground(wave.phase, splash.shape, wave.wavelength, height, forward).x, splash).y;
			splashWhite = saturate(up * ShoreSplashClump(absXY, dir, height, wave.index) / max(0.1 * height, 1e-3));
		}

		// The churned water starts at the foot of the front face, where the lip came down and the splash-up rises.
		ShoreShape hump = ShoreShapeAt(wave.stage, 1.0);
		float aeration = max(ShoreAeration(wave.phase, wave.stage, hump.crest - 0.5 * hump.front), splashWhite) * BreakerStrength(ctx);
		o.aeration = aeration * gate;
		o.impact = ShoreSmooth(0.60, 0.68, wave.stage) * gate;
		o.clearFace = ShoreClearFace(wave.phase, wave.stage) * gate;
		// Whatever the spacing: the projected grid refines the cells where this is set (ProjectedGridCS FlagCS). Only over
		// the part of the wave the lip and the splash reach: from the trough ahead, where the jet lands, to a few lip
		// lengths behind the crest (ShoreCurlLip's turn runs out there).
		bool curlStages = Shore3.z > 0.0 && wave.stage > ShoreCurlWindow.x && wave.stage < ShoreCurlWindow.w;
		float lipEnd = ShoreCrestPhase(wave.stage) + 3.0 * ShoreCurlLipLength * height / max(wave.wavelength, 1e-3) + 0.05;
		o.curlNeed = curlStages && wave.phase < lipEnd && ShoreBreakerPlunging(ShoreBreakerIribarren(ctx)) > 0.0 ? gate : 0.0;

		o.displacement = float3(dir * along, up - meanLevel) * w;
		o.lip = float3(dir * lip.x, lip.y) * w;
		// The phase rises towards the sea, a wavelength per cycle.
		float2 gradPhase = -dir / wave.wavelength;
		o.slope = (dUp * w) * gradPhase;
		o.lipSlope = (dLip.y * w) * gradPhase;

		// The roller churns: a heap of boiling lumps about half a wave height across that catch the light unevenly.
		// Lumps shorter than the vertex spacing or pixel fade out.
		float lumpLength = max(0.6 * height, 8.0);
		float lumps = ShoreRollerLumps * height * aeration * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / lumpLength * scale);
		[branch] if (lumps > 0.0)
		{
			float2 q = absXY / lumpLength;
			float3 churn = ShoreRollerChurn(q, Shore2.x);
			o.displacement.z += lumps * churn.x * w;
			o.slope += (lumps * w / lumpLength) * churn.yz;
			if (previous)
				o.previousDisplacement.z += lumps * ShoreRollerChurn(q, Shore2.y).x * w;
		}
		float3 stretch = float3(dir.x * gradPhase.x, dir.y * gradPhase.y, dir.x * gradPhase.y);
		o.compression = stretch * (dAlong * w);
		o.lipCompression = stretch * (dLip.x * w);
		o.breakStage = smoothstep(0.45, 0.6, wave.stage) * gate;
		if (shading)
			o.foam = profileFoam.x * gate * BreakerStrength(ctx);
		if (previous) {
			float4 last = ShoreProfileFiltered(lastPhase, wave.stage, tent);
			float2 lastShape = float2(last.x * forward, last.y * height);
			lastShape.y += ShoreWhelps(lastPhase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength).x;
			o.previousDisplacement += float3(dir * lastShape.x, lastShape.y - meanLevel) * w;
			o.previousLip = float3(dir * lastLip.x, lastLip.y) * w;
		}
	}

	/**
	 * The shore wave train at undisplaced camera-relative `positionWS`, weighted by its share ctx.train (ComposeSurf
	 * hands it to the surf simulation near the shore). Shared by the geometry, the compute passes and the shading;
	 * mirrored on the CPU (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale vertex spacing or pixel footprint (units): waves shorter than it fade out into the slope variance;
	 *   0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 * @param shading also read the foam (pixel shader)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous, bool shading)
	{
		ShoreTrain o = (ShoreTrain)0;
		// The train ends at the still waterline: up the beach the water is the surf simulation's.
		float gate = ctx.train * (1.0 - smoothstep(0.0, ShoreTrainLanding, ctx.elevation));
		[branch] if (gate > 0.0)
		{
			// The wavelength as ShoreWaveAt finds it: from the ray alone.
			float omega = max(Shore4.x, 1e-3);
			float k = omega * max(ctx.shore.slowness, 1e-6);
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, k * scale);
			[branch] if (fade <= 0.0)
			{
				// Too short for the mesh or the pixel (distant water): only the slope variance, from the sea's height
				// shoaled to the depth here (no ShoreWaveAt).
				float localDepth = max(ctx.depth * MetresPerUnit, 0.05);
				float approx = min(ShoreDeepHeight(ctx) * ShoalingCoefficient(omega, localDepth), ShoreBreakerLimit * localDepth) * UnitsPerMetre;
				float a = 0.5 * approx;
				o.slopeVariance = gate * gate * 0.5 * a * a * k * k;
				return o;
			}
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			ShoreWave wave = ShoreWaveAt(absXY, coast, ctx, Shore2.x);
			float w = gate * fade;
			float height = wave.height * UnitsPerMetre;
			// A broken wave answers to the water it is in: in the inner surf its height saturates at 0.4 to 0.6 of the
			// local depth (Thornton and Guza 1983), not the depth under its crest further out. Down to the waterline it
			// keeps a low front (ShoreBoreFloor).
			float saturated = max(ShoreSaturatedShare(ctx.shore.slope) * max(ctx.depth, 0.0), ShoreBoreFloor) / ShoreBoreCrest;
			height = lerp(height, min(height, saturated), smoothstep(ShoreBoreRise.x, ShoreBoreRise.y, wave.stage));
			float amplitudeUnits = 0.5 * height;
			o.slopeVariance = (1.0 - fade * fade) * gate * gate * 0.5 * amplitudeUnits * amplitudeUnits * k * k;
			[branch] if (w > 0.0)
				ShoreWaveShape(o, positionWS, absXY, coast, ctx, wave, height, gate, w, scale, previous, shading);
		}
		return o;
	}
}

#endif  // __PBR_WATER_SHORE_WAVES_HLSLI__
