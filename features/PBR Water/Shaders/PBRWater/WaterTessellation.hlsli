#ifndef __WATER_TESSELLATION_HLSLI__
#define __WATER_TESSELLATION_HLSLI__

#include "PBRWater/WaterDepthEstimation.hlsli"

// ============================================================================
// WATER TESSELLATION SYSTEM
// ============================================================================
//
// Provides distance-adaptive tessellation with uniform distribution.
//
// Pipeline: VS -> HS -> Tessellator -> DS -> PS
// - Vertex Shader: Outputs control points (no wave displacement)
// - Hull Shader:   Calculates distance-based tessellation factors
// - Tessellator:   Generates vertices based on factors
// - Domain Shader: Applies Gerstner wave displacement with distance LOD
//
// CRITICAL DESIGN PRINCIPLE - Seam Prevention:
// Edge tessellation factors use ONLY the two shared edge vertices as input.
// No wave prediction, no view-dependent scaling, no quantization needed.
// This guarantees that adjacent patches (even across water cell draw call
// boundaries) compute identical factors for shared edges, since:
//   1. Shared edge vertices have identical positions (IEEE 754 commutative add)
//   2. The edge factor function is symmetric in (p0, p1) order
//   3. Only deterministic arithmetic (distance, length) is used
//
// Curvature-adaptive boosting is reserved for the INSIDE tessellation
// factor only, which does not affect edge vertex matching.

// Tessellation parameters passed from CPU (register b9)
cbuffer TessellationParams : register(b9)
{
	float TessellationMinDistance;   // Distance where max tessellation applies
	float TessellationMaxDistance;   // Distance where min tessellation applies
	float TessellationMinFactor;     // Minimum tessellation factor
	float TessellationMaxFactor;     // Maximum tessellation factor (up to 64)
	float TessCameraWorldPosX;       // Backup camera pos (prefer FrameBuffer::CameraPosAdjust)
	float TessCameraWorldPosY;
	float TessCameraWorldPosZ;
	float DetailHeightScale;         // Unused - kept for cbuffer compatibility
}

// Patch constant data (output from hull shader patch constant function)
struct HS_CONSTANT_OUTPUT
{
	float EdgeTess[3] : SV_TessFactor;
	float InsideTess : SV_InsideTessFactor;
};

// ============================================================================
// DISTANCE-BASED TESSELLATION FACTOR
// ============================================================================
// Smooth logarithmic falloff for natural LOD transitions.
// Used for both edge and inside factors.

float CalculateDistanceTessellation(float dist)
{
	if (dist >= TessellationMaxDistance)
		return TessellationMinFactor;
	if (dist <= TessellationMinDistance)
		return TessellationMaxFactor;

	float range = TessellationMaxDistance - TessellationMinDistance;
	float normalizedDist = (dist - TessellationMinDistance) * rcp(range);

	// Quadratic falloff: more natural than exponential, cheaper than exp()
	float t = normalizedDist * normalizedDist;

	return lerp(TessellationMaxFactor, TessellationMinFactor, t);
}

// ============================================================================
// EDGE TESSELLATION - DETERMINISTIC & SEAM-FREE
// ============================================================================
// Computes edge tessellation using ONLY the shared vertex positions.
// This is the critical function for preventing cracks between patches:
// - Uses distance to edge midpoint for LOD
// - Uses screen-space edge length for density
// - NO wave prediction, NO quantization, NO view-dependent scaling
// - Symmetric in (p0, p1): midpoint and length are order-independent

float CalculateEdgeTessellation(float3 p0, float3 p1)
{
	float3 cameraPos = FrameBuffer::CameraPosAdjust[0].xyz;

	// Edge midpoint in absolute world space
	// (p0 + p1) * 0.5 is commutative - same result regardless of argument order
	float3 edgeMid = (p0 + p1) * 0.5f + cameraPos;

	// Distance-based LOD from edge midpoint
	float dist = length(edgeMid - cameraPos);
	float distFactor = CalculateDistanceTessellation(dist);

	// Screen-space edge length scaling
	// Ensures appropriate polygon density regardless of edge orientation
	float edgeLength = length(p1 - p0);
	float screenEdgeLength = edgeLength * rcp(max(dist, 1.0f));
	float screenScale = saturate(screenEdgeLength * 25.0f);
	screenScale = lerp(0.6f, 1.0f, screenScale);

	return clamp(distFactor * screenScale, TessellationMinFactor, TessellationMaxFactor);
}

