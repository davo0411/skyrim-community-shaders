#if defined(UNDERWATERMASK)

struct VS_INPUT
{
	float4 Position: POSITION0;
};

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
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
	float4 Color: SV_Target0;
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
#	include "Common/Color.hlsli"

#	define WATER

#	include "Common/SharedData.hlsli"

#if defined(PBR_WATER)
	# include "PBRWater/WaterBRDF.hlsli"
#endif

struct VS_INPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER) || defined(STENCIL) || defined(SIMPLE)
	float4 Position: POSITION0;
#		if defined(NORMAL_TEXCOORD)
	float2 TexCoord0: TEXCOORD0;
#		endif
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif

#	if defined(LOD)
	float4 Position: POSITION0;
#		if defined(VC)
	float4 Color: COLOR0;
#		endif
#	endif
#	if defined(VR)
	uint InstanceID: SV_INSTANCEID;
#	endif  // VR
};

struct VS_OUTPUT
{
#	if defined(SPECULAR) || defined(UNDERWATER)
	float4 HPosition : SV_POSITION0;
#   if !defined(UNIFIED_WATER)
	float4 FogParam : COLOR0;
#	endif
	float4 WPosition : TEXCOORD0;
	float4 TexCoord1 : TEXCOORD1;
	float4 TexCoord2 : TEXCOORD2;
#		if defined(PBR_WATER)
	// PBR Water: always declare TexCoord3/4/MPosition for tessellation pipeline (HS/DS need all fields)
	float4 TexCoord3 : TEXCOORD3;
	nointerpolation float2 TexCoord4 : TEXCOORD4;
	float4 MPosition : TEXCOORD5;
#		else
#			if defined(WADING) || (defined(FLOWMAP) && (defined(REFRACTIONS) || defined(BLEND_NORMALS))) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)) || ((defined(SPECULAR) && NUM_SPECULAR_LIGHTS == 0) && defined(FLOWMAP) /*!defined(NORMAL_TEXCOORD) && !defined(BLEND_NORMALS) && !defined(VC)*/)
	float4 TexCoord3 : TEXCOORD3;
#			endif
#			if defined(FLOWMAP)
	nointerpolation float2 TexCoord4 : TEXCOORD4;
#			endif
#			if NUM_SPECULAR_LIGHTS == 0
	float4 MPosition : TEXCOORD5;
#			endif
#		endif
#	endif

#	if defined(SIMPLE)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
	float4 TexCoord2: TEXCOORD2;
	float4 MPosition: TEXCOORD5;
#	endif

#	if defined(LOD)
	float4 HPosition: SV_POSITION0;
	float4 FogParam: COLOR0;
	float4 WPosition: TEXCOORD0;
	float4 TexCoord1: TEXCOORD1;
#	endif

#	if defined(STENCIL)
	float4 HPosition: SV_POSITION0;
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#	endif

	float4 NormalsScale : TEXCOORD8;
#	if defined(PBR_WATER)
	float4 UnifiedWaveInfo : TEXCOORD9;       // xy = primary wave direction, z = wave height, w = shore influence
	float4 UnifiedWaveNormal : TEXCOORD10;    // xyz = geometric wave normal, w = horizontal displacement
	float3 Barycentric : TEXCOORD11;          // Barycentric coordinates for wireframe debug
	float4 DepthDebug : TEXCOORD12;           // x=depth, y=debugCode, z=terrainZ, w=waterZ
#	endif
#	if defined(VR)
	float ClipDistance: SV_ClipDistance0;  // o11
	float CullDistance: SV_CullDistance0;  // p11
#	endif  // VR
};

#if defined(PBR_WATER)
#	include "PBRWater/GerstnerWaves.hlsli"
#	include "PBRWater/WaterActorRipples.hlsli"
#	include "PBRWater/WaterFoam.hlsli"
#	include "PBRWater/WaterDepthEstimation.hlsli"
#endif // PBR_WATER

#	ifdef VSHADER

cbuffer PerTechnique : register(b0)
{
#		if !defined(VR)
	float4 QPosAdjust[1] : packoffset(c0);
#		else
	float4 QPosAdjust[2] : packoffset(c0);
#		endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 VSFogParam : packoffset(c0);
	float4 VSFogNearColor : packoffset(c1);
	float4 VSFogFarColor : packoffset(c2);
	float4 NormalsScroll0 : packoffset(c3);
	float4 NormalsScroll1 : packoffset(c4);
	float4 NormalsScale : packoffset(c5);
};

cbuffer PerGeometry : register(b2)
{
#		if !defined(VR)
	row_major float4x4 World[1] : packoffset(c0);
	row_major float4x4 PreviousWorld[1] : packoffset(c4);
	row_major float4x4 WorldViewProj[1] : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
#		else   // VR has 25 vs 13 entries
	row_major float4x4 World[2] : packoffset(c0);
	row_major float4x4 PreviousWorld[2] : packoffset(c8);
	row_major float4x4 WorldViewProj[2] : packoffset(c16);
	float3 ObjectUV : packoffset(c24);
	float4 CellTexCoordOffset : packoffset(c25);
#		endif  // VR
};

#if defined(PBR_WATER) && defined(FLOWMAP)
SamplerState FlowMapSamplerVS : register(s8);
Texture2D<float4> FlowMapTexVS : register(t8);
#endif

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = Stereo::GetEyeIndexVS(
#		if defined(VR)
		input.InstanceID
#		endif
	);
	vsout.NormalsScale = NormalsScale;

#	if defined(PBR_WATER)
	vsout.UnifiedWaveInfo = 0.0.xxxx;
	vsout.UnifiedWaveNormal = float4(0.0f, 0.0f, 1.0f, 0.0f);
	vsout.Barycentric = float3(0.333f, 0.333f, 0.334f);
	vsout.DepthDebug = 0.0.xxxx;
#	endif

	float4 inputPosition = float4(input.Position.xyz, 1.0);
	float4 worldPos;
	float4 worldViewPos;

#if defined(PBR_WATER)
	float4 currentPosition = inputPosition;
	float4 previousPosition = inputPosition;
	float4 worldPosBase = mul(World[eyeIndex], inputPosition);
	float2 waveWorldPos = worldPosBase.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;
	float2 waveWorldPosPrev = worldPosBase.xy + FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xy;
	float2 flowBiasDirVS = float2(0.0f, 0.0f);
	float flowBiasWeightVS = 0.0f;

#	if defined(FLOWMAP)
	if ((ObjectUV.x > 0.0f) && (ObjectUV.y > 0.0f) && (ObjectUV.z > 0.0f)) {
		float2 dims = max(float2(ObjectUV.x, ObjectUV.y), float2(1.0f, 1.0f));
		float2 cellShiftFlow = float2(floor(ObjectUV.z * 0.5f), floor((ObjectUV.z - 1.0f) * 0.5f));
		float2 centerScaledUV = float2(0.5f, 0.5f) * ObjectUV.z - cellShiftFlow;
		float2 flowUV = (CellTexCoordOffset.xy + centerScaledUV) / dims;
		float4 flowSample = FlowMapTexVS.SampleLevel(FlowMapSamplerVS, flowUV, 0.0f);
		float flowStrength = saturate(flowSample.z * flowSample.w);
		float2 rawFlow = -(flowSample.xy * 2.0f - 1.0f);
		float flowLenSq = dot(rawFlow, rawFlow);
		if (flowStrength > 0.001f && flowLenSq > 1e-5f) {
			flowBiasDirVS = rawFlow * rsqrt(flowLenSq);
			flowBiasWeightVS = flowStrength;
		}
	}
