// ============================================================================
// PBR Water - how far the water reaches up the shore, for the lit terrain and objects.
//
// The swash itself is analytic (PBRWater::SwashTip): every crest's run-up and backwash follows from the waves that
// reached the waterline of the ray through a point, so it needs no history. What does need history is kept here,
// on a camera-centred grid (scrolled in whole texels, like the ripple simulation): the wet level follows the highest
// water (the swash on beaches, the waves on rocks and piers) and sinks slowly as it drains and dries.
//
// Output: Wet (x wet level, yz unused, w water surface height), in units above the still water level, read by the lit
// terrain and objects (wetness, and the water height their caustics and shore wetness follow).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer ShoreSimCB : register(b0)
{
	int2 Shift;  // texels the grid scrolled since the last update
	uint GridSize;
	float Dt;         // s (0 while paused)
	float2 Origin;    // absolute position of the grid corner (units)
	float TexelSize;  // units
	float DryRate;    // units/s the wet level sinks as the sand dries
	float4 Pad;
};

Texture2D<float4> PreviousWet : register(t0);
RWTexture2D<float4> Wet : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 source = int2(id.xy) + Shift;
	bool known = all(source >= 0) && all(source < (int)GridSize);
	float4 wet = known ? PreviousWet.Load(int3(source, 0)) : 0.0;

	float2 absXY = Origin + (float2(id.xy) + 0.5) * TexelSize;
	float level = PBRWater::ShoreLevel(absXY);
	if (level < -1e6) {
		Wet[id.xy] = 0.0;
		return;
	}
	float3 positionWS = float3(absXY, level) - FrameBuffer::CameraPosAdjust.xyz;
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, PBRWater::DisplacementFade(positionWS));
	float elevation = ctx.elevation;

	float2 tip = PBRWater::SwashTip(positionWS, ctx, false);

	// The water surface here, for the lit objects (caustics, wetness): waves, swash and ripples.
	float surface = PBRWater::EvaluateWaves(positionWS, ctx, TexelSize, false, false).displacement.z;
	surface += PBRWater::RippleHeight(positionWS, false) * PBRWater::RippleDepthFade(ctx.depth) * ctx.damping;
	surface = max(surface, PBRWater::SwashTop(elevation, tip.x, tip.y));
	float limit = PBRWater::DisplacementLimit();
	surface = clamp(surface, -limit, limit);

	// Wet: whatever the water reached lately - the swash up the beach, the waves against rocks, piers and hulls.
	// It sinks slowly as the water drains and the surface dries.
	float reached = ctx.beach > 0.0 ? max(surface, tip.x) : surface;
	float wetLevel = clamp(max(wet.x - DryRate * Dt, reached), -limit, limit);

	Wet[id.xy] = float4(wetLevel, 0.0, 0.0, surface);
}
