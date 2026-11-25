// ============================================================================
// PBR Water - the deferred surface's material (PBRWater::BakeSurfaceMaterial).
//
// The surface and point light passes used to evaluate the waves, the shore train and the foam coverage in the same pixel
// program that lights the water, and the registers that took capped the pass's occupancy (about a quarter of the warps the
// GPU could keep). After the stencil pass and DeferredShoreCS this pass evaluates them once for every pixel the stencil pass
// recorded (PBRWater::EvaluateSurface), from its record exactly as PBRWaterDeferred::Resolve rebuilds it, and writes the
// material (Material.hlsli: 24 bytes a pixel in two textures). The lighting pixel shader (Water.hlsl, PBRW_MATERIAL) reads
// it back and holds none of that code.
//
// A compute shader has no screen derivatives. The pixel program took two from its quad: the footprint of the pixel on the
// water, and how the cascades' texture coordinates change over it. Both are worked out here from the rays through the
// neighbouring pixels met with the horizontal plane of the undisplaced point (as DeferredShoreCS does the footprint), which
// is the derivative of the undisplaced position on a still sea and follows it on a wavy one to within the waves' own slope.
// ============================================================================

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
};

groupshared uint Nearest;  // asuint of the nearest recorded water's distance in the tile (positive floats order as uints)

Texture2D<uint> Tags : register(t0);
Texture2D<float4> Waves : register(t1);
Texture2D<float> SceneDepth : register(t2);
RWTexture2D<uint4> OutA : register(u0);
RWTexture2D<uint2> OutB : register(u1);

uint DepthBits(float depth)
{
	return (uint)round(saturate(depth) * 16777215.0) & 0xFFFF;
}

/// Whether the stencil pass's tessellated water is still the nearest at `px` (PBRWaterDeferred::Read).
bool RecordedWater(int2 px, out float depth)
{
	uint tag = Tags.Load(int3(px, 0));
	depth = SceneDepth.Load(int3(px, 0));
	uint bits = (DepthBits(depth) - (tag >> 16) + 1) & 0xFFFF;
	return (tag & 0xFF) != 0 && bits <= 2;
}

/// Camera-relative position on the screen at pixel centre `pixel` and depth `depth` (as Resolve finds it).
float3 Unproject(float2 pixel, float depth)
{
	float2 uv = pixel * FrameBuffer::DynamicResolutionParams2.xy / float2(Size);
	float4 world = mul(FrameBuffer::CameraViewProjInverse, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
	return world.xyz / world.w;
}

[numthreads(PBRWATER_DEFERRED_TILE, PBRWATER_DEFERRED_TILE, 1)] void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint gi : SV_GroupIndex) {
	// A group is one of DeferredTilesCS's tiles: the nearest water in it decides whether the tile is a distant one.
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
		displaced = Unproject(px + 0.5, depth);
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
	float3 ray = normalize(Unproject(centre, 1.0));
	float3 rayX = normalize(Unproject(centre + float2(span, 0.0), 1.0));
	float3 rayY = normalize(Unproject(centre + float2(0.0, span), 1.0));
	float3 dx, dy;
	float footprint;
	[branch] if (undisplaced.z * ray.z > 0.0 && abs(ray.z) > 0.02)
	{
		float3 onPlane = ray * (undisplaced.z / ray.z);
		dx = rayX * (undisplaced.z / (abs(rayX.z) > 1e-3 ? rayX.z : ray.z)) - onPlane;
		dy = rayY * (undisplaced.z / (abs(rayY.z) > 1e-3 ? rayY.z : ray.z)) - onPlane;
		footprint = max(length(dx.xy), length(dy.xy));
	}
	else {
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