#	endif

	float waveTimeSeconds = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
	float waveDayPhase = ComputeWaveDayPhase(GameTimeHours);

	// Wave displacement variables
	float3 waveDisplacement = float3(0.0f, 0.0f, 0.0f);
	float3 waveNormal = float3(0.0f, 0.0f, 1.0f);
	float2 wavePrimaryDir = float2(-0.70710678f, 0.70710678f);
	float shoreInfluence = 0.0f;
	float horizontalDisplacement = 0.0f;

	// Depth estimation for wave attenuation
	DepthEstimationDebug depthDebug;
	depthDebug.depth = 1e5f;
	depthDebug.debugCode = 0.0f;
	depthDebug.terrainZ = 0.0f;
	depthDebug.waterZ = 0.0f;
	float estimatedDepth = 1e5f;

	if (TessellationEnabled < 0.5f) {
		float cameraDistVS = length(worldPosBase.xyz);

		// Estimate water depth and shore direction from terrain heightmap
		float3 absoluteWorldPos = worldPosBase.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
		float2 shoreDirVS = float2(0.0f, 0.0f);
		float shoreGradVS = 0.0f;
		estimatedDepth = ComputeShoreDirection(
			absoluteWorldPos,
			float2(TerrainScaleX, TerrainScaleY),
			float2(TerrainOffsetX, TerrainOffsetY),
			TerrainZRangeMin,
			TerrainZRangeMax,
			shoreDirVS,
			shoreGradVS
		);

		// Fill debug info from depth
		depthDebug.depth = estimatedDepth;
		depthDebug.debugCode = (estimatedDepth >= 1e4f) ? 1.0f : 0.0f;
		depthDebug.terrainZ = absoluteWorldPos.z - estimatedDepth;
		depthDebug.waterZ = absoluteWorldPos.z;

		WaveSample currentWave = CalculateWaterDisplacement(waveWorldPos,
			float2(0.0f, 0.0f), float2(0.0f, 0.0f), WaveIntensity, WaveAmplitude, WaveSpeed,
			WaveSteepness, waveTimeSeconds, waveDayPhase, flowBiasDirVS, flowBiasWeightVS, false,
			cameraDistVS, estimatedDepth, shoreDirVS, shoreGradVS);
		waveDisplacement = currentWave.displacement;
		waveNormal = currentWave.normal;
		wavePrimaryDir = currentWave.primaryDirection;
		shoreInfluence = currentWave.shoreInfluence;
		horizontalDisplacement = length(waveDisplacement.xy);
	}

	// Store debug info for pixel shader visualization
	vsout.DepthDebug = float4(depthDebug.depth, depthDebug.debugCode, depthDebug.terrainZ, depthDebug.waterZ);

	vsout.UnifiedWaveInfo = float4(wavePrimaryDir, waveDisplacement.z, shoreInfluence);
	vsout.UnifiedWaveNormal = float4(waveNormal, horizontalDisplacement);

	currentPosition.xyz += waveDisplacement;

	// For DLSS/FG motion vectors, calculate previous frame wave state
	float3 prevWaveDisplacement = float3(0.0f, 0.0f, 0.0f);
	if (TessellationEnabled < 0.5f) {
		float waveTimeSecondsPrev = ComputeWaveTimeSeconds(PrevGameTimeHours, PrevRealTimeSeconds);
		float waveDayPhasePrev = ComputeWaveDayPhase(PrevGameTimeHours);
		float cameraDistVSPrev = length(worldPosBase.xyz);
		WaveSample prevWave = CalculateWaterDisplacement(waveWorldPos, float2(0.0f, 0.0f), float2(0.0f, 0.0f),
			WaveIntensity, WaveAmplitude, WaveSpeed, WaveSteepness, waveTimeSecondsPrev,
			waveDayPhasePrev, flowBiasDirVS, flowBiasWeightVS, true, cameraDistVSPrev);
		prevWaveDisplacement = prevWave.displacement;
	}

	// Apply previous displacement in local space then transform
	previousPosition = float4(input.Position.xyz + prevWaveDisplacement, 1.0);

	inputPosition = currentPosition;
	worldPos = mul(World[eyeIndex], currentPosition);
	worldViewPos = mul(WorldViewProj[eyeIndex], currentPosition);
#else
	worldPos = mul(World[eyeIndex], inputPosition);
	worldViewPos = mul(WorldViewProj[eyeIndex], inputPosition);
#endif

#if defined(PBR_WATER)
	// Don't modify depth with wave displacement - use true projected depth
	vsout.HPosition = worldViewPos;
#else
	float heightMult = min((1.0 / 10000.0) * max(worldViewPos.z - 70000, 0), 1);

	vsout.HPosition.xy = worldViewPos.xy;
	vsout.HPosition.z = heightMult * 0.5 + worldViewPos.z;
	vsout.HPosition.w = worldViewPos.w;
#endif

#		if defined(STENCIL)
	vsout.WorldPosition = worldPos;
#if defined(PBR_WATER)
	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], previousPosition);
#else
	vsout.PreviousWorldPosition = mul(PreviousWorld[eyeIndex], inputPosition);
#endif
#		else

#			if !defined(UNIFIED_WATER)
	float fogDistanceFactor = min(VSFogFarColor.w, pow(saturate(length(worldViewPos.xyz) * VSFogParam.y - VSFogParam.x), NormalsScale.w));
	vsout.FogParam.xyz = lerp(VSFogNearColor.xyz, VSFogFarColor.xyz, fogDistanceFactor);
	vsout.FogParam.w = fogDistanceFactor;
#			endif

	vsout.WPosition.xyz = worldPos.xyz;
	vsout.WPosition.w = length(worldPos.xyz);

#			if defined(LOD)
	float4 posAdjust =
		ObjectUV.x ? 0.0 : (QPosAdjust[eyeIndex].xyxy + worldPos.xyxy) / NormalsScale.xxyy;

	vsout.TexCoord1.xyzw = NormalsScroll0 + posAdjust;
#			else
#				if !defined(SPECULAR) || (NUM_SPECULAR_LIGHTS == 0)
	vsout.MPosition.xyzw = inputPosition.xyzw;
#				endif

	float2 posAdjust = worldPos.xy + QPosAdjust[eyeIndex].xy;

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
#					elif defined(WADING)
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
#					elif (defined(REFRACTIONS) || NUM_SPECULAR_LIGHTS == 0 || defined(BLEND_NORMALS))
#						if defined(UNIFIED_WATER)
	float2 dims = float2(ObjectUV.x, ObjectUV.y);
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + scaledUV) / dims;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + scaledUV;
	vsout.TexCoord3.zw = scaledUV;
#						else
	vsout.TexCoord2.zw = (CellTexCoordOffset.xy + input.TexCoord0.xy) / ObjectUV.xx;
	vsout.TexCoord3.xy = CellTexCoordOffset.zw + input.TexCoord0.xy;
	vsout.TexCoord3.zw = input.TexCoord0.xy;
#						endif
#					endif
	vsout.TexCoord4 = ObjectUV.xy;
#				else
	vsout.TexCoord1.xy = NormalsScroll0.xy + scrollAdjust1;
	vsout.TexCoord1.zw = NormalsScroll0.zw + scrollAdjust2;
	vsout.TexCoord2.xy = NormalsScroll1.xy + scrollAdjust3;
	vsout.TexCoord2.z = worldViewPos.w;
	vsout.TexCoord2.w = 0;
#					if (defined(WADING) || (defined(VERTEX_ALPHA_DEPTH) && defined(VC)))
	vsout.TexCoord3 = 0.0;
#						if (defined(NORMAL_TEXCOORD) && ((!defined(BLEND_NORMALS) && !defined(VERTEX_ALPHA_DEPTH)) || defined(WADING)))
	vsout.TexCoord3.xy = input.TexCoord0;
#						endif
#						if defined(VERTEX_ALPHA_DEPTH) && defined(VC)
	vsout.TexCoord3.z = input.Color.w;
#						endif
#					endif
#				endif
#			endif
#		endif

#		ifdef VR
	Stereo::VR_OUTPUT VRout = Stereo::GetVRVSOutput(vsout.HPosition, eyeIndex);
	vsout.HPosition = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#		endif  // VR
	return vsout;
}

#	endif

// ============================================================================
// HULL SHADER - Tessellation control
// ============================================================================
#	ifdef HSHADER

// Include tessellation system BEFORE any shader definitions
#		if defined(PBR_WATER)
#			include "PBRWater/WaterTessellation.hlsli"
#		else
// Fallback for non-PBR water - simple distance-based tessellation
HS_CONSTANT_OUTPUT PatchConstantFunc(InputPatch<VS_OUTPUT, 3> patch, uint patchID : SV_PrimitiveID)
{
	HS_CONSTANT_OUTPUT output;
	float3 center = (patch[0].WPosition.xyz + patch[1].WPosition.xyz + patch[2].WPosition.xyz) / 3.0f;
	float dist = length(center);
	float factor = lerp(TessellationMaxFactor, TessellationMinFactor, saturate(dist / TessellationMaxDistance));
	output.EdgeTess[0] = output.EdgeTess[1] = output.EdgeTess[2] = output.InsideTess = factor;
	return output;
}
#		endif

// Hull shader control point output - passes VS_OUTPUT through
typedef VS_OUTPUT HS_OUTPUT;

