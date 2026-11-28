// ============================================================================
// PBR Water - the projected grids' vertices (ProjectedGrid::Build), once a frame for every pass that draws them. One grid
// per water level on screen (the sea's, each lake's), each its own run of the buffers (GridLevel).
//
// GridCS   each vertex is a point of the screen, (columns + 1) x (rows + 1) of them spread evenly over the part of the
//          screen the level's water can cover (the rows end at its horizon, the columns and rows at its cells' bounds),
//          cast onto its plane: as dense near the camera as far away, in pixels. The waves are evaluated there
//          (EvaluateVertexWaves), filtered to the grid's spacing on the plane at that point, so a cell never carries a
//          wave shorter than itself: the vertices slide over the water as the camera turns, and only what they can
//          resolve moves with them.
// FlagCS   flags the cells a breaker needs more of: a plunger curls there (VertexWaves shore.y) and the cell is coarser
//          than the lip wants (some 50 vertices across the deep-water wavelength, ShoreCurlWeight; GridSpacing in
//          ShoreWaves.hlsli makes the same test). Each flagged cell takes a patch from the level's run; a cell past the run's end stays
//          coarse.
// ArgsCS   each level's patch count into its draw and dispatch arguments.
// PatchCS  each patch's (P + 1)^2 vertices: its corners the coarse cell's, the points on an edge shared with an unflagged
//          cell on that edge's straight line (the neighbour draws it as one edge: no crack), the rest evaluated at the
//          patch's spacing. Two flagged cells work out their shared edge's points from the same grid coordinates.
// The vertices hold camera-relative positions (Water.hlsl GridWaterVertex brings them into the draw's model space).
// ============================================================================

#include "PBRWater/ComputeTessellation.hlsli"
#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/ProjectedGrid.hlsli"
#include "PBRWater/VertexWaves.hlsli"

cbuffer ProjectedGridCB : register(b0)
{
	row_major float4x4 ClipToWorld;  // camera-relative inverse view-projection, unjittered
	uint4 Run;                       // x the level this dispatch works on, y the levels this frame
	float4 Cells;                    // SeaLevels' mapping: xy the first cell, zw the cells across (ProjectedGrid::CellsMapping)
};

StructuredBuffer<PBRWaterGrid::Level> Levels : register(t0);
RWStructuredBuffer<PBRWaterTess::Vertex> Vertices : register(u0);
RWStructuredBuffer<uint> Flags : register(u1);    // per cell: 0 coarse, 1 refined (a patch drew it), 2 no water seen there (not drawn)
RWStructuredBuffer<uint> Patches : register(u2);  // per patch: its cell, by level runs
RWByteAddressBuffer Counters : register(u3);      // per level: patches asked for
RWStructuredBuffer<PBRWaterTess::Vertex> PatchVertices : register(u4);
RWByteAddressBuffer Args : register(u5);         // per level: draw arguments (16 bytes), then per level: dispatch arguments (12 bytes)
RWByteAddressBuffer CellIndices : register(u6);  // per cell: its two triangles' vertex indices (the cells' index buffer)

/// Where the ray through NDC `ndc` meets the level's plane, camera-relative. A ray that misses it (past the horizon) or meets
/// it beyond plane.y gives the point plane.y away on the plane in its direction: the grid's last rows fold onto that ring.
float3 Cast(float2 ndc, PBRWaterGrid::Level l)
{
	float4 p = mul(ClipToWorld, float4(ndc, 0.5, 1.0));
	float3 dir = normalize(p.xyz / p.w);
	float level = l.plane.x;
	float3 far = float3(dir.xy * (l.plane.y / max(length(dir.xy), 1e-6)), level);
	if (dir.z * level <= 0.0)
		return far;
	float3 hit = dir * (level / dir.z);
	return dot(hit.xy, hit.xy) < l.plane.y * l.plane.y ? hit : far;
}

/// Whether one of the level's cells (SeaLevels, at absolute `level`) lies within `reach` (at most 2048 units) of absolute `absXY`.
bool LevelWaterNear(float2 absXY, float reach, float level)
{
	bool near = false;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		float2 cell = floor((absXY + float2(k & 1 ? reach : -reach, k & 2 ? reach : -reach)) * (1.0 / 4096.0)) - Cells.xy;
		if (all(cell >= 0.0) && all(cell < Cells.zw) && abs(PBRWater::SeaLevels.Load(int3(cell, 0)) - level) < 2.0)
			near = true;
	}
	return near;
}

float2 GridNdc(float2 g, PBRWaterGrid::Level l)
{
	return float2(l.range.x, l.range.z) + g * float2(l.range.y - l.range.x, l.range.w - l.range.z) / float2(l.layout.xy);
}

