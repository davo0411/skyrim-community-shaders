#pragma once

#include "Buffer.h"

#include <memory>
#include <unordered_map>

struct CellLevels;

/**
 * @brief The sea's projected grid: one mesh laid out evenly on the screen and cast onto the sea's plane, drawn in place of
 * every water mesh at the sea's level (the loaded cells' and Unified Water's far tiles).
 *
 * The cells' meshes are tessellated only inside the loaded cells, and the far tiles meet them with T-junctions, so the
 * waves had to fade out at the loaded cells' border. One mesh over the whole plane has no border: the waves reach as far
 * as the sea is seen, and as many triangles near the camera as at the horizon, each a few pixels across (no sub-pixel
 * triangles far away, no quads shaded over and over along them). The vertices are evaluated once a frame
 * (ProjectedGridCS.hlsl), filtered to the grid's spacing at each one so the waves they carry do not swim as the grid slides
 * over the sea with the camera; Water.hlsl draws them (GridWaterVertex, Tess0.x 6). The sea is where Unified Water's
 * coverage has cells at its level: the pixels elsewhere are discarded (PBRWater::SeaCovers).
 */
class ProjectedGrid
{
public:
	/** @brief One frame's layout inputs. */
	struct FrameParams
	{
		float viewProj[4][4]{};  ///< camera-relative view-projection, unjittered (clip[r] = sum viewProj[r][c] v[c])
		float cameraZ = 0.0f;    ///< absolute camera height
		float viewportWidth = 0.0f;
		float viewportHeight = 0.0f;
		float cellPixels = 8.0f;    ///< target cell size on screen (pixels)
		float maxDistance = 0.0f;   ///< the farthest a vertex is cast (units)
	};

	/** @brief Compiles the compute shader and creates the buffers. */
	void Setup();

	/** @brief Takes the worldspace's cells (Unified Water's coverage: each cell's still level), or none outside one. */
	void SetCells(const std::shared_ptr<const CellLevels>& cells);

	/**
	 * @brief Lays the grid out over the part of the screen the sea can cover and evaluates its vertices. The PBR Water
	 * constants (b7), the frame constants (b12), the water textures and samplers must be bound to the compute stage.
	 * @return false when there is no sea to draw this frame (none in the worldspace, none on screen, not ready)
	 */
	bool Build(const FrameParams& params, uint32_t frame);

	/** @brief Draws the grid with the bound pipeline (Water.hlsl's vertex shader in its grid mode). */
	void Draw() const;

	/** @brief Whether the grid was built for frame `frame`. */
	bool IsBuilt(uint32_t frame) const { return builtFrame == frame; }

	/** @brief Whether the worldspace has a sea (cells at one level). */
	bool HasSea() const { return cellsSRV != nullptr; }

	/** @brief The sea's level (absolute z). */
	float SeaLevel() const { return seaLevel; }

	uint32_t Columns() const { return columns; }
	uint32_t Rows() const { return rows; }

	/** @brief The cells texture (SeaLevels, t28): each cell's still level, < -1e8 where it has none. */
	ID3D11ShaderResourceView* CellsSRV() const { return cellsSRV.get(); }

	/** @brief xy the first cell, zw the cells across (PBRWater GpuData::Proj1). */
	float4 CellsMapping() const { return cellsMapping; }

private:
	winrt::com_ptr<ID3D11ComputeShader> gridCS;
	std::unique_ptr<ConstantBuffer> constants;
	std::unique_ptr<Buffer> vertices;
	uint32_t capacity = 0;  ///< vertices `vertices` holds

	winrt::com_ptr<ID3D11Texture2D> cellsTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> cellsSRV;
	float4 cellsMapping{};
	float seaLevel = 0.0f;

	uint32_t columns = 0;
	uint32_t rows = 0;
	uint32_t builtFrame = UINT32_MAX;

	mutable std::unordered_map<ID3D11RasterizerState*, winrt::com_ptr<ID3D11RasterizerState>> twoSided;
};
