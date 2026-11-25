#ifndef __PBR_WATER_PROJECTED_GRID_HLSLI__
#define __PBR_WATER_PROJECTED_GRID_HLSLI__

// ============================================================================
// PBR Water - the sea's projected grid (PBRWater/ProjectedGrid.cpp, ProjectedGridCS.hlsl, Water.hlsl GridWaterVertex).
// One mesh of (columns + 1) x (rows + 1) vertices laid out evenly on the screen and cast onto the sea's plane: as many
// triangles near the camera as at the horizon, each a few pixels across, so the waves reach as far as the sea is seen
// and no triangle is ever smaller than its pixels (no quads shaded over and over along thin far triangles). Drawn as a
// triangle list of two triangles a cell, cells row after row.
// ============================================================================

namespace PBRWaterGrid
{
	uint VertexIndex(uint2 g, uint columns)
	{
		return g.y * (columns + 1) + g.x;
	}

	/// The grid corner of vertex `vertexID` of the triangle list: corners (0,0) (1,0) (0,1), then (1,0) (1,1) (0,1).
	uint2 ListCorner(uint vertexID, uint columns)
	{
		uint cell = vertexID / 6;
		uint corner = vertexID - cell * 6;
		uint2 offset = uint2(corner == 1 || corner == 3 || corner == 4, corner == 2 || corner == 4 || corner == 5);
		return uint2(cell % columns, cell / columns) + offset;
	}
}

#endif  // __PBR_WATER_PROJECTED_GRID_HLSLI__
