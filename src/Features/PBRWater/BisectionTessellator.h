#pragma once

#include "Buffer.h"
#include "WaterMeshes.h"

#include <memory>
#include <unordered_map>
#include <vector>

/**
 * @brief PBR Water's bisection tessellator: the water meshes refined by conforming longest-edge bisection on the GPU,
 * every triangle at its own level (BisectionCS.hlsl; design: docs/development/pbr-water-bisection-tessellator.md).
 *
 * A triangle splits only together with the triangle across its longest edge, so there is never a hanging vertex, and the
 * density follows the spacing rule (PBRWaterBisection::TessellationSpacing) triangle by triangle. (The quadtree of
 * patches it replaced took each leaf's densest side's grid across its whole area and fanned slivers along its coarser
 * sides.) It reads the meshes through WaterMeshes (TakeBaseTriangles) and keeps the subdivision from frame to frame
 * while they stay the same, refining and merging only what the view changed. Every tessellated pass of the main view
 * draws the vertices it evaluated (Water.hlsl, vertex shader mode 1).
 */
class BisectionTessellator
{
public:
	/// Mirrors Bisection.hlsli (PBRW_BISECTION_GRID_N): each bisector is drawn as a grid of GridN segments a side.
	static constexpr uint32_t GridN = 8;
	static constexpr uint32_t GridVertices = (GridN + 1) * (GridN + 2) / 2;
	static constexpr uint32_t GridTriangles = GridN * GridN;
	/// The most bisectors a frame (each GridTriangles triangles), meshes and split iterations.
	static constexpr uint32_t Capacity = 1u << 14;
	static constexpr uint32_t MaxMeshes = 256;
	static constexpr uint32_t MaxIterations = 40;
	static constexpr uint32_t MaxDepth = 30;

	/// Mirrors PBRWaterTess::Vertex (ComputeTessellation.hlsli).
	struct alignas(4) GpuVertex
	{
		float data[18];
	};
	static_assert(sizeof(GpuVertex) == 72);

	/// What the subdivision needs of the frame (PBRWater::UpdateTessellation fills it).
	struct FrameParams
	{
		uint32_t frame = 0;                                  ///< the frame the mesh is for (State::frameCount)
		double cameraX = 0.0, cameraY = 0.0, cameraZ = 0.0;  ///< absolute camera position (CameraPosAdjust)
		float viewProj[4][4] = {};                           ///< camera-relative view-projection (row-major, mul(M, v))
		float cullMarginXY = 0.0f;                           ///< how far the waves can move the surface sideways (units)
		float cullMarginZ = 0.0f;                            ///< and up or down
	};

	struct Stats
	{
		uint32_t meshes = 0;
		uint32_t roots = 0;
		uint32_t twins = 0;  ///< root edges matched to another root's (across meshes too)
		float cpuMs = 0.0f;
		// From the GPU, a few frames late (asynchronous read-back):
		uint32_t bisectors = 0;     ///< triangles of the subdivision
		uint32_t listed = 0;        ///< of them in view (drawn)
		uint32_t iterations = 0;    ///< refinement iterations that changed anything
		uint32_t iterationCap = 0;  ///< iterations issued this frame
	};

	void SetupResources();
	void CompileShaders();
	bool IsReady() const { return ready; }

	/**
	 * @brief Builds this frame's subdivision from the meshes `meshes` had registered (WaterMeshes::Register). The caller
	 * binds what the wave program reads in the compute stage (b7, b12, t80-t97, s14, s15).
	 */
	void Build(const FrameParams& params, WaterMeshes& meshes);

	/// Forgets this frame's subdivision: every draw is untessellated.
	void Clear();

	/**
	 * @brief The mesh slot `geometry` is drawn from this frame.
	 * @return false when the geometry is not in it or changed since it was registered
	 */
	bool Lookup(RE::BSGeometry& geometry, uint32_t& slot) const;

	/**
	 * @brief Draws mesh `slot` with the device's current state (the water draw's shaders and constants, the vertex shader
	 * in its tessellated mode, Tess0.x = 1). Restores b11, t108 and t109.
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
		uint32_t neighbours[4];  ///< xyz the root across each edge, w 1 on a river's mesh
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
		WaterMeshes::Source source;
	};

	winrt::com_ptr<ID3D11ComputeShader> initCS, frameStartCS, mergeDecideCS, mergeLinkCS, compactCountCS, compactWriteCS, compactArgsCS, classifyCS, linkCS,
		argsCS, fixupCS, countCS, offsetsCS, scatterCS, vertexCS;
	std::unique_ptr<ConstantBuffer> constants;
	std::unique_ptr<Buffer> roots;
	uint32_t rootCapacity = 0;
	std::unique_ptr<Buffer> bisectors[2];
	std::unique_ptr<Buffer> childB;
	std::unique_ptr<Buffer> list;
	std::unique_ptr<Buffer> vertices;
	winrt::com_ptr<ID3D11Buffer> state, meshCounts, drawArgs, dispatchArgs, reach;
	/// The state read back without waiting (three frames in flight): how many iterations the subdivision took, so the next
	/// frame issues about that many instead of MaxIterations, each of which costs a few dispatches even once converged.
	winrt::com_ptr<ID3D11Buffer> stateStaging[3];
	uint32_t stagingSlot = 0;
	uint32_t iterationCap = MaxIterations;
	Stats gpuStats;
	void ReadBackState();
	/// The draw arguments as the vertex shader reads them (t108): each mesh's offset in the draw lists.
	winrt::com_ptr<ID3D11ShaderResourceView> drawArgsSRV;
	/// Per draw: the mesh slot (vertex shader b11).
	std::unique_ptr<ConstantBuffer> drawCB;
	struct alignas(16) DrawCB
	{
		uint32_t mesh[4];
	};
	winrt::com_ptr<ID3D11UnorderedAccessView> stateUAV, meshCountsUAV, drawArgsUAV, reachUAV;

	/// The subdivision is kept from frame to frame while the roots stay the same (same meshes, same triangles, same order):
	/// a frame then refines and merges only what the view changed. Meshes stay roots for a while after their last draw, so
	/// turning away from one and back does not start the subdivision over.
	struct Retained
	{
		WaterMeshes::WorldMesh mesh;
		uint32_t lastFrame = 0;
	};
	static constexpr uint32_t RetainFrames = 120;
	std::unordered_map<const void*, Retained> retained;
	uint64_t rootSignature = 0;
	uint32_t rootCount = 0;
	bool persistent = false;  ///< bisectors[current] holds last frame's subdivision of these roots
	uint32_t current = 0;
	uint32_t frameCounter = 0;

	std::unordered_map<const void*, uint32_t> slotOf;  ///< this frame's meshes, by geometry
	std::vector<Slot> slots;
	std::vector<GpuRoot> rootData;
	float4 builtCamera{};
	uint32_t builtFrame = 0;
	bool built = false;
	bool ready = false;
	Stats stats;
};
