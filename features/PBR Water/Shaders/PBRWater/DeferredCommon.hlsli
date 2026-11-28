#ifndef __PBR_WATER_DEFERRED_COMMON_HLSLI__
#define __PBR_WATER_DEFERRED_COMMON_HLSLI__

#include "Common/FrameBuffer.hlsli"

// The deferred surface's tiles (pixels a side) and how many grids it records at most a frame: their tags are
// 1..PBRWATER_DEFERRED_MESHES, one bit each in a tile's mask (DeferredTilesCS). Mirrored in PBRWater.h.
#define PBRWATER_DEFERRED_TILE 8
#define PBRWATER_DEFERRED_MESHES 32

// Each mesh has two tile lists: list m (0..MESHES-1) its near tiles, list MESHES + m its distant ones, which the surface
// pass shades at a quarter of the pixels (DeferredDraw Mode.y = 1) and upsamples (DeferredFar.hlsl).
#define PBRWATER_DEFERRED_LISTS (2 * PBRWATER_DEFERRED_MESHES)

// Deferred.hlsli's Mode.y: what a deferred draw does.
#define PBRWATER_DEFERRED_FULL 0  // shade every pixel of the tiles
#define PBRWATER_DEFERRED_HALF 1  // shade one pixel of every 2x2 into the half-size target (PBRWater::deferredFar)

/// The 16 bits of a depth the record keeps (Deferred.hlsli Pack: the tag's top half).
uint PBRWaterDepthBits(float depth)
{
	return (uint)round(saturate(depth) * 16777215.0) & 0xFFFF;
}

/// Whether the record's `tag` still holds the pixel the depth buffer now has at `depth`: it was written at that depth, to
/// within the record's rounding. A nearer water draw since leaves another depth there.
bool PBRWaterRecordHolds(uint tag, float depth)
{
	return ((PBRWaterDepthBits(depth) - (tag >> 16) + 1) & 0xFFFF) <= 2;
}

/// Camera-relative position on the screen at pixel `pixel` (its centre at + 0.5) with depth `depth`, on a record of
/// `size` pixels.
float3 PBRWaterUnproject(float2 pixel, float depth, float2 size)
{
	float2 uv = pixel * FrameBuffer::DynamicResolutionParams2.xy / size;
	float4 world = mul(FrameBuffer::CameraViewProjInverse, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
	return world.xyz / world.w;
}

#endif  // __PBR_WATER_DEFERRED_COMMON_HLSLI__
