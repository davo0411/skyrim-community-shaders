#pragma once

#include "Buffer.h"
#include "WaveModel.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

/**
 * @brief PBR Water's compute tessellator: the water meshes subdivided once a frame and drawn by every tessellated pass.
 *
 * It replaced a hull/domain shader path, which could not serve the water well. Its patches were the meshes' own triangles, and most water is
 * cell-sized quads of two triangles: a patch had one density for its whole 4096 units, the most the hull shader could ask
 * for (64) left the water near the camera coarse while the rest of the cell was over-tessellated, and the domain shader
 * ran the whole wave program for every vertex of every patch in every pass (depth and motion, surface, each point light
 * pass, underwater), several times a frame for the same surface.
 *
 * Here the CPU cuts each triangle of each water mesh drawn last frame into an adaptive quadtree whose leaves have the
 * same size on screen at every distance (after the clamped-parallelogram and adaptive compute tessellation schemes of
 * John Hable's "Compute Tessellation" posts at filmicworlds.com, on triangles): a leaf splits while its longest side is
 * more than `k` times its distance from the camera. With k < 1 neighbouring leaves differ by at most one level, so an
 * edge between two leaves is one segment or the two halves of the coarser side, and the finer side's segments own its
 * points. Along the mesh's own edges the points come from a 1-D tree of the edge alone, so two meshes or two triangles
 * sharing an edge agree without knowing each other. Every leaf is then an N x N grid; the GPU (TessellationCS) evaluates
 * every vertex's waves once, geomorphs the vertices a coarser level lacks, and writes the triangles. The water draws
 * read the vertices by index from a structured buffer (Water.hlsl, CachedWaterVertex).
 *
 * Render thread only.
 */
class WaterTessellator
{
public:
	/// Matches PBRWaterTess::Vertex (ComputeTessellation.hlsli).
	struct alignas(4) GpuVertex
	{
		float data[20];
	};
	static_assert(sizeof(GpuVertex) == 80);

	/// Matches PBRWaterTess::RawDisplacement.
	struct alignas(4) GpuRawDisplacement
	{
		float data[8];
	};
	static_assert(sizeof(GpuRawDisplacement) == 32);

	/// Matches PBRWaterTess::BaseTriangle.
	struct GpuBaseTriangle
	{
		float4 world[3];
		float4 model[3];
		float4 color[3];
		float4 uv01;
		float uv2[2];
		uint32_t order;
		uint32_t pad;
	};
	static_assert(sizeof(GpuBaseTriangle) == 176);

	/// Matches PBRWaterTess::Leaf.
	struct GpuLeaf
	{
		uint32_t vertexOffset;
		uint32_t triangleOffset;
		uint32_t baseIndex;
		uint32_t shape;
		int32_t corners[6];
		float interiorLog2;
		uint32_t pad;
		uint32_t sides[4];
		float sideLog2[3][4];
	};
	static_assert(sizeof(GpuLeaf) == 112);

	/// What the subdivision needs of the frame (PBRWater::UpdateTessellation fills it from the frame's constants).
	struct FrameParams
	{
		uint32_t frame = 0;                                  ///< the frame the mesh is for (State::frameCount)
		double cameraX = 0.0, cameraY = 0.0, cameraZ = 0.0;  ///< absolute camera position (CameraPosAdjust)
		float viewProj[4][4] = {};                           ///< camera-relative view-projection (row-major, mul(M, v))
		float pixelsPerUnit = 1.0f;                          ///< Tess0.w: pixels per unit at distance 1
		float targetSize = 10.0f;                            ///< Tess0.y: on-screen triangle size (px)
		float minSpacing = 0.0f;                             ///< the finest triangle edge (units): 128 / Maximum Tessellation
		float renderPixels = 2560.0f * 1440.0f;              ///< pixels the water is rendered at (the vertex budget)
		float cullMarginXY = 0.0f;                           ///< how far the waves can move the surface sideways (units)
		float cullMarginZ = 0.0f;                            ///< and up or down
		float fadeStart = 0.0f, fadeEnd = 1.0f;              ///< Params1.zw: the waves' distance fade
		float4 grid{};                                       ///< Grid0: the loaded cells (z <= x: none)
		bool ripples = false;                                ///< Ripple0.w: the ripple simulation's area is detailed
		float rippleX = 0.0f, rippleY = 0.0f, rippleInvExtent = 0.0f;
		const PBRWaterModel::ShoreField* shore = nullptr;                              ///< the shore field the waves follow (Shore0.x)
		float shoreMinX = 0.0f, shoreMinY = 0.0f, shoreInvX = 0.0f, shoreInvY = 0.0f;  ///< ShoreMap
		float deepLength = 0.0f;                                                       ///< Shore3.w: deep-water wavelength L0 (units)
		float curl = 0.0f;                                                             ///< Shore3.z: breaker curl
		float curlDetail = 1.0f;                                                       ///< Shore1.y: curl tessellation multiplier
		float curlDepth = 0.0f;                                                        ///< the deepest water a wave curls in (m): PBRWaterModel::ShoreCurlDepth
	};

