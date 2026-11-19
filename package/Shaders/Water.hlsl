// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#if defined(HORIZON_FIX)
namespace HorizonFix
{
	// Depth (z/w) that water folded back from beyond the far clip plane lands at: eight
	// depth quanta inside the far plane, exactly representable in both D24 and D32F
	// buffers - behind everything the scene rendered, in front of the depth clear.
	static const float FoldedDepth = 1.0 - 8.0 / 16777216.0;

	// Scene depth at or beyond this counts as "nothing rendered behind the water": the
	// clear value, folded far water (FoldedDepth), or an external plugin's depth-clamped
	// far-water backdrop a few quanta inside that. The margin is a few world units at the
	// far plane, where no real surface can render.
	static const float EmptyDepthThreshold = 1.0 - 64.0 / 16777216.0;
}
#endif

#if defined(UNDERWATERMASK)

#	if defined(VSHADER)
struct VS_INPUT
{
	float4 Position : POSITION0;
};
#	endif

struct VS_OUTPUT
{
	float4 Position : SV_POSITION0;
};

#	ifdef VSHADER

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	float z = min(1, 1e-4 * max(0, input.Position.z - 70000)) * 0.5 + input.Position.z;
	vsout.Position = float4(input.Position.xy, z, 1);

	return vsout;
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#	ifdef PSHADER
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	psout.Color = 1;

	return psout;
}
#	endif

#else

#	include "Common/FrameBuffer.hlsli"
#	include "Common/MotionBlur.hlsli"
#	include "Common/Permutation.hlsli"
#	include "Common/Random.hlsli"
#	include "Common/Shading.hlsli"
#	include "Common/ColorManagement.hlsli"
#	include "Common/BRDF.hlsli"
#	include "Common/Game.hlsli"

#	define WATER

#	if defined(SPECULAR) || defined(UNDERWATER)
// WADING is undefined in pixel builds (read from the descriptor): `defined(WADING)` there is always false.
#		define WADING_RT
#		if defined(PSHADER)
#			undef WADING
#		endif
#	endif
// The vertex program's wading permutation: its own define, or in the pixel stage (the deferred surface runs the vertex
// program there) the descriptor's flag.
#	if defined(WADING)
#		define PBRW_VS_WADING true
#	elif defined(PSHADER) && defined(WADING_RT)
#		define PBRW_VS_WADING ((Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Wading) != 0)
#	else
#		define PBRW_VS_WADING false
#	endif

#	include "Common/SharedData.hlsli"

// PBR Water's deferred surface (PBRWater::RestoreDraw): the tessellated water's surface and point light passes draw a
// screen rectangle around each mesh instead of its triangles, and every pixel the stencil pass left to that mesh runs the
// vertex program and the shading once, from what the stencil pass wrote there (PBRWater/Deferred.hlsli). Small triangles
// no longer shade their pixels several times over (2x2 quads along every edge).
#	if defined(PBR_WATER) && defined(SPECULAR) && defined(PSHADER)
#		define PBRW_DEFERRED
#	endif

#	if defined(VSHADER) || defined(PBRW_DEFERRED)
struct VS_INPUT
{
#		if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position : POSITION0;
#			if defined(NORMAL_TEXCOORD)
	float2 TexCoord0 : TEXCOORD0;
#			endif
#			if defined(VC)
	float4 Color : COLOR0;
#			endif
#		endif

#		if defined(LOD)
	float4 Position : POSITION0;
#			if defined(VC)
	float4 Color : COLOR0;
#			endif
#		endif
};
#	endif

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition : SV_POSITION0;
#		if !defined(UNIFIED_WATER)
	float4 FogParam : COLOR0;
#		endif
	float4 WPosition : TEXCOORD0;
	float4 TexCoord1 : TEXCOORD1;
	float4 TexCoord2 : TEXCOORD2;
#		if defined(WADING_RT) || defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3 : TEXCOORD3;
#		endif
#		if defined(FLOWMAP)
	nointerpolation float2 TexCoord4 : TEXCOORD4;
#		endif
#		if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition : TEXCOORD5;
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition : SV_POSITION0;
	float4 FogParam : COLOR0;
	float4 WPosition : TEXCOORD0;
	float4 TexCoord1 : TEXCOORD1;
	float4 TexCoord2 : TEXCOORD2;
	float4 MPosition : TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition : SV_POSITION0;
	float4 FogParam : COLOR0;
	float4 WPosition : TEXCOORD0;
	float4 TexCoord1 : TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition : SV_POSITION0;
	float4 WorldPosition : POSITION1;
	float4 PreviousWorldPosition : POSITION2;
#		if defined(PBR_WATER)
	// What the deferred surface reads back per pixel (PBRWater/Deferred.hlsli): the tessellated mesh's vertices only.
	float3 DeferredWave : TEXCOORD9;    // the waves' displacement (units)
	float2 DeferredUV : TEXCOORD10;     // the mesh's texture coordinate
	float DeferredAlpha : TEXCOORD11;   // the mesh's vertex colour alpha
	float2 DeferredNoise : TEXCOORD13;  // SurfaceNoise
#		endif
#	endif

	float4 NormalsScale : TEXCOORD8;

#	if defined(PBR_WATER) && !defined(STENCIL)
	float4 WaveParam : TEXCOORD9;      // xyz undisplaced camera-relative position, w wave damping (flow)
	float2 SurfaceNoise : TEXCOORD10;  // x sediment patches (TurbidityPatches), y wind gust factor (WindGust)
	float3 Barycentric : TEXCOORD11;
	float2 FlowVelocity : TEXCOORD13;  // the river current (units/s, PBRWater::FlowVelocity), 0 on still water
#	endif
#	if defined(PBR_WATER)
	float4 ShoreState : TEXCOORD12;  // x share of the surf simulation, y unused, z the waves without it (units above the still water), w still water level (camera-relative z; DisplaceSurface)
#	endif
};

#	if defined(PBR_WATER)
#		include "PBRWater/PBRWater.hlsli"
// Draws that may be tessellated (BisectionTessellator's mesh). LOD water never is: it is far away and flat.
#		if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE) || defined(STENCIL)
#			define PBRW_TESSELLATION
#		endif
#		define PBR_WATER_SPACING PBRWater::Draw0.x
// The stencil and colour passes must rasterise identical depth: forbid reassociation.
#		define PBRW_PRECISE precise
#	else
#		define PBR_WATER_SPACING 0.0
#		define PBRW_PRECISE
#	endif

#	if defined(VSHADER) || defined(PBRW_DEFERRED)

// The deferred surface runs the vertex program per pixel: the pixel stage gets the draw's vertex constants as well
// (PBRWater::RestoreDraw binds them at b8-b10), under names of their own beside the pixel stage's.
#		if defined(VSHADER)
#			define PBRW_VS_CBUFFER(name, vsSlot, psSlot) cbuffer name : register(vsSlot)
#		else
#			define PBRW_VS_CBUFFER(name, vsSlot, psSlot) cbuffer VS##name : register(psSlot)
#		endif

PBRW_VS_CBUFFER(PerTechnique, b0, b8)
{
	float4 QPosAdjust : packoffset(c0);
};

PBRW_VS_CBUFFER(PerMaterial, b1, b9)
{
	float4 VSFogParam : packoffset(c0);
	float4 VSFogNearColor : packoffset(c1);
	float4 VSFogFarColor : packoffset(c2);
	float4 NormalsScroll0 : packoffset(c3);
	float4 NormalsScroll1 : packoffset(c4);
	float4 NormalsScale : packoffset(c5);
};

PBRW_VS_CBUFFER(PerGeometry, b2, b10)
{
	row_major float4x4 World : packoffset(c0);
	row_major float4x4 PreviousWorld : packoffset(c4);
	row_major float4x4 WorldViewProj : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
};

#		if defined(PBR_WATER)
#			include "PBRWater/VertexWaves.hlsli"

/// Inverse of the linear (rotation/scale) part of a world matrix: maps world offsets to model space.
float3x3 InverseLinear(float4x4 m)
{
	float3x3 a = (float3x3)m;
	float3 c0 = cross(a[1], a[2]);
	float3 c1 = cross(a[2], a[0]);
	float3 c2 = cross(a[0], a[1]);
	float det = dot(a[0], c0);
	return transpose(float3x3(c0, c1, c2)) / det;
}
#		endif

/**
 * The water vertex program after the waves: everything but their evaluation. Runs with the waves just evaluated
 * (WaterVertex) or as the tessellator evaluated them for this frame (CachedWaterVertex), so both produce the
 * same attributes.
 */
#		if defined(PBR_WATER)
VS_OUTPUT WaterVertexWith(VS_INPUT input, PBRWater::VertexWaves waves)
#		else
VS_OUTPUT WaterVertexWith(VS_INPUT input)
#		endif
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	vsout.NormalsScale = NormalsScale;

	PBRW_PRECISE float4 inputPosition = float4(input.Position.xyz, 1.0);
	PBRW_PRECISE float4 previousInputPosition = inputPosition;
	PBRW_PRECISE float4 scrollWorldPos = mul(World, inputPosition);

#		if defined(PBR_WATER)
	{
		float3x3 worldToModel = InverseLinear(World);
		previousInputPosition.xyz += mul(worldToModel, waves.surface.previous);
		inputPosition.xyz += mul(worldToModel, waves.surface.current);
		vsout.ShoreState = waves.surface.shore;
#			if !defined(STENCIL)
		vsout.WaveParam = float4(scrollWorldPos.xyz, waves.flowDamping);
		vsout.FlowVelocity = waves.flowVelocity;
#				if defined(PSHADER)
		// The deferred surface's vertex program: the stencil pass recorded the noise (Deferred.hlsli).
		vsout.SurfaceNoise = 0.0;
#				else
		float2 absXY = scrollWorldPos.xy + FrameBuffer::CameraPosAdjust.xy;
		vsout.SurfaceNoise = PBRWater::SurfaceNoise(absXY);
#				endif
		vsout.Barycentric = 1.0 / 3.0;
#			endif
	}
#		endif

	PBRW_PRECISE float4 worldPos = mul(World, inputPosition);
	PBRW_PRECISE float4 worldViewPos = mul(WorldViewProj, inputPosition);

	float heightMult = min((1.0 / 10000.0) * max(worldViewPos.z - 70000, 0), 1);

	vsout.HPosition.xy = worldViewPos.xy;
	vsout.HPosition.z = heightMult * 0.5 + worldViewPos.z;
	vsout.HPosition.w = worldViewPos.w;

#		if defined(HORIZON_FIX)
	vsout.HPosition.z = min(vsout.HPosition.z, vsout.HPosition.w * HorizonFix::FoldedDepth);
#		endif

#		if defined(STENCIL)
	vsout.WorldPosition = worldPos;
	vsout.PreviousWorldPosition = mul(PreviousWorld, previousInputPosition);
#			if defined(PBR_WATER)
	vsout.DeferredWave = waves.surface.current;
	vsout.DeferredUV = 0.0;
	vsout.DeferredAlpha = 1.0;
	vsout.DeferredNoise = PBRWater::SurfaceNoise(scrollWorldPos.xy + FrameBuffer::CameraPosAdjust.xy);
#			endif
#		else

#			if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(VSFogNearColor.xyz), ColorManagement::SRGBToWorking(VSFogFarColor.xyz), fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#			endif

	vsout.WPosition.xyz = worldPos.xyz;
	vsout.WPosition.w = length(worldPos.xyz);

#			if defined(LOD)
	float4 posAdjust =
		ObjectUV.x ? 0.0 : (QPosAdjust.xyxy + scrollWorldPos.xyxy) / NormalsScale.xxyy;

	vsout.TexCoord1.xyzw = NormalsScroll0 + posAdjust;