// ============================================================================
// LIGHTWEIGHT CURVATURE ESTIMATE FOR INSIDE FACTOR
// ============================================================================
// Uses only the first 3 dominant waves for a cheap curvature estimate.
// Only called once per patch for the inside factor (not edges).

float EstimatePatchCurvature(float2 worldPos)
{
	float timeSeconds = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);

	// Only evaluate the 3 dominant waves (highest amplitude, lowest frequency)
	float4 waves[3];
	waves[0] = float4(Wave1Amplitude, Wave1Wavelength, Wave1Steepness, Wave1AngleOffset);
	waves[1] = float4(Wave2Amplitude, Wave2Wavelength, Wave2Steepness, Wave2AngleOffset);
	waves[2] = float4(Wave3Amplitude, Wave3Wavelength, Wave3Steepness, Wave3AngleOffset);

	const float2 baseDir = float2(-0.70710678f, 0.70710678f);
	float curvature = 0.0f;

	[unroll]
	for (int i = 0; i < 3; i++) {
		float amplitude = waves[i].x * WaveAmplitude;
		float wavelength = waves[i].y;
		float steepness = waves[i].z * WaveSteepness;

		if (amplitude < 0.001f || wavelength < 0.1f)
			continue;

		float sinAngle, cosAngle;
		sincos(waves[i].w, sinAngle, cosAngle);
		float2 dir = float2(
			baseDir.x * cosAngle - baseDir.y * sinAngle,
			baseDir.x * sinAngle + baseDir.y * cosAngle);

		float k = 6.28318530f / wavelength;
		float omega = sqrt(9.81f * k);
		float phase = k * dot(dir, worldPos) - omega * timeSeconds * WaveSpeed;

		// Only need |sin(phase)| for curvature magnitude
		float sinP = sin(phase);
		curvature += amplitude * k * k * abs(sinP) * steepness;
	}

	return curvature * WaveIntensity;
}

// ============================================================================
// FRUSTUM CULLING
// ============================================================================
// Conservative test - patches outside frustum get factor 1.0 (not culled to 0
// to maintain edge continuity with visible neighbors).

bool IsPatchInFrustum(float3 p0, float3 p1, float3 p2, uint eyeIndex)
{
	float3 cameraPos = FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	float3 center = (p0 + p1 + p2) * 0.333333f;
	float radius = max(length(p0 - center), max(length(p1 - center), length(p2 - center)));

	// Generous wave displacement margin
	radius += WaveAmplitude * WaveIntensity * 2.0f;

	float4 clipPos = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(center, 1.0f));
	float3 ndc = clipPos.xyz * rcp(clipPos.w);
	float ndcRadius = radius * rcp(clipPos.w);

	// Very generous padding to avoid culling edge patches
	const float pad = 0.5f;
	bool inX = (ndc.x + ndcRadius) >= (-1.0f - pad) && (ndc.x - ndcRadius) <= (1.0f + pad);
	bool inY = (ndc.y + ndcRadius) >= (-1.0f - pad) && (ndc.y - ndcRadius) <= (1.0f + pad);
	bool inZ = (ndc.z + ndcRadius) >= -0.1f && (ndc.z - ndcRadius) <= (1.0f + pad);

	return inX && inY && inZ;
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

	// Frustum culling - off-screen patches get minimal tessellation
	uint eyeIndex = 0;
	if (!IsPatchInFrustum(p0, p1, p2, eyeIndex)) {
		output.EdgeTess[0] = 1.0f;
		output.EdgeTess[1] = 1.0f;
		output.EdgeTess[2] = 1.0f;
		output.InsideTess = 1.0f;
		return output;
	}

	float3 cameraPos = FrameBuffer::CameraPosAdjust[0].xyz;
	float3 patchCenter = (p0 + p1 + p2) * 0.333333f;
	float centerDist = length(patchCenter);  // Camera-relative, so length = distance

	// Early exit for very distant patches
	if (centerDist >= TessellationMaxDistance * 0.95f) {
		float minTess = TessellationMinFactor;
		output.EdgeTess[0] = minTess;
		output.EdgeTess[1] = minTess;
		output.EdgeTess[2] = minTess;
		output.InsideTess = minTess;
		return output;
	}

	// Edge tessellation - purely distance + screen-space based
	// Edge 0 = vertices 1-2, Edge 1 = vertices 2-0, Edge 2 = vertices 0-1
	output.EdgeTess[0] = CalculateEdgeTessellation(p1, p2);
	output.EdgeTess[1] = CalculateEdgeTessellation(p2, p0);
	output.EdgeTess[2] = CalculateEdgeTessellation(p0, p1);

	// Inside factor: average of edges, boosted by curvature
	float baseInsideTess = (output.EdgeTess[0] + output.EdgeTess[1] + output.EdgeTess[2]) * 0.333333f;

	// Lightweight curvature estimate (3 dominant waves only)
	float2 absPatchCenter = patchCenter.xy + cameraPos.xy;
	float curvature = EstimatePatchCurvature(absPatchCenter);

	// Smooth curvature response: sqrt gives more uniform distribution than cubic
	// Range: 1.0x (flat) to 2.0x (peak curvature)
	float curvatureNormalized = saturate(curvature * 0.3f);
	float curvatureBoost = lerp(1.0f, 2.0f, sqrt(curvatureNormalized));

	output.InsideTess = clamp(baseInsideTess * curvatureBoost, TessellationMinFactor, TessellationMaxFactor);

	return output;
}

