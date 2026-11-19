// ============================================================================
// PBR Water - the deferred surface's tiles (Deferred.hlsli).
//
// Sorts the screen's 8x8 pixel tiles by the tessellated meshes the stencil pass recorded in them: each mesh's surface
// and point light draws then cover its own tiles only (DrawInstancedIndirect, one instance a tile), not a rectangle
// around it that overlapped every other mesh's. A tile whose nearest recorded water lies beyond FarDistance goes to the
// mesh's distant list instead, which the surface pass shades at half resolution (DeferredFar.hlsl).
// ============================================================================

#include "Common/FrameBuffer.hlsli"
#include "PBRWater/DeferredCommon.hlsli"

cbuffer DeferredTilesCB : register(b0)
{
	uint2 Size;         // the record's size (pixels)
	uint MaxTiles;      // the tile list's room for each list
	float FarDistance;  // camera distance beyond which a tile is distant (units); 0: none is
};

Texture2D<uint> Tags : register(t0);
Texture2D<float> SceneDepth : register(t1);
RWByteAddressBuffer Args : register(u0);        // per list: DrawInstancedIndirect's arguments (6 vertices, tile count, 0, 0)
RWStructuredBuffer<uint> Tiles : register(u1);  // per list, MaxTiles: x | y << 16 of each tile

groupshared uint Meshes;
groupshared uint Nearest;  // asuint of the nearest recorded water's distance (units): positive floats order as uints

uint DepthBits(float depth)
{
	return (uint)round(saturate(depth) * 16777215.0) & 0xFFFF;
}

[numthreads(PBRWATER_DEFERRED_TILE, PBRWATER_DEFERRED_TILE, 1)] void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	if (gi == 0) {
		Meshes = 0;
		Nearest = 0x7F7FFFFF;
	}
	GroupMemoryBarrierWithGroupSync();
	if (all(id.xy < Size)) {
		uint tag = Tags.Load(int3(id.xy, 0));
		uint mesh = tag & 0xFF;
		if (mesh != 0 && mesh <= PBRWATER_DEFERRED_MESHES) {
			InterlockedOr(Meshes, 1u << (mesh - 1));
			// The distance of the water the record still holds here (PBRWaterDeferred::Read's test).
			float depth = SceneDepth.Load(int3(id.xy, 0));
			uint bits = (DepthBits(depth) - (tag >> 16) + 1) & 0xFFFF;
			[branch] if (FarDistance > 0.0 && bits <= 2)
			{
				float2 uv = (id.xy + 0.5) * FrameBuffer::DynamicResolutionParams2.xy / float2(Size);
				float4 world = mul(FrameBuffer::CameraViewProjInverse, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
				InterlockedMin(Nearest, asuint(length(world.xyz / world.w)));
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi < PBRWATER_DEFERRED_MESHES && ((Meshes >> gi) & 1u)) {
		bool distant = FarDistance > 0.0 && asfloat(Nearest) > FarDistance;
		uint list = distant ? PBRWATER_DEFERRED_MESHES + gi : gi;
		uint index;
		// The arguments start cleared (PBRWater::ClassifyDeferredTiles): every group listing a tile writes the vertex count.
		Args.Store(list * 16, 6);
		Args.InterlockedAdd(list * 16 + 4, 1, index);
		if (index < MaxTiles)
			Tiles[list * MaxTiles + index] = gid.x | (gid.y << 16);
	}
}