#			else
#				if !defined(SPECULAR) || (NUM_SPECULAR_LIGHTS == 0)
	vsout.MPosition.xyzw = inputPosition.xyzw;
#				endif

	float2 posAdjust = scrollWorldPos.xy + QPosAdjust.xy;

	float2 scrollAdjust1 = posAdjust / NormalsScale.xx;
	float2 scrollAdjust2 = posAdjust / NormalsScale.yy;
	float2 scrollAdjust3 = posAdjust / NormalsScale.zz;

#				if defined(UNIFIED_WATER) && defined(NORMAL_TEXCOORD)
	float2 cellShift = float2(floor(ObjectUV.z * 0.5), floor((ObjectUV.z - 1.0) * 0.5));
	float2 scaledUV = input.TexCoord0.xy * ObjectUV.z - cellShift;
#				endif

#				if !(defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS) || defined(DEPTH) || NUM_SPECULAR_LIGHTS == 0))
#					if defined(NORMAL_TEXCOORD)
	float3 normalsScale = 0.001 * NormalsScale.xyz;
	if (ObjectUV.x) {
		scrollAdjust1 = input.TexCoord0.xy / normalsScale.xx;
		scrollAdjust2 = input.TexCoord0.xy / normalsScale.yy;
		scrollAdjust3 = input.TexCoord0.xy / normalsScale.zz;
	}
#					else
	if (ObjectUV.x) {
		scrollAdjust1 = 0.0;
		scrollAdjust2 = 0.0;
		scrollAdjust3 = 0.0;
	}
#					endif
#				endif

	vsout.TexCoord1 = 0.0;
	vsout.TexCoord2 = 0.0;
#				if defined(WADING_RT)
	vsout.TexCoord3 = 0.0;
#				endif
#				if defined(FLOWMAP)
#					if !(((defined(SPECULAR) || NUM_SPECULAR_LIGHTS == 0) || (defined(UNDERWATER) && defined(REFRACTIONS))) && !defined(NORMAL_TEXCOORD))
#						if defined(BLEND_NORMALS)
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
#						else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = 0.0;
	vsout.TexCoord2.xy = 0.0;
#						endif
#					endif
#					if !defined(NORMAL_TEXCOORD)
	vsout.TexCoord3 = 0.0;
#					else
	if (PBRW_VS_WADING) {
#						if defined(UNIFIED_WATER)
		float2 wadingUV = (input.TexCoord0.xy - 0.5f) * 0.5f;
		vsout.TexCoord2.zw = (CellTexCoordOffset.xy + wadingUV) / ObjectUV.xy;
		vsout.TexCoord3.xy = CellTexCoordOffset.zw + wadingUV;
#						else
		vsout.TexCoord2.zw = ((-0.5 + input.TexCoord0.xy) * 0.1 + CellTexCoordOffset.xy) +
		                     float2(CellTexCoordOffset.z, -CellTexCoordOffset.w + ObjectUV.x) / ObjectUV.xx;
		vsout.TexCoord3.xy = -0.25 + (input.TexCoord0.xy * 0.5 + ObjectUV.yz);
#						endif
		vsout.TexCoord3.zw = input.TexCoord0.xy;
	}
#						if (defined(REFRACTIONS) || NUM_SPECULAR_LIGHTS == 0 || defined(BLEND_NORMALS))
	else {
#							if defined(UNIFIED_WATER)
		float2 dims = float2(ObjectUV.x, ObjectUV.y);
		vsout.TexCoord2.zw = (CellTexCoordOffset.xy + scaledUV) / dims;
		vsout.TexCoord3.xy = CellTexCoordOffset.zw + scaledUV;
		vsout.TexCoord3.zw = scaledUV;
#							else
		vsout.TexCoord2.zw = (CellTexCoordOffset.xy + input.TexCoord0.xy) / ObjectUV.xx;
		vsout.TexCoord3.xy = CellTexCoordOffset.zw + input.TexCoord0.xy;
		vsout.TexCoord3.zw = input.TexCoord0.xy;
#							endif
	}
#						endif
#					endif
	vsout.TexCoord4 = ObjectUV.xy;
#				else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
	vsout.TexCoord2.z = worldViewPos.w;
	vsout.TexCoord2.w = 0;
#					if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
	const bool alphaDepth = true;
#					else
	const bool alphaDepth = false;
#					endif
#					if defined(WADING_RT) || defined(WADING) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC))
	if (PBRW_VS_WADING || alphaDepth) {
		vsout.TexCoord3 = 0.0;
#						if defined(NORMAL_TEXCOORD)
#							if !defined(BLEND_NORMALS) && !defined(VERTEX_ALPHA_DEPTH)
		vsout.TexCoord3.xy = input.TexCoord0;
#							else
		if (PBRW_VS_WADING)
			vsout.TexCoord3.xy = input.TexCoord0;
#							endif
#						endif
#						if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
		vsout.TexCoord3.z = input.Color.w;
#						endif
	}
#					endif
#				endif
#			endif
#		endif

	return vsout;
}

#		if defined(VSHADER)
/**
 * The complete water vertex program: the waves at this vertex, then the rest (WaterVertexWith).
 * @param spacing local vertex spacing in world units (for wave Nyquist filtering)
 */
VS_OUTPUT WaterVertex(VS_INPUT input, float spacing)
{
#			if defined(PBR_WATER)
	PBRW_PRECISE float4 scrollWorldPos = mul(World, float4(input.Position.xyz, 1.0));
#				if defined(STENCIL)
	return WaterVertexWith(input, PBRWater::EvaluateVertexWaves(scrollWorldPos.xyz, spacing, true));
#				else
	return WaterVertexWith(input, PBRWater::EvaluateVertexWaves(scrollWorldPos.xyz, spacing, false));
#				endif
#			else
	return WaterVertexWith(input);
#			endif
}
#		endif

#	endif  // VSHADER || PBRW_DEFERRED

#	if defined(VSHADER)
#		if defined(PBR_WATER) && defined(PBRW_TESSELLATION)
#			include "PBRWater/ComputeTessellation.hlsli"

// The tessellated water's vertices this frame (BisectionTessellator), each bisector's grid after one another
// (BisectionVertexIndex finds them).
StructuredBuffer<PBRWaterTess::Vertex> TessellatedVertices : register(t109);

/// A vertex the tessellator made and evaluated the waves of this frame: only the cheap rest of the program runs.
VS_OUTPUT CachedWaterVertex(uint vertexID)
{
	PBRWaterTess::Vertex t = TessellatedVertices[vertexID];
	VS_INPUT v = (VS_INPUT)0;
	v.Position = float4(t.position, 1.0);
#			if defined(NORMAL_TEXCOORD)
	v.TexCoord0 = float2(t.texCoordX, t.texCoordY);
#			endif
#			if defined(VC)
	v.Color = float4(f16tof32(t.color.x), f16tof32(t.color.x >> 16), f16tof32(t.color.y), f16tof32(t.color.y >> 16));
#			endif
	PBRWater::VertexWaves waves;
	waves.surface.current = t.current;
	waves.surface.previous = t.previous;
	waves.surface.shore = t.shore;
	waves.flowDamping = t.flowDamping;
	waves.flowVelocity = t.flowVelocity;
	VS_OUTPUT vsout = WaterVertexWith(v, waves);
#			if defined(STENCIL)
	// Whatever the stencil pass's own permutation reads of the mesh: the surface's may read more.
	vsout.DeferredUV = float2(t.texCoordX, t.texCoordY);
	vsout.DeferredAlpha = f16tof32(t.color.y >> 16);
#			endif
	return vsout;
}

#			if defined(SPECULAR)
#				include "PBRWater/Deferred.hlsli"
#			endif
#			include "PBRWater/Bisection.hlsli"

// The bisection tessellator's draw (BisectionTessellator::Draw): the mesh drawn, and where each mesh's bisectors start in
// the draw lists (BisectionCS OffsetsCS).
cbuffer BisectionDraw : register(b11)
{
	uint4 BisectionMesh;  // x the mesh slot
};
ByteAddressBuffer BisectionDrawArgs : register(t108);

/// The bisection tessellator's vertex for this draw's vertex `vertexID` (BisectionTessellator::Draw): each bisector is
/// 3 GridTriangles vertices, the mesh's from its offset in the draw lists on (SV_VertexID starts at 0 whatever the
/// draw's StartVertexLocation), its grid's vertices stored after one another (BisectionCS VertexCS).
uint BisectionVertexIndex(uint vertexID)
{
	static const uint perBisector = 3 * PBRWaterBisection::GridTriangles;
	uint listIndex = vertexID / perBisector + BisectionDrawArgs.Load(PBRW_BISECTION_MAX_MESHES * 16 + BisectionMesh.x * 4);
	uint local = vertexID % perBisector;
	uint2 a, b, c;
	PBRWaterTess::TriangleCorners(local / 3, PBRWaterBisection::GridN, a, b, c);
	uint corner = local % 3;
	uint2 g = corner == 0 ? a : (corner == 1 ? b : c);
	return listIndex * PBRWaterBisection::GridVertices + PBRWaterTess::VertexIndex(g.x, g.y, PBRWaterBisection::GridN);
}

VS_OUTPUT main(VS_INPUT input, uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	// Tess0.x: 0 an untessellated draw, 1 the tessellated mesh's vertices (PBRWater::RestoreDraw, BisectionTessellator::Draw),
	// 2 the engine's own draw of a mesh the tessellator draws instead: every vertex outside the frustum, so it rasterises
	// nothing; 3 the deferred surface's tiles of the mesh (PBRWater/Deferred.hlsli).
#			if defined(SPECULAR)
	if (PBRWater::Tess0.x > 2.5) {
		VS_OUTPUT tile = (VS_OUTPUT)0;
		tile.HPosition = PBRWaterDeferred::TileVertex(vertexID, instanceID);
		return tile;
	}
#			endif
	if (PBRWater::Tess0.x > 1.5) {
		VS_OUTPUT culled = (VS_OUTPUT)0;
		culled.HPosition = float4(2.0, 2.0, 2.0, 1.0);
		return culled;
	}
	if (PBRWater::Tess0.x > 0.5)
		return CachedWaterVertex(BisectionVertexIndex(vertexID));
	return WaterVertex(input, PBR_WATER_SPACING);
}
#		else
VS_OUTPUT main(VS_INPUT input)
{
	return WaterVertex(input, PBR_WATER_SPACING);
}
#		endif
#	endif

#	if defined(GSHADER)
// Only bound for the wireframe debug view: tags each triangle corner with its barycentric.
[maxvertexcount(3)] void main(triangle VS_OUTPUT input[3], inout TriangleStream<VS_OUTPUT> stream) {
	[unroll] for (uint i = 0; i < 3; i++)
	{
		VS_OUTPUT v = input[i];
#		if defined(PBR_WATER) && !defined(STENCIL)
		v.Barycentric = float3(i == 0, i == 1, i == 2);
#		endif
		stream.Append(v);
	}
	stream.RestartStrip();
}
#	endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting : SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask : SV_Target0;
	float2 MotionVector : SV_Target1;
#		if defined(PBR_WATER)
	// The deferred surface's record (PBRWater/Deferred.hlsli): bound for the tessellated meshes only.
	uint DeferredTag : SV_Target2;
	float4 DeferredWaves : SV_Target3;
	float2 DeferredNoise : SV_Target4;
	float2 DeferredUV : SV_Target5;
#		endif
#	endif
};

#	ifdef PSHADER