	struct Stats
	{
		uint32_t meshes = 0;
		uint32_t baseTriangles = 0;
		uint32_t leaves = 0;
		uint32_t vertices = 0;
		uint32_t triangles = 0;
		float cpuMs = 0.0f;
		float detailScale = 1.0f;  ///< how many times the target triangle size the mesh was built at, to fit the vertex budget
	};

	void SetupResources();
	void CompileShaders();
	bool IsReady() const { return ready; }

	/**
	 * @brief Remembers a tessellated main-view draw of `geometry`: next frame's mesh covers it.
	 * @param geometry the water draw's geometry (a BSTriShape)
	 */
	void Register(RE::BSGeometry& geometry);

	/**
	 * @brief The range of this frame's index buffer that draws `geometry`, when this frame's mesh covers it.
	 * @return false when the geometry was not drawn last frame or changed since (draw it untessellated this frame)
	 */
	bool Lookup(RE::BSGeometry& geometry, uint32_t& startIndex, uint32_t& indexCount) const;

	/**
	 * @brief Subdivides and evaluates this frame's mesh for every water geometry registered last frame.
	 * The caller binds what the wave program reads in the compute stage (b7, b12, t8, t110-t127, s8, s12, s13).
	 */
	void Build(const FrameParams& params);

	/// Forgets this frame's mesh: every draw is untessellated.
	void Clear();

	/// Forgets this frame's mesh and the draws registered for the next (compute tessellation off).
	void Reset();

	/**
	 * @brief Draws a range of this frame's mesh with the device's current state (the water draw's shaders and constants,
	 * the vertex shader in its cached mode). Restores the index buffer and t109.
	 */
	void Draw(uint32_t startIndex, uint32_t indexCount) const;

	/// Whether the current mesh was built for this frame and camera (absolute position, CameraPosAdjust).
	bool BuiltFor(uint32_t frameCount, const float4& cameraPosAdjust) const;

	const Stats& GetStats() const { return stats; }
	/// The budget's coarsening of the target size the next subdivision starts from.
	float GetDetailScale() const { return detailScale; }

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
		std::vector<Corner> corners;                ///< three per base triangle (two for a mesh merged into its rectangle)
		uint32_t triangleCount = 0;
		uint64_t vertexDesc = 0;
		uint32_t lastUsed = 0;
		bool read = false;   ///< read back (successfully or not)
		bool valid = false;  ///< and usable
	};

	/// A geometry drawn tessellated in the main view.
	struct Source
	{
		RE::NiTransform world;
		const void* rendererData = nullptr;
		uint32_t triangleCount = 0;
		uint64_t vertexDesc = 0;
	};

	struct Range
	{
		Source source;
		uint32_t startIndex = 0;
		uint32_t indexCount = 0;
	};

	const Mesh* GetMesh(const Source& source);
	void EnsureCapacity(uint32_t leaves, uint32_t baseTriangles, uint32_t vertices, uint32_t triangles);
	/// Takes the oldest finished reach measurement (VertexCS), never waiting for the GPU.
	void ReadReach();

	static bool SameSource(const Source& a, const Source& b);
	static bool MakeSource(RE::BSGeometry& geometry, Source& source);

	std::unordered_map<const void*, Mesh> meshes;
	std::unordered_map<const void*, Source> registered;  ///< this frame's draws, for the next frame's build
	std::unordered_map<const void*, Range> ranges;       ///< this frame's mesh, by geometry
	float4 builtCamera{};
	uint32_t builtFrame = 0;
	bool built = false;
	uint32_t frame = 0;

	std::vector<GpuLeaf> leaves;
	std::vector<GpuBaseTriangle> baseTriangles;

	winrt::com_ptr<ID3D11ComputeShader> vertexCS;
	winrt::com_ptr<ID3D11ComputeShader> morphCS;
	winrt::com_ptr<ID3D11ComputeShader> indexCS;
	std::unique_ptr<ConstantBuffer> constants;
	std::unique_ptr<Buffer> leafBuffer;
	std::unique_ptr<Buffer> baseBuffer;
	std::unique_ptr<Buffer> vertexBuffer;
	std::unique_ptr<Buffer> rawBuffer;
	std::unique_ptr<Buffer> indexBuffer;
	uint32_t leafCapacity = 0;
	uint32_t baseCapacity = 0;
	uint32_t vertexCapacity = 0;
	uint32_t triangleCapacity = 0;
	bool ready = false;
	/// How far the waves moved this frame's vertices (VertexCS), read back a few frames later without a stall: the
	/// frustum culling's margin. -1 until the first measurement arrives.
	winrt::com_ptr<ID3D11Buffer> reachBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> reachUAV;
	static constexpr uint32_t ReachLatency = 3;
	std::array<winrt::com_ptr<ID3D11Buffer>, ReachLatency> reachStaging;
	std::array<bool, ReachLatency> reachPending{};
	uint32_t reachSlot = 0;
	float reachXY = -1.0f;
	float reachZ = -1.0f;

	float detailScale = 1.0f;  ///< the last frame's coarsening (Build): the next starts from it

	Stats stats;
};
