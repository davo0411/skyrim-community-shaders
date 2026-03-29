#ifndef __WATER_TESSELLATION_HLSLI__
#define __WATER_TESSELLATION_HLSLI__

#include "PBRWater/WaterDepthEstimation.hlsli"
#include "Common/FrameBuffer.hlsli"

// ============================================================================
// WATER TESSELLATION SYSTEM
// ============================================================================
//
// Pipeline: VS -> HS -> Tessellator -> DS -> PS
//
// SEAM PREVENTION: Each outer level is a symmetric function of that edge’s two endpoints only
// (same midpoint → same factor on both triangles). Dynamic tess scales by a wave crest proxy
// evaluated only at the edge midpoint (shared edge → identical midpoint → identical scale).

// Fixed tuning (not exposed to CPU)
static const float kTessOffscreenScale = 0.55f;           // degenerate / behind-camera w
static const float kTessFarOffscreenScale = 0.22f;        // clearly outside NDC (generous margin — avoids crawl)
static const float kTessFrustumMarginNDC = 1.35f;         // |ndc| beyond ~viewport; conservative vs tight clip
static const float kTessUltraFarDistanceMul = 1.4f;       // beyond max tess distance × this → extra-low triangles
static const float kTessUltraFarFactorScale = 0.42f;      // scales min factor in ultra-far tier (pre-wave)
static const float kTessDynamicMulMin = 2.0f / 6.0f;      // min mult vs max (flat between waves vs crest)
static const float kTessDynamicMulMax = 1.0f;             // max mult at |sin(phase)|-weighted crests

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
	float minDistSq = 0.0f;  // cbuffer min distance removed from UI; treat full tess from camera
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
// SCREEN-SPACE TESS SCALE (patch-level, uniform on all edges — seam-safe)
// ============================================================================
// Cheap frustum test in clip space. Does not sample the depth buffer (would
// require HS resource binding); large savings come from off-screen water.

float GetPatchScreenTessScale(float3 centerWorld, uint eyeIndex)
{
	float4 clip = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(centerWorld, 1.0f));
	if (clip.w <= 1e-4f) {
		return kTessOffscreenScale;
	}
	float rw = rcp(clip.w);
	float nx = clip.x * rw;
	float ny = clip.y * rw;
	if (abs(nx) > kTessFrustumMarginNDC || abs(ny) > kTessFrustumMarginNDC) {
		return kTessFarOffscreenScale;
	}
	return 1.0f;
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

float CombineEdgeTess(float distBased, float screenScale)
{
	float t = distBased * screenScale;
	float lo = max(1.0f, TessellationMinFactor * screenScale);
	t = max(t, lo);
	return min(t, TessellationMaxFactor);
}

#if defined(HSHADER)
// GerstnerWaves.hlsli must be included before this file (Water.hlsl hull). Cheap proxy for
// primary swells only — matches crest/trough emphasis (|sin(phase)|), not full cell synthesis.
float WaveTessEdgeDynamicMul(float2 worldXYAbs, float timeSeconds)
{
	if (WaveIntensity <= 0.001f) {
		return kTessDynamicMulMin;
	}

	const float gGame = UW_GRAVITY * M_TO_GAME_UNIT;
	float wi = WaveIntensity * WaveAmplitude;

	float acc = 0.0f;
	float wsum = 0.0f;

	{
		float wlGame = max(Wave1Wavelength, 0.25f) * M_TO_GAME_UNIT;
		float k = UW_TWO_PI / wlGame;
		float omega = sqrt(gGame * k) * WaveSpeed;
		float2 dir = float2(cos(Wave1AngleOffset), sin(Wave1AngleOffset));
		float phase = k * dot(dir, worldXYAbs) - omega * timeSeconds;
		float s = abs(sin(phase));
		float ampG = max(Wave1Amplitude, 0.0f) * M_TO_GAME_UNIT;
		float wt = ampG * k * wi * (0.2f + saturate(Wave1Steepness));
		acc += wt * s;
		wsum += wt;
	}
	{
		float wlGame = max(Wave2Wavelength, 0.25f) * M_TO_GAME_UNIT;
		float k = UW_TWO_PI / wlGame;
		float omega = sqrt(gGame * k) * WaveSpeed;
		float2 dir = float2(cos(Wave2AngleOffset), sin(Wave2AngleOffset));
		float phase = k * dot(dir, worldXYAbs) - omega * timeSeconds;
		float s = abs(sin(phase));
		float ampG = max(Wave2Amplitude, 0.0f) * M_TO_GAME_UNIT;
		float wt = ampG * k * wi * (0.2f + saturate(Wave2Steepness));
		acc += wt * s;
		wsum += wt;
	}
	{
		float wlGame = max(Wave3Wavelength, 0.25f) * M_TO_GAME_UNIT;
		float k = UW_TWO_PI / wlGame;
		float omega = sqrt(gGame * k) * WaveSpeed;
		float2 dir = float2(cos(Wave3AngleOffset), sin(Wave3AngleOffset));
		float phase = k * dot(dir, worldXYAbs) - omega * timeSeconds;
		float s = abs(sin(phase));
		float ampG = max(Wave3Amplitude, 0.0f) * M_TO_GAME_UNIT;
		float wt = ampG * k * wi * (0.2f + saturate(Wave3Steepness));
		acc += wt * s;
		wsum += wt;
	}

	float crestMetric = saturate(acc / max(wsum, 1e-5f));
	return lerp(kTessDynamicMulMin, kTessDynamicMulMax, crestMetric);
}