SamplerState ReflectionSampler : register(s0);
SamplerState RefractionSampler : register(s1);
SamplerState DisplacementSampler : register(s2);
SamplerState CubeMapSampler : register(s3);
SamplerState Normals01Sampler : register(s4);
SamplerState Normals02Sampler : register(s5);
SamplerState Normals03Sampler : register(s6);
SamplerState DepthSampler : register(s7);
SamplerState FlowMapSampler : register(s8);
SamplerState FlowMapNormalsSampler : register(s9);
SamplerState SSRReflectionSampler : register(s10);
SamplerState RawSSRReflectionSampler : register(s11);
#		if defined(PHYSICAL_SKY)
SamplerState ApSampler : register(s13);
#		endif

Texture2D<float4> ReflectionTex : register(t0);
Texture2D<float4> RefractionTex : register(t1);
Texture2D<float4> DisplacementTex : register(t2);
TextureCube<float4> CubeMapTex : register(t3);
Texture2D<float4> Normals01Tex : register(t4);
Texture2D<float4> Normals02Tex : register(t5);
Texture2D<float4> Normals03Tex : register(t6);
Texture2D<float4> DepthTex : register(t7);
Texture2D<float4> FlowMapTex : register(t8);
Texture2D<float4> FlowMapNormalsTex : register(t9);
Texture2D<float4> SSRReflectionTex : register(t10);
Texture2D<float4> RawSSRReflectionTex : register(t11);

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);  // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust : packoffset(c1);   // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c2);
	float4 SunDir : packoffset(c3);
	float4 SunColor : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float4 ShallowColor : packoffset(c0);
	float4 DeepColor : packoffset(c1);
	float4 ReflectionColor : packoffset(c2);
	float4 FresnelRI : packoffset(c3);    // Fresnel amount in x, specular power in z
	float4 BlendRadius : packoffset(c4);  // flowmap scale in y, specular radius in z
	float4 VarAmounts : packoffset(c5);   // Sun specular power in x, reflection amount in y, alpha in z, refraction magnitude in w
	float4 NormalsAmplitude : packoffset(c6);
	float4 WaterParams : packoffset(c7);   // noise falloff in x, reflection magnitude in y, sun sparkle power in z, framebuffer range in w
	float4 FogNearColor : packoffset(c8);  // above water fog amount in w
	float4 FogFarColor : packoffset(c9);
	float4 FogParam : packoffset(c10);      // above water fog distance far in z, above water fog range in w
	float4 DepthControl : packoffset(c11);  // depth reflections factor in x, depth refractions factor in y, depth normals factor in z, depth specular lighting factor in w
	float4 SSRParams : packoffset(c12);     // fWaterSSRIntensity in x, fWaterSSRBlurAmount in y, inverse main render target width and height in zw
	float4 SSRParams2 : packoffset(c13);    // fWaterSSRNormalPerturbationScale in x
}

cbuffer PerGeometry : register(b2)
{
	float4x4 TextureProj : packoffset(c0);
	float4 ReflectPlane[1] : packoffset(c4);
	float4 ProjData : packoffset(c5);
	float4 LightPos[8] : packoffset(c6);
	float4 LightColor[8] : packoffset(c14);
}

#		define SampColorSampler Normals01Sampler
#		define LinearSampler Normals01Sampler

static const float WATER_F0 = 0.02f;
static const float WATER_SUN_ANGULAR_RADIUS = 0.00465f;

float GetWaterProjectedSolidAngle(float cosAngularRadius)
{
	return max(Math::PI * (1.0f - cosAngularRadius * cosAngularRadius), EPSILON_DIVISION);
}

float GetWaterLightDiskCos()
{
	float cosAngularRadius = cos(WATER_SUN_ANGULAR_RADIUS);
#		if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled)
		cosAngularRadius = cos(SharedData::physSkyData.diskAngularRadius.w);
#		endif
	return cosAngularRadius;
}

/** @brief Physical Sky switches water to a fixed-F0 fresnel and a sun disk highlight; otherwise the water form's values apply. */
bool UsePhysicalWaterReflections()
{
#		if defined(PHYSICAL_SKY)
	return SharedData::physSkyData.enabled != 0;
#		else
	return false;
#		endif
}

float GetWaterFresnel(float cosTheta)
{
	return BRDF::F_Schlick(WATER_F0.xxx, saturate(cosTheta)).x;
}

float GetWaterDirectSpecularScale()
{
	return Color::PBRLightingCompensation * Color::PBRLightingScale;
}

float GetWaterDeltaLightDistributionFromCos(float3 normal, float3 viewDirection, float3 lightDirection, float cosAngularRadius)
{
	float3 V = -viewDirection;
	float3 R = reflect(viewDirection, normal);

	float NdotV = saturate(dot(normal, V));
	float lightMask = step(cosAngularRadius, dot(R, lightDirection));

	return lightMask * GetWaterFresnel(NdotV) / GetWaterProjectedSolidAngle(cosAngularRadius);
}

#		if defined(SKYLIGHTING)
#			include "Skylighting/Skylighting.hlsli"
#		endif

#		if defined(EXP_HEIGHT_FOG)
#			include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#		endif

#		if defined(PHYSICAL_SKY)
#			include "PhysicalSky/Common.hlsli"
#		endif

#		include "Common/ShadowSampling.hlsli"

#		if defined(PBR_WATER)
#			include "PBRWater/Shading.hlsli"
#			if defined(PBRW_DEFERRED) || defined(STENCIL)
#				include "PBRWater/Deferred.hlsli"
#			endif
#		endif

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
float GetWaterFogFade()
{
#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		return ExponentialHeightFog::GetVanillaFogFade(PosAdjust.w);
	}
#			endif
	return PosAdjust.w;
}

#			if defined(FLOWMAP)

/**
 * Structure containing complete flowmap information
 */
struct FlowmapData
{
	float4 color;       // Raw flowmap color (R=flow_x, G=flow_y, B=flow_strength, A=flow_mask)
	float2 flowVector;  // Flow vector (coordinate space depends on source function)
};

/**
 * Gets raw flowmap data before UV-space coordinate transformation
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for sampling the flowmap texture
 * @return FlowmapData with raw components:
 *         - color: Raw flowmap texture sample (RG=rotation, B=strength, A=mask)
 *         - flowVector: Base flow vector before any coordinate transformation
 *                      Ready for direct application of rotation matrix for world positioning
 *
 * @details This function provides flowmap data in its original coordinate space, suitable
 *          for world-space positioning effects (like ripple movement). The flowVector has
 *          NOT been transformed for UV-space normal sampling - that transformation is only
 *          applied in GetFlowmapDataUV() which uses transpose for UV coordinate perturbation.
 *
 *          Use this function when you need to apply the rotation matrix directly for
 *          world-space effects without needing to reverse any existing transformations.
 *
 * @see GetFlowmapDataUV() for UV-space normal sampling (applies transpose transformation)
 */
FlowmapData GetFlowmapDataTextureSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data;
	data.color = FlowMapTex.SampleLevel(FlowMapSampler, input.TexCoord2.zw + uvShift, 0);
	data.flowVector = (64 * input.TexCoord3.xy) * sqrt(1.01 - data.color.z);
	// NOTE: flowVector is NOT transformed yet - this is the raw vector before rotation matrix
	return data;
}
/**
 * Samples flowmap texture and calculates UV-space flow data for texture sampling
 *
 * @param input Pixel shader input containing texture coordinates and world position data
 * @param uvShift UV offset for sampling the flowmap texture (used for animation/variation)
 * @return FlowmapData Complete flowmap information with UV-space flow vector
 *
 * @details This function:
 *          - Samples the flowmap texture at the specified UV coordinates
 *          - Decodes flow direction from RG channels (remapped from [0,1] to [-1,1])
 *          - Calculates flow strength using the blue channel with sqrt falloff
 *          - Applies transpose rotation matrix to transform flow direction to UV space
 *          - Scales flow vector by world position and strength factors
 *
 * @note Flowmap format:
 *       - Red channel: Flow direction X component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Green channel: Flow direction Y component (0.5 = no flow, 0/1 = negative/positive flow)
 *       - Blue channel: Flow strength (0 = no flow, 1 = maximum flow)
 *       - Alpha channel: Flow mask/intensity multiplier
 */
FlowmapData GetFlowmapDataUV(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowSinCos = data.color.xy * 2 - 1;
	float2x2 flowRotationMatrix = float2x2(flowSinCos.x, flowSinCos.y, -flowSinCos.y, flowSinCos.x);
	data.flowVector = mul(transpose(flowRotationMatrix), data.flowVector);
	return data;
}
// ----------------------------------------------------------------
// Flowmap Parallax Functions
// ----------------------------------------------------------------

/**
 * Samples height from flowmap texture using the same 4-sample blend as flowmap normals
 * This ensures height transitions match the normal transitions exactly
 *
 * @param input PS_INPUT for flowmap coordinate access
 * @param normalMul The blend weights from the flowmap system (same as used for normals)
 * @param uvShift The UV shift value (1 / (128 * flowmapDimensions))
 * @param mipLevel Mip level for texture sampling
 */
float GetFlowmapHeightBlended(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
{
	// Sample height using the EXACT same UV computation as GetFlowmapNormal
	// This ensures the height blending matches the normal blending perfectly

	// Sample 0: uvShift, multiplier=9.92, offset=0
	FlowmapData flowData0 = GetFlowmapDataUV(input, uvShift);
	float2 uv0 = 0 + (flowData0.flowVector - float2(9.92 * ((0.001 * ReflectionColor.w) * flowData0.color.w), 0));
	float height0 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv0, mipLevel).w;

	// Sample 1: float2(0, uvShift.y), multiplier=10.64, offset=0.27
	FlowmapData flowData1 = GetFlowmapDataUV(input, float2(0, uvShift.y));
	float2 uv1 = 0.27 + (flowData1.flowVector - float2(10.64 * ((0.001 * ReflectionColor.w) * flowData1.color.w), 0));
	float height1 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv1, mipLevel).w;

	// Sample 2: 0.0.xx, multiplier=8, offset=0
	FlowmapData flowData2 = GetFlowmapDataUV(input, 0.0.xx);
	float2 uv2 = 0 + (flowData2.flowVector - float2(8 * ((0.001 * ReflectionColor.w) * flowData2.color.w), 0));
	float height2 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv2, mipLevel).w;

	// Sample 3: float2(uvShift.x, 0), multiplier=8.48, offset=0.62
	FlowmapData flowData3 = GetFlowmapDataUV(input, float2(uvShift.x, 0));
	float2 uv3 = 0.62 + (flowData3.flowVector - float2(8.48 * ((0.001 * ReflectionColor.w) * flowData3.color.w), 0));
	float height3 = FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, uv3, mipLevel).w;

	// Use the EXACT same blending formula as flowmap normals
	float blendedHeight =
		normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
		(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);

	return blendedHeight;
}

// Keep this for compatibility - just forwards to the proper function
float GetFlowmapHeightBarycentric(PS_INPUT input, float2 flowmapDimensions, float2 baseUV, float mipLevel)
{
	// This is now unused - we use GetFlowmapHeightBlended directly
	return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
}

/**
 * Computes mip level from continuous (non-flowmapped) UVs.
 * Flow-rotated UVs have discontinuous ddx/ddy and must not be used for LOD.
 */
float GetFlowmapMipLevel(float2 baseUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = baseUV * textureDims;
	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
	return max(0.5 * log2(minTexCoordDelta), 0);
}

/**
 * Samples height from flowmap texture (riverflow.dds alpha channel)
 * Uses the same UV calculation as GetFlowmapNormal for consistency
 */

