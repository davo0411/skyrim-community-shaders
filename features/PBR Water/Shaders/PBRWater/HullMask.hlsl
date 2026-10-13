// ============================================================================
// PBR Water - the maps that keep the water out of a hull (HullMaskAtlas), built once per hull.
//
// Vertex and pixel shader: every render piece of the hull is drawn with an orthographic projection of the hull's
// frame (x along the hull, y across it, z up). The engine's vertex and index buffers are copied into raw buffers and
// read by index, so any vertex layout reads the same way. Two targets keep per texel the lowest (MIN blend) and the
// highest (MAX blend) hull surface along the vertical through the texel centre. Each triangle also draws its edges
// as quads a texel wide into the highest surface only: a vertical wall projects to a line that may cover no texel
// centre at all, yet its top must close the basin it walls in.
//
// Compute shaders:
//   Prepare : the highest surface, widened by a texel, so the wall tops reach the hull texels next to them.
//   Flood   : the height the water must reach before it can flow into each texel, by morphological reconstruction
//             from the open water around the hull (Vincent 1993): level = max(top, min(level, neighbours' levels)).
//             Outside the hull the level is -inf; inside it starts at the top of the hull and sinks until it is the
//             lowest rim on the way out: the lowest point of the gunwale for the inside of a boat, the deck of a raft,
//             the top of a solid part. Each dispatch sweeps every row (or column) in one direction, carrying the level
//             along it; a few rounds of the four directions converge for any hull-like shape, and a level that has
//             not quite sunk yet only keeps a little more of the hull dry.
//   Pack    : lowest surface and level into the hull's atlas slice.
//
// The water shader leaves a point dry when it lies above the lowest surface and below the level
// (PBRWater::InsideDryHull).
//
// Reference: L. Vincent, "Morphological grayscale reconstruction in image analysis" (IEEE TIP 1993)
// ============================================================================

cbuffer HullMaskCB : register(b0)
{
	float4 MeshToHull[3];  // rows of the affine transform from mesh space to the hull frame
	float4 Projection;     // xy hull frame position of the maps' corner (units), zw texels per unit
	uint VertexStride;     // bytes between vertices
	uint PositionOffset;   // bytes from a vertex to its position
	uint HalfPositions;    // 1: the position is stored as half floats, 0: full precision
	uint Size;             // texels per side of the maps
	float Ceiling;         // top of the hull (hull frame z, units)
	uint Direction;        // Flood sweep: 0 +x, 1 -x, 2 +y, 3 -y
	uint Slice;            // atlas slice Pack writes
	uint Pad;
};

static const float Far = 1e30;  // no surface: Bottom is cleared to Far, Top to -Far

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float Height : TEXCOORD0;                // hull frame z (units)
	nointerpolation float Edge : TEXCOORD1;  // 1 on the edge quads: they only raise the highest surface
};

#if defined(VSHADER)
ByteAddressBuffer Vertices : register(t0);
ByteAddressBuffer Indices : register(t1);  // 16-bit triangle list

static const uint VerticesPerTriangle = 21;  // the triangle, then a quad (two triangles) per edge
static const uint QuadCorner[6] = { 0, 1, 2, 2, 1, 3 };

/// Hull frame position of a triangle corner (face * 3 + corner): xy in texels, z in units.
float3 CornerPosition(uint corner)
{
	uint word = Indices.Load((corner * 2) & ~3u);
	uint index = (corner & 1) != 0 ? word >> 16 : word & 0xFFFF;
	uint address = index * VertexStride + PositionOffset;
	float3 position;
	[branch] if (HalfPositions != 0)
	{
		uint2 packed = Vertices.Load2(address);
		position = float3(f16tof32(packed.x), f16tof32(packed.x >> 16), f16tof32(packed.y));
	}
	else
	{
		position = asfloat(Vertices.Load3(address));
	}
	float4 p = float4(position, 1.0);
	float3 hull = float3(dot(MeshToHull[0], p), dot(MeshToHull[1], p), dot(MeshToHull[2], p));
	return float3((hull.xy - Projection.xy) * Projection.zw, hull.z);
}

