// ============================================================================ PBR Water - the deferred surface's
// material (PBRWater::BakeSurfaceMaterial).
//
// After the stencil pass and DeferredShoreCS this pass evaluates the waves, shore train and foam coverage once for
// every recorded pixel (PBRWater::EvaluateSurface), rebuilding each from its record as PBRWaterDeferred::Resolve does,
// and writes the material (Material.hlsli). The lighting pixel shader (Water.hlsl, PBRW_MATERIAL) reads it back and
// holds none of that code, which keeps its register pressure and occupancy in check.
//
// A compute shader has no screen derivatives: the pixel's footprint on the water and the cascades' texture-coordinate
// derivatives come from the rays through the neighbouring pixels met with the horizontal plane of the undisplaced point
// (as DeferredShoreCS does the footprint). ============================================================================

#define PBRW_MATERIAL_CS

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "PBRWater/DeferredCommon.hlsli"

// The tile's baked shore blocks (DeferredShoreCS, one a 2x2 pixels), with one more around it for the bilinear blend: read
// once per tile into group-shared memory (PBRWater.hlsli BakedShoreTrain reads them there, PBRW_BAKED_SHORE_SHARED).
#define PBRW_BAKED_SHORE_SHARED
#define PBRW_SHARED_SHORE_SPAN (PBRWATER_DEFERRED_TILE / 2 + 2)
groupshared uint4 SharedShore0[PBRW_SHARED_SHORE_SPAN * PBRW_SHARED_SHORE_SPAN];
groupshared uint4 SharedShore1[PBRW_SHARED_SHORE_SPAN * PBRW_SHARED_SHORE_SPAN];
groupshared uint4 SharedShore2[PBRW_SHARED_SHORE_SPAN * PBRW_SHARED_SHORE_SPAN];
static int2 SharedShoreOrigin = 0;  // the block at SharedShore*[0]
uint SharedShoreIndex(int2 block)
{
	int2 k = clamp(block - SharedShoreOrigin, 0, PBRW_SHARED_SHORE_SPAN - 1);
	return k.y * PBRW_SHARED_SHORE_SPAN + k.x;
}
uint4 SharedShoreLoad0(int2 block) { return SharedShore0[SharedShoreIndex(block)]; }
uint4 SharedShoreLoad1(int2 block) { return SharedShore1[SharedShoreIndex(block)]; }
uint4 SharedShoreLoad2(int2 block) { return SharedShore2[SharedShoreIndex(block)]; }

#include "PBRWater/Material.hlsli"

cbuffer SurfaceMaterialCB : register(b0)
{
	uint2 Size;         // the record's size (pixels)
	uint ShoreBaked;    // the shore train is baked this frame (DeferredShoreCS)
	float FarDistance;  // camera distance beyond which a tile is shaded at half rate (units; DeferredTilesCS); 0: none is
	uint4 TileList;     // x where DeferredTilesCS's list of every tile with water starts in WaterTiles
};

groupshared uint Nearest;  // asuint of the nearest recorded water's distance in the tile (positive floats order as uints)

Texture2D<uint> Tags : register(t0);
Texture2D<float4> Waves : register(t1);
Texture2D<float> SceneDepth : register(t2);
StructuredBuffer<uint> WaterTiles : register(t3);  // DeferredTilesCS: x | y << 16 of each tile, one group each
RWTexture2D<uint4> OutA : register(u0);
RWTexture2D<uint2> OutB : register(u1);

/// Whether the stencil pass's grid water is still the nearest at `px` (PBRWaterDeferred::Read).
bool RecordedWater(int2 px, out float depth)
{
	uint tag = Tags.Load(int3(px, 0));
	depth = SceneDepth.Load(int3(px, 0));
	return (tag & 0xFF) != 0 && PBRWaterRecordHolds(tag, depth);
}

