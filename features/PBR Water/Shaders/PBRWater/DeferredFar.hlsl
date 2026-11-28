// ============================================================================ PBR Water - the distant water's
// upsampling (PBRWater::DrawDeferredSurface).
//
// Beyond a distance (DeferredTilesCS) the water's detail is far finer than a pixel and the surface program filters it
// down anyway, so those tiles are shaded at a quarter of the pixels, one of every 2x2 into a half-size target
// (Deferred.hlsli, Mode.y = PBRWATER_DEFERRED_HALF). This pass draws the grid's distant tiles at full size with the
// surface draw's own blend, depth and stencil state, and gives each pixel the half-size texels around it whose shaded
// pixel is this grid's water at about the same distance, weighted by distance. The shaded pixel of each 2x2 turns every
// frame, so the temporal filter gathers all four.
//
// Compiled twice: VSHADER for the tiles (Deferred.hlsli's TileVertex), PSHADER for the pixels.
// ============================================================================

#include "Common/FrameBuffer.hlsli"
#include "PBRWater/DeferredCommon.hlsli"

cbuffer DeferredDraw : register(b13)  // Deferred.hlsli's
{
	uint4 Tag;  // x this mesh's tag, y the tile list's room for each list
	float4 Viewport;
	float4 UVMapU;
	float4 UVMapV;
	uint4 Mode;  // x the tile list drawn
};

#if defined(VSHADER)
StructuredBuffer<uint> Tiles : register(t101);

float4 main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) :
	SV_Position
{
	uint packed = Tiles[Mode.x * Tag.y + instanceID];
	static const float2 Corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };
	float2 pixel = (float2(packed & 0xFFFF, packed >> 16) + Corners[vertexID % 6]) * PBRWATER_DEFERRED_TILE;
	float2 ndc = min(pixel / Viewport.xy, 1.0) * 2.0 - 1.0;
	return float4(ndc.x, -ndc.y, 0.0, 1.0);
}
#endif

#if defined(PSHADER)
Texture2D<uint> Tags : register(t38);
Texture2D<float> SceneDepth : register(t42);
Texture2D<float4> Shaded : register(t49);  // the half-rate pass: rgb its colour, a 1 + which of its 2x2 pixels (0 none)

/// The tag of the grid the record still holds at `px` (PBRWaterDeferred::Read), 0 if none.
uint OwnerAt(int2 px, out float depth)
{
	uint tag = Tags.Load(int3(px, 0));
	depth = SceneDepth.Load(int3(px, 0));
	return PBRWaterRecordHolds(tag, depth) ? (tag & 0xFF) : 0;
}

/// Camera distance of the point on the screen at pixel `px` with depth `depth` (units).
float DistanceAt(int2 px, float depth, float2 size)
{
	return length(PBRWaterUnproject(px + 0.5, depth, size));
}

float4 main(float4 position : SV_Position) :
	SV_Target0
{
	int2 px = int2(position.xy);
	uint width, height;
	Tags.GetDimensions(width, height);
	float2 size = float2(width, height);
	float depth;
	if (OwnerAt(px, depth) != Tag.x)
		discard;
	float distance = DistanceAt(px, depth, size);

	uint halfWidth, halfHeight;
	Shaded.GetDimensions(halfWidth, halfHeight);
	int2 own = px >> 1;
	int2 base = (int2)floor((px + 0.5) * 0.5 - 0.5);
	float3 colour = 0.0;
	float total = 0.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 texel = clamp(base + int2(i & 1, i >> 1), 0, int2(halfWidth, halfHeight) - 1);
		float4 s = Shaded.Load(int3(texel, 0));
		[branch] if (s.w > 0.5)
		{
			uint index = (uint)round(s.w) - 1;
			int2 shaded = min(texel * 2 + int2(index & 1, index >> 1), int2(width, height) - 1);
			float shadedDepth;
			// The same mesh's water at about this pixel's distance: not a crest in front of it or the sea behind.
			[branch] if (OwnerAt(shaded, shadedDepth) == Tag.x)
			{
				float d = DistanceAt(shaded, shadedDepth, size);
				float2 offset = abs(float2(shaded - px));
				float w = saturate(1.0 - offset.x * 0.5) * saturate(1.0 - offset.y * 0.5);
				w *= abs(d - distance) < 0.05 * distance ? 1.0 : 0.0;
				colour += w * s.rgb;
				total += w;
			}
		}
	}
	// The pixel's own 2x2 always holds a sample of this mesh (its own water is one of the four the half-rate pass chose from).
	if (total <= 0.0)
		return float4(Shaded.Load(int3(min(own, int2(halfWidth, halfHeight) - 1), 0)).rgb, 0.0);
	return float4(colour / total, 0.0);
}
#endif