/// The vertex at grid coordinates `g` (cells, fractional inside a patch), its waves filtered to `step` cells' spacing.
PBRWaterTess::Vertex Evaluate(float2 g, float step, PBRWaterGrid::Level l)
{
	float3 p = Cast(GridNdc(g, l), l);
	// The spacing's longer side on the plane: the waves shorter than it are filtered out of the displacement.
	float spacing = max(distance(Cast(GridNdc(g + float2(step, 0.0), l), l), p), distance(Cast(GridNdc(g + float2(0.0, step), l), l), p));
	float2 absXY = p.xy + FrameBuffer::CameraPosAdjust.xy;
	// Far from the level's water (farther than any wave carries it sideways) the waves are not evaluated: FlagCS hides the
	// cells there.
	PBRWater::VertexWaves waves = (PBRWater::VertexWaves)0;
	waves.surface.shore = float4(0.0, 0.0, 0.0, p.z);
	[branch] if (LevelWaterNear(absXY, 1024.0, l.plane.x + FrameBuffer::CameraPosAdjust.z))
	{
		// Inside a breaker's patch only the lip is carried finer: every other wave is filtered as on the coarse cells
		// around it, so a cell turning into a patch and back does not show finer waves popping in and out.
		float coarse = spacing;
		[branch] if (step < 1.0)
			coarse = max(distance(Cast(GridNdc(g + float2(1.0, 0.0), l), l), p), distance(Cast(GridNdc(g + float2(0.0, 1.0), l), l), p));
		waves = PBRWater::EvaluateVertexWaves(p, 0.0, max(coarse, 1.0), true, max(spacing, 1.0));
	}
	PBRWaterTess::Vertex o;
	o.position = p;
	o.river = 0.0;
	o.current = waves.surface.current;
	o.previous = waves.surface.previous;
	o.texCoordX = dot(l.texCoordU.xy, absXY) + l.texCoordU.z;
	o.texCoordY = dot(l.texCoordV.xy, absXY) + l.texCoordV.z;
	o.shore = waves.surface.shore;
	o.color = uint2(0x3C003C00, 0x3C003C00);  // white, alpha 1 (half floats)
	return o;
}

PBRWaterTess::Vertex LerpVertex(PBRWaterTess::Vertex a, PBRWaterTess::Vertex b, float t)
{
	PBRWaterTess::Vertex o = a;
	o.position = lerp(a.position, b.position, t);
	o.current = lerp(a.current, b.current, t);
	o.previous = lerp(a.previous, b.previous, t);
	o.texCoordX = lerp(a.texCoordX, b.texCoordX, t);
	o.texCoordY = lerp(a.texCoordY, b.texCoordY, t);
	o.shore = lerp(a.shore, b.shore, t);
	return o;
}

[numthreads(8, 8, 1)] void GridCS(uint3 id : SV_DispatchThreadID) {
	PBRWaterGrid::Level l = Levels[Run.x];
	if (any(id.xy > l.layout.xy))
		return;
	Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy, l.layout.x)] = Evaluate(float2(id.xy), 1.0, l);
}

/**
 * Whether the water of the level at absolute `level` can show in a cell whose displaced corners are `p[4]` (camera-relative):
 * one of the level's cells (SeaLevels, as SeaCovers tests per pixel) under the corners' bounding box, and the ground not
 * well above the water at all four corners. A cell neither covers is drawn collapsed: under the terrain the stencil pass
 * would run its discarding pixel shader (no early depth test) for nothing.
 */
bool WaterCanShow(float3 p[4], float level)
{
	float2 lo = min(min(p[0].xy, p[1].xy), min(p[2].xy, p[3].xy)) + FrameBuffer::CameraPosAdjust.xy;
	float2 hi = max(max(p[0].xy, p[1].xy), max(p[2].xy, p[3].xy)) + FrameBuffer::CameraPosAdjust.xy;
	// Wider than a cell, the box's corners do not reach every cell under it.
	if (any(hi - lo >= 4096.0))
		return true;
	bool covered = false;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		float2 cell = floor(float2(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y) * (1.0 / 4096.0)) - Cells.xy;
		if (all(cell >= 0.0) && all(cell < Cells.zw) && abs(PBRWater::SeaLevels.Load(int3(cell, 0)) - level) < 2.0)
			covered = true;
	}
	if (!covered)
		return false;
	// The ground above the water at every corner by more than it can dip between them (slopes up to 1) and two metres.
	float margin = length(hi - lo) + 2.0 * PBRWater::UnitsPerMetre;
	[unroll] for (uint c = 0; c < 4; c++)
	{
		float ground;
		float2 grad;
		if (!PBRWater::SampleTerrain(p[c].xy + FrameBuffer::CameraPosAdjust.xy, false, ground, grad) || ground - (p[c].z + FrameBuffer::CameraPosAdjust.z) < margin)
			return true;
	}
	return false;
}

