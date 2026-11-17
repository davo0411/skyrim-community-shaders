// ============================================================================
// PBR Water - the bisection tessellator's passes (BisectionTessellator; rules and layout: Bisection.hlsli; design:
// docs/development/pbr-water-bisection-tessellator.md). Once a frame, from the water meshes' base triangles:
//   InitCS      one bisector per root, its neighbours the roots';
//   then MaxIterations times:
//     ClassifyCS  culls, and marks the bisectors whose edge is longer than the spacing asks: a diamond (it and the
//                 bisector across its refinement edge) splits whole; a bisector whose twin is coarser asks the twin to
//                 split first (FlagForced) and waits;
//     AllocateCS  each diamond's leader reserves its children's new slots, both or none;
//     LinkCS      writes the next subdivision: split bisectors their children, the others themselves with their
//                 neighbours re-pointed at the children that now share their edges;
//     ArgsCS      the next iteration's count and dispatch size;
//   CountCS, OffsetsCS, ScatterCS  the visible bisectors in per-mesh lists, and each mesh's draw arguments;
//   VertexCS    every grid vertex of every listed bisector: position and the waves, once for every pass.
// ============================================================================

#include "PBRWater/Bisection.hlsli"
#include "PBRWater/ComputeTessellation.hlsli"
#include "PBRWater/VertexWaves.hlsli"

cbuffer BisectionCB : register(b0)
{
	uint4 Counts;                 // x roots, y bisector capacity, z meshes, w unused
	float4 Cull;                  // x sideways margin, y vertical margin (units), z deepest bisection, w unused
	row_major float4x4 ViewProj;  // camera-relative view-projection
};

StructuredBuffer<PBRWaterBisection::Root> Roots : register(t0);

RWStructuredBuffer<PBRWaterBisection::Bisector> Bisectors : register(u0);  // this iteration's subdivision
RWStructuredBuffer<PBRWaterBisection::Bisector> Next : register(u1);       // LinkCS: the next
RWStructuredBuffer<uint> ChildB : register(u2);                            // AllocateCS: each splitting bisector's child B
// x count, y the allocation cursor, z the end of what was allocated, w listed bisectors; bytes 16-27
// ClassifyCS/AllocateCS/LinkCS's DispatchIndirect arguments, 32-43 VertexCS's (BisectionTessellator copies them out);
// 48 and 52 the largest displacement this frame's vertices reached (asuint), sideways and up or down, as TessellationCS's.
RWByteAddressBuffer State : register(u3);
RWByteAddressBuffer MeshCounts : register(u4);  // per mesh: listed bisectors, then (OffsetsCS) its scatter cursor
RWByteAddressBuffer DrawArgs : register(u5);    // per mesh: DrawInstancedIndirect's arguments (16 bytes)
RWStructuredBuffer<uint> List : register(u6);   // the visible bisectors, mesh by mesh
RWStructuredBuffer<PBRWaterTess::Vertex> Vertices : register(u7);
groupshared uint GroupReach[2];

static const uint StateCount = 0;
static const uint StateCursor = 4;
static const uint StateEnd = 8;
static const uint StateListed = 12;
static const uint StateDispatch = 16;
static const uint StateVertexDispatch = 32;
static const uint StateReach = 48;

uint Count() { return State.Load(StateCount); }

// ----------------------------------------------------------------------------
// Geometry
// ----------------------------------------------------------------------------

void BisectorCorners(PBRWaterBisection::Bisector b, out float3 p0, out float3 p1, out float3 p2)
{
	PBRWaterBisection::Root r = Roots[b.root];
	p0 = PBRWaterBisection::RootPoint(r, b.b0);
	p1 = PBRWaterBisection::RootPoint(r, b.b1);
	p2 = PBRWaterBisection::RootPoint(r, b.b2);
}