// Hull shader main - passes through control points
[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantFunc")]
[maxtessfactor(64.0)]
HS_OUTPUT main(InputPatch<VS_OUTPUT, 3> patch, uint cpID : SV_OutputControlPointID, uint patchID : SV_PrimitiveID)
{
	return patch[cpID];
}

#	endif  // HSHADER

// ============================================================================
// DOMAIN SHADER - Tessellation evaluation
// ============================================================================
#	ifdef DSHADER

#		if defined(PBR_WATER)
#			include "PBRWater/WaterTessellation.hlsli"
#			include "PBRWater/GerstnerWaves.hlsli"
#		endif

// Domain shader cbuffers - need same cbuffers as VS for matrix transforms
cbuffer PerGeometryDS : register(b2)
{
#		if !defined(VR)
	row_major float4x4 World[1] : packoffset(c0);
	row_major float4x4 PreviousWorld[1] : packoffset(c4);
	row_major float4x4 WorldViewProj[1] : packoffset(c8);
	float3 ObjectUV : packoffset(c12);
	float4 CellTexCoordOffset : packoffset(c13);
#		else
	row_major float4x4 World[2] : packoffset(c0);
	row_major float4x4 PreviousWorld[2] : packoffset(c8);
	row_major float4x4 WorldViewProj[2] : packoffset(c16);
	float3 ObjectUV : packoffset(c24);
	float4 CellTexCoordOffset : packoffset(c25);
#		endif
};

// Domain shader - uses DomainShaderImpl from WaterTessellation.hlsli
// Provides curvature-adaptive tessellation with proper Gerstner wave displacement
[domain("tri")]
VS_OUTPUT main(HS_CONSTANT_OUTPUT patchConst, float3 bary : SV_DomainLocation, const OutputPatch<VS_OUTPUT, 3> patch)
{
	uint eyeIndex = 0;  // DS runs after VS which already handled stereo
	return DomainShaderImpl(patchConst, bary, patch, eyeIndex);
}

#	endif  // DSHADER

// ============================================================================
// GEOMETRY SHADER - Assigns per-triangle barycentric coordinates for tri visualization
// ============================================================================
#	if defined(GSHADER) || defined(GEOMETRYSHADER)

[maxvertexcount(3)]
void main(triangle VS_OUTPUT input[3], inout TriangleStream<VS_OUTPUT> outStream)
{
	VS_OUTPUT v0 = input[0];
	VS_OUTPUT v1 = input[1];
	VS_OUTPUT v2 = input[2];

#	if defined(PBR_WATER)
	v0.Barycentric = float3(1.0f, 0.0f, 0.0f);
	v1.Barycentric = float3(0.0f, 1.0f, 0.0f);
	v2.Barycentric = float3(0.0f, 0.0f, 1.0f);
#	endif

	outStream.Append(v0);
	outStream.Append(v1);
	outStream.Append(v2);
	outStream.RestartStrip();
}

#	endif  // GSHADER || GEOMETRYSHADER

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
#	if defined(UNDERWATER) || defined(SIMPLE) || defined(LOD) || defined(SPECULAR)
	float4 Lighting: SV_Target0;
#	endif

#	if defined(STENCIL)
	float4 WaterMask: SV_Target0;
	float2 MotionVector: SV_Target1;
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
#		if !defined(VR)
	float4 VPOSOffset : packoffset(c0);    // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust[1] : packoffset(c1);  // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c2);
	float4 SunDir : packoffset(c3);
	float4 SunColor : packoffset(c4);
#		else
	float4 VPOSOffset : packoffset(c0);    // inverse main render target width and height in xy, 0 in zw
	float4 PosAdjust[2] : packoffset(c1);  // inverse framebuffer range in w
	float4 CameraDataWater : packoffset(c3);
	float4 SunDir : packoffset(c4);
	float4 SunColor : packoffset(c5);
#		endif
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
#		if !defined(VR)
	float4x4 TextureProj[1] : packoffset(c0);
	float4 ReflectPlane[1] : packoffset(c4);
	float4 ProjData : packoffset(c5);
	float4 LightPos[8] : packoffset(c6);
	float4 LightColor[8] : packoffset(c14);
#		else
	float4x4 TextureProj[2] : packoffset(c0);
	float4 ReflectPlane[2] : packoffset(c8);
	float4 ProjData : packoffset(c10);
	float4 LightPos[8] : packoffset(c11);
	float4 LightColor[8] : packoffset(c19);
#		endif  //VR
}

#		if defined(VR)
/**
Calculates the depthMultiplier as used in Water.hlsl

VR appears to require use of CameraProjInverse and does not use ProjData
@param uv UV coords to convert
@param depth The calculated depth
@param eyeIndex The eyeIndex; 0 is left, 1 is right
@returns depthMultiplier
*/
float CalculateDepthMultFromUV(float2 uv, float depth, uint eyeIndex = 0)
{
	float4 temp;
	temp.xy = (uv * 2 - 1);
	temp.z = depth;
	temp.w = 1;
	temp = mul(FrameBuffer::CameraProjInverse[eyeIndex], temp.xyzw);
	temp.xyz /= temp.w;
	return length(temp.xyz);
}
#		endif  // VR

#		define SampColorSampler Normals01Sampler
#		define LinearSampler Normals01Sampler

#		if defined(SKYLIGHTING)
#			include "Skylighting/Skylighting.hlsli"
#		endif

#		if defined(EXP_HEIGHT_FOG)
#			include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#		endif

#		include "Common/ShadowSampling.hlsli"

#		if defined(SIMPLE) || defined(UNDERWATER) || defined(LOD) || defined(SPECULAR)
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
 * Computes mip level for flowmap texture sampling
 */
float GetFlowmapMipLevel(float2 flowmapUV)
{
	float2 textureDims;
	FlowMapNormalsTex.GetDimensions(textureDims.x, textureDims.y);

#				if defined(VR)
	textureDims /= 16.0;
#				else
	textureDims /= 8.0;
#				endif

	float2 texCoordsPerSize = flowmapUV * textureDims;
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
 * Generates flowmap-based normal (no parallax - flowmap normals are not parallax-shifted)
 * Uses mip clamping to preserve detail at distance and prevent over-blurring
 */
float3 GetFlowmapNormal(PS_INPUT input, float2 uvShift, float multiplier, float offset)
{
	FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
	float2 uv = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	float2 dx = ddx(uv);
	float2 dy = ddy(uv);
	float mipLevel = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));
	mipLevel = clamp(mipLevel + SharedData::MipBias, 0, 5);

	float mipScale = exp2(-mipLevel);
	float2 scaledFlowVector = flowData.flowVector * mipScale;
	float2 scaledUv = offset + (scaledFlowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));

	return float3(FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, scaledUv, mipLevel).xy, flowData.color.z);
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
	float3 normal;           // Full-detail normal for specular/reflections
	float3 diffuseNormal;    // Softer normal for diffuse lighting (less wave distortion)
	float4 rippleInfo;       // xyz = scaled ripple normal (normalized normal * intensity), w = splash effect intensity
};

WaterNormalData GetWaterNormal(PS_INPUT input, float distanceFactor, float normalsDepthFactor, float3 viewDirection, float depth, uint eyeIndex, float wetnessOcclusion)
{
	WaterNormalData result;
	result.rippleInfo = float4(0, 0, 0, 0);
	result.diffuseNormal = float3(0, 0, 1);  // Default to flat normal
	float3 normalScalesRcp = rcp(input.NormalsScale.xyz);

#			if defined(WATER_PARALLAX)
	float2 parallaxOffset = WaterEffects::GetParallaxOffset(input, normalScalesRcp);
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
	float parallaxAmount = WaterEffects::GetFlowmapParallaxAmount(input, flowmapDimensions, viewDirection);
	float2 parallaxDir = viewDirection.xy / -viewDirection.z;
	parallaxDir.y = -parallaxDir.y;
	float viewDotUp = -viewDirection.z;
	parallaxDir *= 0.008 * saturate(viewDotUp * 2.0);
	flowmapInput.TexCoord3.xy = input.TexCoord3.xy + parallaxAmount * parallaxDir;
	flowmapParallaxOffset = WaterEffects::GetFlowmapParallaxOffset(input, flowmapDimensions, viewDirection, normalScalesRcp);
#				endif

	// Calculate cell blend weights using parallaxed input
	float2 normalMul = 0.5 + -(-0.5 + abs(frac(flowmapInput.TexCoord2.zw * (64 * flowmapDimensions)) * 2 - 1));

	// Sample flowmap normals with parallax applied
	float3 flowmapNormal0 = GetFlowmapNormal(flowmapInput, uvShift, 9.92, 0);
	float3 flowmapNormal1 = GetFlowmapNormal(flowmapInput, float2(0, uvShift.y), 10.64, 0.27);
	float3 flowmapNormal2 = GetFlowmapNormal(flowmapInput, 0.0.xx, 8, 0);
	float3 flowmapNormal3 = GetFlowmapNormal(flowmapInput, float2(uvShift.x, 0), 8.48, 0.62);

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
#				else
	// FLOWMAP NORMALS ENABLED: Blending flow-based normals with base normals
	float3 finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), flowmapNormal, distanceFactor));
#				endif
#			elif !defined(LOD)

#				if defined(WATER_PARALLAX)
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw + parallaxOffset.xy * normalScalesRcp.y, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy + parallaxOffset.xy * normalScalesRcp.z, SharedData::MipBias).xyz * 2.0 - 1.0;
#				else
	float3 normals2 = Normals02Tex.SampleBias(Normals02Sampler, input.TexCoord1.zw, SharedData::MipBias).xyz * 2.0 - 1.0;
	float3 normals3 = Normals03Tex.SampleBias(Normals03Sampler, input.TexCoord2.xy, SharedData::MipBias).xyz * 2.0 - 1.0;
#				endif

	float3 blendedNormal = normalize(float3(0, 0, 1) + NormalsAmplitude.x * normals1 +
									 NormalsAmplitude.y * normals2 + NormalsAmplitude.z * normals3);
#				if defined(UNDERWATER)
	float3 finalNormal = blendedNormal;
#				else
	float3 finalNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, normalsDepthFactor));
#				endif

#				if defined(FLOWMAP)
	float normalBlendFactor =
		normalMul.y * ((1 - normalMul.x) * flowmapNormal3.z + normalMul.x * flowmapNormal2.z) +
		(1 - normalMul.y) * (normalMul.x * flowmapNormal1.z + (1 - normalMul.x) * flowmapNormal0.z);
	finalNormal = normalize(lerp(normals1 + float3(0, 0, 1), normalize(lerp(finalNormal, flowmapNormal, normalBlendFactor)), distanceFactor));
#				endif
#			else
	float3 finalNormal =
		normalize(float3(0, 0, 1) + NormalsAmplitude.xxx * normals1);
#			endif

#			if defined(WADING)
#				if defined(FLOWMAP)
	float2 displacementUv = input.TexCoord3.zw;
#				else
	float2 displacementUv = input.TexCoord3.xy;