[numthreads(8, 8, 1)] void FlagCS(uint3 id : SV_DispatchThreadID) {
	PBRWaterGrid::Level l = Levels[Run.x];
	if (any(id.xy >= l.layout.xy))
		return;
	uint columns = l.layout.x;
	PBRWaterTess::Vertex a = Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy, columns)];
	PBRWaterTess::Vertex b = Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy + uint2(1, 0), columns)];
	PBRWaterTess::Vertex c = Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy + uint2(0, 1), columns)];
	PBRWaterTess::Vertex d = Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy + uint2(1, 1), columns)];
	float curl = max(max(a.shore.y, b.shore.y), max(c.shore.y, d.shore.y));
	float spacing = max(distance(a.position.xy, b.position.xy), distance(a.position.xy, c.position.xy));
	float lip = max(PBRWater::Shore3.w, PBRWater::UnitsPerMetre) / 25.0;
	uint cell = id.y * columns + id.x;
	float3 corners[4] = { a.position + a.current, b.position + b.current, c.position + c.current, d.position + d.current };
	uint flag = WaterCanShow(corners, l.plane.x + FrameBuffer::CameraPosAdjust.z) ? 0 : 2;
	// Only where the patch brings the cell near what the lip wants (ShoreWaves.hlsli GridSpacing, the same test): further
	// out a patch stays too coarse for any curl. Only where a breaker curls: the whole shore train would overrun the patches.
	[branch] if (flag == 0 && curl > 0.01 && spacing > 0.5 * lip && spacing < 2.0 * l.patches.z * lip)
	{
		uint index;
		Counters.InterlockedAdd(Run.x * 4, 1, index);
		if (index < l.patches.y) {
			Patches[l.patches.x + index] = cell;
			flag = 1;
		}
	}
	Flags[l.layout.w + cell] = flag;
	// And in the cell's first corner, which PatchCS reads (the grids have no rivers: GridWaterVertex clears it).
	Vertices[l.layout.z + PBRWaterGrid::VertexIndex(id.xy, columns)].river = (float)flag;
	// The cell's two triangles, corners (0,0) (1,0) (0,1) and (1,0) (1,1) (0,1) as PBRWaterGrid::ListCorner orders them; a
	// refined or hidden cell is degenerate.
	uint i00 = l.layout.z + PBRWaterGrid::VertexIndex(id.xy, columns);
	uint i10 = i00 + 1;
	uint i01 = i00 + columns + 1;
	uint i11 = i01 + 1;
	uint4 first = flag == 0 ? uint4(i00, i10, i01, i10) : uint4(i00, i00, i00, i00);
	uint2 second = flag == 0 ? uint2(i11, i01) : uint2(i00, i00);
	uint at = (l.layout.w + cell) * 24;
	CellIndices.Store4(at, first);
	CellIndices.Store2(at + 16, second);
}

	[numthreads(1, 1, 1)] void ArgsCS(uint3 id : SV_DispatchThreadID)
{
	for (uint i = 0; i < PBRW_GRID_MAX_LEVELS; i++) {
		uint count = i < Run.y ? min(Counters.Load(i * 4), Levels[i].patches.y) : 0;
		uint n = i < Run.y ? Levels[i].patches.z : 1;
		Args.Store4(i * 16, uint4(n * n * 6, count, 0, 0));
		Args.Store3(PBRW_GRID_MAX_LEVELS * 16 + i * 12, uint3(count, 1, 1));
	}
}

[numthreads(PBRW_GRID_PATCH + 1, PBRW_GRID_PATCH + 1, 1)] void PatchCS(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
	PBRWaterGrid::Level l = Levels[Run.x];
	const uint n = l.patches.z;  // ProjectedGrid::Build: as fine as 2 pixels a cell allows, at most PBRW_GRID_PATCH
	if (any(local.xy > n))
		return;
	uint patch = l.patches.x + group.x;
	uint columns = l.layout.x;
	uint cell = Patches[patch];
	uint2 c = uint2(cell % columns, cell / columns);
	uint2 s = local.xy;
	bool edgeX = s.x == 0 || s.x == n;
	bool edgeY = s.y == 0 || s.y == n;

	PBRWaterTess::Vertex o;
	if (edgeX && edgeY) {
		// A corner: the coarse vertex itself.
		o = Vertices[l.layout.z + PBRWaterGrid::VertexIndex(c + s / n, columns)];
	} else {
		bool coarseNeighbour = false;
		uint2 from = c, to = c;
		float t = 0.0;
		if (edgeX || edgeY) {
			// The cell across this edge, and the edge's corners in grid order.
			int2 across = int2(c) + (edgeY ? int2(0, s.y == 0 ? -1 : 1) : int2(s.x == 0 ? -1 : 1, 0));
			if (all(across >= 0) && all(across < int2(l.layout.xy)))
				coarseNeighbour = Flags[l.layout.w + (uint)across.y * columns + (uint)across.x] == 0;
			if (edgeY) {
				from = c + uint2(0, s.y / n);
				to = from + uint2(1, 0);
				t = (float)s.x / n;
			} else {
				from = c + uint2(s.x / n, 0);
				to = from + uint2(0, 1);
				t = (float)s.y / n;
			}
		}
		if (coarseNeighbour) {
			o = LerpVertex(Vertices[l.layout.z + PBRWaterGrid::VertexIndex(from, columns)], Vertices[l.layout.z + PBRWaterGrid::VertexIndex(to, columns)], t);
		} else {
			o = Evaluate(float2(c) + float2(s) / n, 1.0 / n, l);
		}
	}
	o.river = 0.0;  // the corners carry their cell's flag (FlagCS)
	PatchVertices[PBRWaterGrid::PatchVertex(patch, s, n)] = o;
}