/**
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted).
 * Mip LOD uses non-flowmapped TexCoord3 derivatives — flow-rotated UVs break hardware/manual
 * mip selection (stuck mip 0 shimmer, or excessive LOD + UV shrink that flattens normals).
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset, float2 dx, float2 dy)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	// Match UV footprint: flowVector scales by this (see GetFlowmapDataTextureSpace)
	float flowStrength = sqrt(1.01 - flowData.color.z);
	float mipScale = exp2(SharedData::MipBias) * flowStrength;

	return float3(FlowMapNormalsTex.SampleGrad(FlowMapNormalsSampler, uv, dx * mipScale, dy * mipScale).xy, flowData.color.z);
}

/**
 * Gets flowmap data with world-space flow vector for positioning effects
 *
 * @param input Pixel shader input containing texture coordinates
 * @param uvShift UV offset for flowmap sampling (used for animation phases)
 * @return FlowmapData Complete flowmap information with world-space flow vector
 *
 * @details This function:
 *          - Samples raw flowmap data (before UV-space transformations)
 *          - Decodes flow direction from flowmap RG channels
 *          - Applies component-wise directional transformation
 *          - Returns complete flowmap data with world-space flow vector
 *
 * @note Use this for effects that need to move with water current (ripples, debris, foam, etc.)
 *       For UV-space normal sampling, use GetFlowmapDataUV() instead
 */
FlowmapData GetFlowmapDataWorldSpace(PS_INPUT input, float2 uvShift)
{
	FlowmapData data = GetFlowmapDataTextureSpace(input, uvShift);
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}

/**
 * Converts existing texture-space flowmap data to world-space (avoids duplicate sampling)
 *
 * @param textureSpaceData FlowmapData from GetFlowmapDataTextureSpace()
 * @return FlowmapData Complete flowmap data with world-space flow vector
 *
 * @note Use this overload when you already have texture-space flowmap data to avoid duplicate texture sampling
 */
FlowmapData GetFlowmapDataWorldSpace(FlowmapData textureSpaceData)
{
	FlowmapData data = textureSpaceData;
	float2 flowDirection = -(data.color.xy * 2 - 1);    // Decode direction with 180° correction
	data.flowVector = data.flowVector * flowDirection;  // Transform to world space
	return data;
}
#			endif

#			if defined(LOD)
#				undef WATER_EFFECTS
#				undef WETNESS_EFFECTS
#			endif

#			if defined(WATER_EFFECTS) && !defined(VC)
#				define WATER_PARALLAX
#				include "WaterEffects/WaterParallax.hlsli"
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
#				include "DynamicCubemaps/DynamicCubemaps.hlsli"
#			endif

#			if defined(WETNESS_EFFECTS)
#				include "WetnessEffects/WetnessEffects.hlsli"
#			endif

// Structure to return both normal and ripple/splash color information
struct WaterNormalData
{
	float3 normal;
	float4 rippleInfo;  // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
#			if defined(PBR_WATER)
	float detailVariance;  // slope variance the mip filter averaged out of the normal maps (Toksvig), at their blended amplitude
#			endif
};

#			if defined(PBR_WATER)
/**
 * Toksvig (2005): a mip-filtered normal map texel averages normals that point different ways, so the decoded
 * normal comes out shorter than 1. Its shortening gives the slope variance the filter removed,
 * (1 - |n|) / |n|, which the roughness takes over so distant detail turns into gloss instead of aliasing.
 * A magnified map shortens its normals too, between texels, where nothing was filtered away: that share is
 * dropped (PBRWater::MinifiedShare).
 * @param n   decoded tangent-space normal before normalisation
 * @param map the normal map it was sampled from, at `uv` with the material mip bias
 */
float ToksvigVariance(float3 n, Texture2D<float4> map, float2 uv)
{
	float width, height;
	map.GetDimensions(width, height);
	float2 texels = float2(width, height) * exp2(SharedData::MipBias);
	float len = length(n);
	return max(0.0, (1.0 - len) / max(len, 1e-3)) * PBRWater::MinifiedShare(ddx_fine(uv) * texels, ddy_fine(uv) * texels);
}
#			endif

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);
#			if defined(PBR_WATER)
	result.detailVariance = 0.0;
#			endif

	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

	float4 normalsAmplitude = NormalsAmplitude;
	if (SharedData::enbSettings.EnableWater) {
		normalsAmplitude *= SharedData::enbSettings.WaterWavesAmplitude;
	}

#			if defined(WATER_PARALLAX)
	float2 parallaxOffset = 0.0;
#				if defined(PBR_WATER)
	// With waves the surface is real geometry: the parallax over the normal maps is skipped (PBRWater::Params2.y).
	const bool parallax = PBRWater::Params2.y < 0.5;
	[branch] if (parallax)
#				endif
		parallaxOffset = WaterEffects::GetParallaxOffset(input, normalsAmplitude.xyz, normalScalesRcp);
#			endif

#			if defined(FLOWMAP)
#				if defined(UNIFIED_WATER)
	float2 flowmapDimensions = input.TexCoord4.xy;
#				else
	float2 flowmapDimensions = input.TexCoord4.xx;
#				endif
	float2 uvShift = 1 / (128 * flowmapDimensions);

	// Compute flowmap parallax and create parallaxed input for normal sampling
	PS_INPUT flowmapInput = input;
	float2 flowmapParallaxOffset = float2(0, 0);
#				if defined(WATER_PARALLAX) && !defined(LOD)
#					if defined(PBR_WATER)
	[branch] if (parallax)
#					endif
	{
		float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
		float2 parallaxDir = viewDirection.xy / -viewDirection.z;
		parallaxDir.y = -parallaxDir.y;
		float viewDotUp = -viewDirection.z;
		parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
		flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
		flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalsAmplitude.x, normalScalesRcp);
	}
#				endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Same UV scale as flowVector before rotation — shared so all 4 blend samples use matching LOD
	float2 baseUV = 64 * flowmapInput.TexCoord3.xy;
	float2 flowNormalDx = ddx(baseUV);
	float2 flowNormalDy = ddy(baseUV);

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27, flowNormalDx, flowNormalDy);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0, flowNormalDx, flowNormalDy);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62, flowNormalDx, flowNormalDy);

	float2 flowmapNormalWeighted =
		normalMul.y * (normalMul.x * flowmapNormal2.xy + (1 - normalMul.x) * flowmapNormal3.xy) +
		(1 - normalMul.y) *
			(normalMul.x * flowmapNormal1.xy + (1 - normalMul.x) * flowmapNormal0.xy);
	float2 flowmapDenominator = sqrt(normalMul * normalMul + (1 - normalMul) * (1 - normalMul));
	float3 flowmapNormal =
		float3(((-0.5 + flowmapNormalWeighted) / (flowmapDenominator.x * flowmapDenominator.y)) *
				   max(0.4, normalsDepthFactor),
			0);
	flowmapNormal.z =
		sqrt(1 - flowmapNormal.x * flowmapNormal.x - flowmapNormal.y * flowmapNormal.y);
	float2 baseNormalUv = input.TexCoord1.xy;
#				if defined(WATER_PARALLAX)
	// Use flowmap-derived parallax offset for base normals
	baseNormalUv += flowmapParallaxOffset.xy * normalScalesRcp.x;
#				endif
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, baseNormalUv, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#			endif  // End of FLOWMAP block

#			if !defined(FLOWMAP)
#				if defined(PBR_WATER)
	float2 baseNormalUv = input.TexCoord1.xy;
#				endif
#				if defined(WATER_PARALLAX)
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy + parallaxOffset.xy * normalScalesRcp.x, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				else
	float3 normals1 = Normals01Tex.SampleBias(Normals01Sampler, input.TexCoord1.xy, SharedData::MipBias).xyz * 2.0 + float3(-1, -1, -2);
#				endif
#			endif  // End of !FLOWMAP block
#			if defined(FLOWMAP) && !defined(BLEND_NORMALS)
#				ifdef DISABLE_FLOWMAP_NORMALS
	// FLOWMAP NORMALS DISABLED: Using only base normals (flow system still active for ripples/splashes)
	float3 finalNormal = normalize(normals1 + float3(0, 0, 1));
#					if defined(PBR_WATER)
	result.detailVariance = ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv);
#					endif
#				else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#					if defined(PBR_WATER)
	result.detailVariance = ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv) * (1.0 - distanceFactor) * (1.0 - distanceFactor);
#					endif
#				endif
#			elif !defined(LOD)

#				if defined(WATER_PARALLAX)
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw + parallaxOffset.xy * normalScalesRcp.y, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy + parallaxOffset.xy * normalScalesRcp.z, SharedData::MipBias).xyz * 2.0 - 1.0;
#				else
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy, SharedData::MipBias).xyz * 2.0 - 1.0;
#				endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + normalsAmplitude.x * normals1 +
									 normalsAmplitude.y * normals2 + normalsAmplitude.z * normals3);
#				if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#				else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#				endif
#				if defined(PBR_WATER)
	{
		// The maps' slopes enter the blend with their amplitudes over its z of about 1 + a.y + a.z (normals1 is
		// stored offset by -1 in z); the depth fade scales the slopes once more.
		float3 a = normalsAmplitude.xyz;
		float blendZ = 1.0 + a.y + a.z;
		float variance = (a.x * a.x * ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv) + a.y * a.y * ToksvigVariance(normals2, Normals02Tex, input.TexCoord1.zw) + a.z * a.z * ToksvigVariance(normals3, Normals03Tex, input.TexCoord2.xy)) / (blendZ * blendZ);
#					if !defined(UNDERWATER)
		variance *= normalsDepthFactor * normalsDepthFactor;
#					endif
		result.detailVariance = variance;
	}
#				endif

#				if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#					if defined(PBR_WATER)
	result.detailVariance = lerp(ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv), result.detailVariance * (1.0 - normalBlendFactor) * (1.0 - normalBlendFactor), distanceFactor);
#					endif
#				endif
#			else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + normalsAmplitude.xxx * normals1);
#				if defined(PBR_WATER)
	result.detailVariance = normalsAmplitude.x * normalsAmplitude.x * ToksvigVariance(normals1 + float3(0, 0, 1), Normals01Tex, baseNormalUv);
#				endif
#			endif

	// PBR water has no vanilla wading displacement (TESWaterSystem_InitializeWater: noDisplacement), its ripple simulation
	// stirs the real surface instead. The engine still flags the water as waded while the player is in it: reading the
	// never-written DisplacementTex then tipped every pixel's normal over and the whole surface turned into a mirror.
#			if (defined(WADING_RT) || defined(WADING)) && !defined(PBR_WATER)
#				if defined(WADING_RT)
	const bool isWading = Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Wading;
#				else
	const bool isWading = true;
#				endif
	if (isWading) {
#				if defined(FLOWMAP)
		float2 displacementUv = input.TexCoord3.zw;
#				else
		float2 displacementUv = input.TexCoord3.xy;
#				endif
		float3 displacement = normalize(float3(normalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
			0.04));
		finalNormal = lerp(displacement, finalNormal, displacement.z);
	}
#			endif