#				endif
	float3 displacement = normalize(float3(NormalsAmplitude.w * (-0.5 + DisplacementTex.Sample(DisplacementSampler, displacementUv).zw),
		0.04));
	finalNormal = lerp(displacement, finalNormal, displacement.z);
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
		raindropInfo = WetnessEffects::GetRainDrops(rippleWPosition + FrameBuffer::CameraPosAdjust[eyeIndex].xyz, SharedData::wetnessEffectsSettings.Time, finalNormal, rippleStrengthModifier);

		// Calculate ripple and splash color intensities
		float rippleIntensity = length(raindropInfo.xy) * rippleStrengthModifier;
		float splashIntensity = raindropInfo.w * distanceFadeout;

		// Store ripple and splash information for color effects
		result.rippleInfo.xyz = raindropInfo.xyz * rippleIntensity;
		result.rippleInfo.w = splashIntensity;
	}
	float3 rippleNormal = normalize(raindropInfo.xyz);
	finalNormal = WetnessEffects::ReorientNormal(rippleNormal, finalNormal);
#			endif

#			if defined(PBR_WATER)
	// Blend in Gerstner wave normals from vertex/domain shader
	float3 waveNormalGeom = input.UnifiedWaveNormal.xyz;
	float waveNormalLen = length(waveNormalGeom);

	// Shore influence: reduces texture normal, boosts wave geometric normal
	// This makes shore-directed wave normals more visible near the shoreline
	float shoreInfluence = input.UnifiedWaveInfo.w;  // 0 = deep water, 1 = at shore

	// Save the texture-based normal before wave blending for diffuse calculations
	// Reduce texture normal strength near shore so wave shape dominates
	float textureNormalScale = lerp(1.0f, 0.2f, shoreInfluence);
	float3 textureNormal = normalize(float3(finalNormal.xy * textureNormalScale, finalNormal.z));
	finalNormal = textureNormal;

	if (waveNormalLen > 0.01f && WaveIntensity > 0.01f) {
		waveNormalGeom = normalize(waveNormalGeom);
		// Use UDN blending to combine texture normals with geometric wave normals
		// Boost wave normal strength near shore for more prominent wave shapes
		float waveNormalBoost = lerp(1.0f, 1.5f, shoreInfluence);
		float3 waveNormalTangent = float3(waveNormalGeom.xy, waveNormalGeom.z);
		finalNormal = normalize(float3(
			finalNormal.xy + waveNormalTangent.xy * WaveIntensity * waveNormalBoost,
			finalNormal.z * waveNormalTangent.z
		));
	}

	// Create a softer "diffuse normal" that has reduced wave influence
	// Near shore, reduce dampening so wave lighting is more visible
	float3 diffuseNormalResult = textureNormal;
	if (waveNormalLen > 0.01f && WaveIntensity > 0.01f) {
		float diffuseDampening = lerp(0.35f, 0.7f, shoreInfluence);
		float diffuseBlend = lerp(0.5f, 0.8f, shoreInfluence);
		float3 dampenedWaveNormal = normalize(float3(waveNormalGeom.xy * diffuseDampening, waveNormalGeom.z));
		diffuseNormalResult = normalize(float3(
			textureNormal.xy + dampenedWaveNormal.xy * WaveIntensity * diffuseBlend,
			textureNormal.z * dampenedWaveNormal.z
		));
	}

	// Apply actor wading ripples ON TOP of wave normals.
	// This runs AFTER Gerstner wave blending so ripples are always visible
	// and not overwritten by the dominant wave normal contribution.
	if (RippleStrength > 0.01f) {
		float2 waterWorldPos = input.WPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;
		float2 rippleOffset = PlayerRipples::GetAllActorRippleOffsets(
			waterWorldPos,
			RealTimeSeconds,
			float2(PlayerPosX, PlayerPosY),
			float2(PlayerVelocityX, PlayerVelocityY),
			PlayerInWater,
			PlayerWaterDepth
		);

		float rippleScale = RippleStrength * RippleNormalStrength;
		finalNormal = normalize(float3(
			finalNormal.xy + rippleOffset * rippleScale,
			finalNormal.z));
		diffuseNormalResult = normalize(float3(
			diffuseNormalResult.xy + rippleOffset * rippleScale * 0.5,
			diffuseNormalResult.z));
	}

	// Store both normals: full detail for specular, dampened for diffuse
	result.diffuseNormal = diffuseNormalResult;
#			endif

	result.normal = finalNormal;
	return result;
}

#			include "Common/BRDF.hlsli"

float3 GetWaterSpecularColor(PS_INPUT input, float3 normal, float3 viewDirection,
	float distanceFactor, float refractionsDepthFactor, uint eyeIndex = 0)
{
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Reflections) {
		float3 finalSsrReflectionColor = 0.0.xxx;
		float ssrFraction = 0.0;
		float3 reflectionColor = 0.0.xxx;
		float3 R = reflect(viewDirection, WaterParams.y * normal + float3(0, 0, 1 - WaterParams.y));

		// Water surface roughness for reflection sampling
		float waterReflectionRoughness = 0.08;
#			if defined(PBR_WATER)
		waterReflectionRoughness = lerp(0.05, 0.15, saturate(WaveIntensity));
#			endif

		// Horizon specular occlusion - prevents cubemap from showing where geometry blocks reflections
		float horizon = saturate(1.0 + dot(R, float3(0, 0, 1)));
		horizon *= horizon;

		// Filter reflections by upward-facing normals to prevent underwater elements reflecting on wave tops
		float upwardFacingMask = saturate((normal.z - 0.3) * 2.5);

		if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Cubemap) {
#			if defined(DYNAMIC_CUBEMAPS)
#				if defined(SKYLIGHTING)

			float3 dynamicCubemap;
			float skylightingSpecular = 1.0;
			if (SharedData::InInterior) {
				dynamicCubemap = DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz;
			} else {
#					if defined(VR)
				float3 positionMSSkylight = input.WPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#					else
				float3 positionMSSkylight = input.WPosition.xyz;
#					endif

				sh2 skylighting = Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.HPosition.xy, positionMSSkylight, R);
				sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, waterReflectionRoughness);

				skylightingSpecular = SphericalHarmonics::FuncProductIntegral(skylighting, specularLobe);
				skylightingSpecular = lerp(1.0, skylightingSpecular, Skylighting::getFadeOutFactor(input.WPosition.xyz));
				skylightingSpecular = Skylighting::mixSpecular(SharedData::skylightingSettings, skylightingSpecular);

				float3 specularIrradiance = 1;

				if (skylightingSpecular < 1.0) {
					specularIrradiance = Color::IrradianceToLinear(DynamicCubemaps::EnvTexture.SampleLevel(CubeMapSampler, R, 0).xyz);
				}

				float3 specularIrradianceReflections = 1.0;

				if (skylightingSpecular > 0.0) {
					specularIrradianceReflections = Color::IrradianceToLinear(DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, 0).xyz);
				}

				dynamicCubemap = Color::IrradianceToGamma(lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular));
			}

			// Apply horizon, skylighting, and upward-facing occlusion to cubemap reflections
			skylightingSpecular *= skylightingSpecular;
			dynamicCubemap *= horizon * skylightingSpecular * upwardFacingMask;
#				else
			float3 dynamicCubemap = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(CubeMapSampler, R, 0);
#				endif

#				if defined(VR)
			// Reflection cubemap is incorrect for interiors in VR, ignore it
			if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior || SharedData::HideSky)
				reflectionColor = dynamicCubemap.xyz;
			else
				reflectionColor = lerp(dynamicCubemap.xyz, CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz, saturate(length(input.WPosition.xyz) / 1024.0));
#				else
			if (SharedData::HideSky)
				reflectionColor = dynamicCubemap.xyz;
			else
				reflectionColor = lerp(dynamicCubemap.xyz, CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz, saturate(length(input.WPosition.xyz) / 1024.0));
#				endif
#			else
			reflectionColor = CubeMapTex.SampleLevel(CubeMapSampler, R, 0).xyz;
#			endif
		} else {
#			if !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
			float4 reflectionNormalRaw = float4((VarAmounts.w * refractionsDepthFactor) * normal.xy + input.MPosition.xy, input.MPosition.z, 1);
#			else
			float4 reflectionNormalRaw = float4(VarAmounts.w * normal.xy, 0, 1);
#			endif

			float4 reflectionNormal = mul(transpose(TextureProj[eyeIndex]), reflectionNormalRaw);
			reflectionColor = ReflectionTex.SampleLevel(ReflectionSampler, reflectionNormal.xy / reflectionNormal.ww, 0).xyz;
		}

#			if !defined(LOD) && NUM_SPECULAR_LIGHTS == 0
		if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Cubemap) {
			float pointingDirection = dot(viewDirection, R);
			float pointingAlignment = dot(reflect(viewDirection, float3(0, 0, 1)), R);
			float ssrAmount = min(pointingAlignment, pointingDirection);
			if (SSRParams.x > 0.0 && ssrAmount > 0.0) {
				float2 ssrReflectionUv = ((FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy) * SSRParams.zw) + 0.05 * normal.xy;
				float2 ssrReflectionUvDR = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(ssrReflectionUv);
				float4 ssrReflectionColorBlurred = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
				float4 ssrReflectionColorRaw = RawSSRReflectionTex.Sample(RawSSRReflectionSampler, ssrReflectionUvDR);
				float4 ssrReflectionColor = lerp(ssrReflectionColorBlurred, ssrReflectionColorRaw, ssrAmount * 0.7);

				// Apply horizon and upward-facing occlusion to SSR to prevent underwater elements
				finalSsrReflectionColor = max(0, ssrReflectionColor.xyz) * horizon * upwardFacingMask;

				ssrFraction = saturate(ssrReflectionColor.w * distanceFactor * ssrAmount * horizon * upwardFacingMask);
			}
		}
