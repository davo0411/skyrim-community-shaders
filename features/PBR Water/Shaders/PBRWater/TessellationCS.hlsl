// ============================================================================
// PBR Water - compute tessellation (WaterTessellator). Once a frame, for every water mesh the frame tessellates:
//   VertexCS  each grid vertex of each leaf the CPU made (ComputeTessellation.hlsli): its position, interpolated in
//             canonical order so a vertex shared by two leaves or two meshes comes out bit-identical, and its waves
//             (EvaluateVertexWaves, motion included), the whole vertex program's expensive part, evaluated once;
//   MorphCS   moves the vertices a coarser level does not have from the midpoint of their parents to their own
//             displacement (geomorphing), so the detail fades in and out instead of popping;
//   IndexCS   the leaves' triangles, with the vertices clamped onto their side's points.
// Every tessellated pass of the frame (depth and motion, surface, point lights, underwater) then draws these with the
// water vertex shader's cached path: the waves cost one evaluation per vertex per frame, not one per vertex per pass.
// ============================================================================

#include "PBRWater/ComputeTessellation.hlsli"
#include "PBRWater/PBRWater.hlsli"
#include "PBRWater/VertexWaves.hlsli"

cbuffer TessellationCB : register(b0)
{
	uint4 Counts;    // x leaves, y vertices, z triangles, w unused
	float4 Spacing;  // x target triangle size (px, as the budget coarsened it), y grazing floor, z the finest spacing (units)
};

StructuredBuffer<PBRWaterTess::Leaf> Leaves : register(t0);
StructuredBuffer<PBRWaterTess::BaseTriangle> BaseTriangles : register(t1);
StructuredBuffer<PBRWaterTess::RawDisplacement> RawIn : register(t2);

RWStructuredBuffer<PBRWaterTess::Vertex> Vertices : register(u0);
RWStructuredBuffer<PBRWaterTess::RawDisplacement> RawOut : register(u1);
RWByteAddressBuffer Indices : register(u2);
// The largest displacement this frame's vertices reached (asuint of positive floats, which order as their bits): x
// sideways, y up or down. The CPU reads it back a few frames later for its frustum culling's margin (WaterTessellator).
RWByteAddressBuffer Reach : register(u3);
groupshared uint GroupReach[2];

