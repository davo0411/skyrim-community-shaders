#ifndef __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
#define __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__

// ============================================================================
// PBR Water - the projected grid's vertices (ProjectedGridCS.hlsl, Water.hlsl CachedVertexFrom). They are evaluated once
// a frame and drawn by every pass of the frame that draws the grid.
// ============================================================================

namespace PBRWaterTess
{
	/// One grid vertex, ready for the water vertex shader (Water.hlsl, CachedVertexFrom). 72 bytes.
	struct Vertex
	{
		float3 position;  // camera-relative position (Water.hlsl GridVertexFrom brings it into the draw's model space)
		float river;      // a cell's first corner: 1 where a breaker's patch draws the cell (ProjectedGridCS FlagCS)
		float3 current;   // world-space displacement now (SurfaceDisplacement::current)
		float texCoordX;
		float3 previous;  // and last frame (motion vectors)
		float texCoordY;
		float4 shore;  // SurfaceDisplacement::shore
		uint2 color;   // the vertex colour, half floats
	};
}

#endif  // __PBR_WATER_COMPUTE_TESSELLATION_HLSLI__