#			endif

		float3 finalReflectionColor = Color::IrradianceToGamma(lerp(Color::IrradianceToLinear(reflectionColor), Color::IrradianceToLinear(finalSsrReflectionColor), ssrFraction));

#			if defined(PBR_WATER)
		// Apply physically-based BRDF to reflections
		float3 V = -viewDirection;
		float NdotV = max(dot(normal, V), 1e-5f);

		// Use BRDF.hlsli's analytical DFG approximation
		float2 envBRDF = BRDF::EnvBRDF(waterReflectionRoughness, NdotV);

		// Water F0 and F90 for dielectric interface (air-water)
		const float waterF0 = 0.02f;
		const float waterF90 = 1.0f;

		// Apply the DFG terms: (F0 * envBRDF.x + F90 * envBRDF.y)
		float specularBRDF = waterF0 * envBRDF.x + waterF90 * envBRDF.y;

		finalReflectionColor *= specularBRDF;

		// Apply reflection strength multiplier only when ESP overrides enabled
		if (EnableLightingOverrides > 0.5f) {
			finalReflectionColor *= ReflectionStrength;
		}
#			endif

		return finalReflectionColor;
	}
	return ReflectionColor.xyz * VarAmounts.y;
}

float GetScreenDepthWater(float2 screenPosition, uint a_useVR = 0)
{
	float depth = DepthTex.Load(float3(screenPosition, 0)).x;
#			if defined(VR)  // VR appears to use hard coded values
	return depth * 1.01 + -0.01;
#			else
	return (CameraDataWater.w / (-depth * CameraDataWater.z + CameraDataWater.x));
#			endif
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
	float NdotV = saturate(dot(-viewDirection, actualNormal));

#			if defined(PBR_WATER)
	// Water has IOR ~1.33, giving F0 = ((1.33-1)/(1.33+1))^2 ≈ 0.02
	// Use BRDF.hlsli's Schlick approximation for physically correct fresnel
	float F0;
	if (EnableLightingOverrides > 0.5f) {
		F0 = FresnelBias;
	} else {
		F0 = max(FresnelRI.x, 0.02f);
	}
	float3 fresnelVec = BRDF::F_Schlick(float3(F0, F0, F0), NdotV);
	return fresnelVec.x;
#			else
	float viewAngle = 1 - NdotV;
	return (1 - FresnelRI.x) * pow(viewAngle, 5) + FresnelRI.x;
#			endif
}

struct DiffuseOutput
{
	float3 refractionColor;
	float3 refractionDiffuseColor;
	float depth;
	float refractionMul;
	float3 refractedViewDirection;  // World-space direction used for 3D shadow filtering along the refracted ray
	float3 scatter;                 // Physically-based in-scattered light
	float3 transmittance;           // Light transmission through water
	float3 waveSSS;                 // Wave edge subsurface scattering
};

// ============================================================================
// PHYSICALLY-BASED WATER SCATTERING
// Based on real water absorption/scattering coefficients
// Reference: https://s.campbellsci.com/documents/es/technical-papers/obs_light_absorption.pdf
// ============================================================================
#if defined(PBR_WATER)

// Henyey-Greenstein phase function for anisotropic scattering
float PhaseHenyeyGreenstein(float cosTheta, float g)
{
	const float scale = 0.25f / Math::PI;
	float g2 = g * g;
	float num = 1.0f - g2;
	float denom = pow(abs(1.0f + g2 - 2.0f * g * cosTheta), 1.5f);
	return scale * num / max(denom, 0.0001f);
}

struct WaterScatteringResult
{
	float3 scatter;
	float3 transmittance;
};

WaterScatteringResult CalculateWaterScattering(float3 startPosWS, float3 endPosWS, float3 sunDir, float3 sunColor, float occlusion, float cameraDistance)
{
	WaterScatteringResult result;
	result.scatter = 0.0f;
	result.transmittance = 1.0f;

	float3 worldDir = endPosWS - startPosWS;
	float dist = length(worldDir);
	if (dist < 0.01f) {
		return result;
	}

	worldDir = worldDir / dist;

	// Water optical coefficients (per game unit, ~1.428 cm)
	float3 hue = normalize(rcp(max(ShallowColor.xyz, 0.001f)));
	const float3 scatterCoeff = hue * 0.002f * 2.0f * 1.428f;
	const float3 absorpCoeff = hue * 0.0002f * 2.0f * 1.428f;
	const float3 extinction = scatterCoeff + absorpCoeff;

	float cosTheta = dot(sunDir, worldDir);
	float phase = PhaseHenyeyGreenstein(cosTheta, 0.5f);

	float distRatio = abs(sunDir.z / max(abs(worldDir.z), 0.001f));

	const float cutoffTransmittance = 0.01f;
	float maxExtinction = max(extinction.x, max(extinction.y, extinction.z));
	float cutoffDist = -log(cutoffTransmittance) / ((1.0f + distRatio) * maxExtinction);

	float marchDist = min(dist, cutoffDist);
	float sunMarchDist = marchDist * distRatio;

	// Distance-based LOD: reduce steps at distance for performance
	// Smooth transition zones ensure no popping
	uint nSteps = 8;
	float lodFade = 1.0f;
	
	const float lod1Distance = 4096.0f;   // Medium distance threshold
	const float lod2Distance = 8192.0f;   // Far distance threshold
	
	if (cameraDistance > lod2Distance) {
		nSteps = 4;  // Minimal steps at far distance
		float fadeStart = lod2Distance;
		float fadeEnd = lod2Distance + 2048.0f;
		lodFade = 1.0f - saturate((cameraDistance - fadeStart) / (fadeEnd - fadeStart));
	} else if (cameraDistance > lod1Distance) {
		nSteps = 6;  // Medium quality at medium distance
		float fadeStart = lod1Distance;
		float fadeEnd = lod1Distance + 2048.0f;
		float blend = saturate((cameraDistance - fadeStart) / (fadeEnd - fadeStart));
		nSteps = (uint)lerp(8, 6, blend);
		lodFade = 1.0f - blend * 0.3f;  // Slight intensity reduction
	}
	
	const float step = 1.0f / nSteps;


	float3 scatter = 0.0f;
	float3 transmittance = 1.0f;

	[unroll]
	for (uint i = 0; i < nSteps; ++i) {
		float t = (i + 0.5f) * step;
		float3 sampleTransmittance = exp(-step * marchDist * extinction);
		transmittance *= sampleTransmittance;
		float3 sunTransmittance = exp(-sunMarchDist * t * extinction);
		float3 inScatter = scatterCoeff * phase * sunTransmittance;
		scatter += inScatter * (1.0f - sampleTransmittance) / max(extinction, 0.0001f) * transmittance;
	}

	// Apply LOD fade to maintain scattering presence at distance without popping
	result.scatter = scatter * sunColor * occlusion * 3.0f * lodFade;
	result.transmittance = exp(-dist * (1.0f + distRatio) * extinction);

	return result;
}

// ============================================================================
// WAVE EDGE LIGHTING — Forward scatter, rim glow, subsurface transmission
// ============================================================================
//
// Four terms:
//   1. FORWARD SCATTER — Light transmitted through wave crests. Scales with
//      raw wave height (game units) for amplitude-proportional intensity.
//   2. RIM GLOW — Silhouette edge highlights at grazing view angles.
//   3. AMBIENT SCATTER — Always-on view-dependent baseline that keeps
//      the water surface alive even on gentle waves.
//   4. WRAP DIFFUSE — Half-Lambert term that extends illumination around
//      wave curvature, softening the transition into troughs.

float3 CalculateWaveEdgeSSS(
	float3 viewDirection,
	float3 waveNormal,
	float3 sunDir,
	float3 sunColor,
	float waveHeight,
	float horizontalDisplacement,
	float3 shallowColor,
	float3 deepColor)
{
	float3 N = normalize(waveNormal);
	float3 V = -viewDirection;
	float3 L = sunDir;

	float NdotL = dot(N, L);
	float NdotV = saturate(dot(N, V));

	float maxAmp = max(Wave1Amplitude * WaveAmplitude * M_TO_GAME_UNIT, 1.0f);
	float normHeight = saturate(max(0.0f, waveHeight) / maxAmp);

	float3 sssColor = lerp(deepColor, shallowColor, 0.65f);

	// ── 1. FORWARD SCATTER ──────────────────────────────────────────────
	float LdotV = saturate(dot(L, viewDirection));
	float forwardPhase = pow(LdotV, 4.0f);

	// Surfaces whose normal faces away from the sun transmit more light
	float normalMask = pow(saturate(0.5f - 0.5f * NdotL), 2.0f);

	// normHeight (0-1) drives intensity — no raw game unit blowout
	float forwardTerm = forwardPhase * normalMask * normHeight * 0.8f;

	// ── 2. RIM GLOW ─────────────────────────────────────────────────────
	float rim = pow(1.0f - NdotV, 3.0f);
	float wrapLit = saturate(NdotL * 0.4f + 0.6f);
	float rimTerm = rim * wrapLit * normHeight * 0.35f;

	// ── 3. AMBIENT SCATTER ──────────────────────────────────────────────
	float ambientTerm = 0.08f * NdotV * NdotV;

	// ── 4. WRAP DIFFUSE ─────────────────────────────────────────────────
	float wrapDiffuse = saturate(NdotL * 0.5f + 0.5f);
	float wrapTerm = wrapDiffuse * normHeight * 0.15f;

	// ── COMBINE ─────────────────────────────────────────────────────────
	return (forwardTerm + rimTerm + ambientTerm + wrapTerm) * sssColor * sunColor;
}
#endif  // PBR_WATER

