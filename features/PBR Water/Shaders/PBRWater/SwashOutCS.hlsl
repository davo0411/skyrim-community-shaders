// ============================================================================
// PBR Water - hands the swash to the draws.
//
// Packs the surf's foam (ShoreFoamCS) and the simulated swash (SwashStepCS) into the one texture the water reads
// (ShoreFoamTexture, t127): x the surf's foam, y the swash's surface and z its depth (units above the still water), w its
// foam. Where the surf holds the water (Prep.y) the analytic waves are drawn, so the swash there has no depth.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer SwashOutCB : register(b0)
{
	int2 Shift;
	uint GridSize;
	float Dt;
	float2 Origin;
	float TexelSize;
	float Pad0;
	float4 Pad1;
};

Texture2D<float4> Stage : register(t0);
Texture2D<float4> Swash : register(t1);
Texture2D<float4> Prep : register(t2);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int3 c = int3(id.xy, 0);
	float4 stage = Stage.Load(c);
	float4 swash = Swash.Load(c);
	float4 prep = Prep.Load(c);
	float free = 1.0 - prep.y;
	// No ground (a wall): no swash, and a surface that pulls no neighbour's up.
	bool wall = prep.x > 100.0;
	// The surface stands the shared depth above the bed. At the simulation's full surface over a share of its depth, the
	// first millimetres of share (SwashSurface's SwashMinDepth) switched the whole bore's level on over the analytic waves,
	// and lifted the trough and the side of a curling lip half a metre in one row of vertices: a right-angled step along
	// the hand-over.
	float depth = wall ? 0.0 : swash.x * free * PBRWater::UnitsPerMetre;
	float surface = wall ? 0.0 : prep.x * PBRWater::UnitsPerMetre + depth;
	Output[id.xy] = float4(stage.x, surface, depth, swash.w * free);
}
