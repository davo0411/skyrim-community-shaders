// ============================================================================
// PBR Water - the deferred surface's tiles (Deferred.hlsli).
//
// Sorts the screen's 8x8 pixel tiles by the tessellated meshes the stencil pass recorded in them: each mesh's surface
// and point light draws then cover its own tiles only (DrawInstancedIndirect, one instance a tile), not a rectangle
// around it that overlapped every other mesh's.
// ============================================================================

#include "PBRWater/DeferredCommon.hlsli"

cbuffer DeferredTilesCB : register(b0)
{
	uint2 Size;     // the record's size (pixels)
	uint MaxTiles;  // the tile list's room for each mesh
	uint Pad0;
};

Texture2D<uint> Tags : register(t0);
RWByteAddressBuffer Args : register(u0);        // per mesh: DrawInstancedIndirect's arguments (6 vertices, tile count, 0, 0)
RWStructuredBuffer<uint> Tiles : register(u1);  // per mesh, MaxTiles: x | y << 16 of each tile

groupshared uint Meshes;

[numthreads(PBRWATER_DEFERRED_TILE, PBRWATER_DEFERRED_TILE, 1)] void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	if (gi == 0)
		Meshes = 0;
	GroupMemoryBarrierWithGroupSync();
	if (all(id.xy < Size)) {
		uint tag = Tags.Load(int3(id.xy, 0)) & 0xFF;
		if (tag != 0 && tag <= PBRWATER_DEFERRED_MESHES)
			InterlockedOr(Meshes, 1u << (tag - 1));
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi < PBRWATER_DEFERRED_MESHES && ((Meshes >> gi) & 1u)) {
		uint index;
		// The arguments start cleared (PBRWater::ClassifyDeferredTiles): every group listing a tile writes the vertex count.
		Args.Store(gi * 16, 6);
		Args.InterlockedAdd(gi * 16 + 4, 1, index);
		if (index < MaxTiles)
			Tiles[gi * MaxTiles + index] = gid.x | (gid.y << 16);
	}
}
