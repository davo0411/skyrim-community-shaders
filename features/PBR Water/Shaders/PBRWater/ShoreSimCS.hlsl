// ============================================================================
// PBR Water - swash: the water each breaking wave throws up the beach, and what it leaves behind.
//
// A camera-centred grid (scrolled in whole texels, like the ripple simulation) keeps the swash state of every
// texel. All texels on a wave ray work out the same run-up from the same waterline, so the state is smooth
// along the shore and the water shaders read the run-up level and compare it with their own, exact bed height.
//
// Forcing: the waves arriving at the ray's waterline now are the deep-water FFT sea that the shore warp brings
// there (PBRWater::DeepSeaHeight), so each visible shore wave that reaches the beach runs up it. How far follows
// Stockdon et al. (2006), from the local deep-water height H0, peak wavelength L0 and beach slope beta:
//     set-up <eta> = 0.35 beta sqrt(H0 L0),  swash S = sqrt(H0 L0 (0.563 beta^2 + 0.004))
// and a bore of height eta pushes the tip up to <eta> + S / 2 * eta / (H0 / 2).
//
// Backwash: once nothing pushes it, the tip slides back down the slope under gravity (ballistic swash: Shen &
// Meyer 1963; Peregrine & Williams 2001), decelerating vertically at g sin^2(beta). On a gentle beach the next
// uprush arrives before the backwash has drained (saturated, dissipative surf); on a steep one it drains each time.
//
// Foam: where the water leaves the sand it leaves its foam, most along the line the uprush reached, thinning
// out over the foam lifetime until the next uprush covers it and sweeps it away. Wet: the wet level follows the
// highest water (the swash on beaches, the waves on rocks and piers) and sinks slowly as it drains and dries.
//
// Output: State (x run-up level, y its rise speed, z level the last uprush reached, w s since the bed was under
// water) and Wet (x wet level, y foam left on the bed, z its deposit, w water surface height), in units above the
// still water level. Both are read by the water shaders; Wet also by the lit terrain and objects (wetness, foam,
// and the water height their caustics and shore wetness follow).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer ShoreSimCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last update
	uint GridSize;
	float Dt;            // s (0 while paused)
	float2 Origin;       // absolute position of the grid corner (units)
	float TexelSize;     // units
	float FoamLifetime;  // s
	float DryRate;       // units/s the wet level sinks as the sand dries
	float3 Pad;
};

Texture2D<float4> PreviousState : register(t0);
Texture2D<float4> PreviousWet : register(t1);
RWTexture2D<float4> State : register(u0);
RWTexture2D<float4> Wet : register(u1);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 source = int2(id.xy) + Shift;
	bool known = all(source >= 0) && all(source < (int)GridSize);
	float4 state = known ? PreviousState.Load(int3(source, 0)) : float4(0.0, 0.0, 0.0, 600.0);
	float4 wet = known ? PreviousWet.Load(int3(source, 0)) : 0.0;

	float2 absXY = Origin + (float2(id.xy) + 0.5) * TexelSize;
	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e8) {
		State[id.xy] = float4(0.0, 0.0, 0.0, 600.0);
		Wet[id.xy] = 0.0;
		return;
	}
	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementDistanceFade(length(positionWS)));
	float elevation = ctx.elevation;

	// Run-up forcing from the waves reaching the ray's waterline.
	float forcing = 0.0;
	float accel = 0.0;
	float beta = clamp(ctx.shore.slope, 0.005, 0.3);
	if (ctx.beach > 0.0) {
		float2 shoreline = absXY - ctx.shore.dir * ctx.shore.distance;
		PBRWater::ShoreRay atShore = PBRWater::SampleShore(shoreline);
		float2 dir = atShore.weight > 0.0 ? atShore.dir : ctx.shore.dir;
		// The long waves that make the bores: low-passed past the chop.
		float eta = PBRWater::DeepSeaHeight(shoreline - FrameBuffer::CameraPosAdjust.xy, ctx, dir, atShore.delay * atShore.weight, 2.0);
		float H0 = max(ctx.deepHeight * PBRWater::MetresPerUnit, 0.01);
		float L0 = 6.2831853 * 9.81 / max(ctx.peakOmega * ctx.peakOmega, 1e-4);
		float hl = sqrt(H0 * L0);
		float setup = 0.35 * beta * hl;
		float swash = hl * sqrt(0.563 * beta * beta + 0.004);
		float crest = eta / max(0.5 * ctx.deepHeight, 1.0);
		forcing = ctx.beach * PBRWater::Shore0.z * (setup + 0.5 * swash * clamp(crest, -1.0, 2.0)) * PBRWater::UnitsPerMetre;
		float sinBeta = max(beta, 0.03) * rsqrt(1.0 + beta * beta);
		accel = 9.81 * PBRWater::UnitsPerMetre * sinBeta * sinBeta;
	}
	float reach = max(PBRWater::Shore0.w, 1.0);
	forcing = clamp(forcing, -reach, reach);

	float run = state.x;
	float speed = state.y;
	float peak = state.z;
	float dry = state.w;
	bool wasCovered = run > elevation;
	if (Dt > 0.0) {
		if (ctx.beach > 0.0) {
			float next = run + speed * Dt - 0.5 * accel * Dt * Dt;
			float nextSpeed = speed - accel * Dt;
			if (forcing > next) {
				// The bore pushes the tip up the beach.
				nextSpeed = max(nextSpeed, min((forcing - run) / Dt, 0.5 * reach));
				next = forcing;
			}
			// The line the last uprush reached: where it turned back, or wherever it still climbs above it.
			if (speed > 0.0 && nextSpeed <= 0.0)
				peak = next;
			else if (next > peak)
				peak = next;
			run = clamp(next, -reach, reach);
			speed = clamp(nextSpeed, -reach, reach);
		} else {
			run = 0.0;
			speed = 0.0;
			peak = 0.0;
		}
	}

	bool covered = run > elevation;
	float deposit = wet.z;
	if (Dt > 0.0) {
		dry = covered ? 0.0 : min(dry + Dt, 600.0);
		// Just left by the backwash: its foam stays on the sand, most along the line the uprush reached.
		if (wasCovered && !covered) {
			float band = max(1.5 * PBRWater::UnitsPerMetre * beta, 2.0);
			deposit = 0.3 + 0.7 * smoothstep(peak - band, peak, elevation);
		}
	}
	float residue = covered ? 0.0 : deposit * exp(-dry / max(FoamLifetime, 0.1)) * ctx.beach;

	// The water surface here, for the lit objects (caustics, wetness): waves, swash and ripples.
	float surface = PBRWater::EvaluateWaves(positionWS, ctx, TexelSize, false).displacement.z;
	surface += PBRWater::RippleHeight(positionWS, false) * PBRWater::RippleDepthFade(ctx.depth) * ctx.damping;
	if (ctx.beach > 0.0)
		surface = max(surface, PBRWater::SwashSurface(elevation, run));
	float limit = PBRWater::DisplacementLimit();
	surface = clamp(surface, -limit, limit);

	// Wet: whatever the water reached lately - the swash up the beach, the waves against rocks, piers and hulls.
	// It sinks slowly as the water drains and the surface dries.
	float reached = ctx.beach > 0.0 ? max(surface, run) : surface;
	float wetLevel = clamp(max(wet.x - DryRate * Dt, reached), -limit - reach, limit + reach);

	State[id.xy] = float4(run, speed, peak, dry);
	Wet[id.xy] = float4(wetLevel, residue, deposit, surface);
}
