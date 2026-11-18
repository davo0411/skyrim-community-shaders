// ============================================================================
// PBR Water - the bisection tessellator's passes (BisectionTessellator; rules and layout: Bisection.hlsli; design:
// docs/development/pbr-water-bisection-tessellator.md). The subdivision is kept from frame to frame; once a frame:
//   InitCS         (only when the roots changed) one bisector per root, its neighbours the roots';
//   FrameStartCS   (otherwise) this frame's counters, from last frame's subdivision;
//   MergeDecideCS  marks the diamonds whose four children (two at the mesh's border) are all leaves and whose parents the
//                  view no longer needs split: a level of merging a frame, against a hysteresis below the split rule;
//   MergeLinkCS    writes their parents into the children A's slots and frees the children B's, the neighbours
//                  re-pointed at the parents;
//   CompactCountCS, CompactWriteCS, CompactArgsCS  pack the live bisectors to the front, links renumbered;
//   then up to the host's iteration count:
//     ClassifyCS  culls, and marks the bisectors whose edge is longer than the spacing asks: a diamond (it and the
//                 bisector across its refinement edge) splits whole, the first of its two to ask reserving both new
//                 slots; a bisector whose twin is coarser asks the twin to split first (FlagForced) and waits;
//     LinkCS      writes the next subdivision: split bisectors their children, the others themselves with their
//                 neighbours re-pointed at the children that now share their edges;
//     ArgsCS      the next iteration's count and dispatch size, zero once an iteration changed nothing;
//   FixupCS, then ClassifyCS once more (the culling of the last children);
//   CountCS, OffsetsCS, ScatterCS  the visible bisectors in per-mesh lists, and each mesh's draw arguments;
//   VertexCS    every grid vertex of every listed bisector: position and the waves, once for every pass.
// Following the view frame by frame, the subdivision changes a level or two where the camera moved: most frames need an
// iteration or two, where rebuilding it from the roots took one a level (with the forced chains, some twenty).
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

RWStructuredBuffer<PBRWaterBisection::Bisector> Bisectors : register(u0);  // the subdivision
RWStructuredBuffer<PBRWaterBisection::Bisector> Next : register(u1);       // LinkCS, MergeLinkCS, CompactWriteCS: the next
// ClassifyCS: each splitting bisector's child B; CompactCountCS: each bisector's new slot.
RWStructuredBuffer<uint> ChildB : register(u2);
// x count, y the allocation cursor, z the end of what was allocated, w listed bisectors; bytes 16-27 the per-bisector
// passes' DispatchIndirect arguments, 32-43 VertexCS's (BisectionTessellator copies them out); 48 and 52 the largest
// displacement this frame's vertices reached (asuint), sideways and up or down; 56 the forced flags
// set this iteration; 60 1 while the latest subdivision is in Next, not Bisectors (iterations that dispatch nothing still
// have their buffers swapped: FixupCS copies it back); 64-75 the refinement passes' DispatchIndirect arguments, zero once
// an iteration changed nothing; 76 the iterations that ran (the host sizes the next frame's loop by it); 80 the
// compaction's count of live bisectors.
RWByteAddressBuffer State : register(u3);
RWByteAddressBuffer MeshCounts : register(u4);  // per mesh: listed bisectors, then (OffsetsCS) its scatter cursor
// Per mesh: DrawInstancedIndirect's arguments (16 bytes), then after all of them (from byte DrawOffsets) where its
// bisectors start in the draw lists, which the vertex shader adds to SV_VertexID (BisectionVertexIndex).
RWByteAddressBuffer DrawArgs : register(u5);
RWStructuredBuffer<uint> List : register(u6);  // the visible bisectors, mesh by mesh
RWStructuredBuffer<PBRWaterTess::Vertex> Vertices : register(u7);
groupshared uint GroupReach[2];

static const uint StateCount = 0;
static const uint StateCursor = 4;
static const uint StateEnd = 8;
static const uint StateListed = 12;
static const uint StateDispatch = 16;
static const uint StateVertexDispatch = 32;
static const uint StateReach = 48;
static const uint StateForced = 56;
static const uint StateParity = 60;
static const uint StateIterate = 64;
static const uint StateIterations = 76;
static const uint StateCompact = 80;
static const uint DrawOffsets = PBRW_BISECTION_MAX_MESHES * 16;

/// A parent is merged back only once its edge is this far under the split rule's threshold: a bisector at the edge of
/// it would otherwise split and merge on alternate frames.
static const float MergeHysteresis = 0.8;

uint Count() { return State.Load(StateCount); }

