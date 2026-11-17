#pragma once

#include "ComputeTessellation.h"

/**
 * @brief PBR Water's bisection tessellator: the water meshes refined by conforming longest-edge bisection on the GPU,
 * every triangle at its own level (BisectionCS.hlsl; design: docs/development/pbr-water-bisection-tessellator.md).
 *
 * The alternative to WaterTessellator's quadtree of patches, whose leaves take their densest side's grid across their
 * whole area and fan slivers along their coarser sides. Here a triangle splits only together with the triangle across
 * its longest edge, so there is never a hanging vertex, and the density follows the spacing rule
 * (PBRWaterBisection::TessellationSpacing) triangle by triangle. It reads the meshes through WaterTessellator
 * (TakeBaseTriangles) and rebuilds the subdivision from them every frame: no state survives the frame.
 */
class BisectionTessellator
{
public:
	/// Mirrors Bisection.hlsli (PBRW_BISECTION_GRID_N): each bisector is drawn as a grid of GridN segments a side.
	static constexpr uint32_t GridN = 4;
	static constexpr uint32_t GridVertices = (GridN + 1) * (GridN + 2) / 2;
	static constexpr uint32_t GridTriangles = GridN * GridN;
	/// The most bisectors a frame (each GridTriangles triangles), meshes and split iterations.
	static constexpr uint32_t Capacity = 1u << 16;
	static constexpr uint32_t MaxMeshes = 256;
	static constexpr uint32_t MaxIterations = 40;
	static constexpr uint32_t MaxDepth = 30;

	struct Stats
	{
		uint32_t meshes = 0;
		uint32_t roots = 0;
		uint32_t twins = 0;  ///< root edges matched to another root's (across meshes too)
		float cpuMs = 0.0f;
	};

	void SetupResources();
	void CompileShaders();
	bool IsReady() const { return ready; }

	/**
	 * @brief Builds this frame's subdivision from the meshes `meshes` had registered (WaterTessellator::Register). The
	 * caller binds what the wave program reads in the compute stage, as for WaterTessellator::Build.
	 */
	void Build(const WaterTessellator::FrameParams& params, WaterTessellator& meshes);

	/// Forgets this frame's subdivision: every draw is untessellated.
	void Clear();

	/**
	 * @brief The mesh slot `geometry` is drawn from this frame.
	 * @return false when the geometry is not in it or changed since it was registered
	 */
	bool Lookup(RE::BSGeometry& geometry, uint32_t& slot) const;

	/**
	 * @brief Draws mesh `slot` with the device's current state (the water draw's shaders and constants, the vertex shader
	 * in its bisection mode, Tess0.x = 4). Restores t109.
	 */
	void Draw(uint32_t slot) const;

	bool BuiltFor(uint32_t frameCount, const float4& cameraPosAdjust) const;
	const Stats& GetStats() const { return stats; }

private:
	/// Mirrors PBRWaterBisection::Root. 192 bytes.
	struct GpuRoot
	{
		float4 world[3];
		float4 model[3];
		float4 color[3];
		float4 uv01;
		float uv2[2];
		uint32_t mesh;
		uint32_t order;
		uint32_t neighbours[4];
	};
	static_assert(sizeof(GpuRoot) == 192);
	/// Mirrors PBRWaterBisection::Bisector.
	static constexpr uint32_t BisectorStride = 48;

	struct alignas(16) BisectionCB
	{
		uint32_t counts[4];  // x roots, y capacity, z meshes
		float cull[4];       // x sideways margin, y vertical margin, z deepest bisection
		float viewProj[4][4];
	};

	struct Slot
	{
		WaterTessellator::Source source;
	};

	winrt::com_ptr<ID3D11ComputeShader> initCS, classifyCS, allocateCS, linkCS, argsCS, countCS, offsetsCS, scatterCS, vertexCS;
	std::unique_ptr<ConstantBuffer> constants;
	std::unique_ptr<Buffer> roots;
	uint32_t rootCapacity = 0;
	std::unique_ptr<Buffer> bisectors[2];
	std::unique_ptr<Buffer> childB;
	std::unique_ptr<Buffer> list;
	std::unique_ptr<Buffer> vertices;
	winrt::com_ptr<ID3D11Buffer> state, meshCounts, drawArgs, dispatchArgs, reach;
	winrt::com_ptr<ID3D11UnorderedAccessView> stateUAV, meshCountsUAV, drawArgsUAV, reachUAV;

	std::unordered_map<const void*, uint32_t> slotOf;  ///< this frame's meshes, by geometry
	std::vector<Slot> slots;
	std::vector<GpuRoot> rootData;
	float4 builtCamera{};
	uint32_t builtFrame = 0;
	bool built = false;
	bool ready = false;
	Stats stats;
};
