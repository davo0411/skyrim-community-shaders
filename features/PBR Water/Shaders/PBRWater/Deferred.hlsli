#ifndef __PBR_WATER_DEFERRED_HLSLI__
#define __PBR_WATER_DEFERRED_HLSLI__

// ============================================================================
// PBR Water - the deferred surface.
//
// The surface and point light passes shade a pixel once for every triangle of its 2x2 quad: with the tessellator's
// small triangles each water pixel ran the whole surface program about 1.75 times. The stencil pass draws the same
// triangles first; for the tessellated meshes it also writes, per pixel, which mesh is there and what the vertex program
// reads of the waves (Pack). DeferredTilesCS sorts the screen's tiles by the meshes in them; the surface and point light
// passes then draw each mesh's tiles (TileVertex), and every pixel the stencil pass left to that mesh runs the vertex
// program and the shading once, from that record (Resolve). How finely the mesh is tessellated no longer costs pixel
// shading.
//
// The record is what the vertex program cannot rebuild: the waves' displacement, the surface noise (procedural beyond its
// bake), the vertex alpha and, for meshes whose texture coordinates are not one plane over the mesh, those. The displaced
// position comes from the depth buffer, the undisplaced one from that minus the displacement. Writing it is bandwidth, so it is small: 14 bytes, 22 with texture coordinates.
//
// A record is this frame's where the stencil pass's depth is still what the depth buffer holds: nothing draws depth
// between the two passes, and a nearer untessellated water draw, which writes no record, leaves another depth there. The
// tag target is cleared every frame (PBRWater::DrawRecordedStencil) and carries 16 bits of the depth to compare.
// Needs Water.hlsl's VS_INPUT, VS_OUTPUT, WaterVertexWith and the vertex constants (b8-b10 in the pixel stage).
// ============================================================================

#include "PBRWater/DeferredCommon.hlsli"
#if defined(PBRW_MATERIAL)
#	include "PBRWater/Material.hlsli"
#endif

namespace PBRWaterDeferred
{
	cbuffer DeferredDraw : register(b13)
	{
		uint4 Tag;        // x this mesh's tag (1..PBRWATER_DEFERRED_MESHES), y the tile list's room for each mesh, z texture coordinates recorded (else UVMapU/V), w the shore train is baked (DeferredShoreCS)
		float4 Viewport;  // xy the viewport's size (pixels)
		float4 UVMapU;    // the mesh's texture coordinate u over its model-space xy: x d/dx, y d/dy, z at the origin
		float4 UVMapV;
		uint4 Mode;  // x the tile list drawn (DeferredCommon.hlsli), y PBRWATER_DEFERRED_FULL or _HALF, z the frame's sample rotation (_HALF)
	};

	/// The half-rate pass's key for the pixel it shaded (DeferredFar.hlsl): 1 + which of its 2x2 pixels, 0 none.
	static float HalfRateKey = 0.0;

	/// The stencil pass's record of a pixel (SV_Target2-4). The surface noise is not in it: Resolve reads the bake
	/// (PBRWater::SurfaceNoise) at the undisplaced position.
	struct Record
	{
		uint tag;      // tag | vertex alpha << 8 | depth (24-bit) & 0xFFFF << 16
		float4 waves;  // xyz the waves' displacement (units), w the waves without the surf (ShoreState.z)
		float2 uv;     // the mesh's texture coordinate (where Tag.z)
	};

	uint DepthBits(float depth)
	{
		return (uint)round(saturate(depth) * 16777215.0) & 0xFFFF;
	}

	/**
	 * @param depth  the pixel's depth (SV_Position.z)
	 */
	Record Pack(float depth, float3 current, float wavesAlone, float2 uv, float alpha)
	{
		Record r;
		r.tag = Tag.x | ((uint)round(saturate(alpha) * 255.0) << 8) | (DepthBits(depth) << 16);
		r.waves = float4(current, wavesAlone);
		r.uv = uv;
		return r;
	}

#if defined(VSHADER)
	StructuredBuffer<uint> Tiles : register(t101);  // DeferredTilesCS: each mesh's tiles

