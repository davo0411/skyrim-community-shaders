#ifndef __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
#define __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__

// ============================================================================
// PBR Water - compute tessellation: the data the CPU and the compute passes share (WaterTessellator, TessellationCS).
//
// Every triangle of a water mesh is cut by the CPU into an adaptive, 2:1 balanced quadtree of triangles (leaves) whose
// on-screen size stays the same at every distance; each leaf is a regular grid of N x N (a power of two) small
// triangles. Where two leaves meet, the edge's points belong to its finer side: each side of a leaf is one, two or four
// segments, each with its own power-of-two point count, and the leaf's grid vertices on that side are clamped onto
// the segments' points (duplicates become degenerate triangles). Both leaves along an edge therefore draw the same
// points, with no T-junctions. Vertices that a coarser level does not have morph in from the midpoint of their two
// parents (geomorphing, by the factor the CPU worked out for their segment or leaf), so nothing pops as the detail
// changes. The vertices are evaluated once a frame and drawn by every tessellated pass of the frame.
// Must match src/Features/PBRWater/ComputeTessellation.h.
// ============================================================================

namespace PBRWaterTess
{
	/// One tessellated vertex, ready for the water vertex shader (Water.hlsl, CachedWaterVertex). 80 bytes.
	struct Vertex
	{
		float3 position;  // undisplaced model-space position
		float flowDamping;
		float3 current;  // world-space displacement now (SurfaceDisplacement::current), morphed
		float texCoordX;
		float3 previous;  // and last frame (motion vectors)
		float texCoordY;
		float4 shore;  // SurfaceDisplacement::shore (x and z morphed)
		float2 flowVelocity;
		uint2 color;  // the mesh's vertex colour, half floats
	};

	/// A vertex's own displacement before morphing: what its children morph from. 32 bytes.
	struct RawDisplacement
	{
		float3 current;
		float shoreX;
		float3 previous;
		float shoreZ;
	};

	/// One triangle of a water mesh this frame. 176 bytes.
	struct BaseTriangle
	{
		float4 world0;  // xyz camera-relative world position of each corner (from double precision on the CPU)
		float4 world1;
		float4 world2;
		float4 model0;  // xyz model-space position of each corner
		float4 model1;
		float4 model2;
		float4 color0;
		float4 color1;
		float4 color2;
		float4 uv01;  // xy corner 0's texture coordinate, zw corner 1's
		float2 uv2;
		uint order;  // the corners in canonical order (by absolute position), 2 bits each: the sums run in that order
		uint pad;
	};

	/// One leaf of a base triangle's quadtree. 112 bytes.
	struct Leaf
	{
		uint vertexOffset;
		uint triangleOffset;
		uint baseIndex;
		uint shape;          // bits 0-7 log2 N, bits 8-15 the leaf's depth D
		int4 corners01;      // lattice coordinates of corners 0 and 1 at resolution 2^D: the weights of base corners 1 and 2
		int2 corner2;        // and of corner 2
		float interiorLog2;  // log2 of the leaf's interior factor: the morph of its inner vertices
		uint pad;
		uint4 sides;         // xyz the segments of each side (bits 0-2 count; then per segment 2 bits log2 length in quarters, 4 bits log2 points)
		float4 sideLog2[3];  // log2 of the factor of each side's segments: their vertices' morph
	};

	uint LeafLog2N(Leaf l) { return l.shape & 0xFF; }
	uint LeafN(Leaf l) { return 1u << LeafLog2N(l); }
	uint LeafDepth(Leaf l) { return (l.shape >> 8) & 0xFF; }

	uint VertexCount(uint n) { return (n + 1) * (n + 2) / 2; }
	uint TriangleCount(uint n) { return n * n; }

	/// Grid vertex (i, j) of a leaf with n segments: rows of constant j, row j holding i = 0 .. n - j.
	uint VertexIndex(uint i, uint j, uint n) { return j * (n + 1) - (j * (j - 1)) / 2 + i; }