DiffuseOutput GetWaterDiffuseColor(PS_INPUT input, float3 normal, float3 viewDirection, inout float4 distanceMul, float refractionsDepthFactor, float fresnel, uint eyeIndex, float3 viewPosition, float noise, float depth)
{
#			if defined(REFRACTIONS)
	float4 refractionNormal = mul(transpose(TextureProj[eyeIndex]), float4((VarAmounts.w * refractionsDepthFactor * normal.xy) + input.MPosition.xy, input.MPosition.z, 1));

	float2 refractionUvRaw = float2(refractionNormal.x, refractionNormal.w - refractionNormal.y) / refractionNormal.ww;
	refractionUvRaw = Stereo::ConvertToStereoUV(refractionUvRaw, eyeIndex);  // need to convert here for VR due to refractionNormal values

#				if defined(VR)
	float2 refractionUvRawNoStereo = Stereo::ConvertFromStereoUV(refractionUvRaw, eyeIndex, 1);
#				endif

	float2 screenPosition = FrameBuffer::DynamicResolutionParams1.xy * (FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy);

	float2 refractionScreenPosition = FrameBuffer::DynamicResolutionParams1.xy * (refractionUvRaw / VPOSOffset.xy);
	float4 refractionWorldPosition = float4(input.WPosition.xyz * depth / viewPosition.z, 0);

#				if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float refractionDepth = GetScreenDepthWater(refractionScreenPosition);
	depth = refractionDepth;
#					if !defined(VR)
	float refractionDepthMul = length(float3((((VPOSOffset.zw + refractionUvRaw) * 2 - 1)) * refractionDepth / ProjData.xy, refractionDepth));
#					else
	float refractionDepthMul = CalculateDepthMultFromUV(refractionUvRawNoStereo, refractionDepth, eyeIndex);
#					endif  //VR

	float3 refractionDepthAdjustedViewDirection = -viewDirection * refractionDepthMul;
	float refractionViewSurfaceAngle = dot(refractionDepthAdjustedViewDirection, ReflectPlane[eyeIndex].xyz);

	float refractionPlaneMul = (1 - ReflectPlane[eyeIndex].w / refractionViewSurfaceAngle);

	if (refractionPlaneMul < 0.0) {
		refractionUvRaw = FrameBuffer::DynamicResolutionParams2.xy * input.HPosition.xy * VPOSOffset.xy + VPOSOffset.zw;  // This value is already stereo converted for VR
	} else {
		distanceMul = saturate(refractionPlaneMul * float4(length(refractionDepthAdjustedViewDirection).xx, abs(refractionViewSurfaceAngle).xx) / FogParam.z);

#					if defined(VR)
		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4((refractionUvRawNoStereo * 2 - 1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
#					else
		refractionWorldPosition = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4((refractionUvRaw * 2 - 1) * float2(1, -1), DepthTex.Load(float3(refractionScreenPosition, 0)).x, 1));
#					endif
		refractionWorldPosition.xyz /= refractionWorldPosition.w;
	}
#				endif

	float2 refractionUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(refractionUvRaw);
	float3 refractionColor = RefractionTex.Sample(RefractionSampler, refractionUV).xyz;
	float3 refractionDiffuseColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), distanceMul.y);

#				if defined(UNDERWATER)
	float refractionMul = 0;
#				else
	float refractionMul = 1 - pow(saturate((-distanceMul.x * FogParam.z + FogParam.z) / FogParam.w), FogNearColor.w);
#				endif

#				if defined(PBR_WATER)
	// Physically-based water scattering
	WaterScatteringResult scatterResult;
	scatterResult.scatter = 0.0f;
	scatterResult.transmittance = 1.0f;

	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)) {
		float scatterOcclusion = 1.0f;

#					if defined(TERRAIN_SHADOWS)
		scatterOcclusion *= TerrainShadows::GetTerrainShadow(input.WPosition.xyz, LinearSampler);
#					endif

#					if defined(SKYLIGHTING)
#						if defined(VR)
		float3 scatterPosMSSkylight = input.WPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#						else
		float3 scatterPosMSSkylight = input.WPosition.xyz;
#						endif
		sh2 scatterSkylightingSH = Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.HPosition.xy, scatterPosMSSkylight, float3(0, 0, 1));
		float scatterSkylighting = SphericalHarmonics::Unproject(scatterSkylightingSH, float3(0, 0, 1));
		scatterOcclusion *= saturate(scatterSkylighting);
#					endif

		float camDist = input.WPosition.w;
		scatterResult = CalculateWaterScattering(
			input.WPosition.xyz,
			refractionWorldPosition.xyz,
			SunDir.xyz,
			SunColor.xyz * SunDir.w,
			scatterOcclusion,
			camDist
		);
	}

	// Enhanced depth-based absorption using Beer-Lambert law
	if (EnableLightingOverrides > 0.5f) {
		float depthMeters = distanceMul.x * FogParam.z * 0.01f;
		float absorption = 1.0f - exp(-AbsorptionDensity * depthMeters);
		float transparencyFactor = saturate(WaterTransparency * (1.0f - absorption));
		refractionDiffuseColor = lerp(ShallowColor.xyz, DeepColor.xyz, saturate(distanceMul.y + absorption * 0.5f));
		float3 scatterColor = DeepColor.xyz * ScatteringCoeff * (1.0f - absorption);
		refractionDiffuseColor += scatterColor;
		refractionMul = saturate(refractionMul * transparencyFactor);
	}
#				endif

	// Direction for shadow ray march: toward refracted sample, or view direction if degenerate
	float3 refractedViewDirection = viewDirection;
	float3 refractedDelta = refractionWorldPosition.xyz - input.WPosition.xyz;
	float refractedLenSq = dot(refractedDelta, refractedDelta);
	if (refractedLenSq > 1e-10)
		refractedViewDirection = refractedDelta * rsqrt(refractedLenSq);

	DiffuseOutput output;
	output.refractionColor = refractionColor;
	output.refractionDiffuseColor = refractionDiffuseColor;
	output.depth = depth;
	output.refractionMul = refractionMul;
	output.refractedViewDirection = refractedViewDirection;
#				if defined(PBR_WATER)
	// Apply water scattering if enabled
	if (SharedData::pbrWaterSettings.EnableWaterScattering) {
		output.scatter = scatterResult.scatter;
		output.transmittance = scatterResult.transmittance;
	} else {
		output.scatter = 0.0f;
		output.transmittance = 1.0f;
	}

	float3 rawWaveSSS = CalculateWaveEdgeSSS(
		viewDirection,
		input.UnifiedWaveNormal.xyz,
		SunDir.xyz,
		SunColor.xyz * SunDir.w,
		input.UnifiedWaveInfo.z,
		input.UnifiedWaveNormal.w,
		ShallowColor.xyz,
		DeepColor.xyz
	);

	// Smooth distance fade for wave SSS
	float camDist = input.WPosition.w;
	float sssFade = 1.0f - saturate((camDist - WaveFadeStart) / max(WaveFadeEnd - WaveFadeStart, 1.0f));
	sssFade = sssFade * sssFade * (3.0f - 2.0f * sssFade);  // Smoothstep

	float scatterLuminance = dot(scatterResult.scatter, float3(0.2126f, 0.7152f, 0.0722f));
	float scatterMuting = saturate(1.0f - scatterLuminance * 0.5f);
	output.waveSSS = rawWaveSSS * sssFade * scatterMuting;
#				else
	output.scatter = 0.0f;
	output.transmittance = 1.0f;
	output.waveSSS = 0.0f;
#				endif
	return output;
#			else
	DiffuseOutput output;
	output.refractionColor = lerp(Color::Water(ShallowColor.xyz), Color::Water(DeepColor.xyz), fresnel) * GetLdotN(normal);
	output.refractionDiffuseColor = output.refractionColor;
	output.depth = 1;
	output.refractionMul = 1;
	output.refractedViewDirection = viewDirection;
	output.scatter = 0.0f;
	output.transmittance = 1.0f;
	output.waveSSS = 0.0f;
	return output;
#			endif
}