/// Closest point of triangle abc to the origin (the camera; Ericson, Real-Time Collision Detection 5.1.5).
float3 ClosestToCamera(float3 a, float3 b, float3 c)
{
	float3 ab = b - a, ac = c - a, ap = -a;
	float d1 = dot(ab, ap), d2 = dot(ac, ap);
	float3 result;
	if (d1 <= 0.0 && d2 <= 0.0)
		result = a;
	else {
		float3 bp = -b;
		float d3 = dot(ab, bp), d4 = dot(ac, bp);
		float3 cp = -c;
		float d5 = dot(ab, cp), d6 = dot(ac, cp);
		float vc = d1 * d4 - d3 * d2;
		float vb = d5 * d2 - d1 * d6;
		float va = d3 * d6 - d5 * d4;
		if (d3 >= 0.0 && d4 <= d3)
			result = b;
		else if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
			result = a + ab * (d1 / (d1 - d3));
		else if (d6 >= 0.0 && d5 <= d6)
			result = c;
		else if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
			result = a + ac * (d2 / (d2 - d6));
		else if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
			result = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
		else {
			float denom = 1.0 / (va + vb + vc);
			result = a + ab * (vb * denom) + ac * (vc * denom);
		}
	}
	return result;
}

/// Conservative frustum test of the triangle's box, grown by how far the waves can carry its points (Cull.xy).
bool OutsideFrustum(float3 p0, float3 p1, float3 p2)
{
	float3 lo = min(p0, min(p1, p2)) - Cull.xxy;
	float3 hi = max(p0, max(p1, p2)) + Cull.xxy;
	uint outside = 0x1F;
	[unroll] for (uint i = 0; i < 8; i++)
	{
		float3 p = float3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
		float4 clip = mul(ViewProj, float4(p, 1.0));
		uint mask = (clip.x < -clip.w ? 1u : 0u) | (clip.x > clip.w ? 2u : 0u) | (clip.y < -clip.w ? 4u : 0u) | (clip.y > clip.w ? 8u : 0u) |
		            (clip.w <= 0.0 ? 16u : 0u);
		outside &= mask;
	}
	return outside != 0;
}

// ----------------------------------------------------------------------------
// Refinement
// ----------------------------------------------------------------------------

[numthreads(64, 1, 1)] void InitCS(uint3 id : SV_DispatchThreadID) {
	if (id.x == 0) {
		State.Store4(StateCount, uint4(Counts.x, Counts.x, Counts.x, 0));
		State.Store3(StateDispatch, uint3((Counts.x + 63) / 64, 1, 1));
		State.Store2(StateReach, uint2(0, 0));
	}
	if (id.x >= Counts.x)
		return;
	PBRWaterBisection::Bisector b;
	b.b0 = float2(0.0, 0.0);
	b.b1 = float2(1.0, 0.0);
	b.b2 = float2(0.0, 1.0);
	b.root = id.x;
	b.depth = 0;
	b.n = Roots[id.x].neighbours.xyz;  // the roots are the first bisectors: root i is bisector i
	b.flags = 0;
	Bisectors[id.x] = b;
}

	[numthreads(64, 1, 1)] void ClassifyCS(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	float3 p0, p1, p2;
	BisectorCorners(b, p0, p1, p2);
	bool culled = OutsideFrustum(p0, p1, p2);
	uint previous;
	if (culled)
		InterlockedOr(Bisectors[id.x].flags, PBRWaterBisection::FlagCulled, previous);
	else
		InterlockedAnd(Bisectors[id.x].flags, ~PBRWaterBisection::FlagCulled, previous);

	// The grid carries GridN segments along each edge: split while they are longer than the spacing wanted where the
	// triangle comes nearest the camera. A culled bisector splits only when a neighbour waits on it.
	// (A branch: || does not short-circuit, and a forced bisector needs no spacing.)
	bool want = (b.flags & PBRWaterBisection::FlagForced) != 0;
	[branch] if (!want && !culled && b.depth < (uint)Cull.z)
	{
		float edge = length(p2 - p0) / PBRWaterBisection::GridN;
		want = edge > PBRWaterBisection::TessellationSpacing(ClosestToCamera(p0, p1, p2));
	}
	if (!want || b.depth >= (uint)Cull.z)
		return;
	uint twin = b.n.z;
	if (twin == PBRWaterBisection::None || Bisectors[twin].n.z == id.x) {
		InterlockedOr(Bisectors[id.x].flags, PBRWaterBisection::FlagSplit, previous);
		if (twin != PBRWaterBisection::None)
			InterlockedOr(Bisectors[twin].flags, PBRWaterBisection::FlagSplit, previous);
	} else {
		// The twin is coarser: our refinement edge is one of its legs. It splits first, and one of its children then
		// shares the edge as its own refinement edge.
		InterlockedOr(Bisectors[twin].flags, PBRWaterBisection::FlagForced, previous);
	}
}