[numthreads(PBRWATER_DEFERRED_TILE, PBRWATER_DEFERRED_TILE, 1)] void main(uint3 local : SV_GroupThreadID, uint3 listed : SV_GroupID, uint gi : SV_GroupIndex) {
	// A group is one of DeferredTilesCS's tiles with water: the nearest water in it decides whether the tile is a distant one.
	uint packed = WaterTiles[TileList.x + listed.x];
	uint2 group = uint2(packed & 0xFFFF, packed >> 16);
	uint2 id = group * PBRWATER_DEFERRED_TILE + local.xy;
	if (gi == 0)
		Nearest = 0x7F7FFFFF;
	// Its baked shore blocks: the pixels' bilinear blend reaches one block past the tile's own on each side.
	SharedShoreOrigin = int2(group.xy) * (PBRWATER_DEFERRED_TILE / 2) - 1;
	[branch] if (ShoreBaked != 0 && gi < PBRW_SHARED_SHORE_SPAN * PBRW_SHARED_SHORE_SPAN)
	{
		uint width, height;
		PBRWater::BakedShore0.GetDimensions(width, height);
		int2 c = clamp(SharedShoreOrigin + int2(gi % PBRW_SHARED_SHORE_SPAN, gi / PBRW_SHARED_SHORE_SPAN), 0, int2(width, height) - 1);
		SharedShore0[gi] = PBRWater::BakedShore0.Load(int3(c, 0));
		SharedShore1[gi] = PBRWater::BakedShore1.Load(int3(c, 0));
		SharedShore2[gi] = PBRWater::BakedShore2.Load(int3(c, 0));
	}
	GroupMemoryBarrierWithGroupSync();
	int2 px = int2(id.xy);
	float depth = 1.0;
	bool water = all(id.xy < Size) && RecordedWater(px, depth);
	float3 displaced = 0.0;
	if (water) {
		displaced = PBRWaterUnproject(px + 0.5, depth, float2(Size));
		if (FarDistance > 0.0)
			InterlockedMin(Nearest, asuint(length(displaced)));
	}
	GroupMemoryBarrierWithGroupSync();
	if (!water)
		return;
	// The half-rate pass shades one pixel of every 2x2 of a distant tile, whose derivatives span two pixels: the footprint
	// the coarser sampling has (the pixel program filters the waves and the foam by it).
	const float span = FarDistance > 0.0 && asfloat(Nearest) > FarDistance ? 2.0 : 1.0;

	float2 centre = px + 0.5;
	float3 undisplaced = displaced - Waves.Load(int3(px, 0)).xyz;

	// How the undisplaced position changes to the next pixel right and down: the rays through them met with the plane of
	// this point; the pixel's footprint is the longer of the two.
	float3 ray = normalize(PBRWaterUnproject(centre, 1.0, float2(Size)));
	float3 rayX = normalize(PBRWaterUnproject(centre + float2(span, 0.0), 1.0, float2(Size)));
	float3 rayY = normalize(PBRWaterUnproject(centre + float2(0.0, span), 1.0, float2(Size)));
	float3 dx, dy;
	float footprint;
	[branch] if (undisplaced.z * ray.z > 0.0 && abs(ray.z) > 0.02)
	{
		float3 onPlane = ray * (undisplaced.z / ray.z);
		dx = rayX * (undisplaced.z / (abs(rayX.z) > 1e-3 ? rayX.z : ray.z)) - onPlane;
		dy = rayY * (undisplaced.z / (abs(rayY.z) > 1e-3 ? rayY.z : ray.z)) - onPlane;
		footprint = max(length(dx.xy), length(dy.xy));
	}
	else
	{
		float scale = length(displaced) / max(abs(ray.z), 0.02);
		dx = (rayX - ray) * scale;
		dy = (rayY - ray) * scale;
		footprint = max(length(dx), length(dy));
	}
	PBRWater::PositionDdx = dx.xy;
	PBRWater::PositionDdy = dy.xy;

	PBRWater::BakedShoreActive = ShoreBaked != 0;
	PBRWater::BakedShorePixel = px;
	PBRWater::BakedShoreDistance = max(length(displaced), 1.0);

	PBRWater::SurfaceShading surface = PBRWater::EvaluateSurface(float4(undisplaced, 0.0), footprint);
	uint4 a;
	uint2 b;
	PBRWaterMaterial::Encode(surface, a, b);
	OutA[px] = a;
	OutB[px] = b;
}