// ============================================================================
// DOMAIN SHADER IMPLEMENTATION
// ============================================================================
// Interpolates tessellated vertices and applies Gerstner wave displacement.
// Uses distance-based LOD to reduce wave octaves for far vertices.

VS_OUTPUT DomainShaderImpl(HS_CONSTANT_OUTPUT patchConst, float3 bary, const OutputPatch<VS_OUTPUT, 3> patch, uint eyeIndex)
{
	VS_OUTPUT output;

	// Barycentric interpolation of all vertex attributes
	float4 interpHPosition = patch[0].HPosition * bary.x + patch[1].HPosition * bary.y + patch[2].HPosition * bary.z;
	float4 interpWPosition = patch[0].WPosition * bary.x + patch[1].WPosition * bary.y + patch[2].WPosition * bary.z;

	float4 interpTexCoord1 = patch[0].TexCoord1 * bary.x + patch[1].TexCoord1 * bary.y + patch[2].TexCoord1 * bary.z;
#if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE)
	float4 interpTexCoord2 = patch[0].TexCoord2 * bary.x + patch[1].TexCoord2 * bary.y + patch[2].TexCoord2 * bary.z;
#endif

#if defined(UNIFIED_WATER)
	// Absolute world position for wave sampling
	float2 waveWorldPos = interpWPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;

	float waveTimeSeconds = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
	float waveDayPhase = ComputeWaveDayPhase(GameTimeHours);

	float cameraDistDS = length(interpWPosition.xyz);

	// Estimate water depth and shore direction from terrain heightmap
	float3 absoluteWorldPos = interpWPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	DepthEstimationDebug depthDebug;
	float2 shoreDirDS = float2(0.0f, 0.0f);
	float shoreGradDS = 0.0f;
	float estimatedDepthDS = ComputeShoreDirection(
		absoluteWorldPos,
		float2(TerrainScaleX, TerrainScaleY),
		float2(TerrainOffsetX, TerrainOffsetY),
		TerrainZRangeMin,
		TerrainZRangeMax,
		shoreDirDS,
		shoreGradDS);
	
	// Fill debug info
	depthDebug.depth = estimatedDepthDS;
	depthDebug.debugCode = (estimatedDepthDS >= 1e4f) ? 1.0f : 0.0f;
	depthDebug.terrainZ = absoluteWorldPos.z - estimatedDepthDS;
	depthDebug.waterZ = absoluteWorldPos.z;

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

	// Apply wave displacement then transform to clip space
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
	// Non-unified water: simple interpolation
	output.HPosition = interpHPosition;
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