	uint2 VertexCoords(uint v, uint n)
	{
		float b = 2.0 * n + 3.0;
		int j = (int)floor(0.5 * (b - sqrt(max(b * b - 8.0 * v, 0.0))));
		j = clamp(j, 0, (int)n);
		// Exact integer correction of the float estimate.
		[loop] while (j > 0 && VertexIndex(0, (uint)j, n) > v)
			j--;
		[loop] while ((uint)j < n && VertexIndex(0, (uint)j + 1, n) <= v)
			j++;
		return uint2(v - VertexIndex(0, (uint)j, n), (uint)j);
	}

	/// First triangle of row j: row r holds 2 (n - r) - 1 triangles.
	uint TriangleRowStart(uint j, uint n) { return j * (2 * n - j); }

	/// The grid corners of triangle t of a leaf with n segments, in the base triangle's winding.
	void TriangleCorners(uint t, uint n, out uint2 a, out uint2 b, out uint2 c)
	{
		int j = (int)floor(n - sqrt(max((float)n * n - (float)t, 0.0)));
		j = clamp(j, 0, (int)n - 1);
		[loop] while (j > 0 && TriangleRowStart((uint)j, n) > t)
			j--;
		[loop] while ((uint)j + 1 < n && TriangleRowStart((uint)j + 1, n) <= t)
			j++;
		uint k = t - TriangleRowStart((uint)j, n);
		uint row = n - (uint)j;  // up triangles in this row
		if (k < row) {
			a = uint2(k, j);
			b = uint2(k + 1, j);
			c = uint2(k, j + 1);
		} else {
			uint i = k - row;
			a = uint2(i + 1, j);
			b = uint2(i + 1, j + 1);
			c = uint2(i, j + 1);
		}
	}

	uint SegmentCount(uint side) { return side & 7; }
	uint SegmentLengthQuarters(uint side, uint s) { return 1u << ((side >> (3 + 6 * s)) & 3); }
	uint SegmentLog2Points(uint side, uint s) { return (side >> (5 + 6 * s)) & 15; }

	/// The segment of a side holding grid index p (0 .. n along the side, from its first corner).
	struct Segment
	{
		uint start;   // first grid index of the segment
		uint length;  // grid indices it spans
		uint log2Points;
		uint stride;  // grid indices between two of its points
		uint index;
	};

	Segment FindSegment(uint side, uint n, uint p)
	{
		Segment seg = (Segment)0;
		uint count = max(SegmentCount(side), 1);
		uint start = 0;
		[loop] for (uint s = 0; s < count; s++)
		{
			uint length = SegmentLengthQuarters(side, s) * n / 4;
			seg.start = start;
			seg.length = max(length, 1);
			seg.log2Points = SegmentLog2Points(side, s);
			seg.index = s;
			if (p <= start + length || s + 1 == count)
				break;
			start += length;
		}
		seg.stride = max(seg.length >> seg.log2Points, 1);
		return seg;
	}

	/// Grid index of the side point that grid index p is clamped onto (rounding half up).
	uint SnapToSide(uint side, uint n, uint p)
	{
		Segment seg = FindSegment(side, n, p);
		uint q = p - seg.start;
		uint s = min((q + (seg.stride >> 1)) / seg.stride, seg.length / seg.stride);
		return seg.start + s * seg.stride;
	}

	/// Grid coordinates of point p along side s (0: corner 0 to 1, 1: corner 1 to 2, 2: corner 2 to 0).
	uint2 SidePoint(uint s, uint n, uint p)
	{
		return s == 0 ? uint2(p, 0) : (s == 1 ? uint2(n - p, p) : uint2(0, n - p));
	}

	/// The side a non-corner boundary vertex lies on and its index along it; false inside the leaf.
	bool OnSide(uint2 g, uint n, out uint s, out uint p)
	{
		s = 0;
		p = 0;
		if (g.y == 0) {
			p = g.x;
			return true;
		}
		if (g.x + g.y == n) {
			s = 1;
			p = g.y;
			return true;
		}
		if (g.x == 0) {
			s = 2;
			p = n - g.y;
			return true;
		}
		return false;
	}