	/// One corner of one of the drawn list's tiles (instance), as two triangles (vertices 0-5). The half-rate pass draws
	/// them over the half-size target, whose Viewport is half the screen's.
	float4 TileVertex(uint vertexID, uint instanceID)
	{
		uint packed = Tiles[Mode.x * Tag.y + instanceID];
		static const float2 Corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
		float tile = Mode.y == PBRWATER_DEFERRED_HALF ? PBRWATER_DEFERRED_TILE / 2 : PBRWATER_DEFERRED_TILE;
		float2 pixel = (float2(packed & 0xFFFF, packed >> 16) + Corners[vertexID % 6]) * tile;
		float2 ndc = min(pixel / Viewport.xy, 1.0) * 2.0 - 1.0;
		return float4(ndc.x, -ndc.y, 0.0, 1.0);
	}
#endif

#if defined(PBRW_DEFERRED)
	Texture2D<uint> Tags : register(t38);
	Texture2D<float4> Waves : register(t39);
	// t40: free (the record's surface noise, now read from the bake)
	Texture2D<float2> TexCoords : register(t41);
	Texture2D<float> SceneDepth : register(t42);
#	if defined(PBRW_MATERIAL)
	// The surface's material (SurfaceMaterialCS.hlsl), at the pixel Resolve took its record from.
	Texture2D<uint4> MaterialA : register(t75);
	Texture2D<uint2> MaterialB : register(t107);
	static int2 MaterialPixel = 0;

	/**
	 * The material at the pixel Resolve took its record from.
	 * @param later any value computed just before the words are wanted (compared with NaN, so the load can only follow it):
	 *              the compiler would otherwise load and unpack every field early and keep them in registers through the lighting
	 */
	PBRWaterMaterial::Words ReadMaterial(float later)
	{
		int2 pixel = MaterialPixel + (later != later ? 1 : 0);
		PBRWaterMaterial::Words w;
		w.a = MaterialA.Load(int3(pixel, 0));
		w.b = MaterialB.Load(int3(pixel, 0));
		return w;
	}
#	endif

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
		// The half-rate pass: this pixel of the half-size target stands for 2x2 pixels of the screen and shades one of them
		// that is this mesh's, starting from a different one each frame so the temporal filter gathers all four. The
		// screen-space reads then use that pixel's position, and the screen derivatives span two pixels: the footprint the
		// coarser sampling has.
		[branch] if (Mode.y == PBRWATER_DEFERRED_HALF)
		{
			uint width, height;
			Tags.GetDimensions(width, height);
			int2 corner = px * 2;
			int2 chosen = min(corner, int2(width, height) - 1);
			HalfRateKey = 0.0;
			[unroll] for (uint i = 0; i < 4; i++)
			{
				uint index = (i + Mode.z) & 3;
				int2 candidate = min(corner + int2(index & 1, index >> 1), int2(width, height) - 1);
				Pixel c = Read(candidate);
				if (HalfRateKey == 0.0 && c.water && c.tag == Tag.x) {
					chosen = candidate;
					HalfRateKey = 1.0 + index;
				}
			}
			px = chosen;
			input.HPosition.xy = px + 0.5;
		}
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
		[branch] if (p.water)
			recordWaves = Waves.Load(int3(source, 0));

#	if defined(PBRW_MATERIAL)
		MaterialPixel = source;
#	endif

		// The displaced surface at this pixel, camera-relative, from its depth.
		float2 screenUV = input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy * VPOSOffset.xy;
		float4 world = mul(FrameBuffer::CameraViewProjInverse, float4(screenUV.x * 2.0 - 1.0, 1.0 - screenUV.y * 2.0, p.depth, 1.0));
		float3 displaced = world.xyz / world.w;
		float3 undisplaced = displaced - recordWaves.xyz;
		// The shore train from DeferredShoreCS (Tag.w: it ran this frame), read where the pixel program evaluates it.
		PBRWater::BakedShoreActive = Tag.w != 0 && p.water;
		PBRWater::BakedShorePixel = px;
		PBRWater::BakedShoreDistance = max(length(displaced), 1.0);

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
		waves.river = 0.0;  // river meshes are never recorded (PBRWater::RestoreDraw)
		VS_OUTPUT o = WaterVertexWith(v, waves);
		o.SurfaceNoise = p.water ? PBRWater::SurfaceNoise(undisplaced.xy + FrameBuffer::CameraPosAdjust.xy) : float2(0.0, 1.0);
		o.HPosition = float4(input.HPosition.xy, p.depth, input.HPosition.w);
		input = o;
		// A half-rate pixel whose 2x2 holds none of this mesh's water writes nothing (the upsampling reads its key 0).
		return owned && (Mode.y != PBRWATER_DEFERRED_HALF || HalfRateKey > 0.0);
	}
#endif
}

#endif  // __PBR_WATER_DEFERRED_HLSLI__