#			if defined(WETNESS_EFFECTS)
	// Wetness Effects Debug System:
	// DEBUG_WETNESS_EFFECTS Color Legend:
	// - BRIGHT MAGENTA: Ripples, BRIGHT GREEN: Splashes, CYAN: Both effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	float maxRainDropDistance = SharedData::wetnessEffectsSettings.RaindropFxRange * SharedData::wetnessEffectsSettings.RaindropFxRange * 3;
	float rainDropDistance = dot(input.WPosition.xyz, input.WPosition.xyz);
	float distanceFadeout = saturate((1 - saturate(rainDropDistance / maxRainDropDistance)) * 3);
	if (finalNormal.z > 0 && SharedData::wetnessEffectsSettings.Raining > 0.0f && SharedData::wetnessEffectsSettings.EnableRaindropFx &&
		(rainDropDistance < maxRainDropDistance) && wetnessOcclusion > 0.05) {
		float rippleStrengthModifier = (wetnessOcclusion * wetnessOcclusion) * distanceFadeout;
		float3 rippleWPosition = input.WPosition.xyz + finalNormal * 16;
#				if defined(WATER_PARALLAX)
		rippleWPosition.xy += parallaxOffset;
#				endif
#				if defined(FLOWMAP)
		// Flow-following ripple enhancement: Makes raindrops follow water current
		FlowmapData worldFlowData = GetFlowmapDataWorldSpace(input, float2(0, 0));

		// Calculate flow-aware ripple offset using centralized timing logic
		// Parameters: avgFlowmapMultiplier=9.26 (average of GetWaterNormal flowmap normal multipliers: 9.92, 10.64, 8, 8.48)
		// uvToWorldScale=0.125 (1/8 - relates to 64× texture coordinate scaling factor)
		float2 flowOffset = WetnessEffects::GetFlowAwareRippleOffset(
			worldFlowData.flowVector,
			worldFlowData.color.w,      // Flow strength from flowmap alpha
			0.001 * ReflectionColor.w,  // Reflection timing scale (matches GetFlowmapNormal)
			9.26,                       // Average flowmap normal multiplier
			0.125                       // UV-to-world scale factor (1/8)
		);

		rippleWPosition.xy += flowOffset;
#				endif
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust.xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = ReorientNormal(rippleNormal, finalNormal);
#			endif

	result.normal = finalNormal;
	return result;
}

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection, float distanceFactor, float skylightingSpecular, float roughness = 0.0)
{
#			if !defined(DYNAMIC_CUBEMAPS)
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections))
		return ReflectionColor.xyz * VarAmounts.y;
#			endif

	float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));
#			if defined(PBR_WATER)
	// Glossy reflections: rough water (wind, rain, or wave detail filtered out at a distance) reflects a
	// blurred environment, through the reflection cubemap's mips when it has them.
	uint pbrCubeWidth, pbrCubeHeight, pbrCubeLevels;
	CubeMapTex.GetDimensions(0, pbrCubeWidth, pbrCubeHeight, pbrCubeLevels);
	float pbrCubeLevel = roughness * (float)(pbrCubeLevels - 1);
	float pbrEnvironmentLevel = roughness * 8.0;
#			else
	float pbrCubeLevel = 0;
	float pbrEnvironmentLevel = 0;
#			endif

#			if defined(DYNAMIC_CUBEMAPS)
	if (SharedData::InInterior)
		return DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;

	float3 workingSpecularIrradiance = 0.0;
	if (skylightingSpecular < 1.0)
		workingSpecularIrradiance = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;

	float3 workingSpecularIrradianceReflections = 0.0;
	if (skylightingSpecular > 0.0) {
		workingSpecularIrradianceReflections = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, pbrEnvironmentLevel).xyz;
#				if !defined(PBR_WATER)
		// PBR water keeps one environment at every distance: switching to the vanilla reflection cubemap (much darker)
		// past 1024 units made the whole water change brightness as the camera rose above it.
		if ((Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections) && !SharedData::HideSky) {
			// Only the sky-visible lobe, without the specular floor, fades to the engine cube
			float minVisibility = SharedData::skylightingSettings.MinSpecularVisibility;
			float skyVisibility = minVisibility < 1.0 ? saturate((skylightingSpecular - minVisibility) / (1.0 - minVisibility)) : 1.0;
			float reflectionAmount = saturate(length(input.WPosition.xyz) / 1024.0) * skyVisibility;
			workingSpecularIrradianceReflections = lerp(workingSpecularIrradianceReflections, CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz, reflectionAmount);
		}
#				endif
	}

	float3 reflectionColor = ColorManagement::SceneColor::LerpInLinear(workingSpecularIrradiance, workingSpecularIrradianceReflections, skylightingSpecular);
#			else
	float3 reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, pbrCubeLevel).xyz;
#			endif

#			if !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
	float pointingDirection = dot(viewDirection, R) * 0.5 + 0.5;
	float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R) * 0.5 + 0.5;
#				if defined(PBR_WATER)
	// The reflection normal is a blend, not unit length, so the dots can dip just below -1 and the
	// sqrt would return NaN.
	float ssrAmount = sqrt(max(min(pointingAlignment, pointingDirection), 0.0));
#				else
	float ssrAmount = sqrt(min(pointingAlignment, pointingDirection));
#				endif
	float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
	float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
	float4 ssrReflectionColorBlurred = SSRReflectionTex.Sample(SSRReflectionSampler, ssrReflectionUvDR);
	float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
#				if defined(PBR_WATER)
	// Rough water only shows the blurred screen-space reflection.
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7 * saturate(1.0 - roughness * 3.0));
#				else
	float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);
#				endif
	float3 finalSsrReflectionColor = max(0, PreExposure::Decode(ssrReflectionColor.xyz));
	float ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount);
	reflectionColor = lerp(reflectionColor, finalSsrReflectionColor, ssrFraction);
#			endif

	return reflectionColor;
}

// Effects 11 water: depth over which the surface reaches full opacity at the shore
static const float ShoreFadeDepth = 12.0;

float GetScreenDepthWater(float2 screenPosition)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
}

float3 GetLdotN(float3 normal)
{
#			if defined(UNDERWATER)
	return 1;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 1;
	return saturate(dot(SunDir.xyz, normal));
#			endif
}

float GetFresnelValue(float3 normal, float3 viewDirection)
{
#			if defined(UNDERWATER)
	float3 actualNormal = -normal;
#			else
	float3 actualNormal = normal;
#			endif
	float viewAngle = 1 - saturate(dot(-viewDirection, actualNormal));

	if (SharedData::enbSettings.EnableWater) {
		float fresnelExponent = SharedData::enbSettings.WaterFresnelMultiplier;
		// pow(0, 0) is NaN on GPU
		float fresnelRI = fresnelExponent != 0.0 ? pow(abs(FresnelRI.x), fresnelExponent) : 1.0;
		float fresnel = (1 - fresnelRI) * pow(viewAngle, 5) + fresnelRI;
		fresnel = lerp(SharedData::enbSettings.WaterFresnelMin, SharedData::enbSettings.WaterFresnelMax, fresnel);
		return fresnel * SharedData::enbSettings.WaterReflectionAmount;
	}

	if (UsePhysicalWaterReflections())
		return GetWaterFresnel(dot(-viewDirection, actualNormal));

	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;
};

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, float3 viewPosition, float depth)
{
#			if defined(REFRACTIONS)
#				if defined(PBR_WATER)
	float refractionMagnitude = VarAmounts.w * PBRWater::Light1.z;
#				else
	float refractionMagnitude = VarAmounts.w;
#				endif
	float4 refractionNormal = mul(transpose(TextureProj), float4((refractionMagnitude * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#				if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, ReflectPlane[0].xyz);

	float refractionPlaneMul = (1 - ReflectPlane[0].w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	} else {
		float muddiness = SharedData::enbSettings.EnableWater ? SharedData::enbSettings.WaterMuddiness : 1.0;
		distanceMul = saturate(float4(1, muddiness, 1, 1) * refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse, float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(refractionScreenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = PreExposure::Decode(RefractionTex.Sample(RefractionSampler, refractionUV).xyz);

#				if defined(UNDERWATER)
	float refractionMul = 0;
#				else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#				endif

	float3 refractionDiffuseColor;

#				if defined(PBR_WATER) && !defined(UNDERWATER)
	// PBR Water: the body colour is the light an optically deep column scatters back, the water form's deep colour.
	// Its shallow colour is the column's transmission tint (Beer-Lambert in the water column, Water.hlsl), not a colour
	// of its own. Vanilla's blend by distanceMul left the body at the shallow colour wherever its refraction-plane test
	// failed (water at the eye seen edge-on, anything just under the surface): an olive line along the horizon and
	// olive shapes around everything in the water, the underwater fog's colour.
	refractionDiffuseColor = DeepColor.xyz;
#				else
	if (SharedData::enbSettings.EnableWater) {
		float3 shallowColor = ShallowColor.xyz;
		float maxValue = max(shallowColor.x, max(shallowColor.y, shallowColor.z));
		if (maxValue > 0.0)
			shallowColor /= maxValue;
		else
			shallowColor = 1.0;

		// Tinted refraction only reads near the surface; deeper water fades to the flat shallow color
		shallowColor = lerp(shallowColor.xyz * refractionColor, ShallowColor.xyz, lerp(SharedData::enbSettings.WaterMuddiness, 1.0, refractionMul));
		refractionDiffuseColor = lerp(shallowColor.xyz, DeepColor.xyz, distanceMul.y);
	} else {
		refractionDiffuseColor = lerp(ShallowColor.xyz, DeepColor.xyz, distanceMul.y);
	}
#				endif

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = normalize(refractionWorldPosition.xyz - input.WPosition.xyz);
	return output;
#			else
	DiffuseOutput output;
	output.refractionColor = lerp(ShallowColor.xyz, DeepColor.xyz, fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	return output;
#			endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition, float roughness = 0.0)
{
#			if defined(UNDERWATER)
	return 0.0.xxx;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

#				if defined(PBR_WATER)
	// Clamp what one pixel can reflect: a sub-pixel glint the AA kernel misses must not bloom across the screen.
	float lightDistribution = min(PBRWater::SpecularGGX(normal, -viewDirection, SunDir.xyz, roughness, 1.0 / PBRWater::WaterIOR, PBRWater::SunAngularRadius), 12.0) * PBRWater::Light0.w;
#				else
	float lightDistribution;
	if (UsePhysicalWaterReflections()) {
		lightDistribution = GetWaterDeltaLightDistributionFromCos(normal, viewDirection, SunDir.xyz, GetWaterLightDiskCos()) * GetWaterDirectSpecularScale();
	} else {
		float3 reflectionDirection = reflect(viewDirection, normal);
		lightDistribution = VarAmounts.x > 0 ? exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz)))) : 1.0;
	}
#				endif

	float3 sunColor = Color::DirectionalLight(ColorManagement::SRGBToWorking(SunColor.xyz)) * SunDir.w * (1.0 - exp(-DeepColor.w));
#				if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		sunColor *= PhysSky::SampleTr(normalize(reflect(viewDirection, normal)), DepthSampler);
		sunColor *= PhysSky::GetDirlightTransmittance(worldPosition + FrameBuffer::CameraPosAdjust.xyz, DepthSampler, true);
	}
#				endif

#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#				endif
	return lightDistribution * sunColor;
#			endif
}
#		endif

#		if defined(LIGHT_LIMIT_FIX)
#			include "LightLimitFix/LightLimitFix.hlsli"
#		endif

#		if defined(IBL)
#			include "IBL/IBL.hlsli"
#		endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#		if defined(PBRW_DEFERRED)
	// The deferred surface's tiles (Deferred.hlsli): the pixels the stencil pass left to this mesh, already tested
	// against the waterline there. The rest only lend their quad its derivatives (discard demotes them to helpers).
	[branch] if (abs(PBRWater::Tess0.x - 3.0) < 0.5)
	{
		if (!PBRWaterDeferred::Resolve(input))
			discard;
	}
	else
#		endif
#		if defined(PBR_WATER)
	{
		// Dry: the bed rises above the waves and the surf, where DisplaceSurface held the mesh just above it. Both passes
		// test the same pixels; the colour pass then finds the exact waterline against the ground behind it.
#			if defined(STENCIL)
		if (PBRWater::AboveWaterline(input.WorldPosition.xyz, input.ShoreState))
			discard;
#			else
		if (PBRWater::AboveWaterline(input.WPosition.xyz, input.ShoreState))
			discard;
#			endif
	}
#		endif

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
	float3 viewDirection = normalize(input.WPosition.xyz);

	float distanceFactor = saturate(lerp(FrameBuffer::FrameParams.w, 1, (length(input.WPosition.xyz) - 8192) / (WaterParams.x - 8192)));
	float4 distanceMul = saturate(lerp(VarAmounts.z, 1, -(distanceFactor - 1))).xxxx;
	float distanceBlendFactor = distanceFactor;
#			if defined(UNIFIED_WATER)
	distanceBlendFactor = 1.0f;
#			endif

	bool isSpecular = false;

	float depth = 0;

#			if defined(DEPTH)
#				if defined(VERTEX_ALPHA_DEPTH)
#					if defined(VC)
	distanceMul = saturate(input.TexCoord3.z);
#					endif
#				else
	distanceMul = 0;

	depth = GetScreenDepthWater(screenPosition);
	float2 depthOffset =
		FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, ReflectPlane[0].xyz);

	float planeMul = (1 - ReflectPlane[0].w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);

#					if defined(HORIZON_FIX)
	if (DepthTex.Load(float3(screenPosition, 0)).x >= HorizonFix::EmptyDepthThreshold)
		distanceMul = 1.0.xxxx;
#					endif
#				endif
#			endif

#			if defined(UNDERWATER)
	float4 depthControl = float4(0, 1, 1, 0);
#			elif defined(LOD)
	float4 depthControl = float4(1, 0, 0, 1);
#			elif defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float4 depthControl = float4(0, 0, 1, 0);
#			else
	float4 depthControl = DepthControl * (distanceMul - 1) + 1;
#			endif
	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#			if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

	float3 positionMSSkylight = input.WPosition.xyz;

#				if defined(PBR_WATER)
	// The waves are in Skylighting's occlusion maps (PBR Water): read the sky just above this point of the surface,
	// where a trough sees less of it past the crests around than a crest does. The water surface itself is never lifted
	// out of the water (Skylighting::SampleNoBias).
	positionMSSkylight.z += Skylighting::CELL_SIZE.z;
	sh2 skylightingSH = Skylighting::SampleNoBias(positionMSSkylight, false);
#				else
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, float3(0, 0, 1));
#				endif
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1));

	float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, float3(0, 0, 1), Skylighting::GetFadeOutFactor(input.WPosition.xyz));

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#			endif

