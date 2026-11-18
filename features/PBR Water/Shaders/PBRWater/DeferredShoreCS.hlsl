// ============================================================================
// PBR Water - the shore wave train for the deferred surface, once per 2x2 pixels.
//
// The shore train (PBRWater::ShoreWaves) is a large part of the surface's pixel program: each pixel finds its wave,
// shoals and breaks it, looks up its profile and turns its curl, some hundreds of instructions where the train reaches,
// which near a coast is most of the water on screen. Everything it makes varies over a wave's length, metres, not from
// one pixel to the next. So after the stencil pass this pass evaluates it once for every 2x2 pixels, from the stencil
// pass's record (Deferred.hlsli), and the surface and point light passes read it back, blended between the four nearest
// blocks that lie at the pixel's own depth (BakedShoreTrain). Pixels with no such block, along the edges of the water
// in front of other water, evaluate it themselves.
//
// Each block's train is packed into three uint4 (PBRWater::BakedShoreTrain unpacks them): half floats for the
// displacements, slopes and stretches, eight bits for the 0..1 shares, and the distance of the pixel it was evaluated at.
// ============================================================================

#include "Common/FrameBuffer.hlsli"
#include "PBRWater/DeferredCommon.hlsli"
#include "PBRWater/PBRWater.hlsli"

cbuffer DeferredShoreCB : register(b0)
{
	uint2 Size;      // the record's size (pixels)
	uint2 HalfSize;  // this pass's grid: a texel per 2x2 pixels
};

Texture2D<uint> Tags : register(t0);
Texture2D<float4> Waves : register(t1);
Texture2D<float> SceneDepth : register(t2);
Texture2D<float4> WorldFlowMap : register(t3);
RWTexture2D<uint4> Out0 : register(u0);
RWTexture2D<uint4> Out1 : register(u1);
RWTexture2D<uint4> Out2 : register(u2);

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

uint Pack(float a, float b)
{
	return f32tof16(a) | (f32tof16(b) << 16);
}

/// Four 0..1 values, eight bits each.
uint PackUnorm4(float4 v)
{
	uint4 b = (uint4)round(saturate(v) * 255.0);
	return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= HalfSize))
		return;
	int2 px = 0;
	float depth = 1.0;
	bool found = false;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 candidate = min(int2(id.xy * 2 + uint2(i & 1, i >> 1)), int2(Size) - 1);
		float d;
		if (!found && RecordedWater(candidate, d)) {
			px = candidate;
			depth = d;
			found = true;
		}
	}
	if (!found) {
		Out0[id.xy] = 0;  // distance 0: no train here
		return;
	}

	float2 centre = px + 0.5;
	float3 displaced = Unproject(centre, depth);
	float3 undisplaced = displaced - Waves.Load(int3(px, 0)).xyz;
	float2 flowVelocity;
	float flowDamping = PBRWater::FlowDampingAt(WorldFlowMap, PBRWater::LinearClampSampler, undisplaced.xy + FrameBuffer::CameraPosAdjust.xy, flowVelocity);

	// The pixel's footprint on the still water (ShadeSurface's, from the derivatives of the undisplaced position there):
	// the rays through the neighbouring pixels met with the plane of this point.
	float3 ray = normalize(Unproject(centre, 1.0));
	float3 rayX = normalize(Unproject(centre + float2(1.0, 0.0), 1.0));
	float3 rayY = normalize(Unproject(centre + float2(0.0, 1.0), 1.0));
	float footprint;
	[branch] if (undisplaced.z * ray.z > 0.0 && abs(ray.z) > 0.02)
	{
		float3 onPlane = ray * (undisplaced.z / ray.z);
		float3 onPlaneX = rayX * (undisplaced.z / (abs(rayX.z) > 1e-3 ? rayX.z : ray.z));
		float3 onPlaneY = rayY * (undisplaced.z / (abs(rayY.z) > 1e-3 ? rayY.z : ray.z));
		footprint = max(length(onPlaneX.xy - onPlane.xy), length(onPlaneY.xy - onPlane.xy));
	}
	else footprint = length(displaced) * max(length(rayX - ray), length(rayY - ray)) / max(abs(ray.z), 0.02);

	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(undisplaced, flowDamping);
	ctx.flow = flowVelocity;
	PBRWater::ShoreTrain t = PBRWater::ShoreWaves(undisplaced, ctx, footprint, false, true);

	float distance = max(length(displaced), 1.0);
	Out0[id.xy] = uint4(Pack(t.displacement.x, t.displacement.y), Pack(t.displacement.z, distance), Pack(t.lip.x, t.lip.y), Pack(t.lip.z, t.curl));
	Out1[id.xy] = uint4(Pack(t.slope.x, t.slope.y), Pack(t.lipSlope.x, t.lipSlope.y), Pack(t.compression.x, t.compression.y), Pack(t.compression.z, t.slopeVariance));
	Out2[id.xy] = uint4(Pack(t.lipCompression.x, t.lipCompression.y), Pack(t.lipCompression.z, t.crest), PackUnorm4(float4(t.foam, t.breakStage, t.bore, t.aeration)), PackUnorm4(float4(t.impact, t.clearFace, 0.0, 0.0)));
}