[numthreads(64, 1, 1)] void AllocateCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	if ((b.flags & PBRWaterBisection::FlagSplit) == 0)
		return;
	uint twin = b.n.z;
	// The diamond's leader reserves both children B at once: a diamond splits whole or not at all.
	if (twin != PBRWaterBisection::None && twin < id.x)
		return;
	uint need = twin == PBRWaterBisection::None ? 1 : 2;
	uint base;
	State.InterlockedAdd(StateCursor, need, base);
	uint previous;
	if (base + need > Counts.y) {
		InterlockedAnd(Bisectors[id.x].flags, ~PBRWaterBisection::FlagSplit, previous);
		if (twin != PBRWaterBisection::None)
			InterlockedAnd(Bisectors[twin].flags, ~PBRWaterBisection::FlagSplit, previous);
		return;
	}
	ChildB[id.x] = base;
	if (twin != PBRWaterBisection::None)
		ChildB[twin] = base + 1;
	// Every slot below the end is written: allocations past the capacity come after all that fit.
	State.InterlockedMax(StateEnd, base + need, previous);
}

/// The bisector across an edge of `self` once this iteration's splits are done: `x` itself, or the child of `x` that
/// now holds the edge (its edge 0 goes to its child A, its edge 1 to its child B).
uint Resolve(uint x, uint self)
{
	uint result = x;
	if (x != PBRWaterBisection::None) {
		PBRWaterBisection::Bisector other = Bisectors[x];
		if ((other.flags & PBRWaterBisection::FlagSplit) != 0 && other.n.y == self)
			result = ChildB[x];
	}
	return result;
}

[numthreads(64, 1, 1)] void LinkCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	if ((b.flags & PBRWaterBisection::FlagSplit) == 0) {
		b.n = uint3(Resolve(b.n.x, id.x), Resolve(b.n.y, id.x), Resolve(b.n.z, id.x));
		b.flags &= ~PBRWaterBisection::FlagSplit;
		Next[id.x] = b;
		return;
	}
	uint slotB = ChildB[id.x];
	uint twin = b.n.z;
	PBRWaterBisection::Bisector a, c;
	PBRWaterBisection::Children(b, a, c);
	// The twin's child A (its own slot) covers m-c2, its child B c0-m.
	uint twinA = twin;
	uint twinB = twin != PBRWaterBisection::None ? ChildB[twin] : PBRWaterBisection::None;
	a.n = uint3(slotB, twinB, Resolve(b.n.x, id.x));
	c.n = uint3(twinA, id.x, Resolve(b.n.y, id.x));
	Next[id.x] = a;
	Next[slotB] = c;
}

	[numthreads(1, 1, 1)] void ArgsCS(uint3 id : SV_DispatchThreadID)
{
	uint count = max(State.Load(StateEnd), Count());
	State.Store3(StateCount, uint3(count, count, count));
	State.Store3(StateDispatch, uint3((count + 63) / 64, 1, 1));
}

// ----------------------------------------------------------------------------
// Draw lists
// ----------------------------------------------------------------------------

[numthreads(64, 1, 1)] void CountCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	if ((b.flags & PBRWaterBisection::FlagCulled) != 0)
		return;
	uint previous;
	MeshCounts.InterlockedAdd(Roots[b.root].mesh * 4, 1, previous);
}

	[numthreads(1, 1, 1)] void OffsetsCS(uint3 id : SV_DispatchThreadID)
{
	// Each mesh's draw: its bisectors' triangles, non-indexed. SV_VertexID starts at StartVertexLocation, so the vertex
	// shader finds the bisector from it alone (list index = vertex id / (3 GridTriangles)).
	static const uint VerticesPerBisector = 3 * PBRWaterBisection::GridTriangles;
	uint offset = 0;
	for (uint m = 0; m < Counts.z; m++) {
		uint count = MeshCounts.Load(m * 4);
		DrawArgs.Store4(m * 16, uint4(count * VerticesPerBisector, 1, offset * VerticesPerBisector, 0));
		MeshCounts.Store(m * 4, offset);  // the scatter's cursor from here on
		offset += count;
	}
	State.Store(StateListed, offset);
	State.Store3(StateVertexDispatch, uint3((offset * PBRWaterBisection::GridVertices + 63) / 64, 1, 1));
}