#			if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, wetnessOcclusion);
#			else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, inWorld);
#			endif

	float3 normal = waterData.normal;

#			if defined(PBR_WATER)
	PBRWater::SurfaceShading pbrSurface = PBRWater::ShadeSurface(input.WaveParam, input.FlowVelocity, input.SurfaceNoise.y, normal, waterData.detailVariance);
	normal = pbrSurface.normal;
	// A curling lip can turn its far side to the camera (seen from behind the wave, or where the lip passes through the
	// water behind it). Shaded about a normal facing away, its reflection, sun and foam lighting turned odd along the crest:
	// mirrored across the view direction, the normal keeps its lean along the surface and faces the side that is seen.
	float pbrFacing = dot(normal, viewDirection);
	if (pbrFacing > 0.0)
		normal = normalize(normal - 2.0 * pbrFacing * viewDirection);
	pbrSurface.normal = normal;  // the normals debug view shows what is shaded
	float pbrRoughness = pbrSurface.roughness;
#			else
	float pbrRoughness = 0.0;
#			endif

#			if defined(SKYLIGHTING)
#				if defined(PBR_WATER)
	// The sky each wave facet sees: tilted towards a cliff or a hull it sees less of it.
	skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, normal, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#				endif
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = Skylighting::EvaluateSpecular(skylightingSH, specularLobe, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#			endif

	float fresnel = GetFresnelValue(normal, viewDirection);
#			if defined(PBR_WATER) && !defined(UNDERWATER)
	{
		// Roughness-aware environment Fresnel for F0 = 0.02 (air -> water).
		float2 envBRDF = BRDF::EnvBRDF(pbrRoughness, saturate(dot(-viewDirection, normal)));
		float pbrFresnel = saturate(0.02 * envBRDF.x + envBRDF.y);
		fresnel = lerp(pbrFresnel, fresnel, PBRWater::Light0.z);
	}
#			endif

#			if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

#				if !defined(LIGHT_LIMIT_FIX)
	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust.xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
#					if defined(ENABLE_LL)
		lightColorMul = pow(lightColorMul, TransferFunctions::GAME_GAMMA);
#					endif
#					if defined(PBR_WATER)
		float pbrLightDistance = length(lightVector);
		float3 lightColor = Color::PointLight(LightColor[lightIndex].xyz) * lightColorMul *
		                    PBRWater::SpecularGGX(normal, -viewDirection, PBRWater::SafeNormalize(lightVector, normal), pbrRoughness, 1.0 / PBRWater::WaterIOR,
								PBRWater::EmitterAngularRadius(PBRWater::PointLightEmitterRadius(), pbrLightDistance)) *
		                    PBRWater::Light1.w;
#					else
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
#					endif
		finalColor += lightColor;
	}
#				endif

#				if !defined(PBR_WATER)
	finalColor *= fresnel;
#				endif
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

	isSpecular = true;
#			else

#				if defined(SKYLIGHTING)
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, skylightingSpecular, pbrRoughness);
#				else
	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceBlendFactor, 1.0, pbrRoughness);
#				endif

	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, normal, viewDirection, distanceMul, depthControl.y, fresnel, viewPosition, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, surfaceShadow);
#				if defined(PBR_WATER) && !defined(UNDERWATER)
	// The waves' own shadows: the water the sunlight crosses to get here (the engine's shadow map holds no water).
	PBRWater::SunThroughWaves pbrWaveSun = PBRWater::SunPathThroughWaves(input.WPosition.xyz, SunDir.xyz, input.ShoreState.w, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));
#				endif

	float3 dirColor;
	float3 ambientColor;
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, input.WPosition.xyz, dirColor, ambientColor);

	dirColor *= dirShadow;

#				if defined(SKYLIGHTING)
	ambientColor = ColorManagement::SceneColor::ScaleByLinear(ambientColor, skylightingDiffuse);
#				endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;

#				if !defined(UNDERWATER)
	if (SharedData::enbSettings.EnableWater) {
		float3 dirScatter = saturate(dot(normal.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * saturate(dot(viewDirection.xyz, SharedData::DirLightDirection.xyz) * 0.5 + 0.5) * ShadowSampling::GetDirectionalLighting();
		diffuseOutput.refractionDiffuseColor += DeepColor.xyz * dirScatter * surfaceShadow * SharedData::enbSettings.WaterSunLightingMultiplier;
	}
#				endif

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;

	float3 specularLighting = 0;

#				if defined(LIGHT_LIMIT_FIX)
	uint lightCount = 0;

	uint clusterIndex = 0;
	if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
		uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
		[loop] for (uint i = 0; i < lightCount; i++)
		{
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + i];
			LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];
			// Water has no engine shadow mask, so only shadow casters with a cached local shadow can light it.
			const bool localShadow = light.lightFlags & LightLimitFix::LightFlags::LocalShadow;
			if (LightLimitFix::IsLightIgnored(light) || ((light.lightFlags & LightLimitFix::LightFlags::ShadowCaster) && !localShadow)) {
				continue;
			}

			float3 lightDirection = light.positionWS.xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

			float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);
			// Out of the light's reach here (the cluster holds it for its other pixels): no shadow lookup or lobe.
			if (intensityMultiplier < 1e-5)
				continue;

			float3 normalizedLightDirection = normalize(lightDirection);

#					if defined(PBR_WATER)
			float3 lightColor = Color::PointLight(light.color.xyz) * light.fade *
			                    PBRWater::SpecularGGX(normal, -viewDirection, PBRWater::SafeNormalize(lightDirection, normal), pbrRoughness, 1.0 / PBRWater::WaterIOR,
									PBRWater::EmitterAngularRadius(PBRWater::PointLightEmitterRadius(), lightDist)) *
			                    PBRWater::Light1.w * intensityMultiplier;
			// Water lights only with its specular lobe, which a lamp's reflection fills on a few pixels: the local shadow
			// (a filtered cube map lookup) is only worth reading where it does. Read for every pixel in a lit cluster it was
			// the costliest part of the surface pass in a lit town on the coast (1.4 of 5.3 ms at Dawnstar).
			[branch] if (localShadow && max(lightColor.x, max(lightColor.y, lightColor.z)) > 1e-4)
				lightColor *= LightLimitFix::GetLocalShadow(DepthSampler, light.localShadowIndex, input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, normalizedLightDirection, false, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));
			specularLighting += lightColor;
#					else
			float lightShadow = 1.0;
			[branch] if (localShadow)
				lightShadow = LightLimitFix::GetLocalShadowWithLightFade(DepthSampler, light.localShadowIndex, input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, normalizedLightDirection, false, Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount));

			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			float3 lightColor = Color::PointLight(light.color.xyz) * pow(HdotN, FresnelRI.z) * light.fade;

			specularLighting += lightColor * intensityMultiplier * lightShadow;
#					endif
		}
	}
	specularColor += specularLighting * 3;
#				endif

#				if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(ShallowColor.xyz, specularColor, 0.5);
	float3 finalColor = saturate(1 - length(input.WPosition.xyz) * 0.002) * ((1 - fresnel) * (diffuseColor - finalSpecularColor)) + finalSpecularColor;
	// Add ripple and splash color effects for underwater
#					if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization (darker for underwater)
	float3 debugColor = WetnessEffects::GetDebugWetnessColorUnderwater(waterData.rippleInfo, 1.5, 2.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#					endif
#				else

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz, pbrRoughness) * surfaceShadow;

	// Reflection and sun weights. On refracting water vanilla fades both with refractionMul, which only
	// reaches 1 once the water is deep enough to fog, so shallow water lost its reflections.
	float surfaceMul = diffuseOutput.refractionMul;
	float sunMul = depthControl.w;
	bool shoreFadedSurface = false;

	if (SharedData::enbSettings.EnableWater) {
		sunColor *= SharedData::enbSettings.WaterSunSpecularMultiplier;
#					if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH) && defined(REFRACTIONS)
		// distanceMul.w is the water depth below the surface over FogParam.z, saturated
		surfaceMul = saturate(distanceMul.w * FogParam.z / max(min(ShoreFadeDepth, FogParam.z), 1e-4));
		sunMul = max(sunMul, surfaceMul);
		shoreFadedSurface = true;
#					endif
	}

#					if defined(VC) && !defined(PBR_WATER)
	float specularFraction = lerp(1, fresnel * surfaceMul, distanceBlendFactor);
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * sunMul;

#						if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const float3 waterViewDir = normalize(input.WPosition.xyz);
		const float waterDist = length(input.WPosition.xyz);
		const float4 apSample = PhysSky::SampleAp(waterViewDir, input.HPosition.xy, waterDist, ApSampler);
		finalColorPreFog = finalColorPreFog * apSample.w + apSample.xyz;
		if (!SharedData::PostWaterComposite)
			finalColorPreFog = PhysSky::CompositeVolumetricClouds(finalColorPreFog, input.HPosition.xy);
	}
#						endif

	if (!SharedData::PostWaterComposite) {
#						if !defined(UNIFIED_WATER)
		float fogDistanceFactor = input.FogParam.w;
		float3 fogColor = input.FogParam.xyz;
#						else
		float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
		float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			fogColor = ImageBasedLighting::GetFogIBLColor(fogColor, input.WPosition.xyz);
		}
#						endif
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
			if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
				fogColor = exponentialHeightFog.xyz;
				fogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, fogColor, exponentialHeightFog.w);
			} else {
				fogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
				float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
			}
		} else {
			fogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
		}
#						else
		fogColor *= GetWaterFogFade();
		finalColorPreFog = lerp(finalColorPreFog, fogColor, fogDistanceFactor);
