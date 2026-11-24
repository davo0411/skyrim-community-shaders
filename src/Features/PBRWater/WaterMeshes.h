#pragma once

#include "Buffer.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

/**
 * @brief The water meshes the tessellated draws drew, read back once from their GPU buffers: what the bisection
 * tessellator (BisectionTessellator) subdivides.
 *
 * Each tessellated main-view draw registers its geometry; the next frame's build takes them, with their triangles in
 * world space. A mesh that is one flat rectangle cut into a grid (the cells' water: 32 x 32 quads) is taken as its
 * rectangle's two triangles (MergeRectangle), so the subdivision alone sets its detail.
 *
 * Render thread only.
 */
class WaterMeshes
{
public:
	/// A geometry drawn tessellated in the main view.
	struct Source
	{
		RE::NiTransform world;
		const void* rendererData = nullptr;
		uint32_t triangleCount = 0;
		uint64_t vertexDesc = 0;
		bool river = false;  ///< a river's mesh (IsRiver)
	};

	/// A base triangle of a registered water mesh.
	struct WorldTriangle
	{
		double world[3][3];  ///< absolute world positions of the corners
		float model[3][3];   ///< model-space positions
		float uv[3][2];
		float color[3][4];
	};

	/// A registered water geometry and its base triangles (merged into its rectangle where it is one: MergeRectangle).
	struct WorldMesh
	{
		const void* geometry = nullptr;
		Source source;
		std::vector<WorldTriangle> triangles;
	};

	/**
	 * @brief Remembers a tessellated main-view draw of `geometry`: next frame's subdivision covers it.
	 * @param geometry the water draw's geometry (a BSTriShape)
	 */
	void Register(RE::BSGeometry& geometry);

	/// Takes the draws registered since the last call and returns their meshes' base triangles in world space.
	void TakeBaseTriangles(std::vector<WorldMesh>& out);

	/// Forgets the draws registered for the next frame (tessellation off).
	void Reset() { registered.clear(); }

	/// Whether `geometry` is a river's mesh: its water shader property has the flowmap on (Unified Water sets it from the water form).
	static bool IsRiver(RE::BSGeometry& geometry);

	/// Whether `geometry` is still the mesh `source` was made from (its buffers and transform unchanged).
	static bool StillSource(RE::BSGeometry& geometry, const Source& source);

	/**
	 * @brief The texture coordinates of `geometry`'s mesh as one plane over its model-space xy, where they are one (a mesh
	 * merged into its rectangle, MergeRectangle): u = dot(mapU.xy, xy) + mapU.z, and v from mapV. The deferred surface
	 * then records none for it.
	 * @return false when they are not, or the mesh has not been read
	 */
	bool AffineTexCoords(RE::BSGeometry& geometry, float4& mapU, float4& mapV) const;

private:
	struct Corner
	{
		float position[3];
		float uv[2];
		float color[4];
	};

	/// A water mesh's triangles, read back once from its GPU buffers.
	struct Mesh
	{
		winrt::com_ptr<ID3D11Buffer> vertexBuffer;  ///< held so its address can never name another mesh
		std::vector<Corner> corners;                ///< three per base triangle (two triangles for a mesh merged into its rectangle)
		uint32_t triangleCount = 0;
		uint64_t vertexDesc = 0;
		uint32_t lastUsed = 0;
		bool read = false;   ///< read back (successfully or not)
		bool valid = false;  ///< and usable
	};

	const Mesh* GetMesh(const Source& source);
	static bool SameSource(const Source& a, const Source& b);
	static bool MakeSource(RE::BSGeometry& geometry, Source& source);

	std::unordered_map<const void*, Mesh> meshes;
	std::unordered_map<const void*, Source> registered;  ///< this frame's draws, for the next frame's build
	uint32_t frame = 0;
};