[numthreads(64, 1, 1)] void ScatterCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	if ((b.flags & PBRWaterBisection::FlagCulled) != 0)
		return;
	uint index;
	MeshCounts.InterlockedAdd(Roots[b.root].mesh * 4, 1, index);
	List[index] = id.x;
}

// ----------------------------------------------------------------------------
// Vertices
// ----------------------------------------------------------------------------

/// Evaluates grid vertex `v` of the listed bisectors; returns how far its waves moved it (x sideways, y up or down).
float2 EvaluateVertex(uint v)
{
	uint listIndex = v / PBRWaterBisection::GridVertices;
	uint2 g = PBRWaterTess::VertexCoords(v % PBRWaterBisection::GridVertices, PBRWaterBisection::GridN);
	PBRWaterBisection::Bisector b = Bisectors[List[listIndex]];
	PBRWaterBisection::Root r = Roots[b.root];

	// The grid point as weights of the root's corners: dyadic (GridN is a power of two), so a point two bisectors share
	// comes out the same from both.
	float2 rootB = b.b0 + (b.b1 - b.b0) * ((float)g.x / PBRWaterBisection::GridN) + (b.b2 - b.b0) * ((float)g.y / PBRWaterBisection::GridN);
	float3 w = PBRWaterBisection::Weights(rootB);
	float weights[3] = { w.x, w.y, w.z };

	// Summed in canonical corner order: a weight of exactly 0 adds exactly 0, so a point on an edge two roots (two
	// meshes) share comes out the same from both.
	precise float3 world = 0.0;
	precise float3 model = 0.0;
	float2 uv = 0.0;
	float4 color = 0.0;
	float2 uvs[3] = { r.uv01.xy, r.uv01.zw, r.uv2 };
	[unroll] for (uint k = 0; k < 3; k++)
	{
		uint c = (r.order >> (2 * k)) & 3;
		world += r.world[c].xyz * weights[c];
		model += r.model[c].xyz * weights[c];
		uv += uvs[c] * weights[c];
		color += r.color[c] * weights[c];
	}

	// Filtered to the spacing wanted at this point (not the bisector's own), so a split only adds samples of a surface
	// already smoothed to them: nothing pops by more than the sampling changes.
	PBRWater::VertexWaves waves = PBRWater::EvaluateVertexWaves(world, PBRWaterBisection::TessellationSpacing(world), true);

	PBRWaterTess::Vertex o;
	o.position = model;
	o.flowDamping = waves.flowDamping;
	o.current = waves.surface.current;
	o.previous = waves.surface.previous;
	o.texCoordX = uv.x;
	o.texCoordY = uv.y;
	o.shore = waves.surface.shore;
	o.flowVelocity = waves.flowVelocity;
	o.color = uint2(f32tof16(color.x) | (f32tof16(color.y) << 16), f32tof16(color.z) | (f32tof16(color.w) << 16));
	Vertices[v] = o;
	return float2(length(o.current.xy), abs(o.current.z));
}

[numthreads(64, 1, 1)] void VertexCS(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
	if (gi == 0) {
		GroupReach[0] = 0;
		GroupReach[1] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	// A branch, not ?: (which may evaluate both sides): the group's threads past the last vertex run nothing.
	float2 reach = 0.0;
	[branch] if (id.x < State.Load(StateListed) * PBRWaterBisection::GridVertices)
		reach = EvaluateVertex(id.x);
	InterlockedMax(GroupReach[0], asuint(reach.x));
	InterlockedMax(GroupReach[1], asuint(reach.y));
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0) {
		uint previous;
		State.InterlockedMax(StateReach, GroupReach[0], previous);
		State.InterlockedMax(StateReach + 4, GroupReach[1], previous);
	}
}
