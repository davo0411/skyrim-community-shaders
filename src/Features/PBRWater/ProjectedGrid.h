#pragma once

#include "Buffer.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct CellLevels;

/**
 * @brief The projected grids: per water level on screen (the sea's, each lake's), one mesh laid out evenly on the screen
 * and cast onto the level's plane, drawn in place of every water mesh at that level (the loaded cells' and Unified Water's
 * far tiles).
 *
 * The cells' meshes are tessellated only inside the loaded cells, and the far tiles meet them with T-junctions, so the
 * waves had to fade out at the loaded cells' border. One mesh over the whole plane has no border: the waves reach as far
 * as the water is seen, and as many triangles near the camera as far away, each a few pixels across (no sub-pixel
 * triangles far away, no quads shaded over and over along them). The vertices are evaluated once a frame
 * (ProjectedGridCS.hlsl), filtered to the grid's spacing at each one so the waves they carry do not swim as the grid slides
 * over the water with the camera; where a breaker needs more than a cell gives, the cell is drawn as a patch of
 * Subdivision^2 cells stitched to its coarse neighbours. Water.hlsl draws them (Tess0.x 6 the cells, 7 the patches). The
 * water is where Unified Water's coverage has cells at the grid's level (PBRWater::SeaCovers); rivers keep their meshes, and
 * the cells under them are taken out of the grids.
 */
class ProjectedGrid
{
public:
	static constexpr uint32_t MaxLevels = 4;     ///< grids a frame (the sea and three lakes); PBRW_GRID_MAX_LEVELS
	static constexpr uint32_t Subdivision = 4;   ///< a breaker's patch is Subdivision^2 cells; PBRW_GRID_PATCH
	static constexpr uint32_t MaxVertices = 300000;
	static constexpr uint32_t MaxPatches = 8192;

	/** @brief One frame's layout inputs. */
	struct FrameParams
	{
		float viewProj[4][4]{};  ///< camera-relative view-projection, unjittered (clip[r] = sum viewProj[r][c] v[c])
		float cameraX = 0.0f;    ///< absolute camera position
		float cameraY = 0.0f;
		float cameraZ = 0.0f;
		float viewportWidth = 0.0f;
		float viewportHeight = 0.0f;
		float cellPixels = 8.0f;    ///< target cell size on screen (pixels)
		float maxDistance = 0.0f;   ///< the farthest a vertex is cast (units)
	};

	/** @brief A grid built this frame, for the draws (PBRWater GpuData::Proj0-2). */
	struct BuiltLevel
	{
		float z = 0.0f;  ///< absolute level
		uint32_t columns = 0;
		uint32_t rows = 0;
		uint32_t firstVertex = 0;
		uint32_t firstCell = 0;
		uint32_t firstPatch = 0;
	};

	/** @brief Compiles the compute shaders and creates the buffers. */
	void Setup();

	/** @brief Compiles the compute shaders (again when the shader cache is cleared). */
	void CompileShaders();

	/** @brief Takes the worldspace's cells (Unified Water's coverage: each cell's still level), or none outside one. */
	void SetCells(const std::shared_ptr<const CellLevels>& cells);

	/** @brief A water mesh at absolute level `z` was drawn in the main view: its level gets a grid from the next frame. */
	void Want(float z, uint32_t frame);

	/** @brief A river's mesh over `centerX/Y` (radius `radius`) at level `z`: its cells leave the grid at that level. */
	void ExcludeRiver(float centerX, float centerY, float radius, float z);

	/** @brief The sea meshes' (or a lake's) texture coordinates as planes over absolute xy, for the level at `z`. */
	void SetTexCoords(float z, const float4& u, const float4& v);
	bool HasTexCoords(float z) const;

	/**
	 * @brief Lays the grids out over the parts of the screen their levels can cover and evaluates their vertices and the
	 * breakers' patches. The PBR Water constants (b7), the frame constants (b12), the water textures and samplers must be
	 * bound to the compute stage.
	 */
	void Build(const FrameParams& params, uint32_t frame);

	/** @brief The slot of the grid built this frame at absolute level `z`, or -1. */
	int SlotOf(float z, uint32_t frame) const;

	/** @brief The grids built this frame. */
	uint32_t BuiltCount(uint32_t frame) const { return builtFrame == frame ? static_cast<uint32_t>(built.size()) : 0; }
	const BuiltLevel& Built(uint32_t slot) const { return built[slot]; }

	/** @brief Draws grid `slot`'s cells with the bound pipeline (Water.hlsl's vertex shader, Tess0.x 6). */
	void DrawCells(uint32_t slot) const;
	/** @brief Draws grid `slot`'s breaker patches (Tess0.x 7). */
	void DrawPatches(uint32_t slot) const;

	/** @brief Whether the worldspace has water levels for grids. */
	bool HasCells() const { return cellsSRV != nullptr; }
	float SeaLevel() const { return levels.empty() ? 0.0f : levels[seaIndex].z; }

	/** @brief The cells texture (SeaLevels, t28): each cell's still level, < -1e8 where it has none or a river's. */
	ID3D11ShaderResourceView* CellsSRV() const { return cellsSRV.get(); }

	/** @brief xy the first cell, zw the cells across (PBRWater GpuData::Proj1). */
	float4 CellsMapping() const { return cellsMapping; }

private:
	/** @brief ProjectedGridCS PBRWaterGrid::Level. */
	struct GpuLevel
	{
		float range[4];
		float plane[4];
		uint32_t layout[4];
		uint32_t patches[4];
		float4 texCoordU;
		float4 texCoordV;
	};

	/** @brief A water level of the worldspace: its cells' bounds (absolute units) and what the grid knows of it. */
	struct Level
	{
		float z = 0.0f;
		uint32_t cells = 0;
		float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
		uint32_t wantedFrame = 0;
		bool hasTexCoords = false;
		float4 texCoordU{};
		float4 texCoordV{};
	};

	void CreateBuffers();
	int FindLevel(float z) const;
	void Draw(uint32_t vertexCount, const ID3D11ShaderResourceView* vertexView, uint32_t argsOffset) const;

	winrt::com_ptr<ID3D11ComputeShader> gridCS, flagCS, argsCS, patchCS;
	std::unique_ptr<ConstantBuffer> constants;
	std::unique_ptr<Buffer> levelsBuffer;
	std::unique_ptr<Buffer> vertices;
	std::unique_ptr<Buffer> flags;
	std::unique_ptr<Buffer> patches;
	std::unique_ptr<Buffer> patchVertices;
	winrt::com_ptr<ID3D11Buffer> counters, args;
	winrt::com_ptr<ID3D11UnorderedAccessView> countersUAV, argsUAV;

	winrt::com_ptr<ID3D11Texture2D> cellsTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> cellsSRV;
	float4 cellsMapping{};
	int32_t cellsMinX = 0, cellsMinY = 0;
	uint32_t cellsWidth = 0, cellsHeight = 0;
	std::vector<Level> levels;
	uint32_t seaIndex = 0;
	std::unordered_set<uint64_t> excluded;  ///< cells taken out for rivers

	std::vector<BuiltLevel> built;
	uint32_t builtFrame = UINT32_MAX;

	mutable std::unordered_map<ID3D11RasterizerState*, winrt::com_ptr<ID3D11RasterizerState>> twoSided;
};
