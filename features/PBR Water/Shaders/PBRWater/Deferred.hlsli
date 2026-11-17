#ifndef __PBR_WATER_DEFERRED_HLSLI__
#define __PBR_WATER_DEFERRED_HLSLI__

// ============================================================================
// PBR Water - the deferred surface.
//
// The surface and point light passes shade a pixel once for every triangle of its 2x2 quad: with the compute tessellator's
// small triangles each water pixel ran the whole surface program about 1.75 times. The stencil pass draws the same
// triangles first; for the tessellated meshes it also writes, per pixel, which mesh is there and what the vertex program
// reads of the waves (Pack). DeferredTilesCS sorts the screen's tiles by the meshes in them; the surface and point light
// passes then draw each mesh's tiles (TileVertex), and every pixel the stencil pass left to that mesh runs the vertex
// program and the shading once, from that record (Resolve). How finely the mesh is tessellated no longer costs pixel
// shading.
//
// The record is what the vertex program cannot rebuild: the waves' displacement, the surface noise (procedural beyond its
// bake), the vertex alpha and, for meshes whose texture coordinates are not one plane over the mesh, those. The displaced
// position comes from the depth buffer, the undisplaced one from that minus the displacement, the river current from the
// flowmap there. Writing it is bandwidth, so it is small: 14 bytes, 22 with texture coordinates.
//
// A record is this frame's where the stencil pass's depth is still what the depth buffer holds: nothing draws depth
// between the two passes, and a nearer untessellated water draw, which writes no record, leaves another depth there. The
// tag target is cleared every frame (PBRWater::DrawRecordedStencil) and carries 16 bits of the depth to compare.
// Needs Water.hlsl's VS_INPUT, VS_OUTPUT, WaterVertexWith and the vertex constants (b8-b10 in the pixel stage).
// ============================================================================

#include "PBRWater/DeferredCommon.hlsli"

namespace PBRWaterDeferred
{
	cbuffer DeferredDraw : register(b13)
	{
		uint4 Tag;        // x this mesh's tag (1..PBRWATER_DEFERRED_MESHES), y the tile list's room for each mesh, z texture coordinates recorded (else UVMapU/V)
		float4 Viewport;  // xy the viewport's size (pixels)
		float4 UVMapU;    // the mesh's texture coordinate u over its model-space xy: x d/dx, y d/dy, z at the origin
		float4 UVMapV;
	};

	/// The stencil pass's record of a pixel (SV_Target2-5).
	struct Record
	{
		uint tag;      // tag | vertex alpha << 8 | depth (24-bit) & 0xFFFF << 16
		float4 waves;  // xyz the waves' displacement (units), w the waves without the surf (ShoreState.z)
		float2 noise;  // SurfaceNoise: x sediment patches, y (wind gust factor - 0.35) / 1.3
		float2 uv;     // the mesh's texture coordinate (where Tag.z)
	};

	uint DepthBits(float depth)
	{
		return (uint)round(saturate(depth) * 16777215.0) & 0xFFFF;
	}

	/**
	 * @param depth  the pixel's depth (SV_Position.z)
	 * @param noise  SurfaceNoise at the undisplaced position
	 */
	Record Pack(float depth, float3 current, float wavesAlone, float2 noise, float2 uv, float alpha)
	{
		Record r;
		r.tag = Tag.x | ((uint)round(saturate(alpha) * 255.0) << 8) | (DepthBits(depth) << 16);
		r.waves = float4(current, wavesAlone);
		r.noise = float2(noise.x, (noise.y - 0.35) / 1.3);
		r.uv = uv;
		return r;
	}

#if defined(VSHADER)
	StructuredBuffer<uint> Tiles : register(t101);  // DeferredTilesCS: each mesh's tiles

	/// One corner of one of this mesh's tiles (instance), as two triangles (vertices 0-5).
	float4 TileVertex(uint vertexID, uint instanceID)
	{
		uint packed = Tiles[(Tag.x - 1) * Tag.y + instanceID];
		static const float2 Corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
		float2 pixel = (float2(packed & 0xFFFF, packed >> 16) + Corners[vertexID % 6]) * PBRWATER_DEFERRED_TILE;
		float2 ndc = min(pixel / Viewport.xy, 1.0) * 2.0 - 1.0;
		return float4(ndc.x, -ndc.y, 0.0, 1.0);
	}
#endif

#if defined(PBRW_DEFERRED)
	Texture2D<uint> Tags : register(t38);
	Texture2D<float4> Waves : register(t39);
	Texture2D<float2> Noise : register(t40);
	Texture2D<float2> TexCoords : register(t41);
	Texture2D<float> SceneDepth : register(t42);
	Texture2D<float4> WorldFlowMap : register(t43);  // the vertex stage's flowmap (VertexWaves.hlsli)

