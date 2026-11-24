#ifndef __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
#define __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__

// ============================================================================
// PBR Water - the tessellated water's vertices and the triangular grid each bisector is drawn as (BisectionCS.hlsl,
// Water.hlsl CachedWaterVertex and BisectionVertexIndex). The vertices are evaluated once a frame and drawn by every
// tessellated pass of the frame. Vertex mirrors BisectionTessellator::GpuVertex.
// ============================================================================

namespace PBRWaterTess
{
	/// One tessellated vertex, ready for the water vertex shader (Water.hlsl, CachedWaterVertex). 72 bytes.
	struct Vertex
	{
		float3 position;  // undisplaced model-space position
		float river;  // 1 on a river's mesh
		float3 current;  // world-space displacement now (SurfaceDisplacement::current)
		float texCoordX;
		float3 previous;  // and last frame (motion vectors)
		float texCoordY;
		float4 shore;  // SurfaceDisplacement::shore
		uint2 color;  // the mesh's vertex colour, half floats
	};

	// The triangular grid of n segments a side: vertex (i, j), i + j <= n, row j holding n + 1 - j of them.
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

	/// The grid corners of triangle t of a grid of n segments, in its triangle's winding.
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
}

#endif  // __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
