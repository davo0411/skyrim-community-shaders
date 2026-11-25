#ifndef __PBR_WATER_PROJECTED_GRID_HLSLI__
#define __PBR_WATER_PROJECTED_GRID_HLSLI__

// ============================================================================
// PBR Water - the projected grids (PBRWater/ProjectedGrid.cpp, ProjectedGridCS.hlsl, Water.hlsl GridWaterVertex).
// One mesh per water level on screen, (columns + 1) x (rows + 1) vertices laid out evenly on the screen and cast onto the
// level's plane: as many triangles near the camera as far away, each a few pixels across, so the waves reach as far as
// the water is seen and no triangle is smaller than its pixels. Drawn as a triangle list of two triangles a cell, cells
// row after row; the cells a breaker needs more of are drawn instead as patches of PBRW_GRID_PATCH^2 cells.
// ============================================================================

#define PBRW_GRID_MAX_LEVELS 4  // ProjectedGrid::MaxLevels
#define PBRW_GRID_PATCH 4       // ProjectedGrid::Subdivision

namespace PBRWaterGrid
{
	/// One grid's layout (ProjectedGrid::GpuLevel).
	struct Level
	{
		float4 range;      // xy the grid's NDC x from and to, zw its NDC y from and to
		float4 plane;      // x the level relative to the camera (units, never within a unit of 0), y the farthest a vertex is cast (units)
		uint4 layout;      // x columns, y rows, z its first vertex, w its first cell
		uint4 patches;     // x its first patch, y patches it may take
		float4 texCoordU;  // texture coordinate u = dot(xy, absolute xy) + z
		float4 texCoordV;  // and v
	};

	uint VertexIndex(uint2 g, uint columns)
	{
		return g.y * (columns + 1) + g.x;
	}

	/// The grid corner of vertex `vertexID` of a triangle list over `columns`-wide cells: corners (0,0) (1,0) (0,1), then
	/// (1,0) (1,1) (0,1).
	uint2 ListCorner(uint vertexID, uint columns)
	{
		uint cell = vertexID / 6;
		uint corner = vertexID - cell * 6;
		uint2 offset = uint2(corner == 1 || corner == 3 || corner == 4, corner == 2 || corner == 4 || corner == 5);
		return uint2(cell % columns, cell / columns) + offset;
	}
}

#endif  // __PBR_WATER_PROJECTED_GRID_HLSLI__
