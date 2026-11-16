// ============================================================================
// PBR Water - the surf simulation's active tiles (ShoreSimulation::UpdateFoam).
//
// After a frame's first step (SwashStepCS over the whole grid), most of the grid steps to the same state again and again:
// walls, the open water where the train alone is the water (its state is the train's, a function of this frame's
// Prep and Stage), and dry sand with no foam. The later steps run only on the 8 x 8 tiles near anything else.
//   SWASH_TILES_MARK  one thread per cell: marks its tile busy when the cell is none of those
//   SWASH_TILES_LIST  one thread per tile: appends it when a busy tile lies within Radius tiles. Water moves at most a
//                     cell a step (a quarter of one at the velocity cap), so a radius of ceil(steps / 8) tiles keeps it
//                     inside the active tiles for the whole frame.
// ============================================================================

cbuffer SwashTilesCB : register(b0)
{
	uint GridSize;   // cells
	uint TileCount;  // tiles across
	int Radius;      // tiles
	uint Pad0;
};

#if defined(SWASH_TILES_MARK)

Texture2D<float4> Surf : register(t0);  // the state after the frame's first step
Texture2D<float4> Prep : register(t1);  // ShoreFoamCS: x bed (m above the still water), z outgoing share
RWTexture2D<uint> Busy : register(u0);  // one texel per tile, cleared to 0 each frame

/// No ground: a wall (ShoreFoamCS, SwashStepCS).
static const float Wall = 100.0;

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= GridSize))
		return;
	int2 c = int2(id.xy);
	float4 prep = Prep.Load(int3(c, 0));
	if (prep.x > Wall)
		return;
	// The momentum pass takes the train's state where this cell and its east and north neighbours are the train's alone.
	int2 e = min(c + int2(1, 0), (int)GridSize - 1);
	int2 n = min(c + int2(0, 1), (int)GridSize - 1);
	if (min(prep.z, min(Prep.Load(int3(e, 0)).z, Prep.Load(int3(n, 0)).z)) > 0.999)
		return;
	// Dry sand with no foam: both passes leave it at rest while its neighbours are too.
	float4 s = Surf.Load(int3(c, 0));
	if (prep.x >= 0.0 && s.x < 1e-5 && s.w < 0.01)
		return;
	Busy[id.xy / 8] = 1;
}

#elif defined(SWASH_TILES_LIST)

Texture2D<uint> Busy : register(t0);
AppendStructuredBuffer<uint> Tiles : register(u0);  // x | y << 16

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= TileCount))
		return;
	int2 t = int2(id.xy);
	int2 lo = max(t - Radius, 0);
	int2 hi = min(t + Radius, (int)TileCount - 1);
	[loop] for (int y = lo.y; y <= hi.y; y++)
	{
		[loop] for (int x = lo.x; x <= hi.x; x++)
		{
			if (Busy.Load(int3(x, y, 0)) != 0) {
				Tiles.Append(id.x | (id.y << 16));
				return;
			}
		}
	}
}

#endif