VS_OUTPUT main(uint id : SV_VertexID)
{
	uint face = id / VerticesPerTriangle;
	uint k = id % VerticesPerTriangle;
	float3 p;
	float edge = 0.0;
	if (k < 3) {
		p = CornerPosition(face * 3 + k);
	} else {
		// Edge e from corner e to the next, widened half a texel to either side and past either end.
		uint e = (k - 3) / 6;
		uint c = QuadCorner[(k - 3) % 6];
		float3 a = CornerPosition(face * 3 + e);
		float3 b = CornerPosition(face * 3 + (e + 1) % 3);
		float2 d = b.xy - a.xy;
		float length2 = dot(d, d);
		float2 along = length2 > 1e-8 ? d * rsqrt(length2) : float2(1.0, 0.0);
		float2 across = float2(-along.y, along.x);
		p = (c & 1) != 0 ? b : a;
		p.xy += 0.5 * (((c & 1) != 0 ? along : -along) + ((c & 2) != 0 ? across : -across));
		edge = 1.0;
	}

	VS_OUTPUT o;
	float2 clip = p.xy * (2.0 / Size);
	o.Position = float4(clip.x - 1.0, 1.0 - clip.y, 0.5, 1.0);
	o.Height = p.z;
	o.Edge = edge;
	return o;
}
#endif

#if defined(PSHADER)
struct PS_OUTPUT
{
	float Bottom : SV_Target0;  // MIN blend
	float Top : SV_Target1;     // MAX blend
};

PS_OUTPUT main(VS_OUTPUT input)
{
	PS_OUTPUT o;
	o.Bottom = input.Edge > 0.5 ? Far : input.Height;
	o.Top = input.Height;
	return o;
}
#endif

#if defined(COMPUTESHADER)
Texture2D<float> Bottom : register(t0);         // lowest hull surface per texel, Far: no hull
Texture2D<float> Top : register(t1);            // highest hull surface per texel, -Far: no hull
RWTexture2D<float> Image : register(u0);        // what holds the water back per texel, -Far outside the hull
RWTexture2D<float> Level : register(u1);        // height the water must reach to flow in, -Far outside the hull
RWTexture2DArray<float2> Atlas : register(u2);  // x lowest hull surface, y level

[numthreads(8, 8, 1)] void Prepare(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Size))
		return;
	int last = (int)Size - 1;
	float top = -Far;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
			top = max(top, Top[clamp(int2(id.xy) + int2(x, y), 0, last)]);
	}
	bool hull = Bottom[id.xy] < Far;
	Image[id.xy] = hull ? top : -Far;
	Level[id.xy] = hull ? max(Ceiling, top) : -Far;
}

/// Level of a texel; beyond the maps' edge lies open water.
float LevelAt(int2 p)
{
	return all(p >= 0) && all(p < (int)Size) ? Level[p] : -Far;
}

// One thread per row (or column). The rows next to it are being swept at the same time: whatever value is read
// there, old or new, never lies below the final level, so the levels still only sink towards it.
[numthreads(64, 1, 1)] void Flood(uint3 id : SV_DispatchThreadID) {
	if (id.x >= Size)
		return;
	bool horizontal = Direction < 2;
	int step = (Direction & 1) != 0 ? -1 : 1;
	int first = step > 0 ? 0 : (int)Size - 1;
	int2 along = horizontal ? int2(step, 0) : int2(0, step);
	int2 side = horizontal ? int2(0, 1) : int2(1, 0);
	int2 p = horizontal ? int2(first, id.x) : int2(id.x, first);
	float previous = -Far;
	[loop] for (uint i = 0; i < Size; i++)
	{
		float level = min(min(Level[p], previous), min(LevelAt(p - side), LevelAt(p + side)));
		level = max(level, Image[p]);
		Level[p] = level;
		previous = level;
		p += along;
	}
}

[numthreads(8, 8, 1)] void Pack(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= Size))
		return;
	Atlas[uint3(id.xy, Slice)] = float2(Bottom[id.xy], Level[id.xy]);
}
#endif
