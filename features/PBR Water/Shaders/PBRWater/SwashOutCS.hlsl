// ============================================================================
// PBR Water - hands the surf simulation to the draws.
//
// Packs the foam the surf left on the water (ShoreFoamCS) and the surf simulation (SwashStepCS) into the one texture
// the water reads (ShoreFoamTexture, t127): x the surf's foam, y the simulation's surface (units above the still water:
// the bed where it is dry, so the surface is continuous across the water's edge), z its depth (units), w its foam.
// Where the draws show it is theirs to decide (PBRWater::ComposeSurf).
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
	// No ground (a wall): no water, and a surface that pulls no neighbour's up.
	bool wall = prep.x > 100.0;
	float depth = wall ? 0.0 : swash.x * PBRWater::UnitsPerMetre;
	float surface = wall ? 0.0 : prep.x * PBRWater::UnitsPerMetre + depth;
	// The foam per area thins with the sheet: the last millimetres of a front or a draining film carry little of it.
	float foam = swash.w * smoothstep(0.003, 0.03, swash.x);
	Output[id.xy] = float4(stage.x, surface, depth, foam);
}
