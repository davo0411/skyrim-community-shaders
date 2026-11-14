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
	float waterDepth = swash.x;
	// Holes: where an uprush and a backwash meet or part, the simulation's cells drain to nothing for a moment between
	// them, and the ground showed through the middle of the swash. Real water there stays one sheet. A dry cell with water
	// on both sides of it along any of four directions, within HoleReach cells, is drawn as a film at its neighbours'
	// level where that stands above its bed. The water's own edge, wet on one side only, stays where the simulation puts it.
	static const int HoleReach = 6;  // cells (about a metre)
	static const float Wet = 0.005;  // m
	[branch] if (!wall && waterDepth < Wet && prep.x > -1.0)
	{
		static const int2 Directions[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };
		float level = -1e6;
		[unroll] for (uint d = 0; d < 4; d++)
		{
			float levelA = -1e6, levelB = -1e6;
			[loop] for (int r = 1; r <= HoleReach; r++)
			{
				int2 a = clamp(int2(id.xy) + Directions[d] * r, 0, (int)GridSize - 1);
				int2 b = clamp(int2(id.xy) - Directions[d] * r, 0, (int)GridSize - 1);
				float2 sa = float2(Swash.Load(int3(a, 0)).x, Prep.Load(int3(a, 0)).x);
				float2 sb = float2(Swash.Load(int3(b, 0)).x, Prep.Load(int3(b, 0)).x);
				if (levelA < -1e5 && sa.x >= Wet && sa.y < 100.0)
					levelA = sa.x + sa.y;
				if (levelB < -1e5 && sb.x >= Wet && sb.y < 100.0)
					levelB = sb.x + sb.y;
			}
			if (levelA > -1e5 && levelB > -1e5)
				level = max(level, min(levelA, levelB));
		}
		// Only below the water around it: a ridge or a rock standing above that level is dry ground in the swash.
		if (level > prep.x)
			waterDepth = max(waterDepth, max(level - prep.x, Wet));
	}
	float depth = wall ? 0.0 : waterDepth * PBRWater::UnitsPerMetre;
	float surface = wall ? 0.0 : prep.x * PBRWater::UnitsPerMetre + depth;
	// The foam per area thins with the sheet: the last millimetres of a front or a draining film carry little of it.
	float foam = swash.w * smoothstep(0.003, 0.03, swash.x);
	Output[id.xy] = float4(stage.x, surface, depth, foam);
}
