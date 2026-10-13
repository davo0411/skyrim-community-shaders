#pragma once

#include "Buffer.h"
#include "FloatingObjects.h"

#include <array>
#include <memory>
#include <vector>

/**
 * @brief Render thread: per-hull maps that keep the water out of the hulls near the camera, exact to their meshes.
 *
 * Built once per hull shape (FloatingObjects::HullShape), when it first comes near: its render pieces are drawn from
 * straight above and below in the hull's own frame, giving per texel the lowest and highest hull surface, and a
 * flood from the open water around the hull finds per texel the height the water must reach before it can flow in
 * there (HullMask.hlsl). Every frame the water shader only transforms its point into each nearby hull's frame as it
 * moves this frame and reads that hull's slice: dry above the lowest surface and below that level
 * (PBRWater::InsideDryHull), so a wave rising over the gunwale pours in.
 */
class HullMaskAtlas
{
public:
	static constexpr uint32_t Slots = FloatingObjects::MaxHullMasks;
	static constexpr uint32_t Size = 256;  ///< texels per side of a hull's maps (PBRW_HULL_MASK_SIZE in PBRWater.hlsli)

	void SetupResources();
	void ClearShaderCache();

	/**
	 * @brief Render thread, before the water is drawn: builds the maps of the hulls that have none yet, a few a frame,
	 * into slots no published hull uses. Saves and restores the pipeline state it changes.
	 */
	void Update(const std::vector<FloatingObjects::HullMask>& masks);

	/** @brief The atlas slice holding the maps of a hull shape, or -1 while it has none. */
	int FindSlot(const FloatingObjects::HullShape* shape) const;

	/** @brief One slice per slot: x lowest hull surface, y height the water must reach to flow in (hull frame z, units). */
	ID3D11ShaderResourceView* GetSRV() const { return ready ? atlas->srv.get() : nullptr; }

private:
	/** @brief Matches HullMaskCB in HullMask.hlsl. */
	struct alignas(16) MaskCB
	{
		float4 meshToHull[3];
		float4 projection;
		uint32_t vertexStride;
		uint32_t positionOffset;
		uint32_t halfPositions;
		uint32_t size;
		float ceiling;
		uint32_t direction;
		uint32_t slice;
		uint32_t pad;
	};
	STATIC_ASSERT_ALIGNAS_16(MaskCB);

	/** @brief Renders a shape's maps into a slot. */
	void Build(uint32_t slot, const FloatingObjects::HullShape& shape);
	/** @brief Copies an engine buffer into a raw buffer the vertex shader reads by index, growing it as needed; false when it is out of reach. */
	static bool CopyRaw(std::unique_ptr<Buffer>& raw, ID3D11Buffer* source, const char* name);

	std::array<std::shared_ptr<const FloatingObjects::HullShape>, Slots> slots;  ///< what each slice holds (kept alive, so a pointer never names another shape)
	std::unique_ptr<Texture2D> atlas;
	std::unique_ptr<Texture2D> bottom;  ///< scratch: lowest surface per texel (MIN blend)
	std::unique_ptr<Texture2D> top;     ///< scratch: highest surface per texel (MAX blend)
	std::unique_ptr<Texture2D> image;   ///< scratch: what holds the water back per texel
	std::unique_ptr<Texture2D> level;   ///< scratch: the flood
	std::unique_ptr<Buffer> vertices;   ///< raw copy of the vertex buffer being drawn
	std::unique_ptr<Buffer> indices;    ///< raw copy of its index buffer
	std::unique_ptr<ConstantBuffer> constants;
	winrt::com_ptr<ID3D11BlendState> blendState;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthState;
	winrt::com_ptr<ID3D11VertexShader> rasterVS;
	winrt::com_ptr<ID3D11PixelShader> rasterPS;
	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> floodCS;
	winrt::com_ptr<ID3D11ComputeShader> packCS;
	bool ready = false;
};
