#ifndef __PBR_WATER_DEFERRED_COMMON_HLSLI__
#define __PBR_WATER_DEFERRED_COMMON_HLSLI__

// The deferred surface's tiles (pixels a side) and how many tessellated meshes it records at most a frame: their tags are
// 1..PBRWATER_DEFERRED_MESHES, one bit each in a tile's mask (DeferredTilesCS). Mirrored in PBRWater.h.
#define PBRWATER_DEFERRED_TILE 8
#define PBRWATER_DEFERRED_MESHES 32

#endif  // __PBR_WATER_DEFERRED_COMMON_HLSLI__