float3 GetSunColor(float3 normal, float3 viewDirection, float3 worldPosition, uint eyeIndex)
{
#			if defined(UNDERWATER)
	return 0.0.xxx;
#			else
	if (Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)
		return 0.0.xxx;

#			if defined(PBR_WATER)
	if (SharedData::pbrWaterSettings.EnableBRDFSpecular) {
		return WaterBRDF::GetSunSpecular(normal, viewDirection, SunDir, SunColor.xyz, VarAmounts.x, DeepColor.w);
	}
#			endif

	float3 reflectionDirection = reflect(viewDirection, normal);
	float reflectionMul = exp2(VarAmounts.x * log2(saturate(dot(reflectionDirection, SunDir.xyz))));

	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	float3 sunColor = Color::DirectionalLight((SunColor.xyz * SunDir.w) / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * (1.0 - exp(-DeepColor.w)) * llDirLightMult;
#				if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		sunColor *= ExponentialHeightFog::GetSunlightFogAttenuation(worldPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	}
#				endif
	return reflectionMul * sunColor;
#			endif
}
#		endif

#		if defined(LIGHT_LIMIT_FIX)
#			include "LightLimitFix/LightLimitFix.hlsli"
#		endif

#		if defined(ISL) && defined(LIGHT_LIMIT_FIX)
#			include "InverseSquareLighting/InverseSquareLighting.hlsli"
#		endif

#		if defined(IBL)
#			include "IBL/IBL.hlsli"
#		endif

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	uint eyeIndex = Stereo::GetEyeIndexPS(input.HPosition, VPOSOffset);
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
#					if !defined(VR)
	float depthMul = length(float3((depthOffset * 2 - 1) * depth / ProjData.xy, depth));
#					else
	float depthMul = CalculateDepthMultFromUV(Stereo::ConvertFromStereoUV(depthOffset, eyeIndex, 1), depth, eyeIndex);
#					endif  //VR
	float3 depthAdjustedViewDirection = -viewDirection * depthMul;
	float viewSurfaceAngle = dot(depthAdjustedViewDirection, ReflectPlane[eyeIndex].xyz);

	float planeMul = (1 - ReflectPlane[eyeIndex].w / viewSurfaceAngle);
	distanceMul = saturate(
		planeMul * float4(length(depthAdjustedViewDirection).xx, abs(viewSurfaceAngle).xx) /
		FogParam.z);
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
	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(input.WPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);

#			if defined(PBR_WATER) && defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
	float foamIntersectionProximity = 0.0f;
	if (FoamEnabled > 0.5f && FoamIntersectionIntensity > 0.0f && FoamIntersectionRange > 0.0f) {
		float sceneVSDepth = depth;
		float waterVSDepth = viewPosition.z;
		float depthGap = max(sceneVSDepth - waterVSDepth, 0.0f);
		foamIntersectionProximity = (1.0f - saturate(depthGap / FoamIntersectionRange)) * FoamIntersectionIntensity;
	}
#			endif

#			if defined(SKYLIGHTING)
	float wetnessOcclusion = 1.0;

#				if defined(VR)
	float3 positionMSSkylight = input.WPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#				else
	float3 positionMSSkylight = input.WPosition.xyz;
#				endif

	sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, positionMSSkylight);
	float skylighting = SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1));

	float skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
	skylightingDiffuse = saturate(skylightingDiffuse);
	skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(input.WPosition.xyz));
	skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);

	wetnessOcclusion = inWorld ? pow(saturate(skylighting), 2) : 0;
#			endif

#			if defined(SKYLIGHTING)
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, eyeIndex, wetnessOcclusion);
#			else
	WaterNormalData waterData = GetWaterNormal(input, distanceBlendFactor, depthControl.z, viewDirection, depth, eyeIndex, inWorld);
#			endif

	float3 normal = waterData.normal;

#			if defined(SKYLIGHTING)
	sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, 0.0);
	float skylightingSpecular = SphericalHarmonics::FuncProductIntegral(skylightingSH, specularLobe);
	skylightingSpecular = saturate(skylightingSpecular);
	skylightingSpecular = Skylighting::mixSpecular(SharedData::skylightingSettings, skylightingSpecular);
#			endif

#		if defined(PBR_WATER)
	// Use the softer diffuse normal for diffuse lighting to prevent wave distortion
	float3 diffuseNormal = waterData.diffuseNormal;
#		else
	float3 diffuseNormal = normal;
#		endif

#		if defined(PBR_WATER)
	float waterDepth = 1e5f;
	{
#		if defined(DEPTH)
		float surfaceDepth = -viewPosition.z;
		if (depth > 0.0f)
			waterDepth = max(0.0f, depth - surfaceDepth);
#		endif
	}
#		endif

	float fresnel = GetFresnelValue(normal, viewDirection);

#			if defined(SKYLIGHTING) && defined(PBR_WATER)
	// Reset PBR path defaults (skylightingDiffuse / skylightingSpecular already declared above when SKYLIGHTING)
	skylightingDiffuse = 1.0;
	skylightingSpecular = 1.0;
	const bool inWorldSkylight = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
	if (inWorldSkylight && !(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior)) {
#				if defined(VR)
		float3 positionMSSkylight = input.WPosition.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#				else
		float3 positionMSSkylight = input.WPosition.xyz;
#				endif
		sh2 skylightingSH = Skylighting::sample(SharedData::skylightingSettings, Skylighting::SkylightingProbeArray, Skylighting::stbn_vec3_2Dx1D_128x128x64, input.HPosition.xy, positionMSSkylight, normal);
		skylightingDiffuse = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
		skylightingDiffuse = saturate(skylightingDiffuse);
		skylightingDiffuse = lerp(1.0, skylightingDiffuse, Skylighting::getFadeOutFactor(input.WPosition.xyz));
		skylightingDiffuse = Skylighting::mixDiffuse(SharedData::skylightingSettings, skylightingDiffuse);

		float waterRoughness = lerp(0.05, 0.15, saturate(WaveIntensity));
		sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normal, -viewDirection, waterRoughness);
		skylightingSpecular = SphericalHarmonics::FuncProductIntegral(skylightingSH, specularLobe);
		skylightingSpecular = lerp(1.0, skylightingSpecular, Skylighting::getFadeOutFactor(input.WPosition.xyz));
		skylightingSpecular = Skylighting::mixSpecular(SharedData::skylightingSettings, skylightingSpecular);
	}
#			endif

#			if defined(SPECULAR) && (NUM_SPECULAR_LIGHTS != 0)
	float3 finalColor = 0.0.xxx;

	[unroll] for (int lightIndex = 0; lightIndex < NUM_SPECULAR_LIGHTS; ++lightIndex)
	{
		float3 lightVector = LightPos[lightIndex].xyz - (PosAdjust[eyeIndex].xyz + input.WPosition.xyz);
		float3 lightDirection = normalize(normalize(lightVector) - viewDirection);
		float lightFade = saturate(length(lightVector) / LightPos[lightIndex].w);
		float lightColorMul = (1 - lightFade * lightFade);
		float LdotN = saturate(dot(lightDirection, normal));
		float3 lightColor = (Color::PointLight(LightColor[lightIndex].xyz) * pow(LdotN, FresnelRI.z)) * lightColorMul;
		finalColor += lightColor;
	}

	finalColor *= fresnel;
#				if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override specular color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorSpecular(waterData.rippleInfo, 2.5, 4.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#				endif

	isSpecular = true;
#			else

	float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);

	// Diffuse first: refraction direction is required for filtered directional shadow sampling
	DiffuseOutput diffuseOutput = GetWaterDiffuseColor(input, diffuseNormal, viewDirection, distanceMul, depthControl.y, fresnel, eyeIndex, viewPosition, screenNoise, depth);

	float surfaceShadow;
	float dirShadow = ShadowSampling::Get3DFilteredShadow(input.WPosition.xyz, diffuseOutput.refractedViewDirection, input.HPosition.xy, eyeIndex, surfaceShadow);

	float3 dirColor;
	float3 ambientColor;
#				if defined(SKYLIGHTING) && !defined(INTERIOR)
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor, skylightingDiffuse);
#				else
	ShadowSampling::ExtractLighting(diffuseOutput.refractionDiffuseColor, dirColor, ambientColor);
#				endif

	dirColor *= dirShadow;

#				if defined(SKYLIGHTING)
	ambientColor = Color::IrradianceToLinear(ambientColor);
	ambientColor *= skylightingDiffuse;
	ambientColor = Color::IrradianceToGamma(ambientColor);
#				endif

	diffuseOutput.refractionDiffuseColor = dirColor + ambientColor;

	float3 specularColor = GetWaterSpecularColor(input, normal, viewDirection, distanceFactor, depthControl.y, eyeIndex);

	float3 diffuseColor = lerp(diffuseOutput.refractionColor, diffuseOutput.refractionDiffuseColor, diffuseOutput.refractionMul);

	depthControl = DepthControl * (distanceMul - 1) + 1;
#				if defined(PBR_WATER)
	// Use enhanced depth control values only when ESP overrides enabled
	if (EnableLightingOverrides > 0.5f) {
		float4 overrideDepth = float4(DepthReflections, DepthRefractions, DepthNormals, DepthSpecularLighting);
		depthControl = overrideDepth * (distanceMul - 1) + 1;
	}
#				endif

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
			if (LightLimitFix::IsLightIgnored(light) || light.lightFlags & LightLimitFix::LightFlags::Shadow) {
				continue;
			}

			float3 lightDirection = light.positionWS[eyeIndex].xyz - input.WPosition.xyz;
			float lightDist = length(lightDirection);

#					if defined(ISL)
			float intensityMultiplier = InverseSquareLighting::GetAttenuation(lightDist, light);
#					else
			float intensityFactor = saturate(lightDist / light.radius);
			float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#					endif

			float3 normalizedLightDirection = normalize(lightDirection);

			float3 H = normalize(normalizedLightDirection - viewDirection);
			float HdotN = saturate(dot(H, normal));

			const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * pow(HdotN, FresnelRI.z) * light.fade;
			specularLighting += lightColor * intensityMultiplier;
		}
	}
	specularColor += specularLighting * 3;
#				endif