float2 WaveTessEdgeMidWorldXY(float3 edgeMidCamRel)
{
	return edgeMidCamRel.xy + FrameBuffer::CameraPosAdjust[0].xy;
}
#endif // HSHADER

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
	float ultraMaxDistSq = maxDistSq * (kTessUltraFarDistanceMul * kTessUltraFarDistanceMul);

	const uint eyeIdx = 0u;
	float screenScale = GetPatchScreenTessScale(patchCenter, eyeIdx);

#if defined(HSHADER)
	// Backface: discard patches facing away from the camera (camera at origin in cam-relative space).
	float3 e1 = p1 - p0;
	float3 e2 = p2 - p0;
	float3 faceN = cross(e1, e2);
	float faceNLen = length(faceN);
	if (faceNLen < 1e-15f) {
		output.EdgeTess[0] = output.EdgeTess[1] = output.EdgeTess[2] = 0.0f;
		output.InsideTess = 0.0f;
		return output;
	}
	faceN *= rcp(faceNLen);
	float centerLenSq = max(centerDistSq, 1e-12f);
	float3 viewTowardCamera = (-patchCenter) * rsqrt(centerLenSq);
	if (dot(faceN, viewTowardCamera) <= 0.0f) {
		output.EdgeTess[0] = output.EdgeTess[1] = output.EdgeTess[2] = 0.0f;
		output.InsideTess = 0.0f;
		return output;
	}

	float waveTimeHull = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
#endif

	// Beyond normal max distance: skip wave dynamic mult — fewer triangles, no seam mismatch vs edges
	if (centerDistSq >= ultraMaxDistSq) {
		float ultraBase = max(1.0f, TessellationMinFactor * kTessUltraFarFactorScale);
		float t = CombineEdgeTess(ultraBase, screenScale);
		output.EdgeTess[0] = output.EdgeTess[1] = output.EdgeTess[2] = t;
		output.InsideTess = t;
		return output;
	}

	// Distance-only: patches at/outer band of max tess distance get minimum factor (no wave mult)
	if (centerDistSq >= maxDistSq) {
		float t = CombineEdgeTess(TessellationMinFactor, screenScale);
		output.EdgeTess[0] = t;
		output.EdgeTess[1] = t;
		output.EdgeTess[2] = t;
		output.InsideTess = t;
		return output;
	}

	// Edge 0 = vertices 1-2, Edge 1 = vertices 2-0, Edge 2 = vertices 0-1
	float d0 = CalculateEdgeTessellation(p1, p2);
	float d1 = CalculateEdgeTessellation(p2, p0);
	float d2 = CalculateEdgeTessellation(p0, p1);