#						endif
	}

	float3 finalColor = finalColorPreFog;

#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif

#					else
#						if defined(PBR_WATER)
	// Water column: Beer-Lambert transmittance of the refracted scene plus in-scattered body colour.
	float pbrPathLength = distanceMul.x * FogParam.z;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	{
		// Nothing rendered behind the water (sky / far plane): treat the column as infinitely deep.
		float pbrRawDepth = DepthTex.Load(float3(screenPosition, 0)).x;
#								if defined(HORIZON_FIX)
		bool pbrEmpty = pbrRawDepth >= HorizonFix::EmptyDepthThreshold;
#								else
		bool pbrEmpty = pbrRawDepth >= 0.999999;
#								endif
		pbrPathLength = pbrEmpty ? 1e7 : max(depthMul - length(input.WPosition.xyz), 0.0);
	}
#							endif
	// Vertical depth of what is seen through the water, and the path the refracted ray really takes to it:
	// Snell bends it towards the vertical, so at grazing angles light crosses far less water than the
	// straight view ray suggests.
	float pbrBottomDepth = pbrPathLength * abs(viewDirection.z);
	float3 pbrRefracted = refract(viewDirection, normal, 1.0 / PBRWater::WaterIOR);
	float pbrWaterPath = pbrBottomDepth / max(-pbrRefracted.z, 0.2);
	// A breaker's face is no slab: the ray seen through it runs into the crest nearly level and leaves through its back,
	// crossing only the crest's thickness at that height, about three times the height still above it (a crest's back is
	// about a third as steep as a breaking face). As a slab down to the bed far behind, every face shaded as the same deep
	// body colour, a wall; through the crest the face is thin and bright near the top and deepens towards the trough.
	float pbrSteepFace = smoothstep(0.6, 0.2, abs(normal.z)) * saturate(pbrSurface.crest / (0.1 * PBRWater::UnitsPerMetre));
	float pbrCrestPath = 3.0 * max(pbrSurface.crest - pbrSurface.height, 0.0) + 0.05 * PBRWater::UnitsPerMetre;
	pbrWaterPath = lerp(pbrWaterPath, min(pbrWaterPath, pbrCrestPath), pbrSteepFace);
	// What the depth buffer shows behind a wave that rises above the still water (icebergs, cliffs, the far shore) is no
	// part of the water's background: the ray refracted into the wave bends down into the sea and never reaches it, and
	// the scene behind showed through every crest standing above the horizon. Only the bed under the water is seen.
	// (One depth-tested layer of water: the nearest wave already hides the ones behind it.)
	float pbrReachable = 1.0;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	if (pbrPathLength < 1e6) {
		float pbrBehind = input.WPosition.z + viewDirection.z * pbrPathLength - input.WaveParam.z;
		// Nor is anything far behind: the scene seen through the water holds no water, so past the wave the ray would
		// cross the waves behind, not the horizon and the land beyond them (a line along the horizon through every crest).
		float pbrBeyond = pbrPathLength * length(viewDirection.xy);
		float pbrUnreachable = max(saturate(pbrBehind / (0.25 * PBRWater::UnitsPerMetre)), saturate((pbrBeyond - 5.0 * PBRWater::UnitsPerMetre) / (5.0 * PBRWater::UnitsPerMetre))) *
		                       saturate(pbrSurface.height / (0.05 * PBRWater::UnitsPerMetre));
		// Blended as a share of the light, not of the path: lerp(path, 1e7, w) is ~1e5 units for even w = 0.01, and
		// everything just under a raised surface (a body floating in it, a leg) turned opaque body colour.
		pbrReachable = 1.0 - pbrUnreachable;
	}
#							endif

	// Clarity: drifting sediment patches, wave-stirred shallows, river silt, wading.
	float pbrFlow = saturate((1.0 - input.WaveParam.w) / max(PBRWater::Foam1.w, 0.01));
	float pbrColumnDepth = min(pbrSurface.depth, pbrBottomDepth);
	PBRWater::Turbidity pbrTurbidity = PBRWater::GetTurbidity(input.WaveParam.xyz, input.SurfaceNoise.x, pbrColumnDepth, PBRWater::FetchHeightRatio(pbrSurface.fetchMetres), input.WaveParam.w, pbrSurface.breakStage, pbrFlow);
	pbrSurface.turbidity = PBRWater::ColumnTurbidity(pbrTurbidity, pbrColumnDepth);
	PBRWater::WaterOptics pbrOptics = PBRWater::GetWaterOptics(PBRWater::Extinction(ShallowColor.xyz, FogParam.z * PBRWater::Light1.x), pbrSurface.turbidity);

	// Body colour: the water form's, shifted towards the sediment colour as sediment takes over.
	float3 pbrBodyLinear = ColorManagement::SceneToLinear(diffuseOutput.refractionDiffuseColor);
	pbrBodyLinear = PBRWater::BodyColour(pbrBodyLinear, PBRWater::SedimentBodyColour(pbrBodyLinear), pbrOptics);

	// Transmission (Beer-Lambert along the refracted path, in linear light). What is seen through the water
	// was itself lit through the water above it, so light reaching the bottom is attenuated by K_d.
	float pbrSunShare = Color::RGBToLuminance(dirColor) / max(Color::RGBToLuminance(dirColor + ambientColor), 1e-5);
	// Sunlight that reached this water through a crest lost what that water took (K_d: what it scatters forwards still
	// arrives): the body colour's and the bed's share of it, and the foam's.
	float3 pbrWaveSunLight = exp(-pbrOptics.downwelling * pbrWaveSun.path);
	float3 pbrWaveSunShare = lerp(1.0, pbrWaveSunLight, pbrSunShare);
	pbrBodyLinear *= pbrWaveSunShare;
#							if defined(REFRACTIONS)
	float3 pbrTransmittance = exp(-pbrOptics.extinction * pbrWaterPath) * pbrReachable;
	float3 pbrBottomLight = PBRWater::DownwellingTransmittance(pbrOptics, pbrBottomDepth, PBRWater::RefractedSunDirection(SunDir.xyz).z, pbrSunShare);
#							else
	float3 pbrTransmittance = 0.0;
	float3 pbrBottomLight = 1.0;
#							endif
	pbrBottomLight *= pbrWaveSunShare;
	float3 pbrTransmitted = ColorManagement::LinearToScene(ColorManagement::SceneToLinear(diffuseOutput.refractionColor) * pbrTransmittance * pbrBottomLight +
														   pbrBodyLinear * (1.0 - pbrTransmittance));

	// Sunlight transmitted through thin wave crests, coloured by the water it crosses.
	float pbrCrest = saturate(input.ShoreState.z / max(PBRWater::Optics0.w * 0.5, 1.0));
	// Only crests glow (WaveTranslucency scales by the height within the crest): the troughs skip it.
	[branch] if (pbrCrest > 0.0)
	{
		float3 pbrSunLight, pbrSkyLight;
		ShadowSampling::ExtractLighting(1.0.xxx, pbrSunLight, pbrSkyLight);
		float pbrCrestThickness = max(PBRWater::Optics0.w * 0.6, 0.4 * PBRWater::UnitsPerMetre);
		// Through the crest's real thickness towards the sun where the heightfield reaches.
		float pbrCrestSunPath = pbrWaveSun.valid ? pbrWaveSun.path : -1.0;
		pbrTransmitted += pbrSunLight * dirShadow * PBRWater::WaveTranslucency(normal, -viewDirection, SunDir.xyz, pbrCrest, pbrOptics, pbrCrestThickness, pbrCrestSunPath) * PBRWater::Light0.y;
	}

	// The waterline per pixel: the water's surface here (the waves at this pixel, or the surf simulation's sheet)
	// against the ground seen through it, so the edge follows the true bed - and rocks, piles and legs standing in the
	// water - instead of the mesh's triangles cutting through the terrain. Over the last few centimetres the surface
	// (reflection, body colour) fades into the ground; foam lies on the water, so it stays on the thin sheet.
	float pbrBed = pbrSurface.elevation;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	if (pbrPathLength < 1e6) {
		// Only ground just behind the surface is the bed under it. Seen through a wave that rises in front of it, an
		// iceberg, a mountain or the horizon far behind stood near or above the crest and the pixel was taken for (nearly)
		// dry ground: the wave showed it. Further out, the terrain under the pixel.
		if (pbrPathLength * length(viewDirection.xy) < 3.0 * PBRWater::UnitsPerMetre)
			pbrBed = input.WPosition.z + viewDirection.z * pbrPathLength - input.WaveParam.z;
	}
#							endif
	float pbrFilm = pbrSurface.height - pbrBed;
	// With refractions, dry pixels show the ground instead (pbrEdge = 0): the stencil pass put them in the water mask,
	// and ISWaterBlend composites whatever this target holds there - after a discard, an old frame's water.
	// The last centimetres of a sheet on sand are no smooth mirror: grains and ripple marks break through, the meniscus
	// clings to them, and the water reads as glossy wet sand that turns into water as it deepens. Faded over 6 cm, the
	// edge of a backwash pool on a gentle beach was a hard glassy rim a slab's thickness above the sand.
	float pbrEdge = smoothstep(0.0, 0.12 * PBRWater::UnitsPerMetre, pbrFilm);
	pbrEdge *= pbrEdge * (3.0 - 2.0 * pbrEdge);

	// Foam (PBRWater.hlsli): the shore waves', the swash's and the open sea's
	// (pbrSurface.fftFoam, the accumulated cascade foam of GarrettGunnell/Water), drawn together below.
	// Nothing is drawn from what the depth buffer shows through the water:
	// that put foam in the shape of whatever stood in it (legs, rocks) and of the shoreline seen from below. It is drawn
	// with the foam texture (Crest's black-point fade) where the surface is, a diffuse scatterer
	// (Dupuy & Bruneton 2012) lit by the sun and the sky like any surface.
	// The surf simulation's water foams of its own: each bore's foam, made at its front, carried in and up the beach with
	// its water and thinning as the sheet spreads and drains (SwashStepCS). It is drawn where the simulation is the water.
	float pbrTipGap = pbrSurface.swashDepth > PBRWater::SwashMinDepth ? pbrSurface.swashSurface - pbrBed : -PBRWater::UnitsPerMetre;
	// Antialiased over the pixel: a hard step drew the uprush's edge in stairs.
	float pbrSheet = saturate(pbrTipGap / max(fwidth(pbrTipGap), 0.01) + 0.5) * pbrSurface.simShare;
	// One foam, one draw. Its coverage is the most of: the open sea's lace (fftFoam) and the breaking wave's white water
	// (breakerFoam: the folding of its surface and the profile's foam, made of the same lace), and the flat white water
	// the lace does not reach (the bore once it lies near horizontal, what the surf left where it lies flat, the surf
	// simulation's). All of it takes the foam texture's bubbles where the surface is flat; on steep faces (crest, lip,
	// tumbling white water) the pattern would stretch, so it fades to the coverage there, as it does with distance. Drawn
	// apart (the lace raw, the shore foam patterned) the sea, breakers and shore read as three different foams.
	// The pattern holds still where the surface is, projected from above: the foam moves over it, as its coverage.
	// SurfaceFoam takes screen derivatives and skips the pattern where it gets no foam, so it is called unconditionally.
	float3 pbrFoamPosition = input.WPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
	// Near the waterline the mesh lies on the sand above the water (DisplaceSurface) and climbs every ledge in the bed;
	// the water seen there is further along the view ray, where it reaches the water's surface. Projected from the mesh,
	// the pattern ran down the steep faces over the ledges in vertical streaks, a step along each one.
	float pbrMeshAbove = (input.WPosition.z - input.WaveParam.z) - pbrSurface.height;
	if (pbrMeshAbove > 0.0 && viewDirection.z < -0.01)
		pbrFoamPosition += viewDirection * min(pbrMeshAbove / -viewDirection.z, pbrPathLength);
	float pbrFlat = smoothstep(0.80, 0.95, pbrSurface.waveNormal.z);
	float pbrBoreFlat = pbrSurface.bore * pbrFlat;
	// The swash's edge carries a lace of foam: what each uprush pushes ahead of it and the backwash leaves along the
	// water's edge, where the sheet thins over the sand. Without it the water past the last of the bores' foam ended in a
	// clean line on the sand.
	float pbrEdgeLace = 0.4 * pbrSurface.simShare * (1.0 - smoothstep(0.02 * PBRWater::UnitsPerMetre, 0.10 * PBRWater::UnitsPerMetre, pbrFilm));
	float pbrFlatCoverage = max(max(max(pbrSurface.foam * pbrBoreFlat, pbrSurface.surfFoam * pbrFlat * (1.0 - pbrSurface.simShare)), pbrSurface.swashFoam * pbrSheet), pbrEdgeLace);
	// The surf's leftover foam on faces that are not flat is in pbrSurface.breakerFoam, drawn with the sea's (BreakerFoamDrawn).
	// The foam settings reach all of it: Foam Amount and Foam Coverage scale the shore's coverages as they do the sea's
	// (Foam1.z; the breakers' in BreakerFoamDrawn), and the distance fade is applied to the whole.
	pbrFlatCoverage = saturate(pbrFlatCoverage * PBRWater::Foam1.z);
	float pbrFoamCoverage = max(max(saturate(pbrSurface.fftFoam), pbrSurface.breakerFoam), pbrFlatCoverage) * PBRWater::FoamFade(length(input.WaveParam.xyz));
	float pbrFoamPattern = PBRWaterFoam::SurfaceFoam(pbrFoamPosition.xy, pbrFoamCoverage, PBRWater::FoamTile(), PBRWater::Tune5.y, PBRWater::OceanSampler, pbrFlat);

	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float3 finalColorPreFog = lerp(pbrTransmitted, specularColor, specularFraction);
	float3 pbrSunGlint = sunColor * (depthControl.w * pbrWaveSun.visible);
