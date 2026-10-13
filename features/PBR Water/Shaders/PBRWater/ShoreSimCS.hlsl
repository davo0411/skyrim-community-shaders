// ============================================================================
// PBR Water - what the swash leaves on the beach, for the lit terrain and objects.
//
// The swash itself is analytic (PBRWater::SwashTip): every crest's run-up and backwash follows from the waves that
// reached the waterline of the ray through a point, so it needs no history. What does need history is kept here,
// on a camera-centred grid (scrolled in whole texels, like the ripple simulation):
//
// Foam: where the water leaves the sand it leaves its foam, most along the line the uprush reached, thinning out
// over the foam lifetime (three wave periods) until the next uprush covers it and sweeps it away. Wet: the wet
// level follows the highest water (the swash on beaches, the waves on rocks and piers) and sinks slowly as it drains
// and dries.
//
// Output: State (x level the last uprush reached, yz unused, w s since the bed was under water) and Wet (x wet level,
// y foam left on the bed, z its deposit, w water surface height), in units above the still water level. Wet is read
// by the lit terrain and objects (wetness, foam, and the water height their caustics and shore wetness follow).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer ShoreSimCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last update
	uint GridSize;
	float Dt;            // s (0 while paused)
	float2 Origin;       // absolute position of the grid corner (units)
	float TexelSize;     // units
	float FoamLifetime;  // s the foam lasts on the sand (three wave periods)
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
	if (level < -1e6) {
		State[id.xy] = float4(0.0, 0.0, 0.0, 600.0);
		Wet[id.xy] = 0.0;
		return;
	}
	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementDistanceFade(length(positionWS)));
	float elevation = ctx.elevation;

	float2 tip = PBRWater::SwashTip(positionWS, ctx, false);
	float peak = state.x;
	float dry = state.w;
	bool wasCovered = dry <= 0.0;
	bool covered = ctx.beach > 0.0 && tip.x > elevation;
	float deposit = wet.z;
	if (Dt > 0.0) {
		// The line the last uprush reached: the tip while it climbs, where it turned back once it falls.
		peak = ctx.beach > 0.0 ? (tip.y > 0.0 ? tip.x : peak) : 0.0;
		dry = covered ? 0.0 : min(dry + Dt, 600.0);
		// Just left by the backwash: its foam stays on the sand, most along the line the uprush reached.
		if (wasCovered && !covered) {
			float band = max(1.5 * PBRWater::UnitsPerMetre * clamp(ctx.shore.slope, 0.01, 0.3), 2.0);
			deposit = 0.15 + 0.85 * smoothstep(peak - band, peak, elevation);
		}
	}
	float residue = covered ? 0.0 : deposit * exp(-dry / max(FoamLifetime, 0.1)) * ctx.beach;

	// The water surface here, for the lit objects (caustics, wetness): waves, swash and ripples.
	float surface = PBRWater::EvaluateWaves(positionWS, ctx, TexelSize, false).displacement.z;
	surface += PBRWater::RippleHeight(positionWS, false) * PBRWater::RippleDepthFade(ctx.depth) * ctx.damping;
	surface = max(surface, PBRWater::SwashTop(elevation, tip.x));
	float limit = PBRWater::DisplacementLimit();
	surface = clamp(surface, -limit, limit);

	// Wet: whatever the water reached lately - the swash up the beach, the waves against rocks, piers and hulls.
	// It sinks slowly as the water drains and the surface dries.
	float reached = ctx.beach > 0.0 ? max(surface, tip.x) : surface;
	float wetLevel = clamp(max(wet.x - DryRate * Dt, reached), -limit, limit);

	State[id.xy] = float4(clamp(peak, -limit, limit), 0.0, 0.0, dry);
	Wet[id.xy] = float4(wetLevel, residue, deposit, surface);
}