/// The leaf whose range holds element `e` (vertex or triangle offsets, ascending).
uint FindLeaf(uint e, bool triangles)
{
	uint lo = 0;
	uint hi = Counts.x;
	[loop] while (hi - lo > 1)
	{
		uint mid = (lo + hi) >> 1;
		uint offset = triangles ? Leaves[mid].triangleOffset : Leaves[mid].vertexOffset;
		if (offset <= e)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

float3 Corner(PBRWaterTess::BaseTriangle t, uint c, uint which)
{
	if (which == 0)
		return c == 0 ? t.world0.xyz : (c == 1 ? t.world1.xyz : t.world2.xyz);
	return c == 0 ? t.model0.xyz : (c == 1 ? t.model1.xyz : t.model2.xyz);
}

/// Evaluates vertex v; returns how far its waves moved it (x sideways, y up or down), 0 for vertices nothing reads.
float2 EvaluateVertex(uint v)
{
	PBRWaterTess::Leaf leaf = Leaves[FindLeaf(v, false)];
	uint n = PBRWaterTess::LeafN(leaf);
	uint2 g = PBRWaterTess::VertexCoords(v - leaf.vertexOffset, n);
	// Clamped side vertices are drawn as the point they are clamped onto: nothing reads them.
	uint2 rep = PBRWaterTess::Representative(leaf, g);
	if (any(rep != g))
		return 0.0;

	PBRWaterTess::BaseTriangle base = BaseTriangles[leaf.baseIndex];
	float3 w = PBRWaterTess::LatticeWeights(leaf, g);
	float weights[3] = { w.x, w.y, w.z };

	// Summed in canonical corner order: a weight of exactly 0 adds exactly 0, so a point on an edge two triangles share
	// comes out the same from both.
	precise float3 world = 0.0;
	precise float3 model = 0.0;
	float2 uv = 0.0;
	float4 color = 0.0;
	float2 uvs[3] = { base.uv01.xy, base.uv01.zw, base.uv2 };
	float4 colors[3] = { base.color0, base.color1, base.color2 };
	[unroll] for (uint k = 0; k < 3; k++)
	{
		uint c = (base.order >> (2 * k)) & 3;
		world += Corner(base, c, 0) * weights[c];
		model += Corner(base, c, 1) * weights[c];
		uv += uvs[c] * weights[c];
		color += colors[c] * weights[c];
	}

	// Filtered to the mesh's spacing at this point alone: a vertex shared by two
	// leaves gets the same waves from both. The open water's target (twice the setting), sparser at grazing angles as the
	// CPU made it (WaterTessellator Factor), never finer than the finest spacing.
	float distance = max(length(world), 1.0);
	float grazing = min(abs(world.z) / distance + Spacing.y, 1.0);
	float spacing = max(2.0 * max(Spacing.x, 1.0) * distance / max(PBRWater::Tess0.w, 1e-3) * rsqrt(grazing), Spacing.z);
	PBRWater::VertexWaves waves = PBRWater::EvaluateVertexWaves(world, spacing, true);

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

	PBRWaterTess::RawDisplacement raw;
	raw.current = o.current;
	raw.previous = o.previous;
	raw.shoreX = o.shore.x;
	raw.shoreZ = o.shore.z;
	RawOut[v] = raw;
	return float2(length(o.current.xy), abs(o.current.z));
}

[numthreads(64, 1, 1)] void VertexCS(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
	if (gi == 0) {
		GroupReach[0] = 0;
		GroupReach[1] = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	float2 reach = id.x < Counts.y ? EvaluateVertex(id.x) : 0.0;
	InterlockedMax(GroupReach[0], asuint(reach.x));
	InterlockedMax(GroupReach[1], asuint(reach.y));
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0) {
		uint previous;
		Reach.InterlockedMax(0, GroupReach[0], previous);
		Reach.InterlockedMax(4, GroupReach[1], previous);
	}
}

	[numthreads(64, 1, 1)] void MorphCS(uint3 id : SV_DispatchThreadID)
{
	uint v = id.x;
	if (v >= Counts.y)
		return;
	PBRWaterTess::Leaf leaf = Leaves[FindLeaf(v, false)];
	uint n = PBRWaterTess::LeafN(leaf);
	uint2 g = PBRWaterTess::VertexCoords(v - leaf.vertexOffset, n);
	if (any(PBRWaterTess::Representative(leaf, g) != g))
		return;
	uint2 a, b;
	float m = PBRWaterTess::MorphWeight(leaf, g, a, b);
	[branch] if (m < 1.0)
	{
		PBRWaterTess::RawDisplacement self = RawIn[v];
		PBRWaterTess::RawDisplacement pa = RawIn[leaf.vertexOffset + PBRWaterTess::VertexIndex(a.x, a.y, n)];
		PBRWaterTess::RawDisplacement pb = RawIn[leaf.vertexOffset + PBRWaterTess::VertexIndex(b.x, b.y, n)];
		PBRWaterTess::Vertex o = Vertices[v];
		o.current = lerp(0.5 * (pa.current + pb.current), self.current, m);
		o.previous = lerp(0.5 * (pa.previous + pb.previous), self.previous, m);
		o.shore.x = lerp(0.5 * (pa.shoreX + pb.shoreX), self.shoreX, m);
		o.shore.z = lerp(0.5 * (pa.shoreZ + pb.shoreZ), self.shoreZ, m);
		Vertices[v] = o;
	}
}

[numthreads(64, 1, 1)] void IndexCS(uint3 id : SV_DispatchThreadID) {
	uint t = id.x;
	if (t >= Counts.z)
		return;
	PBRWaterTess::Leaf leaf = Leaves[FindLeaf(t, true)];
	uint n = PBRWaterTess::LeafN(leaf);
	uint2 a, b, c;
	PBRWaterTess::TriangleCorners(t - leaf.triangleOffset, n, a, b, c);
	a = PBRWaterTess::Representative(leaf, a);
	b = PBRWaterTess::Representative(leaf, b);
	c = PBRWaterTess::Representative(leaf, c);
	uint3 tri = leaf.vertexOffset + uint3(PBRWaterTess::VertexIndex(a.x, a.y, n), PBRWaterTess::VertexIndex(b.x, b.y, n), PBRWaterTess::VertexIndex(c.x, c.y, n));
	// A triangle collapsed by the clamping repeats a vertex: it has no area and rasterises nothing, at no extra vertex.
	Indices.Store3(t * 12, tri);
}