#							if !defined(REFRACTIONS)
	if (pbrFilm <= 0.0)
		discard;
#							endif
	pbrSunGlint *= pbrEdge;
	float pbrFilmFoam = smoothstep(0.0, 0.04 * PBRWater::UnitsPerMetre, pbrFilm);
	float pbrFoam = pbrFoamPattern * pbrFilmFoam;
	// Lit only where there is foam: most of the water has none.
	[branch] if (pbrFoam > 0.0)
	{
		// Whitewater is a layer of bubbles light scatters through, not a surface: it is lit about the normal bent halfway up,
		// with the sun wrapping around it. Lit about the water's own normal, the foam on the underside of a curling lip (facing
		// down, seeing neither the sun nor the sky) drew a dark line along every curling crest.
		float3 pbrFoamNormal = PBRWater::SafeNormalize(normal + float3(0.0, 0.0, 1.0), float3(0.0, 0.0, 1.0));
		float3 pbrFoamIrradiance = ColorManagement::SceneToLinear(ShadowSampling::GetDirectionalLighting()) * (dirShadow * saturate((dot(pbrFoamNormal, SunDir.xyz) + 0.5) / 1.5)) * pbrWaveSunLight;
		float3 pbrSkyIrradiance = ColorManagement::SceneToLinear(ShadowSampling::GetAmbientLighting());
#							if defined(SKYLIGHTING)
		pbrSkyIrradiance *= Skylighting::EvaluateDiffuse(skylightingSH, pbrFoamNormal, Skylighting::GetFadeOutFactor(input.WPosition.xyz));
#							endif
		float3 pbrFoamColor = ColorManagement::LinearToScene(PBRWaterFoam::Albedo * (pbrFoamIrradiance + pbrSkyIrradiance));
		finalColorPreFog = lerp(finalColorPreFog, pbrFoamColor, pbrFoam);
	}
	// Foam scatters the glint with the rest of the light that reaches it.
	finalColorPreFog += pbrSunGlint * (1.0 - pbrFoam);
#						else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
	float waterOpacity = diffuseOutput.refractionMul;
	float3 finalColorPreFog;
	if (shoreFadedSurface) {
		// Reflection takes its share first; the rest splits between the water color and the see-through refraction.
		// The sun is a reflection too, so it fades in with the surface instead of rimming see-through water.
		// Divided by the opacity because the composite below lerps from the refraction by it; the floor keeps that
		// continuous and moves at most 0.1% of the weight off the refraction.
		float reflectionWeight = specularFraction * surfaceMul;
		waterOpacity = max(lerp(diffuseOutput.refractionMul, 1.0, reflectionWeight), 1e-3);
		finalColorPreFog = (diffuseOutput.refractionDiffuseColor * (diffuseOutput.refractionMul * (1.0 - reflectionWeight)) + specularColor * reflectionWeight + sunColor * (sunMul * surfaceMul)) / waterOpacity;
	} else {
		finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;
	}
#						endif

#						if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled) {
		const float3 waterViewDir = normalize(input.WPosition.xyz);
		const float waterDist = length(input.WPosition.xyz);
		const float4 apSample = PhysSky::SampleAp(waterViewDir, input.HPosition.xy, waterDist, ApSampler);
		finalColorPreFog = finalColorPreFog * apSample.w + apSample.xyz;
		if (!SharedData::PostWaterComposite)
			finalColorPreFog = PhysSky::CompositeVolumetricClouds(finalColorPreFog, input.HPosition.xy);
	}
#						endif

	if (!SharedData::PostWaterComposite) {
#						if !defined(UNIFIED_WATER)
		float fogDistanceFactor = input.FogParam.w;
		float3 preFogColor = input.FogParam.xyz;
#						else
		float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
		float3 preFogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogDistanceFactor);
#						endif

#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor, input.WPosition.xyz);
		}
#						endif
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, preFogColor, float4(input.HPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, input.HPosition.z, 1));
			if (ExponentialHeightFog::ShouldDisableVanillaFog()) {
				preFogColor = exponentialHeightFog.xyz;
				preFogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, preFogColor, exponentialHeightFog.w);
			} else {
				preFogColor *= GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
				float3 expFogColor = exponentialHeightFog.xyz * GetWaterFogFade();
				finalColorPreFog = lerp(finalColorPreFog, expFogColor, exponentialHeightFog.w);
			}
		} else {
			preFogColor *= GetWaterFogFade();
			finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
		}
#						else
		preFogColor *= GetWaterFogFade();

		finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);
#						endif
	}

#						if defined(PBR_WATER)
	float3 finalColor = finalColorPreFog;
	// Debug views go through the same per-pixel waterline as the water: drawn over the whole coverage, they showed the
	// coarser, triangle-shaped region the passes keep (where the ground shows) as hard corners that are never on screen.
	if (PBRWater::Draw0.z > 0.5)
		finalColor = PBRWater::DebugView((uint)PBRWater::Draw0.z, pbrSurface, pbrFoam, input.ShoreState.z);
	// The waves' sun shadows: red the sun's glint hidden, green the water the sunlight crosses (2 m), blue off the heightfield.
	if ((uint)PBRWater::Draw0.z == PBRWater::DebugWaveSun)
		finalColor = float3(1.0 - pbrWaveSun.visible, saturate(pbrWaveSun.path / (2.0 * PBRWater::UnitsPerMetre)), pbrWaveSun.valid ? 0.0 : 0.3);
#							if defined(REFRACTIONS)
	// The thinnest water shows the ground it covers (as it is, already lit and fogged).
	float3 pbrGround = RefractionTex.Sample(RefractionSampler, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw)).xyz;
	finalColor = lerp(pbrGround, finalColor, pbrEdge);
#							endif
#						else
	float3 refractionColor = diffuseOutput.refractionColor;

	if (!SharedData::PostWaterComposite) {
		float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
		float3 fogColor = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogFactor);
#						if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled && ExponentialHeightFog::ShouldDisableVanillaFog()) {
			fogFactor = 0;
		}
#						endif
#						if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			fogColor = ImageBasedLighting::GetFogIBLColor(fogColor, input.WPosition.xyz);
		}
#						endif
		refractionColor = lerp(refractionColor, fogColor, fogFactor);
	}

	float3 finalColor = lerp(refractionColor, finalColorPreFog, waterOpacity);
#						endif
#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif
#					endif

#				endif
#			endif
#			if defined(PBR_WATER)
	if (PBRWater::Draw0.y > 0.5) {
		float wire = PBRWater::WireframeMask(input.Barycentric);
		float3 background = PBRWater::Draw0.y > 1.5 ? 0.0 : finalColor;
		finalColor = lerp(background, float3(0.1, 1.0, 0.6), wire);
	}
#			endif
	psout.Lighting = float4(PreExposure::EncodeScene(finalColor), isSpecular);
#			if defined(PBRW_DEFERRED)
	// The half-rate pass's target carries which of its 2x2 pixels was shaded (DeferredFar.hlsl), not the blend's alpha:
	// only the surface technique draws it, whose alpha is 0 (isSpecular).
	if (PBRWaterDeferred::HalfRateKey > 0.0)
		psout.Lighting.w = PBRWaterDeferred::HalfRateKey;
#			endif
#		endif

#		if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
#			if defined(PBR_WATER)
	// The mask also marks the water for ISWaterBlend and the upscaler: on a wave face seen edge-on or from behind
	// (the coarse derivatives span a crest) it rounded to 0 in the UNORM target and punched 2x2 holes in the water.
	VdotN = max(abs(VdotN), 2.0 / 255.0);
#			endif
	psout.WaterMask = float4(0, 0, VdotN, 0);

	float4 previousPosition = input.PreviousWorldPosition;
#			if defined(PBR_WATER)
	// The surf's foam, read at the displaced position (this pass has no undisplaced one); the foam grid's texels are
	// coarser than the difference almost everywhere.
	// The foam's pattern holds still where the surface is, so where it covers the water it does not move sideways with the
	// mesh: temporal AA and the upscalers reprojected it from where the mesh was and smeared it.
	// Last frame's position is relative to last frame's camera.
	float3 pbrFoamPrevious = input.WorldPosition.xyz + (FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz);
	// Only where the water barely moves: the leftover foam lies thickest just behind a breaker, and a crest running in at
	// metres a second, its white water moving with it, was given the still foam's motion. TAA reprojected it from the wrong
	// place and smeared it along the break.
	float pbrSurfaceMotion = length(previousPosition.xyz - pbrFoamPrevious);
	float pbrStill = 1.0 - smoothstep(2.0, 6.0, pbrSurfaceMotion);
	[branch] if (pbrStill > 0.0)
	{
		float pbrMotionFoam = PBRWater::ShoreFoamAt(input.WorldPosition.xy + FrameBuffer::CameraPosAdjust.xy);
		previousPosition.xy = lerp(previousPosition.xy, pbrFoamPrevious.xy, saturate(2.0 * pbrMotionFoam) * pbrStill);
	}
#			endif
	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, previousPosition);
#			if defined(PBR_WATER)
	PBRWaterDeferred::Record pbrRecord = PBRWaterDeferred::Pack(input.HPosition.z, input.DeferredWave, input.ShoreState.z, input.DeferredNoise, input.DeferredUV, input.DeferredAlpha);
	psout.DeferredTag = pbrRecord.tag;
	psout.DeferredWaves = pbrRecord.waves;
	psout.DeferredNoise = pbrRecord.noise;
	psout.DeferredUV = pbrRecord.uv;
#			endif
#		endif

	return psout;
}

#	endif

#endif