	/// The river current at a camera-relative undisplaced position: GetWaveFlowDamping, per pixel.
	float FlowAt(float3 positionWS, out float2 velocity)
	{
		velocity = 0.0;
		if (PBRWater::Flow0.x == 0.0)
			return 1.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float4 flow = WorldFlowMap.SampleLevel(PBRWater::LinearClampSampler, absXY * PBRWater::Flow0.xz + PBRWater::Flow0.yw, 0);
		velocity = PBRWater::FlowVelocity(flow);
		return 1.0 - PBRWater::Foam1.w * saturate(flow.z * flow.w);
	}

	/// Whether a pixel holds a record, and its tag's fields: the record's waves and noise are loaded once a pixel is chosen.
	struct Pixel
	{
		bool water;  // the stencil pass's tessellated water is still the nearest here
		uint tag;
		float depth;
		float alpha;
	};

	Pixel Read(int2 px)
	{
		Pixel p;
		uint tag = Tags.Load(int3(px, 0));
		p.depth = SceneDepth.Load(int3(px, 0));
		p.tag = tag & 0xFF;
		uint bits = (DepthBits(p.depth) - (tag >> 16) + 1) & 0xFFFF;
		p.water = p.tag != 0 && bits <= 2;
		p.alpha = ((tag >> 8) & 0xFF) / 255.0;
		return p;
	}

	/**
	 * Rebuilds the vertex program's output at this pixel from the stencil pass's record.
	 * @param input the tile's interpolants (only HPosition is set), replaced with the mesh's at this pixel
	 * @return whether the pixel is this mesh's to shade. The others still run as helpers for their quad's derivatives,
	 *         on a neighbour's record where they have none.
	 */
	bool Resolve(inout VS_OUTPUT input)
	{
		int2 px = int2(input.HPosition.xy);
		Pixel p = Read(px);
		bool owned = p.water && p.tag == Tag.x;
		int2 source = px;
		[branch] if (!p.water)
		{
			Pixel n = Read(px ^ int2(1, 0));
			source = px ^ int2(1, 0);
			[branch] if (!n.water)
			{
				n = Read(px ^ int2(0, 1));
				source = px ^ int2(0, 1);
			}
			[branch] if (!n.water)
			{
				n = Read(px ^ int2(1, 1));
				source = px ^ int2(1, 1);
			}
			if (n.water)
				p = n;
			else {
				p.alpha = 1.0;
				source = px;
			}
		}
		float4 recordWaves = 0.0;
		float2 recordNoise = float2(0.0, 0.5);
		[branch] if (p.water)
		{
			recordWaves = Waves.Load(int3(source, 0));
			recordNoise = Noise.Load(int3(source, 0));
		}

		// The displaced surface at this pixel, camera-relative, from its depth.
		float2 screenUV = input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy * VPOSOffset.xy;
		float4 world = mul(FrameBuffer::CameraViewProjInverse, float4(screenUV.x * 2.0 - 1.0, 1.0 - screenUV.y * 2.0, p.depth, 1.0));
		float3 displaced = world.xyz / world.w;
		float3 undisplaced = displaced - recordWaves.xyz;

		VS_INPUT v = (VS_INPUT)0;
		float3 worldOrigin = float3(World[0].w, World[1].w, World[2].w);
		float3 model = mul(InverseLinear(World), undisplaced - worldOrigin);
		v.Position = float4(model, 1.0);
#	if defined(NORMAL_TEXCOORD)
		[branch] if (Tag.z != 0)
			v.TexCoord0 = TexCoords.Load(int3(source, 0));
		else v.TexCoord0 = float2(dot(UVMapU.xy, model.xy) + UVMapU.z, dot(UVMapV.xy, model.xy) + UVMapV.z);
#	endif
#	if defined(VC)
		v.Color = float4(1.0, 1.0, 1.0, p.alpha);
#	endif
		PBRWater::VertexWaves waves;
		waves.surface.current = recordWaves.xyz;
		waves.surface.previous = 0.0;
		waves.surface.shore = float4(0.0, 0.0, recordWaves.w, undisplaced.z);
		waves.flowDamping = FlowAt(undisplaced, waves.flowVelocity);
		VS_OUTPUT o = WaterVertexWith(v, waves);
		o.SurfaceNoise = float2(recordNoise.x, 0.35 + 1.3 * recordNoise.y);
		o.HPosition = float4(input.HPosition.xy, p.depth, input.HPosition.w);
		input = o;
		return owned;
	}
#endif
}

#endif  // __PBR_WATER_DEFERRED_HLSLI__