#if defined(HSHADER)
	float3 mid0 = (p1 + p2) * 0.5f;
	float3 mid1 = (p2 + p0) * 0.5f;
	float3 mid2 = (p0 + p1) * 0.5f;
	float wMul0 = WaveTessEdgeDynamicMul(WaveTessEdgeMidWorldXY(mid0), waveTimeHull);
	float wMul1 = WaveTessEdgeDynamicMul(WaveTessEdgeMidWorldXY(mid1), waveTimeHull);
	float wMul2 = WaveTessEdgeDynamicMul(WaveTessEdgeMidWorldXY(mid2), waveTimeHull);

	output.EdgeTess[0] = min(CombineEdgeTess(d0, screenScale) * wMul0, TessellationMaxFactor);
	output.EdgeTess[1] = min(CombineEdgeTess(d1, screenScale) * wMul1, TessellationMaxFactor);
	output.EdgeTess[2] = min(CombineEdgeTess(d2, screenScale) * wMul2, TessellationMaxFactor);
#else
	output.EdgeTess[0] = CombineEdgeTess(d0, screenScale);
	output.EdgeTess[1] = CombineEdgeTess(d1, screenScale);
	output.EdgeTess[2] = CombineEdgeTess(d2, screenScale);
#endif
	output.InsideTess = (output.EdgeTess[0] + output.EdgeTess[1] + output.EdgeTess[2]) * 0.333333f;

	return output;
}

// ============================================================================
// DOMAIN SHADER IMPLEMENTATION
// ============================================================================
// Interpolates tessellated vertices and applies Gerstner wave displacement.
// Depth / shore must run for every vertex: shallow water often extends far from the
// camera along the shore (large horizontal distance) while still needing depthBlend
// to attenuate cell waves — camera-distance LOD incorrectly treated that as deep water.

VS_OUTPUT DomainShaderImpl(HS_CONSTANT_OUTPUT patchConst, float3 bary, const OutputPatch<VS_OUTPUT, 3> patch, uint eyeIndex)
{
	VS_OUTPUT output;

	// Barycentric interpolation of position
	float4 interpWPosition = patch[0].WPosition * bary.x + patch[1].WPosition * bary.y + patch[2].WPosition * bary.z;
	float4 interpTexCoord1 = patch[0].TexCoord1 * bary.x + patch[1].TexCoord1 * bary.y + patch[2].TexCoord1 * bary.z;
#if defined(SPECULAR) || defined(UNDERWATER) || defined(SIMPLE)
	float4 interpTexCoord2 = patch[0].TexCoord2 * bary.x + patch[1].TexCoord2 * bary.y + patch[2].TexCoord2 * bary.z;
#endif

// PBR_WATER: VS always runs Gerstner when tess is off; domain must match. UNIFIED_WATER alone is optional.
#if defined(PBR_WATER)
	float2 waveWorldPos = interpWPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;

	float waveTimeSeconds = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
	float waveDayPhase = ComputeWaveDayPhase(GameTimeHours);

	float cameraDistSqDS = dot(interpWPosition.xyz, interpWPosition.xyz);
	float cameraDistDS = sqrt(cameraDistSqDS);

	float3 absoluteWorldPos = interpWPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float2 shoreDirDS = float2(0.0f, 0.0f);
	float shoreGradDS = 0.0f;
	float estimatedDepthDS;
	if (cameraDistSqDS > kWaterTerrainGradientDetailDistSq) {
		estimatedDepthDS = ComputeShoreDepthOnly(
			absoluteWorldPos,
			float2(TerrainScaleX, TerrainScaleY),
			float2(TerrainOffsetX, TerrainOffsetY),
			TerrainZRangeMin,
			TerrainZRangeMax);
	} else {
		estimatedDepthDS = ComputeShoreDirection(
			absoluteWorldPos,
			float2(TerrainScaleX, TerrainScaleY),
			float2(TerrainOffsetX, TerrainOffsetY),
			TerrainZRangeMin,
			TerrainZRangeMax,
			shoreDirDS,
			shoreGradDS);
	}

	DepthEstimationDebug depthDebug;
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

#if defined(UNIFIED_WATER) || defined(PBR_WATER)
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
