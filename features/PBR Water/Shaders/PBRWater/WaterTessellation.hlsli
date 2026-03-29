#ifndef __WATER_TESSELLATION_HLSLI__
#define __WATER_TESSELLATION_HLSLI__

#include "PBRWater/WaterDepthEstimation.hlsli"

// ============================================================================
// WATER TESSELLATION SYSTEM
// ============================================================================
//
// Pipeline: VS -> HS -> Tessellator -> DS -> PS
//
// SEAM PREVENTION: Edge factors use ONLY the two shared vertices.
// No view-dependent or wave-dependent scaling on edges — guarantees matching
// factors across draw call boundaries via IEEE 754 commutativity.

cbuffer TessellationParams : register(b9)
{
	float TessellationMinDistance;
	float TessellationMaxDistance;
	float TessellationMinFactor;
	float TessellationMaxFactor;
	float TessCameraWorldPosX;
	float TessCameraWorldPosY;
	float TessCameraWorldPosZ;
	float DetailHeightScale;
}

struct HS_CONSTANT_OUTPUT
{
	float EdgeTess[3] : SV_TessFactor;
	float InsideTess : SV_InsideTessFactor;
};

// ============================================================================
// DISTANCE-BASED TESSELLATION FACTOR
// ============================================================================

float CalculateDistanceTessellation(float distSq)
{
	float minDistSq = TessellationMinDistance * TessellationMinDistance;
	float maxDistSq = TessellationMaxDistance * TessellationMaxDistance;

	if (distSq >= maxDistSq)
		return TessellationMinFactor;
	if (distSq <= minDistSq)
		return TessellationMaxFactor;

	float t = (distSq - minDistSq) * rcp(maxDistSq - minDistSq);
	// smoothstep on squared distance — avoids sqrt while giving smooth falloff
	t = t * t * (3.0f - 2.0f * t);

	return lerp(TessellationMaxFactor, TessellationMinFactor, t);
}

// ============================================================================
// EDGE TESSELLATION — DETERMINISTIC & SEAM-FREE
// ============================================================================
// Uses squared distances to avoid sqrt. Symmetric in (p0, p1).

float CalculateEdgeTessellation(float3 p0, float3 p1)
{
	float3 edgeMid = (p0 + p1) * 0.5f;
	float distSq = dot(edgeMid, edgeMid);  // Camera-relative: length² = distance²

	return CalculateDistanceTessellation(distSq);
}

// ============================================================================
// HULL SHADER PATCH CONSTANT FUNCTION
// ============================================================================

HS_CONSTANT_OUTPUT PatchConstantFunc(InputPatch<VS_OUTPUT, 3> patch, uint patchID : SV_PrimitiveID)
{
	HS_CONSTANT_OUTPUT output;

	float3 p0 = patch[0].WPosition.xyz;
	float3 p1 = patch[1].WPosition.xyz;
	float3 p2 = patch[2].WPosition.xyz;

	float3 patchCenter = (p0 + p1 + p2) * 0.333333f;
	float centerDistSq = dot(patchCenter, patchCenter);
	float maxDistSq = TessellationMaxDistance * TessellationMaxDistance;

	// Distance-only cull: patches well beyond max distance get minimum factor
	if (centerDistSq >= maxDistSq) {
		output.EdgeTess[0] = TessellationMinFactor;
		output.EdgeTess[1] = TessellationMinFactor;
		output.EdgeTess[2] = TessellationMinFactor;
		output.InsideTess = TessellationMinFactor;
		return output;
	}

	// Edge tessellation — purely distance-based, deterministic
	// Edge 0 = vertices 1-2, Edge 1 = vertices 2-0, Edge 2 = vertices 0-1
	output.EdgeTess[0] = CalculateEdgeTessellation(p1, p2);
	output.EdgeTess[1] = CalculateEdgeTessellation(p2, p0);
	output.EdgeTess[2] = CalculateEdgeTessellation(p0, p1);

	// Inside factor: average of edges (no curvature boost — saves 3× sincos per patch)
	output.InsideTess = (output.EdgeTess[0] + output.EdgeTess[1] + output.EdgeTess[2]) * 0.333333f;

	return output;
}

// ============================================================================
// DOMAIN SHADER IMPLEMENTATION
// ============================================================================
// Interpolates tessellated vertices and applies Gerstner wave displacement.
// Distance LOD: skips depth estimation + shore waves for far vertices.