/// The counts, cursor and dispatch sizes for `count` bisectors, the refinement running.
void StoreCount(uint count)
{
	State.Store4(StateCount, uint4(count, count, count, 0));
	State.Store3(StateDispatch, uint3((count + 63) / 64, 1, 1));
	State.Store3(StateIterate, uint3((count + 63) / 64, 1, 1));
}

// ----------------------------------------------------------------------------
// Geometry
// ----------------------------------------------------------------------------

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

/// The camera-relative corners of the triangle with root weights `b0`, `b1`, `b2` of root `root`.
void CornersOf(uint root, float2 b0, float2 b1, float2 b2, out float3 p0, out float3 p1, out float3 p2)
{
	PBRWaterBisection::Root r = Roots[root];
	p0 = PBRWaterBisection::RootPoint(r, b0);
	p1 = PBRWaterBisection::RootPoint(r, b1);
	p2 = PBRWaterBisection::RootPoint(r, b2);
}

/**
 * How much finer than the spacing asks the triangle (p0, p1, p2) is: its grid's segment along its legs over the spacing
 * wanted where it comes nearest the camera. Over 1 it splits. The legs, not the refinement edge, are what the grid's
 * segments run along (measured on the hypotenuse, every triangle came out a factor of two finer in area than asked).
 */
float SplitRatio(float3 p0, float3 p1, float3 p2)
{
	float edge = 0.70710678 * length(p2 - p0) / PBRWaterBisection::GridN;
	return edge / PBRWaterBisection::TessellationSpacing(ClosestToCamera(p0, p1, p2));
}

// ----------------------------------------------------------------------------
// The frame's start
// ----------------------------------------------------------------------------

[numthreads(64, 1, 1)] void InitCS(uint3 id : SV_DispatchThreadID) {
	if (id.x == 0) {
		StoreCount(Counts.x);
		State.Store2(StateReach, uint2(0, 0));
		State.Store2(StateForced, uint2(0, 0));  // and the parity
		State.Store(StateIterations, 0);
	}
	if (id.x >= Counts.x)
		return;
	PBRWaterBisection::Bisector b;
	b.b0 = float2(0.0, 0.0);
	b.b1 = float2(1.0, 0.0);
	b.b2 = float2(0.0, 1.0);
	b.root = id.x;
	b.path = 0;
	b.n = Roots[id.x].neighbours.xyz;  // the roots are the first bisectors: root i is bisector i
	b.flags = 0;                       // depth 0
	Bisectors[id.x] = b;
}

[numthreads(1, 1, 1)] void FrameStartCS(uint3 id : SV_DispatchThreadID) {
	StoreCount(Count());
	State.Store2(StateReach, uint2(0, 0));
	State.Store2(StateForced, uint2(0, 0));
	State.Store(StateIterations, 0);
	State.Store(StateCompact, 0);
}

// ----------------------------------------------------------------------------
// Merging
// ----------------------------------------------------------------------------

/// Bisector `a` is a child A whose sibling B (across its edge 0) is a leaf: B's slot, or None.
uint LeafSibling(uint a, PBRWaterBisection::Bisector A)
{
	uint s = A.n.x;
	if (s == PBRWaterBisection::None)
		return s;
	PBRWaterBisection::Bisector B = Bisectors[s];
	bool ok = B.root == A.root && PBRWaterBisection::Depth(B) == PBRWaterBisection::Depth(A) && B.path == (A.path | 1u) && B.n.y == a;
	return ok ? s : PBRWaterBisection::None;
}

/// Whether the parent of child A `A` and child B `B` would still be split by the view (with the hysteresis).
bool ParentStaysSplit(PBRWaterBisection::Bisector A, PBRWaterBisection::Bisector B)
{
	float3 p0, p1, p2;
	CornersOf(A.root, A.b2, A.b0, B.b0, p0, p1, p2);
	if (OutsideFrustum(p0, p1, p2))
		return false;
	return SplitRatio(p0, p1, p2) > MergeHysteresis;
}

