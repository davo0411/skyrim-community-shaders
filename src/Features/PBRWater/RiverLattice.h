#pragma once

#include "Buffer.h"

#include <memory>
#include <unordered_map>
#include <vector>

/**
 * @brief The river lattice: river meshes (which keep their own meshes, flowmaps and all, outside the projected grids)
 * drawn with every triangle cut into a lattice of n^2 triangles, so their waves get the vertices the engine's 128-unit
 * cell meshes lack.
 *
 * The mesh's triangles are read back once (WaterMeshes::Triangles) into a buffer the vertex shader reads (Water.hlsl
 * RiverLatticeVertex). Once a frame each triangle takes its level, n = 1, 2, 4, 8 or 16 cuts a side, from its longest edge
 * on screen (RiverEdgeCuts, with a margin so it is never below the shader's own count), and the triangles are sorted by
 * level: each level is one instanced draw of its lattice. Each edge snaps its points to its own cuts, the same from both
 * triangles that share it and at most either one's level, so the lattice is fine near the camera, coarse far off and never
 * cracks. The engine's draw of such a mesh rasterises nothing (PBRWRiver.y < 0) and Draw replaces it in every pass.
 *
 * Render thread only.
 */
class RiverLattice
{
public:
	static constexpr uint32_t Levels = 5;  ///< the levels: 1, 2, 4, 8, 16 cuts a side; Water.hlsl RiverLatticeVertex

	/** @brief Creates the lattices' index buffers and the vertex stage's constants (b8, Water.hlsl PBRWRiver). */
	void Setup();

	/**
	 * @brief Whether `geometry` is drawn as its lattice this frame. Reads the mesh back the first time it needs one and
	 * sorts its triangles by level once a frame.
	 * @param pixelsPerUnit pixels per unit at a distance of one unit (PBRWater GpuData::Tess0.w)
	 * @param targetPixels the longest a lattice edge may stand on screen (Water.hlsl RiverLatticePixels)
	 * @param estimate the mesh's vertex spacing from its bound (units): no read-back where even that is fine enough
	 */
	bool Choose(RE::BSGeometry& geometry, float pixelsPerUnit, float targetPixels, float estimate, uint32_t frame);

	/** @brief b8 for the engine's draw of a mesh: its own vertices, or culled when Choose took its lattice. */
	ID3D11Buffer* EngineCB(bool river, bool lattice) const;

	/**
	 * @brief Draws `geometry`'s lattice (Choose took it this frame) with the bound pipeline and the engine's draw's state,
	 * then puts back what it changed (index buffer, topology, t106, t108). The caller rebinds b8.
	 */
	void Draw(RE::BSGeometry& geometry, bool river);

	/** @brief Drops the meshes read back (a worldspace change). */
	void Clear() { meshes.clear(); }

private:
	struct Mesh
	{
		winrt::com_ptr<ID3D11ShaderResourceView> corners;
		winrt::com_ptr<ID3D11Buffer> order;  ///< the triangles by level this frame (Water.hlsl RiverTriangles)
		winrt::com_ptr<ID3D11ShaderResourceView> orderSRV;
		std::vector<float> positions;  ///< model space, three corners a triangle
		std::vector<uint32_t> sorted;
		uint32_t counts[Levels]{};
		uint32_t triangles = 0;
		uint64_t vertexDesc = 0;
		const void* vertexBuffer = nullptr;
		float longestEdge = 0.0f;  ///< model space
		uint32_t usedFrame = 0;
		uint32_t sortedFrame = UINT32_MAX;
		bool failed = false;
	};

	Mesh* Find(RE::BSGeometry& geometry, uint32_t frame);
	/// The triangles' levels for this frame's camera; false when every one is level 1.
	bool Sort(Mesh& mesh, RE::BSGeometry& geometry, float pixelsPerUnit, float targetPixels);
	void Evict(uint32_t frame);

	winrt::com_ptr<ID3D11Buffer> indices[Levels];
	uint32_t indexCounts[Levels]{};
	/// b8 for the engine's draws: [river][0 the mesh's own vertices, 1 culled]
	std::unique_ptr<ConstantBuffer> engineConstants[2][2];
	std::unique_ptr<ConstantBuffer> drawConstants;  ///< b8 for the lattice's draws, updated per level drawn
	std::unordered_map<const void*, Mesh> meshes;
	uint32_t evictedFrame = 0;
};
