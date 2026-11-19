#ifndef __PBR_WATER_DEFERRED_COMMON_HLSLI__
#define __PBR_WATER_DEFERRED_COMMON_HLSLI__

// The deferred surface's tiles (pixels a side) and how many tessellated meshes it records at most a frame: their tags are
// 1..PBRWATER_DEFERRED_MESHES, one bit each in a tile's mask (DeferredTilesCS). Mirrored in PBRWater.h.
#define PBRWATER_DEFERRED_TILE 8
#define PBRWATER_DEFERRED_MESHES 32

// Each mesh has two tile lists: list m (0..MESHES-1) its near tiles, list MESHES + m its distant ones, which the surface
// pass shades at a quarter of the pixels (DeferredDraw Mode.y = 1) and upsamples (DeferredFar.hlsl).
#define PBRWATER_DEFERRED_LISTS (2 * PBRWATER_DEFERRED_MESHES)

// Deferred.hlsli's Mode.y: what a deferred draw does.
#define PBRWATER_DEFERRED_FULL 0  // shade every pixel of the tiles
#define PBRWATER_DEFERRED_HALF 1  // shade one pixel of every 2x2 into the half-size target (PBRWater::deferredFar)

#endif  // __PBR_WATER_DEFERRED_COMMON_HLSLI__