[numthreads(64, 1, 1)] void MergeDecideCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector A = Bisectors[id.x];
	// Child A of its parent leads (its slot will hold the parent), and of a diamond the A with the lower slot.
	if (PBRWaterBisection::Depth(A) == 0 || (A.path & 1u) != 0)
		return;
	uint b = LeafSibling(id.x, A);
	if (b == PBRWaterBisection::None)
		return;
	PBRWaterBisection::Bisector B = Bisectors[b];
	// The parent's twin's children: A' across B's edge 0, B' across A's edge 1 (or neither, at the border).
	uint a2 = B.n.x;
	uint b2 = A.n.y;
	if ((a2 == PBRWaterBisection::None) != (b2 == PBRWaterBisection::None))
		return;
	PBRWaterBisection::Bisector A2 = A, B2 = B;
	if (a2 != PBRWaterBisection::None) {
		if (a2 < id.x)
			return;
		A2 = Bisectors[a2];
		if (PBRWaterBisection::Depth(A2) != PBRWaterBisection::Depth(A) || (A2.path & 1u) != 0 || LeafSibling(a2, A2) != b2)
			return;
		B2 = Bisectors[b2];
	}
	if (ParentStaysSplit(A, B) || (a2 != PBRWaterBisection::None && ParentStaysSplit(A2, B2)))
		return;
	uint previous;
	InterlockedOr(Bisectors[id.x].flags, PBRWaterBisection::FlagMergeA, previous);
	InterlockedOr(Bisectors[b].flags, PBRWaterBisection::FlagMergeB, previous);
	if (a2 != PBRWaterBisection::None) {
		InterlockedOr(Bisectors[a2].flags, PBRWaterBisection::FlagMergeA, previous);
		InterlockedOr(Bisectors[b2].flags, PBRWaterBisection::FlagMergeB, previous);
	}
}

/// The slot that holds what was in slot `x` once the merges are done: a merged child B's parent sits in its sibling A's.
uint Merged(uint x)
{
	uint result = x;
	if (x != PBRWaterBisection::None && (Bisectors[x].flags & PBRWaterBisection::FlagMergeB) != 0)
		result = Bisectors[x].n.y;
	return result;
}

[numthreads(64, 1, 1)] void MergeLinkCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	if ((b.flags & PBRWaterBisection::FlagMergeB) != 0) {
		b.flags = PBRWaterBisection::FlagDead;
		Next[id.x] = b;
		return;
	}
	if ((b.flags & PBRWaterBisection::FlagMergeA) != 0) {
		// The parent (c0, c1, c2) of A = (c1, m, c0) and B = (c2, m, c1); its twin in the slot of the twin's child A,
		// across B's edge 0.
		PBRWaterBisection::Bisector B = Bisectors[b.n.x];
		PBRWaterBisection::Bisector p = b;
		p.b0 = b.b2;
		p.b1 = b.b0;
		p.b2 = B.b0;
		p.path = b.path >> 1;
		p.flags = (PBRWaterBisection::Depth(b) - 1) << PBRWaterBisection::DepthShift;
		p.n = uint3(Merged(b.n.z), Merged(B.n.z), B.n.x);
		Next[id.x] = p;
		return;
	}
	b.n = uint3(Merged(b.n.x), Merged(b.n.y), Merged(b.n.z));
	Next[id.x] = b;
}

[numthreads(64, 1, 1)] void CompactCountCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	uint slot = PBRWaterBisection::None;
	if ((Bisectors[id.x].flags & PBRWaterBisection::FlagDead) == 0)
		State.InterlockedAdd(StateCompact, 1, slot);
	ChildB[id.x] = slot;
}

uint Renumbered(uint x)
{
	return x == PBRWaterBisection::None ? x : ChildB[x];
}

[numthreads(64, 1, 1)] void CompactWriteCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	uint slot = ChildB[id.x];
	if (slot == PBRWaterBisection::None)
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	b.n = uint3(Renumbered(b.n.x), Renumbered(b.n.y), Renumbered(b.n.z));
	Next[slot] = b;
}

[numthreads(1, 1, 1)] void CompactArgsCS(uint3 id : SV_DispatchThreadID) {
	StoreCount(State.Load(StateCompact));
}

// ----------------------------------------------------------------------------
// Refinement
// ----------------------------------------------------------------------------