	/// The vertex that grid vertex g is drawn as: itself inside the leaf, the side point it is clamped onto on its border.
	uint2 Representative(Leaf l, uint2 g)
	{
		uint n = LeafN(l);
		uint s, p;
		if (!OnSide(g, n, s, p) || p == 0 || p == n)
			return g;
		return SidePoint(s, n, SnapToSide(l.sides[s], n, p));
	}

	/**
	 * How far vertex g (a representative) has morphed in from the midpoint of its parents, and the parents. 1 for the
	 * vertices a coarser level also has.
	 */
	float MorphWeight(Leaf l, uint2 g, out uint2 parentA, out uint2 parentB)
	{
		// One exit: fxc reads the early returns of a function with out parameters as a maybe-uninitialised result (X4000).
		uint n = LeafN(l);
		parentA = g;
		parentB = g;
		float weight = 1.0;
		uint s, p;
		if (OnSide(g, n, s, p)) {
			if (p != 0 && p != n) {
				Segment seg = FindSegment(l.sides[s], n, p);
				uint q = p - seg.start;
				uint index = q / seg.stride;
				uint points = seg.length / seg.stride;
				if (index != 0 && index < points) {
					uint low = firstbitlow(index);
					uint step = 1u << low;
					parentA = SidePoint(s, n, seg.start + (index - step) * seg.stride);
					parentB = SidePoint(s, n, seg.start + (index + step) * seg.stride);
					float level = (float)seg.log2Points - (float)low;
					// Selected rather than indexed: dynamic indexing copies the leaf into an indexable temporary.
					float4 side = s == 0 ? l.sideLog2[0] : (s == 1 ? l.sideLog2[1] : l.sideLog2[2]);
					float factorLog2 = dot(side, float4(seg.index == 0, seg.index == 1, seg.index == 2, seg.index == 3));
					weight = saturate(factorLog2 - level + 1.0);
				}
			}
		} else {
			uint k = n - g.x - g.y;
			uint low = min(firstbitlow(g.x), min(firstbitlow(g.y), firstbitlow(k)));
			uint step = 1u << low;
			bool oddI = ((g.x >> low) & 1) != 0;
			bool oddJ = ((g.y >> low) & 1) != 0;
			// Of i, j and k at this step exactly two are odd: the vertex is the midpoint of the edge along the third.
			if (oddI && oddJ) {
				parentA = uint2(g.x + step, g.y - step);
				parentB = uint2(g.x - step, g.y + step);
			} else if (oddI) {
				parentA = uint2(g.x - step, g.y);
				parentB = uint2(g.x + step, g.y);
			} else {
				parentA = uint2(g.x, g.y - step);
				parentB = uint2(g.x, g.y + step);
			}
			parentA = Representative(l, parentA);
			parentB = Representative(l, parentB);
			float level = (float)LeafLog2N(l) - (float)low;
			weight = saturate(l.interiorLog2 - level + 1.0);
		}
		return weight;
	}

	/// Exact barycentric weights of grid vertex g of a leaf: (base corner 0, 1, 2). Dyadic, so exact in float.
	float3 LatticeWeights(Leaf l, uint2 g)
	{
		int n = (int)LeafN(l);
		int2 c0 = l.corners01.xy;
		int2 c1 = l.corners01.zw;
		int2 c2 = l.corner2;
		int2 a = c0 * n + (c1 - c0) * (int)g.x + (c2 - c0) * (int)g.y;
		uint shift = LeafDepth(l) + LeafLog2N(l);
		float scale = asfloat((127u - shift) << 23);  // 2^-shift, exact
		float u = (float)a.x * scale;
		float v = (float)a.y * scale;
		return float3(1.0 - u - v, u, v);
	}
}

#endif  // __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
