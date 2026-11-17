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

// Holes: where an uprush and a backwash meet or part, the simulation's cells drain to nothing for a moment between
// them, and the ground showed through the middle of the swash. Real water there stays one sheet. A dry cell with water
// on both sides of it along any of four directions, within HoleReach cells, is drawn as a film at its neighbours'
// level where that stands above its bed. The water's own edge, wet on one side only, stays where the simulation puts it.
static const int HoleReach = 6;  // cells (about a metre)
static const float Wet = 0.005;  // m
static const int Group = 8;
static const int Apron = Group + 2 * HoleReach;
// The group's cells and HoleReach around them (depth, bed), read once: the hole search reads each up to 48 times.
groupshared float2 Cells[Apron * Apron];
groupshared uint AnyWet;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	if (gi == 0)
		AnyWet = 0;
	GroupMemoryBarrierWithGroupSync();
	int2 corner = int2(gid.xy) * Group - HoleReach;
	uint wet = 0;
	for (uint s = gi; s < Apron * Apron; s += Group * Group) {
		int2 p = clamp(corner + int2(s % Apron, s / Apron), 0, (int)GridSize - 1);
		float2 cell = float2(Swash.Load(int3(p, 0)).x, Prep.Load(int3(p, 0)).x);
		Cells[s] = cell;
		wet |= (cell.x >= Wet && cell.y < 100.0) ? 1u : 0u;
	}
	if (wet != 0)
		InterlockedOr(AnyWet, 1u);
	GroupMemoryBarrierWithGroupSync();

	if (any(id.xy >= GridSize))
		return;
	int3 c = int3(id.xy, 0);
	float4 stage = Stage.Load(c);
	float4 swash = Swash.Load(c);
	float4 prep = Prep.Load(c);
	// No ground (a wall): no water, and a surface that pulls no neighbour's up.
	bool wall = prep.x > 100.0;
	float waterDepth = swash.x;
	// No water anywhere within reach: no hole to fill.
	[branch] if (AnyWet != 0 && !wall && waterDepth < Wet && prep.x > -1.0)
	{
		static const int2 Directions[4] = { int2(1, 0), int2(0, 1), int2(1, 1), int2(1, -1) };
		int2 local = int2(gtid.xy) + HoleReach;
		float level = -1e6;
		[unroll] for (uint d = 0; d < 4; d++)
		{
			float levelA = -1e6, levelB = -1e6;
			[unroll] for (int r = 1; r <= HoleReach; r++)
			{
				int2 a = local + Directions[d] * r;
				int2 b = local - Directions[d] * r;
				float2 sa = Cells[a.y * Apron + a.x];
				float2 sb = Cells[b.y * Apron + b.x];
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