#				if defined(UNDERWATER)
	float3 finalSpecularColor = lerp(Color::Water(ShallowColor.xyz), specularColor, 0.5);
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

	float3 sunColor = GetSunColor(normal, viewDirection, input.WPosition.xyz, eyeIndex) * surfaceShadow;
	if (!(Permutation::PixelShaderDescriptor & Permutation::WaterFlags::Interior) && any(sunColor > 0.0)) {
#						if defined(SKYLIGHTING) && defined(PBR_WATER)
		sunColor *= skylightingSpecular;
#						endif

#						if defined(PBR_WATER)
		// Wave self-shadowing: waves can cast shadows on other wave surfaces
		float waveTimeSec = ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds);
		float waveDayPh = ComputeWaveDayPhase(GameTimeHours);
		float2 waveWorldPosPS = input.WPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy;
		float currentWaveHeight = input.UnifiedWaveInfo.z;

		float waveSelfShadow = CalculateWaveSelfShadow(
			waveWorldPosPS,
			currentWaveHeight,
			SunDir.xyz,
			WaveIntensity,
			WaveAmplitude,
			waveTimeSec,
			waveDayPh
		);
		sunColor *= waveSelfShadow;
#						endif
	}

#					if defined(VC)
#						if defined(PBR_WATER)
	// PBR fresnel-based blending: fresnel determines reflection/refraction ratio directly
	float specularFraction = fresnel * diffuseOutput.refractionMul;
#						else
	float specularFraction = lerp(1, fresnel * diffuseOutput.refractionMul, distanceBlendFactor);
#						endif
	float3 finalColorPreFog = lerp(diffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;

#						if defined(PBR_WATER)
	finalColorPreFog += diffuseOutput.scatter;
	finalColorPreFog += diffuseOutput.waveSSS;

	if (FoamEnabled > 0.5f) {
		float intersectVC = 0.0f;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
		intersectVC = foamIntersectionProximity;
#							endif
		FoamResult foam = CalculateFoam(
			input.WPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy,
			input.UnifiedWaveNormal.xyz,
			input.UnifiedWaveInfo.z,
			input.UnifiedWaveNormal.w,
			input.UnifiedWaveInfo.xy,
			input.UnifiedWaveInfo.w,
			ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds),
			input.WPosition.w,
			FoamIntensity,
			SunDir.xyz,
			SunColor.xyz * SunDir.w,
			intersectVC);
		finalColorPreFog = lerp(finalColorPreFog, foam.color, foam.alpha);
	}
#						endif

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 fogColor = Color::Fog(input.FogParam.xyz);
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#						endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, fogColor);
		fogColor = exponentialHeightFog.xyz;
		fogDistanceFactor = exponentialHeightFog.w;
	} else
#						endif
		fogColor *= PosAdjust[eyeIndex].w;

	float3 finalColor = lerp(finalColorPreFog, fogColor, fogDistanceFactor);

#						if defined(WETNESS_EFFECTS) && defined(DEBUG_WETNESS_EFFECTS)
	// DEBUG MODE: Override water color with debug visualization
	float3 debugColor = WetnessEffects::GetDebugWetnessColorStandard(waterData.rippleInfo, 2.0, 3.0);
	if (any(debugColor)) {
		finalColor = debugColor;
	}
#						endif

#					else
#						if defined(PBR_WATER)
	// PBR fresnel-based blending: fresnel determines reflection/refraction ratio directly
	float specularFraction = fresnel;
#						else
	float specularFraction = lerp(1, fresnel, distanceBlendFactor);
#						endif
	float3 finalColorPreFog = lerp(diffuseOutput.refractionDiffuseColor, specularColor, specularFraction) + sunColor * depthControl.w;

#						if defined(PBR_WATER)
	finalColorPreFog += diffuseOutput.scatter;
	finalColorPreFog += diffuseOutput.waveSSS;

	if (FoamEnabled > 0.5f) {
		float intersectFM = 0.0f;
#							if defined(DEPTH) && !defined(VERTEX_ALPHA_DEPTH)
		intersectFM = foamIntersectionProximity;
#							endif
		FoamResult foam2 = CalculateFoam(
			input.WPosition.xy + FrameBuffer::CameraPosAdjust[eyeIndex].xy,
			input.UnifiedWaveNormal.xyz,
			input.UnifiedWaveInfo.z,
			input.UnifiedWaveNormal.w,
			input.UnifiedWaveInfo.xy,
			input.UnifiedWaveInfo.w,
			ComputeWaveTimeSeconds(GameTimeHours, RealTimeSeconds),
			input.WPosition.w,
			FoamIntensityFlowmap,
			SunDir.xyz,
			SunColor.xyz * SunDir.w,
			intersectFM);
		finalColorPreFog = lerp(finalColorPreFog, foam2.color, foam2.alpha);
	}
#						endif

#						if !defined(UNIFIED_WATER)
	float fogDistanceFactor = input.FogParam.w;
	float3 preFogColor = Color::Fog(input.FogParam.xyz);
#						else
	float fogDistanceFactor = min(FogFarColor.w, pow(saturate(length(input.WPosition.xyz) * FogParam.y - FogParam.x), FresnelRI.y));
	float3 preFogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogDistanceFactor));
#						endif

	fogDistanceFactor = Color::FogAlpha(fogDistanceFactor);

#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		preFogColor = ImageBasedLighting::GetFogIBLColor(preFogColor);
	}
#						endif
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFog(input.WPosition.xyz, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, preFogColor);
		preFogColor = exponentialHeightFog.xyz;
		fogDistanceFactor = exponentialHeightFog.w;
	} else
#						endif
		preFogColor *= PosAdjust[eyeIndex].w;

	finalColorPreFog = lerp(finalColorPreFog, preFogColor, fogDistanceFactor);

	float3 refractionColor = diffuseOutput.refractionColor;

	float fogFactor = min(FogParam.w, pow(saturate(-diffuseOutput.depth * FogParam.y - FogParam.x), FogParam.z));
	float3 fogColor = Color::Fog(lerp(FogNearColor.xyz, FogFarColor.xyz, fogFactor));
#						if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		fogFactor = 0;
	}
#						endif
#						if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#						endif
	refractionColor = lerp(refractionColor, fogColor, Color::FogAlpha(fogFactor));

	float3 finalColor = lerp(refractionColor, finalColorPreFog, diffuseOutput.refractionMul);
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

#		if defined(PBR_WATER)
	// Depth Estimation Debug Visualization
	// Uncomment to enable depth debug view
	//#define DEBUG_WATER_DEPTH
	#if defined(DEBUG_WATER_DEPTH)
	{
		float depthDbg = input.DepthDebug.x;
		float debugCode = input.DepthDebug.y;
		float terrainZ = input.DepthDebug.z;
		float waterZ = input.DepthDebug.w;

		DepthEstimationDebug debugInfo;
		debugInfo.depth = depthDbg;
		debugInfo.debugCode = debugCode;
		debugInfo.terrainZ = terrainZ;
		debugInfo.waterZ = waterZ;
		finalColor = VisualizeDepthEstimation(debugInfo);
	}
	#endif

	if (WireframeEnabled > 0.5f) {
		float3 baryCoords = input.Barycentric;

		if (WireframeEnabled > 1.5f) {
			// Mode 2: Raw barycentric visualization (debug)
			finalColor = baryCoords;
		} else {
			// Mode 1: Wireframe visualization
			float3 baryDeriv = fwidth(baryCoords);

			bool geometryShaderActive = any(baryDeriv > 1e-5.xxx);

			if (geometryShaderActive) {
				float3 edgeDist = baryCoords / max(baryDeriv, 1e-6.xxx);
				float minEdgeDist = min(edgeDist.x, min(edgeDist.y, edgeDist.z));

				float wireThickness = 0.4f;
				float wireSmooth = 0.3f;
				float wireHighlight = 1.0f - smoothstep(wireThickness - wireSmooth, wireThickness + wireSmooth, minEdgeDist);

				float vertexThickness = 1.2f;
				float vertexSmooth = 1.5f;
				float3 vertexDist = (1.0f - baryCoords) / max(baryDeriv, 1e-6.xxx);
				float minVertDist = min(vertexDist.x, min(vertexDist.y, vertexDist.z));
				float vertexHighlight = 1.0f - smoothstep(vertexThickness - vertexSmooth, vertexThickness + vertexSmooth, minVertDist);

				float3 wireColor = float3(0.95f, 0.45f, 0.15f);
				float3 vertexColor = float3(0.15f, 0.95f, 0.35f);

				finalColor = lerp(finalColor, wireColor, wireHighlight);
				finalColor = lerp(finalColor, vertexColor, vertexHighlight);
			} else {
				float2 worldUV = input.WPosition.xy * 0.01f;
				float2 gridFrac = frac(worldUV);
				float2 gridDist = min(gridFrac, 1.0f - gridFrac);
				float2 gridFw = max(fwidth(worldUV), 1e-4.xx);
				float gridLine = 1.0f - smoothstep(gridFw.x * 0.5f, gridFw.x * 1.5f, min(gridDist.x, gridDist.y));

				float3 fallbackColor = float3(0.8f, 0.2f, 0.2f);
				finalColor = lerp(finalColor, fallbackColor, gridLine * 0.8f);
			}
		}
	}
#		endif

	psout.Lighting = saturate(float4(finalColor, isSpecular));
#		endif

#		if defined(STENCIL)
	float3 viewDirection = normalize(input.WorldPosition.xyz);
	float3 normal =
		normalize(cross(ddx_coarse(input.WorldPosition.xyz), ddy_coarse(input.WorldPosition.xyz)));
	float VdotN = dot(viewDirection, normal);
	psout.WaterMask = float4(0, 0, VdotN, 0);

#	if defined(PBR_WATER)
	// Output camera-only motion vectors for Gerstner waves
	// Pass the SAME world position for both current and previous frame projection
	// This captures camera movement without the wave animation component
	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.WorldPosition);
#	else
	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);
#	endif
#		endif

	return psout;
}

#	endif

#endif
