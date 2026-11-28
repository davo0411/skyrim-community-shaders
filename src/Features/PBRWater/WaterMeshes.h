#pragma once

#include "Buffer.h"

#include <cstdint>
#include <vector>

/**
 * @brief What the projected grid and the river lattice need of the engine's water meshes: whether one is a river's, its
 * texture coordinates as one plane (the cells' water) and its triangles, read back from its GPU buffers.
 *
 * Render thread only.
 */
class WaterMeshes
{
public:
	/// Whether `geometry` is a river's mesh: its water shader property has the flowmap on (Unified Water sets it from the water form).
	static bool IsRiver(RE::BSGeometry& geometry);

	/**
	 * @brief The texture coordinates of `geometry`'s mesh as one plane over its model-space xy, where they are one (a mesh
	 * that is one rectangle cut into a grid, MergeRectangle): u = dot(mapU.xy, xy) + mapU.z, and v from mapV. Reads the
	 * mesh back from the GPU: once per grid level (ProjectedGrid::SetTexCoords).
	 * @return false when they are not, or the mesh cannot be read
	 */
	static bool AffineTexCoords(RE::BSGeometry& geometry, float4& mapU, float4& mapV);

	/// One corner of a mesh's triangle, in model space.
	struct Corner
	{
		float position[3];
		float uv[2];
		float color[4];
	};

	/**
	 * @brief `geometry`'s triangles as they are drawn, three corners each (RiverLattice subdivides them). Reads the mesh back
	 * from the GPU.
	 * @return false when the mesh cannot be read
	 */
	static bool Triangles(RE::BSGeometry& geometry, std::vector<Corner>& corners);

private:
	/// The mesh behind a geometry's draw.
	struct Source
	{
		const void* rendererData = nullptr;
		uint32_t triangleCount = 0;
		uint64_t vertexDesc = 0;
	};

	/// The mesh's triangles, three corners each (with `merge`, two triangles for a mesh merged into its rectangle).
	static bool ReadCorners(const Source& source, std::vector<Corner>& corners, bool merge = true);
	static bool MakeSource(RE::BSGeometry& geometry, Source& source);
};