[numthreads(64, 1, 1)] void ClassifyCS(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Count())
		return;
	// Settled in an earlier iteration (its spacing and view do not change within the frame): only a finer neighbour waiting
	// on it can make it split now. Its flags alone are read first: most of the subdivision is settled.
	if ((Bisectors[id.x].flags & (PBRWaterBisection::FlagSettled | PBRWaterBisection::FlagForced)) == PBRWaterBisection::FlagSettled)
		return;
	PBRWaterBisection::Bisector b = Bisectors[id.x];
	float3 p0, p1, p2;
	CornersOf(b.root, b.b0, b.b1, b.b2, p0, p1, p2);
	bool culled = OutsideFrustum(p0, p1, p2);
	uint previous;
	if (culled)
		InterlockedOr(Bisectors[id.x].flags, PBRWaterBisection::FlagCulled, previous);
	else
		InterlockedAnd(Bisectors[id.x].flags, ~PBRWaterBisection::FlagCulled, previous);

	// A culled bisector splits only when a neighbour waits on it. (A branch: || does not short-circuit, and a forced
	// bisector needs no spacing.)
	uint depth = PBRWaterBisection::Depth(b);
	bool want = (b.flags & PBRWaterBisection::FlagForced) != 0;
	[branch] if (!want && !culled && depth < (uint)Cull.z)
		want = SplitRatio(p0, p1, p2) > 1.0;
	if (!want || depth >= (uint)Cull.z) {
		InterlockedOr(Bisectors[id.x].flags, PBRWaterBisection::FlagSettled, previous);
		return;
	}
	uint twin = b.n.z;
	if (twin == PBRWaterBisection::None || Bisectors[twin].n.z == id.x) {
		// A diamond splits whole. The first of its two to ask reserves both children B's slots, both or none (the lower
		// slot holds the claim). A pass of its own for that cost a dispatch and a barrier every iteration.
		uint leader = (twin == PBRWaterBisection::None || id.x < twin) ? id.x : twin;
		InterlockedOr(Bisectors[leader].flags, PBRWaterBisection::FlagSplit, previous);
		if ((previous & PBRWaterBisection::FlagSplit) != 0)
			return;
		uint need = twin == PBRWaterBisection::None ? 1 : 2;
		uint base;
		State.InterlockedAdd(StateCursor, need, base);
		if (base + need > Counts.y) {
			InterlockedAnd(Bisectors[leader].flags, ~PBRWaterBisection::FlagSplit, previous);
			return;
		}
		ChildB[leader] = base;
		if (twin != PBRWaterBisection::None) {
			uint other = leader == id.x ? twin : id.x;
			ChildB[other] = base + 1;
			InterlockedOr(Bisectors[other].flags, PBRWaterBisection::FlagSplit, previous);
		}
		// Every slot below the end is written: allocations past the capacity come after all that fit.
		State.InterlockedMax(StateEnd, base + need, previous);
	} else {
		// The twin is coarser: our refinement edge is one of its legs. It splits first, and one of its children then
		// shares the edge as its own refinement edge.
		InterlockedOr(Bisectors[twin].flags, PBRWaterBisection::FlagForced, previous);
		if ((previous & PBRWaterBisection::FlagForced) == 0)
			State.InterlockedAdd(StateForced, 1, previous);
	}
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

[numthreads(1, 1, 1)] void ArgsCS(uint3 id : SV_DispatchThreadID) {
	uint before = Count();
	uint count = max(State.Load(StateEnd), before);
	bool running = State.Load(StateIterate) != 0;
	// Nothing split and no bisector began waiting on another: the subdivision is final, the frame's remaining iterations
	// dispatch nothing.
	bool changed = count > before || State.Load(StateForced) != 0;
	State.Store3(StateCount, uint3(count, count, count));
	State.Store3(StateDispatch, uint3((count + 63) / 64, 1, 1));
	State.Store3(StateIterate, uint3(changed && running ? (count + 63) / 64 : 0, 1, 1));
	State.Store(StateForced, 0);
	// LinkCS wrote the subdivision to Next, which the host's swap makes Bisectors; an iteration that ran nothing leaves it
	// where it was, which the swap moves to the other side.
	State.Store(StateParity, running ? 0 : State.Load(StateParity) ^ 1);
	State.Store(StateIterations, State.Load(StateIterations) + (running ? 1 : 0));
}

/// After the iterations: the latest subdivision back into Bisectors, where the iterations that ran nothing left it in Next.
[numthreads(64, 1, 1)] void FixupCS(uint3 id : SV_DispatchThreadID) {
	if (State.Load(StateParity) == 0 || id.x >= Count())
		return;
	Bisectors[id.x] = Next[id.x];
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
	// Each mesh's draw: its bisectors' triangles, non-indexed, from vertex 0. SV_VertexID does not count the draw's
	// StartVertexLocation: drawn from there, every mesh but the first drew the first mesh's bisectors through its own
	// transform (copies of the water standing up out of the sea). The vertex shader adds the mesh's offset itself.
	static const uint VerticesPerBisector = 3 * PBRWaterBisection::GridTriangles;
	uint offset = 0;
	for (uint m = 0; m < Counts.z; m++) {
		uint count = MeshCounts.Load(m * 4);
		DrawArgs.Store4(m * 16, uint4(count * VerticesPerBisector, 1, 0, 0));
		DrawArgs.Store(DrawOffsets + m * 4, offset);
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
	// This frame's marks go: the next frame classifies everything again for its own view (the culling stays for it).
	uint previous;
	InterlockedAnd(Bisectors[id.x].flags, ~PBRWaterBisection::FrameFlags, previous);
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