VS_OUTPUT DomainShaderImpl(HS_CONSTANT_OUTPUT patchConst, float3 bary, const OutputPatch<VS_OUTPUT, 3> patch, uint eyeIndex)
{
	VS_OUTPUT output;

	// Barycentric interpolation of position
	float4 interpWPosition = patch[0].WPosition * bary.x + patch[1].WPosition * bary.y + patch[2].WPosition * bary.z;
	float4 interpTexCoord1 = patch[0].TexCoord1 * bary.x + patch[1].TexCoord1 * bary.y + patch[2].TexCoord1 * bary.z;
#if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE)
	float4 interpTexCoord2 = patch[0].TexCoord2 * bary.x + patch[1].TexCoord2 * bary.y + patch[2].TexCoord2 * bary.z;
#endif

#if defined(UNIFIED_WATER)
	float2 waveWorldPos = interpWPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;

	float waveTimeSeconds = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
	float waveDayPhase = ComputeWaveDayPhase(GameTimeHours);

	float cameraDistSq = dot(interpWPosition.xyz, interpWPosition.xyz);
	float cameraDistDS = sqrt(cameraDistSq);

	// Distance LOD for depth estimation — skip terrain sampling for far vertices
	float estimatedDepthDS = 1e5f;
	float2 shoreDirDS = float2(0.0f, 0.0f);
	float shoreGradDS = 0.0f;
	DepthEstimationDebug depthDebug;
	depthDebug.depth = 1e5f;
	depthDebug.debugCode = 0.0f;
	depthDebug.terrainZ = 0.0f;
	depthDebug.waterZ = 0.0f;

	// Only sample terrain heightmap for near vertices (where shore detail matters)
	float depthSampleMaxDist = TessellationMaxDistance * 0.5f;
	if (cameraDistSq < depthSampleMaxDist * depthSampleMaxDist) {
		float3 absoluteWorldPos = interpWPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		estimatedDepthDS = ComputeShoreDirection(
			absoluteWorldPos,
			float2(TerrainScaleX, TerrainScaleY),
			float2(TerrainOffsetX, TerrainOffsetY),
			TerrainZRangeMin,
			TerrainZRangeMax,
			shoreDirDS,
			shoreGradDS);

		depthDebug.depth = estimatedDepthDS;
		depthDebug.debugCode = (estimatedDepthDS >= 1e4f) ? 1.0f : 0.0f;
		depthDebug.terrainZ = absoluteWorldPos.z - estimatedDepthDS;
		depthDebug.waterZ = absoluteWorldPos.z;
	}

	WaveSample waveSample = CalculateWaterDisplacement(
		waveWorldPos,
		float2(0.0f, 0.0f),
		float2(0.0f, 0.0f),
		WaveIntensity,
		WaveAmplitude,
		WaveSpeed,
		WaveSteepness,
		waveTimeSeconds,
		waveDayPhase,
		float2(0.0f, 0.0f),
		0.0f,
		false,
		cameraDistDS,
		estimatedDepthDS,
		shoreDirDS,
		shoreGradDS);

	output.DepthDebug = float4(depthDebug.depth, depthDebug.debugCode, depthDebug.terrainZ, depthDebug.waterZ);

	float3 displacedWorldPos = interpWPosition.xyz + waveSample.displacement;
	output.HPosition = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(displacedWorldPos, 1.0f));

	output.WPosition.xyz = displacedWorldPos;
	output.WPosition.w = length(displacedWorldPos);

	float horizontalDisplacement = length(waveSample.displacement.xy);
	output.UnifiedWaveInfo = float4(waveSample.primaryDirection, waveSample.displacement.z, waveSample.shoreInfluence);
	output.UnifiedWaveNormal = float4(waveSample.normal, horizontalDisplacement);
	output.Barycentric = bary;

#if !defined(LOD)
	float4 interpMPosition = patch[0].MPosition * bary.x + patch[1].MPosition * bary.y + patch[2].MPosition * bary.z;
	output.MPosition = float4(interpMPosition.xyz + waveSample.displacement, 1.0f);
#endif
#else
	output.HPosition = patch[0].HPosition * bary.x + patch[1].HPosition * bary.y + patch[2].HPosition * bary.z;
	output.WPosition = interpWPosition;
#endif

	output.TexCoord1 = interpTexCoord1;

#if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE)
	output.TexCoord2 = interpTexCoord2;
#endif

#if !defined(UNIFIED_WATER) && !defined(LOD)
	output.FogParam = patch[0].FogParam * bary.x + patch[1].FogParam * bary.y + patch[2].FogParam * bary.z;
#endif

#if defined(UNIFIED_WATER)
	output.TexCoord3 = patch[0].TexCoord3 * bary.x + patch[1].TexCoord3 * bary.y + patch[2].TexCoord3 * bary.z;
	output.TexCoord4 = patch[0].TexCoord4;
#else
#if defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP))
	output.TexCoord3 = patch[0].TexCoord3 * bary.x + patch[1].TexCoord3 * bary.y + patch[2].TexCoord3 * bary.z;
#endif
#if defined(FLOWMAP)
	output.TexCoord4 = patch[0].TexCoord4;
#endif
#if NUM_SPECULAR_LIGHTS == 0 || defined(SIMPLE)
	output.MPosition = patch[0].MPosition * bary.x + patch[1].MPosition * bary.y + patch[2].MPosition * bary.z;
#endif
#endif

#if defined(STENCIL)
	output.WorldPosition = patch[0].WorldPosition * bary.x + patch[1].WorldPosition * bary.y + patch[2].WorldPosition * bary.z;
	output.PreviousWorldPosition = patch[0].PreviousWorldPosition * bary.x + patch[1].PreviousWorldPosition * bary.y + patch[2].PreviousWorldPosition * bary.z;
#endif

	output.NormalsScale = patch[0].NormalsScale * bary.x + patch[1].NormalsScale * bary.y + patch[2].NormalsScale * bary.z;

#if defined(VR)
	output.ClipDistance = patch[0].ClipDistance * bary.x + patch[1].ClipDistance * bary.y + patch[2].ClipDistance * bary.z;
	output.CullDistance = patch[0].CullDistance * bary.x + patch[1].CullDistance * bary.y + patch[2].CullDistance * bary.z;
#endif

	return output;
}

#endif // __WATER_TESSELLATION_HLSLI__
