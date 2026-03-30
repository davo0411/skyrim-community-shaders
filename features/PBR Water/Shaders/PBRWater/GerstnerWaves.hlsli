#ifndef __GERSTNER_WAVES_HLSLI__
#define __GERSTNER_WAVES_HLSLI__

#include "Common/Game.hlsli"
#include "PBRWater/WaterDepthEstimation.hlsli"

// ============================================================================
// STATISTICAL OCEAN WAVE SYNTHESIS
// ============================================================================
// 
// This system creates non-repeating ocean waves by generating unique wave sets
// per spatial cell, then blending smoothly between cells. This approximates
// FFT ocean simulation without requiring compute shader infrastructure.
//
// Key principles:
// 1. Spatial cells - world divided into large cells, each with unique waves
// 2. Hash-based generation - cell coordinates seed deterministic random waves
// 3. Ocean spectrum - Phillips/JONSWAP spectrum for realistic energy distribution
// 4. Cell blending - smooth interpolation at cell boundaries
// 5. Multiple octaves - different cell sizes for different wave scales
//
// References:
// - Tessendorf "Simulating Ocean Water" (SIGGRAPH 2001)
// - GPU Gems Chapter 1 "Effective Water Simulation"
// - Catlike Coding "Waves" tutorial

static const float UW_PI = 3.14159265f;
static const float UW_TWO_PI = 6.28318530f;
static const float UW_GRAVITY = 9.81f;  // m/s²

float WrapUnifiedPhase(float phase)
{
	float wrapped = fmod(phase, UW_TWO_PI);
	return wrapped < 0.0f ? wrapped + UW_TWO_PI : wrapped;
}

float ComputeWaveTimeSeconds(float gameTimeHours, float realTimeSeconds)
{
	float gameSeconds = gameTimeHours * 3600.0f;
	float combined = gameSeconds + realTimeSeconds;
	return frac(combined / 65536.0f) * 65536.0f;
}

float ComputeWaveDayPhase(float gameTimeHours)
{
	float dayFraction = frac(gameTimeHours / 24.0f);
	return dayFraction * UW_TWO_PI;
}

// ============================================================================
// CONSTANT BUFFER - Wave parameters from CPU
// ============================================================================

#define UNIFIED_WATER_HAS_PER_FRAME_CBUFFER 1
cbuffer UnifiedWaterPerFrame : register(b7)
{
	// Main wave controls
	float WaveIntensity : packoffset(c0.x);      // Master wave strength (0-1)
	float WaveAmplitude : packoffset(c0.y);      // Amplitude multiplier for all waves
	float WaveSpeed : packoffset(c0.z);          // Speed multiplier (1.0 = physically accurate)
	float WaveSteepness : packoffset(c0.w);      // Global steepness multiplier
	
	// Time synchronization
	float GameTimeHours : packoffset(c1.x);
	float RealTimeSeconds : packoffset(c1.y);
	float TimeScale : packoffset(c1.z);
	float CellWorldSize : packoffset(c1.w);
	float PrevGameTimeHours : packoffset(c2.x);
	float PrevRealTimeSeconds : packoffset(c2.y);
	float PrevTimeScale : packoffset(c2.z);
	float EnableLightingOverrides : packoffset(c2.w);
	
	// Fresnel and reflection
	float FresnelBias : packoffset(c3.x);
	float FresnelPower : packoffset(c3.y);
	float ReflectionStrength : packoffset(c3.z);
	float RefractionStrength : packoffset(c3.w);
	
	// Water optical properties
	float WaterTransparency : packoffset(c4.x);
	float AbsorptionDensity : packoffset(c4.y);
	float ScatteringCoeff : packoffset(c4.z);
	float SpecularIntensity : packoffset(c4.w);
	
	// Sun specular
	float SunSpecularPower : packoffset(c5.x);
	float SunSpecularMagnitude : packoffset(c5.y);
	float SunSparklePower : packoffset(c5.z);
	float SunSparkleMagnitude : packoffset(c5.w);
	float SpecularRadius : packoffset(c6.x);
	float SpecularBrightness : packoffset(c6.y);
	
	// Fog
	float AboveWaterFogDistNear : packoffset(c6.z);
	float AboveWaterFogDistFar : packoffset(c6.w);
	float AboveWaterFogAmount : packoffset(c7.x);
	float UnderwaterFogDistNear : packoffset(c7.y);
	float UnderwaterFogDistFar : packoffset(c7.z);
	float UnderwaterFogAmount : packoffset(c7.w);
	
	// Depth controls
	float DepthReflections : packoffset(c8.x);
	float DepthRefractions : packoffset(c8.y);
	float DepthNormals : packoffset(c8.z);
	float DepthSpecularLighting : packoffset(c8.w);
	
	// Wireframe view
	float WireframeEnabled : packoffset(c9.x);
	float PerFramePad0 : packoffset(c9.y);
	float PerFramePad1 : packoffset(c9.z);
	float PerFramePad2 : packoffset(c9.w);
	
	// Individual wave parameters (wavelength & amplitude in METERS for intuitive editing)
	// Wave 1: Primary ocean swell (largest, slowest)
	float Wave1Amplitude : packoffset(c10.x);     // Amplitude in meters (typ. 0.3-1.0m)
	float Wave1Wavelength : packoffset(c10.y);    // Wavelength in meters (typ. 30-100m)
	float Wave1Steepness : packoffset(c10.z);     // Steepness 0-1 (typ. 0.3-0.5)
	float Wave1AngleOffset : packoffset(c10.w);   // Direction offset radians
	
	// Wave 2: Secondary swell (medium)
	float Wave2Amplitude : packoffset(c11.x);     // typ. 0.15-0.5m
	float Wave2Wavelength : packoffset(c11.y);    // typ. 15-40m
	float Wave2Steepness : packoffset(c11.z);
	float Wave2AngleOffset : packoffset(c11.w);
	
	// Wave 3: Wind waves (smaller, faster)
	float Wave3Amplitude : packoffset(c12.x);     // typ. 0.08-0.25m
	float Wave3Wavelength : packoffset(c12.y);    // typ. 8-20m
	float Wave3Steepness : packoffset(c12.z);
	float Wave3AngleOffset : packoffset(c12.w);
	
	// Wave 4: Chop (short period)
	float Wave4Amplitude : packoffset(c13.x);     // typ. 0.03-0.1m
	float Wave4Wavelength : packoffset(c13.y);    // typ. 3-8m
	float Wave4Steepness : packoffset(c13.z);
	float Wave4AngleOffset : packoffset(c13.w);
	
	// Wave 5: Fine ripples
	float Wave5Amplitude : packoffset(c14.x);     // typ. 0.01-0.05m
	float Wave5Wavelength : packoffset(c14.y);    // typ. 1-4m
	float Wave5Steepness : packoffset(c14.z);
	float Wave5AngleOffset : packoffset(c14.w);
	
	// Wave 6: Micro detail
	float Wave6Amplitude : packoffset(c15.x);     // typ. 0.005-0.02m
	float Wave6Wavelength : packoffset(c15.y);    // typ. 0.5-2m
	float Wave6Steepness : packoffset(c15.z);
	float Wave6AngleOffset : packoffset(c15.w);
	
	// Tessellation
	float TessellationEnabled : packoffset(c16.x);
	float WaveFadeStart : packoffset(c16.y);       // Distance where waves start fading
	float WaveFadeEnd : packoffset(c16.z);         // Distance where waves fully fade
	float TessPadding3 : packoffset(c16.w);
	
	// Player ripples
	float PlayerPosX : packoffset(c17.x);
	float PlayerPosY : packoffset(c17.y);
	float PlayerPosZ : packoffset(c17.z);
	float PlayerSpeed : packoffset(c17.w);
	float PlayerInWater : packoffset(c18.x);
	float PlayerVelocityX : packoffset(c18.y);  // Actual velocity for wake direction
	float PlayerVelocityY : packoffset(c18.z);
	float PlayerWaterDepth : packoffset(c18.w);  // Depth below water surface
	float RippleStrength : packoffset(c19.x);
	float RippleRadius : packoffset(c19.y);
	float RippleWaveSpeed : packoffset(c19.z);
	float RippleWaveFreq1 : packoffset(c19.w);
	float RippleWaveFreq2 : packoffset(c20.x);
	float RippleWaveFreq3 : packoffset(c20.y);
	float RippleNormalStrength : packoffset(c20.z);
	
	// Foam System
	float FoamEnabled : packoffset(c20.w);
	float FoamIntensity : packoffset(c21.x);
	float FoamIntensityFlowmap : packoffset(c21.y);
	float FoamThreshold : packoffset(c21.z);
	float FoamSharpness : packoffset(c21.w);
	float FoamIntersectionRange : packoffset(c22.x);
	float FoamIntersectionIntensity : packoffset(c22.y);
	float FoamPad_c22z : packoffset(c22.z);
	float FoamPad_c22w : packoffset(c22.w);
	float FoamPad_c23x : packoffset(c23.x);
	float FoamPad_c23y : packoffset(c23.y);
	float FoamPad_c23z : packoffset(c23.z);
	
	// Legacy padding (unused — shallow large-wave range is fixed in-shader: 5m cutoff, 30m full)
	float ShallowWaveDepthMin : packoffset(c23.w);
	float ShallowWaveDepthMax : packoffset(c24.x);
	float ShoreWavePad0 : packoffset(c24.y);
	float ShoreWaveStrength : packoffset(c24.z);        // Shore-directed swell intensity (5m–30m band)
	
	// Terrain heightmap parameters (for vertex shader depth estimation)
	float TerrainHeightmapEnabled : packoffset(c24.w);  // Is terrain heightmap available (0 or 1)
	float TerrainScaleX : packoffset(c25.x);            // Heightmap UV scale X
	float TerrainScaleY : packoffset(c25.y);            // Heightmap UV scale Y
	float TerrainOffsetX : packoffset(c25.z);           // Heightmap UV offset X
	float TerrainOffsetY : packoffset(c25.w);           // Heightmap UV offset Y
	float TerrainZRangeMin : packoffset(c26.x);         // Terrain Z range minimum
	float TerrainZRangeMax : packoffset(c26.y);         // Terrain Z range maximum
	float FFTWavesEnabled : packoffset(c26.z);          // 1.0 = FFT waves active

	// FFT cascade parameters (game units)
	float FFTCascade0TileLenX : packoffset(c26.w);
	float FFTCascade0TileLenY : packoffset(c27.x);
	float FFTCascade0DispScale : packoffset(c27.y);
	float FFTCascade0NormScale : packoffset(c27.z);
	float FFTCascade1TileLenX : packoffset(c27.w);
	float FFTCascade1TileLenY : packoffset(c28.x);
	float FFTCascade1DispScale : packoffset(c28.y);
	float FFTCascade1NormScale : packoffset(c28.z);
	float FFTCascade2TileLenX : packoffset(c28.w);
	float FFTCascade2TileLenY : packoffset(c29.x);
	float FFTCascade2DispScale : packoffset(c29.y);
	float FFTCascade2NormScale : packoffset(c29.z);
	float FFTChoppiness : packoffset(c29.w);
	float FFTNumCascades : packoffset(c30.x);
	// GodotOceanWaves-style far falloff: displacement *= min(exp(-(camDist - start)*rate), 1). start<=0 disables.
	float FFTDispFarStart : packoffset(c30.y);
	float FFTDispFarFalloff : packoffset(c30.z);
	// Pixel shader only: blend toward bicubic normal/foam fetch (0=bilinear SampleLevel, 1=full bicubic).
	float FFTBicubicNormals : packoffset(c30.w);

	// FFT-only (Gerstner WaveIntensity / WaveFade / WaveSteepness not used when FFTWavesEnabled)
	float FFTMasterIntensity : packoffset(c31.x);
	float FFTFadeStart : packoffset(c31.y);
	float FFTFadeEnd : packoffset(c31.z);
	float FFTTessActivity : packoffset(c31.w);
}

cbuffer UnifiedWaterPerTile : register(b8)
{
	float4 PrevData : packoffset(c0);
	float4 TileData : packoffset(c1);
}

// FFT ocean textures (bound from C++ at slots 61-63, sampler at 13)
Texture2DArray<float4> FFTDisplacementMap : register(t61);
Texture2DArray<float4> FFTNormalFoamMap : register(t62);
Texture2DArray<float4> FFTPrevDisplacementMap : register(t63);
SamplerState FFTLinearWrapSampler : register(s13);

// ============================================================================
// HIGH QUALITY HASH FUNCTIONS
// ============================================================================
// These need to be high quality to avoid visible patterns in wave generation

uint uhash(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

uint uhash2(uint2 v)
{
	return uhash(v.x ^ uhash(v.y));
}

float hashf(uint x)
{
	return float(uhash(x)) / 4294967295.0f;
}

float hashf2(uint2 v)
{
	return float(uhash2(v)) / 4294967295.0f;
}

float2 hashf22(uint2 v)
{
	uint h = uhash2(v);
	return float2(
		float(h & 0xFFFF) / 65535.0f,
		float(h >> 16) / 65535.0f
	);
}

float4 hashf24(uint2 v)
{
	uint h1 = uhash2(v);
	uint h2 = uhash(h1);
	return float4(
		float(h1 & 0xFFFF) / 65535.0f,
		float(h1 >> 16) / 65535.0f,
		float(h2 & 0xFFFF) / 65535.0f,
		float(h2 >> 16) / 65535.0f
	);
}

// ============================================================================
// WAVE OUTPUT STRUCTURE
// ============================================================================

struct WaveSample
{
	float3 displacement;      // XYZ offset in game units
	float3 normal;            // Surface normal (full detail for specular/reflections)
	float3 geometricNormal;   // Softer normal for diffuse lighting (prevents distortion)
	float2 primaryDirection;  // Dominant wave direction for texture scrolling
	float shoreInfluence;     // Shore-directed wave influence (0-1)
	float shoreDistance;      // Distance to shore in game units (depth gradient)
};

// ============================================================================
// CELL-BASED WAVE GENERATION
// ============================================================================
// Each cell generates N unique waves based on its coordinates
// Adjacent cells blend together smoothly to hide transitions

// Number of waves per cell per octave
#define WAVES_PER_CELL 4

struct CellWaveData
{
	float3 displacement;
	float3 tangentAccum;   // Accumulated tangent perturbation
	float3 binormalAccum;  // Accumulated binormal perturbation
};

CellWaveData EvaluateCellWaves(
	int2 cellCoord,
	float2 localPos,        // Position within cell [0,1]
	float cellSize,         // Cell size in game units
	float baseWavelength,   // Base wavelength for this octave (meters)
	float baseAmplitude,    // Base amplitude for this octave (meters)
	float steepness,
	float speedMult,
	float timeSeconds,
	uint octaveIndex
)
{
	CellWaveData result;
	result.displacement = float3(0, 0, 0);
	result.tangentAccum = float3(0, 0, 0);
	result.binormalAccum = float3(0, 0, 0);
	
	float2 worldPosInCell = localPos * cellSize;
	
	uint cellSeed = uhash2(uint2(
		uint(cellCoord.x + 10000) ^ (octaveIndex * 7919),
		uint(cellCoord.y + 10000) ^ (octaveIndex * 6271)
	));
	
	static const float gravityGame = UW_GRAVITY * M_TO_GAME_UNIT;
	
	[unroll]
	for (int w = 0; w < WAVES_PER_CELL; w++) {
		uint waveSeed = uhash(cellSeed ^ (w * 104729));
		
		float4 rnd = hashf24(uint2(waveSeed, w));
		
		float angle = rnd.x * UW_TWO_PI;
		float2 dir = float2(cos(angle), sin(angle));
		
		float wavelengthVariation = 0.4f + rnd.y * 1.4f;
		float wavelengthM = baseWavelength * wavelengthVariation;
		float wavelengthGame = wavelengthM * M_TO_GAME_UNIT;
		
		float amplitudeVariation = 0.2f + rnd.z * 1.8f;
		float amplitudeM = baseAmplitude * amplitudeVariation;
		float amplitudeGame = amplitudeM * M_TO_GAME_UNIT;
		
		float phaseOffset = rnd.w * UW_TWO_PI;
		
		float k = UW_TWO_PI / wavelengthGame;
		float omega = sqrt(gravityGame * k) * speedMult;
		
		float phase = k * dot(dir, worldPosInCell) - omega * timeSeconds + phaseOffset;
		
		float sinP, cosP;
		sincos(phase, sinP, cosP);
		
		float QA = steepness * amplitudeGame;
		
		result.displacement.x += dir.x * QA * cosP;
		result.displacement.y += dir.y * QA * cosP;
		result.displacement.z += amplitudeGame * sinP;
		
		float kA = k * amplitudeGame;
		float QkA = steepness * kA;
		
		float DxDxQkAsin = dir.x * dir.x * QkA * sinP;
		float DxDyQkAsin = dir.x * dir.y * QkA * sinP;
		float DyDyQkAsin = dir.y * dir.y * QkA * sinP;
		float DxkAcos = dir.x * kA * cosP;
		float DykAcos = dir.y * kA * cosP;
		
		result.tangentAccum.x += DxDxQkAsin;
		result.tangentAccum.y += DxDyQkAsin;
		result.tangentAccum.z += DxkAcos;
		
		result.binormalAccum.x += DxDyQkAsin;
		result.binormalAccum.y += DyDyQkAsin;
		result.binormalAccum.z += DykAcos;
	}
	
	return result;
}

float smoothBlend(float t)
{
	return t * t * (3.0f - 2.0f * t);
}

CellWaveData BlendCellWaves(
	float2 worldPos,
	float cellSize,
	float baseWavelength,
	float baseAmplitude,
	float steepness,
	float speedMult,
	float timeSeconds,
	uint octaveIndex
)
{
	float2 cellPos = worldPos / cellSize;
	int2 cellCoord = int2(floor(cellPos));
	float2 localPos = frac(cellPos);
	
	float2 blend = float2(smoothBlend(localPos.x), smoothBlend(localPos.y));
	
	CellWaveData c00 = EvaluateCellWaves(cellCoord + int2(0, 0), localPos + float2(0, 0), cellSize, baseWavelength, baseAmplitude, steepness, speedMult, timeSeconds, octaveIndex);
	CellWaveData c10 = EvaluateCellWaves(cellCoord + int2(1, 0), localPos + float2(-1, 0), cellSize, baseWavelength, baseAmplitude, steepness, speedMult, timeSeconds, octaveIndex);
	CellWaveData c01 = EvaluateCellWaves(cellCoord + int2(0, 1), localPos + float2(0, -1), cellSize, baseWavelength, baseAmplitude, steepness, speedMult, timeSeconds, octaveIndex);
	CellWaveData c11 = EvaluateCellWaves(cellCoord + int2(1, 1), localPos + float2(-1, -1), cellSize, baseWavelength, baseAmplitude, steepness, speedMult, timeSeconds, octaveIndex);
	
	CellWaveData result;
	
	result.displacement = lerp(
		lerp(c00.displacement, c10.displacement, blend.x),
		lerp(c01.displacement, c11.displacement, blend.x),
		blend.y
	);
	
	result.tangentAccum = lerp(
		lerp(c00.tangentAccum, c10.tangentAccum, blend.x),
		lerp(c01.tangentAccum, c11.tangentAccum, blend.x),
		blend.y
	);
	
	result.binormalAccum = lerp(
		lerp(c00.binormalAccum, c10.binormalAccum, blend.x),
		lerp(c01.binormalAccum, c11.binormalAccum, blend.x),
		blend.y
	);
	
	return result;
}

// ============================================================================
// SHORE WAVE GENERATION
// ============================================================================
// Generates simple shore-directed Gerstner waves that replace attenuated cell waves
// in the shallow band. Beach shoaling (Green's law) applies only when terrain gradient
// is meaningful; flat stream beds skip it so shallow-depth attenuation is not undone.
// Direction comes from terrain gradient, else flow map, else the default cell diagonal.

struct ShoreWaveResult
{
	float3 displacement;
	float3 tangentAccum;
	float3 binormalAccum;
};

ShoreWaveResult EvaluateShoreWaves(
	float2 worldPos,
	float2 shoreDir,
	float shoreInfluence,
	float depthGameUnits,
	float timeSeconds,
	float amplitudeMult,
	float steepnessMult,
	float shoreGradientMag
)
{
	ShoreWaveResult result;
	result.displacement = float3(0, 0, 0);
	result.tangentAccum = float3(0, 0, 0);
	result.binormalAccum = float3(0, 0, 0);

	if (shoreInfluence < 0.01f)
		return result;

	float depthM = depthGameUnits / M_TO_GAME_UNIT;

	// Shoaling (beach / sloping shore): amplitude ~ depth^(-1/4). Not valid for flat stream
	// beds where terrain gradient is ~0 — there we skip amplification so shallow-depth
	// crossfade is not fought by artificial shoal gain.
	static const float kShoreGradForShoaling = 0.001f;
	float shoalFactor = 1.0f;
	if (shoreGradientMag > kShoreGradForShoaling) {
		shoalFactor = pow(max(depthM, 0.15f), -0.25f);
		shoalFactor = min(shoalFactor, 2.5f);
	}

	float baseAngle = atan2(shoreDir.y, shoreDir.x);
	float gravityGame = UW_GRAVITY * M_TO_GAME_UNIT;

	// Shore wave components: wavelengths derived from user Wave1-3 settings
	// scaled down for the shorter fetch near shore, amplitudes proportional
	// to user settings so shore waves stay in proportion with open water.
	float wavelengthsGame[3] = {
		max(Wave1Wavelength * 0.6f, 4.0f) * M_TO_GAME_UNIT,
		max(Wave2Wavelength * 0.5f, 2.0f) * M_TO_GAME_UNIT,
		max(Wave3Wavelength * 0.4f, 1.0f) * M_TO_GAME_UNIT
	};
	float amplitudesGame[3] = {
		Wave1Amplitude * 0.5f * M_TO_GAME_UNIT,
		Wave2Amplitude * 0.4f * M_TO_GAME_UNIT,
		Wave3Amplitude * 0.3f * M_TO_GAME_UNIT
	};
	static const float angleOffsets[3] = { 0.0f, 0.25f, -0.3f };

	[unroll]
	for (int w = 0; w < 3; w++) {
		float angle = baseAngle + angleOffsets[w];
		float2 dir = float2(cos(angle), sin(angle));

		float ampGame = amplitudesGame[w] * amplitudeMult * shoalFactor * shoreInfluence;

		float k = UW_TWO_PI / wavelengthsGame[w];
		float omega = sqrt(gravityGame * k);

		float phase = k * dot(dir, worldPos) - omega * timeSeconds;
		float sinP, cosP;
		sincos(phase, sinP, cosP);

		float steepness = min(Wave1Steepness, 0.5f) * steepnessMult;
		float QA = steepness * ampGame;

		result.displacement.x += dir.x * QA * cosP;
		result.displacement.y += dir.y * QA * cosP;
		result.displacement.z += ampGame * sinP;

		float kA = k * ampGame;
		float QkA = steepness * kA;

		float DxDxQkAsin = dir.x * dir.x * QkA * sinP;
		float DxDyQkAsin = dir.x * dir.y * QkA * sinP;
		float DyDyQkAsin = dir.y * dir.y * QkA * sinP;
		float DxkAcos = dir.x * kA * cosP;
		float DykAcos = dir.y * kA * cosP;

		result.tangentAccum.x += DxDxQkAsin;
		result.tangentAccum.y += DxDyQkAsin;
		result.tangentAccum.z += DxkAcos;

		result.binormalAccum.x += DxDyQkAsin;
		result.binormalAccum.y += DyDyQkAsin;
		result.binormalAccum.z += DykAcos;
	}

	return result;
}

// ============================================================================
// TESSELLATION HINT (VS, no trig — matches octave LOD in CalculateWaterDisplacement)
// ============================================================================
// Per-vertex [0,1] score: higher where short wavelengths and large amplitudes need more mesh density.
// Symmetric edge factors in the hull use max(hint0, hint1) so shared edges stay seam-free.

float EstimateWaveTessellationHint(float cameraDistance)
{
	if (WaveIntensity <= 0.001f) {
		return 0.0f;
	}

	float distanceFade = 1.0f;
	if (cameraDistance > 0.0f && WaveFadeEnd > WaveFadeStart) {
		distanceFade = 1.0f - saturate((cameraDistance - WaveFadeStart) / (WaveFadeEnd - WaveFadeStart));
		distanceFade = distanceFade * distanceFade * (3.0f - 2.0f * distanceFade);
		if (distanceFade <= 0.001f) {
			return 0.0f;
		}
	}

	float fadeRange = max(WaveFadeEnd - WaveFadeStart, 1.0f);
	float lodNorm = saturate((cameraDistance - WaveFadeStart * 0.25f) * rcp(fadeRange));
	bool octaveActive3 = (lodNorm < 0.6f);
	bool octaveActive45 = (lodNorm < 0.3f);

	float deepAmp = WaveIntensity * WaveAmplitude * distanceFade;

	float wl[6] = {
		max(Wave1Wavelength, 1.0f),
		max(Wave2Wavelength, 0.5f),
		max(Wave3Wavelength, 0.25f),
		max(Wave4Wavelength, 0.1f),
		max(Wave5Wavelength, 0.05f),
		max(Wave6Wavelength, 0.025f)
	};
	float ampM[6] = {
		max(Wave1Amplitude, 0.0f),
		max(Wave2Amplitude, 0.0f),
		max(Wave3Amplitude, 0.0f),
		max(Wave4Amplitude, 0.0f),
		max(Wave5Amplitude, 0.0f),
		max(Wave6Amplitude, 0.0f)
	};
	float st[6] = {
		saturate(Wave1Steepness),
		saturate(Wave2Steepness),
		saturate(Wave3Steepness),
		saturate(Wave4Steepness),
		saturate(Wave5Steepness),
		saturate(Wave6Steepness)
	};

	float score = 0.0f;
	[unroll]
	for (int i = 0; i < 6; i++) {
		if (ampM[i] < 1e-5f) {
			continue;
		}
		if (i == 3 && !octaveActive3) {
			continue;
		}
		if (i >= 4 && !octaveActive45) {
			continue;
		}
		float wlGame = max(wl[i] * M_TO_GAME_UNIT, 1.0f);
		float k = UW_TWO_PI / wlGame;
		float ampGame = ampM[i] * M_TO_GAME_UNIT * deepAmp;
		score += ampGame * k * st[i] * WaveSteepness;
	}
	return saturate(score * 0.0018f);
}

// ============================================================================
// FFT WAVE SAMPLING — O(1) per vertex via texture lookups
// ============================================================================

#if defined(PSHADER)
// Bicubic B-spline filtering (GPU Gems 2 ch. 20), matching GodotOceanWaves water.gdshader.
float4 FFTCubicWeights(float a)
{
	float a2 = a * a;
	float a3 = a2 * a;
	float w0 = -a3 + a2 * 3.0f - a * 3.0f + 1.0f;
	float w1 = a3 * 3.0f - a2 * 6.0f + 4.0f;
	float w2 = -a3 * 3.0f + a2 * 3.0f + a * 3.0f + 1.0f;
	float w3 = a3;
	return float4(w0, w1, w2, w3) * (1.0f / 6.0f);
}

float4 SampleFFTNormalFoamBicubic(float2 uvTile, float layerIdx)
{
	const float dim = 256.0f; // must match FFT_MAP_SIZE / ocean texture resolution
	float2 dims = float2(dim, dim);
	float2 dimsInv = 1.0f / dims;
	float3 uvw = float3(uvTile * dims + 0.5f, layerIdx);
	float2 fuv = frac(uvw.xy);
	float4 wx = FFTCubicWeights(fuv.x);
	float4 wy = FFTCubicWeights(fuv.y);
	float4 g = float4(wx.xz + wx.yw, wy.xz + wy.yw);
	float4 h = (float4(wx.yw, wy.yw) / g + float4(-1.5f, 0.5f, -1.5f, 0.5f) + floor(uvw.xy).xyxy) * float4(dimsInv.x, dimsInv.y, dimsInv.x, dimsInv.y);
	float2 wMix = g.xz / (g.xz + g.yw);
	float4 s0 = lerp(
		FFTNormalFoamMap.SampleLevel(FFTLinearWrapSampler, float3(h.yw, layerIdx), 0),
		FFTNormalFoamMap.SampleLevel(FFTLinearWrapSampler, float3(h.xw, layerIdx), 0),
		wMix.x);
	float4 s1 = lerp(
		FFTNormalFoamMap.SampleLevel(FFTLinearWrapSampler, float3(h.yz, layerIdx), 0),
		FFTNormalFoamMap.SampleLevel(FFTLinearWrapSampler, float3(h.xz, layerIdx), 0),
		wMix.x);
	return lerp(s0, s1, wMix.y);
}
#endif

// waveIntensity argument kept for call-site compatibility; FFT path uses FFTMasterIntensity from b7.
WaveSample SampleFFTWaves(
	float2 worldPos,
	float waveIntensity,
	float cameraDistance,
	float waterDepth,
	float2 shoreDirection,
	float shoreGradientMag,
	bool usePreviousFrame
)
{
	WaveSample result;
	result.displacement = float3(0.0f, 0.0f, 0.0f);
	result.normal = float3(0.0f, 0.0f, 1.0f);
	result.geometricNormal = float3(0.0f, 0.0f, 1.0f);
	result.primaryDirection = normalize(float2(0.707f, 0.707f));
	result.shoreInfluence = 0.0f;
	result.shoreDistance = waterDepth;

	if (FFTMasterIntensity <= 0.001f)
		return result;

	// Distance fade: FFTFadeEnd > FFTFadeStart uses FFT-only sliders; otherwise use Wave fade (same as Gerstner path).
	// Prevents "flat ocean" when Wave fade was widened in presets but FFT fade stayed at old tight defaults.
	float fadeStart = (FFTFadeEnd > FFTFadeStart) ? FFTFadeStart : WaveFadeStart;
	float fadeEnd = (FFTFadeEnd > FFTFadeStart) ? FFTFadeEnd : WaveFadeEnd;

	float distanceFade = 1.0f;
	if (cameraDistance > 0.0f && fadeEnd > fadeStart) {
		distanceFade = 1.0f - saturate((cameraDistance - fadeStart) / (fadeEnd - fadeStart));
		distanceFade = distanceFade * distanceFade * (3.0f - 2.0f * distanceFade);
		if (distanceFade <= 0.001f)
			return result;
	}

	// GodotOceanWaves vertex shader: min(exp(-(horizontalDist - 150)*0.007), 1). We use cameraDistance (radial).
	float farDispFalloff = 1.0f;
	if (FFTDispFarStart > 0.0f && cameraDistance > 0.0f && FFTDispFarFalloff > 0.0f)
		farDispFalloff = min(exp(-(cameraDistance - FFTDispFarStart) * FFTDispFarFalloff), 1.0f);

	const float sampleFade = distanceFade * farDispFalloff;

	// Shallow water attenuation for large cascades
	float depthMeters = waterDepth / M_TO_GAME_UNIT;
	float largeWaveBlend = 1.0f;
	if (waterDepth < 1e4f) {
		depthMeters = max(depthMeters, 0.06f);
		largeWaveBlend = saturate((depthMeters - 5.0f) / max(30.0f - 5.0f, 0.001f));
		result.shoreInfluence = saturate(1.0f - largeWaveBlend) * ShoreWaveStrength;
	}
	result.shoreDistance = depthMeters * M_TO_GAME_UNIT;

	// Cascade tile lengths and scales (from cbuffer)
	float2 tileLens[3] = {
		float2(FFTCascade0TileLenX, FFTCascade0TileLenY),
		float2(FFTCascade1TileLenX, FFTCascade1TileLenY),
		float2(FFTCascade2TileLenX, FFTCascade2TileLenY)
	};
	float dispScales[3] = { FFTCascade0DispScale, FFTCascade1DispScale, FFTCascade2DispScale };
	float normScales[3] = { FFTCascade0NormScale, FFTCascade1NormScale, FFTCascade2NormScale };

	float3 totalDisp = float3(0, 0, 0);
	float2 totalGrad = float2(0, 0);
	float totalFoam = 0;

	uint numCascades = (uint)FFTNumCascades;

	[unroll]
	for (uint c = 0; c < 3; c++) {
		if (c >= numCascades)
			break;

		float2 uv = worldPos / max(tileLens[c], float2(1.0f, 1.0f));
		float scale = dispScales[c] * sampleFade;

		// Attenuate large cascades in shallow water (Gerstner: kill swell in very shallow).
		// FFT: never zero cascade 0 — only short tiles remain → dense spiky noise (“rocks”).
		if (c == 0)
			scale *= max(largeWaveBlend, 0.35f);

		float4 disp;
		if (usePreviousFrame)
			disp = FFTPrevDisplacementMap.SampleLevel(FFTLinearWrapSampler, float3(uv, float(c)), 0);
		else
			disp = FFTDisplacementMap.SampleLevel(FFTLinearWrapSampler, float3(uv, float(c)), 0);

		// disp.xyz = (hx, hy, hz) where hy is vertical
		totalDisp.x += disp.x * scale * FFTChoppiness;
		totalDisp.y += disp.z * scale * FFTChoppiness;
		totalDisp.z += disp.y * scale;

		float4 nfLin = FFTNormalFoamMap.SampleLevel(FFTLinearWrapSampler, float3(uv, float(c)), 0);
#if defined(PSHADER)
		float4 nf = nfLin;
		if (FFTBicubicNormals > 0.001f) {
			float4 nfB = SampleFFTNormalFoamBicubic(uv, float(c));
			nf = lerp(nfLin, nfB, saturate(FFTBicubicNormals));
		}
#else
		float4 nf = nfLin;
#endif
		totalGrad += nf.xy * normScales[c] * sampleFade;
		totalFoam += nf.w;
	}

	// FFT-only path: Gerstner uses tight caps (±25 / ±100 game units). Ocean chop + three
	// cascades can exceed that without being “wrong”; saturating every vertex caused the
	// jagged rock mesh. Keep a generous safety bound instead.
	const float maxHorizDispFFT = 400.0f;
	const float maxVertDispFFT = 400.0f;
	totalDisp.xy = clamp(totalDisp.xy, -maxHorizDispFFT, maxHorizDispFFT);
	totalDisp.z = clamp(totalDisp.z, -maxVertDispFFT, maxVertDispFFT);

	result.displacement = totalDisp;

	// Normal from gradient
	float3 waveNormal = normalize(float3(-totalGrad.x, -totalGrad.y, 1.0f));
	if (waveNormal.z < 0.0f)
		waveNormal = -waveNormal;

	// Clamp extreme slope
	float2 nXY = waveNormal.xy;
	float xyLen = length(nXY);
	if (xyLen > 2.0f) {
		nXY *= 2.0f / xyLen;
		waveNormal = normalize(float3(nXY, sqrt(max(1.0f - dot(nXY, nXY), 0.05f))));
	}

	result.normal = waveNormal;
	result.geometricNormal = waveNormal;

	// Shore direction blending
	float2 shoreDirEff = shoreDirection;
	float shoreDirLenSq = dot(shoreDirEff, shoreDirEff);
	if (shoreDirLenSq > 1e-8f && result.shoreInfluence > 0.01f) {
		shoreDirEff *= rsqrt(shoreDirLenSq);
		float blendT = saturate(result.shoreInfluence * 2.0f);
		float2 defaultDir = normalize(float2(0.707f, 0.707f));
		result.primaryDirection = normalize(lerp(defaultDir, shoreDirEff, blendT));
	}

	return result;
}

// ============================================================================
// MAIN WAVE CALCULATION - STATISTICAL SYNTHESIS (Legacy Gerstner fallback)
// ============================================================================

WaveSample CalculateWaterDisplacement(
	float2 worldPos,
	float2 textureDims,
	float2 texCoordOffset,
	float waveIntensity,
	float amplitudeMult,
	float speedMult,
	float steepnessMult,
	float timeSeconds,
	float dayPhase,
	float2 flowBiasDir,
	float flowBiasWeight,
	bool usePreviousFrame,
	float cameraDistance = 0.0f,
	float waterDepth = 1e5f,
	float2 shoreDirection = float2(0.0f, 0.0f),
	float shoreGradientMag = 0.0f
)
{
	// Use FFT path when available
	if (FFTWavesEnabled > 0.5f) {
		return SampleFFTWaves(
			worldPos,
			waveIntensity,
			cameraDistance,
			waterDepth,
			shoreDirection,
			shoreGradientMag,
			usePreviousFrame
		);
	}

	// Legacy Gerstner path (fallback)
	WaveSample result;
	result.displacement = float3(0.0f, 0.0f, 0.0f);
	result.normal = float3(0.0f, 0.0f, 1.0f);
	result.geometricNormal = float3(0.0f, 0.0f, 1.0f);
	result.primaryDirection = normalize(float2(0.707f, 0.707f));
	result.shoreInfluence = 0.0f;

	if (waveIntensity <= 0.001f) {
		return result;
	}
	
	float distanceFade = 1.0f;
	if (cameraDistance > 0.0f && WaveFadeEnd > WaveFadeStart) {
		distanceFade = 1.0f - saturate((cameraDistance - WaveFadeStart) / (WaveFadeEnd - WaveFadeStart));
		distanceFade = distanceFade * distanceFade * (3.0f - 2.0f * distanceFade);
		
		if (distanceFade <= 0.001f) {
			return result;
		}
	}
	
	static const float kLargeWaveShallowCutoffM = 5.0f;
	static const float kLargeWaveFullDepthM = 30.0f;
	static const float kMinDepthForWavesM = 0.06f;
	static const float kStreamDetailDepthM = 1.25f;

	float depthMeters = waterDepth / M_TO_GAME_UNIT;
	float largeWaveBlend = 1.0f;
	float shoreWaveInfluence = 0.0f;
	bool hasShoreData = false;

	if (waterDepth < 1e4f) {
		depthMeters = max(depthMeters, kMinDepthForWavesM);
		largeWaveBlend = saturate((depthMeters - kLargeWaveShallowCutoffM) /
			max(kLargeWaveFullDepthM - kLargeWaveShallowCutoffM, 0.001f));

		shoreWaveInfluence = saturate(1.0f - largeWaveBlend) * ShoreWaveStrength;
		result.shoreInfluence = shoreWaveInfluence;

		hasShoreData = (depthMeters > kLargeWaveShallowCutoffM) && (largeWaveBlend < 0.995f) &&
		               (ShoreWaveStrength > 0.001f);
	} else {
		result.shoreInfluence = 0.0f;
	}

	result.shoreDistance = depthMeters * M_TO_GAME_UNIT;

	float baseAmplitude = waveIntensity * amplitudeMult * distanceFade;
	
	float userWavelengths[6] = {
		max(Wave1Wavelength, 1.0f),
		max(Wave2Wavelength, 0.5f),
		max(Wave3Wavelength, 0.25f),
		max(Wave4Wavelength, 0.1f),
		max(Wave5Wavelength, 0.05f),
		max(Wave6Wavelength, 0.025f)
	};
	
	float userAmplitudes[6] = {
		max(Wave1Amplitude, 0.0f),
		max(Wave2Amplitude, 0.0f),
		max(Wave3Amplitude, 0.0f),
		max(Wave4Amplitude, 0.0f),
		max(Wave5Amplitude, 0.0f),
		max(Wave6Amplitude, 0.0f)
	};
	
	float userSteepness[6] = {
		saturate(Wave1Steepness),
		saturate(Wave2Steepness),
		saturate(Wave3Steepness),
		saturate(Wave4Steepness),
		saturate(Wave5Steepness),
		saturate(Wave6Steepness)
	};
	
	float3 totalDisp = float3(0, 0, 0);
	float3 totalTangent = float3(0, 0, 0);
	float3 totalBinormal = float3(0, 0, 0);
	float3 geoTangent = float3(0, 0, 0);
	float3 geoBinormal = float3(0, 0, 0);
	
	float fadeRange = max(WaveFadeEnd - WaveFadeStart, 1.0f);
	float lodNorm = saturate((cameraDistance - WaveFadeStart * 0.25f) * rcp(fadeRange));
	bool forceShallowDetailOctaves = (waterDepth < 1e4f) && (depthMeters < kStreamDetailDepthM);
	bool octaveActive3 = (lodNorm < 0.6f) || forceShallowDetailOctaves;
	bool octaveActive45 = (lodNorm < 0.3f) || forceShallowDetailOctaves;
	
	[unroll]
	for (int oct = 0; oct < 6; oct++) {
		if (userAmplitudes[oct] < 0.0001f)
			continue;
		if (oct == 3 && !octaveActive3)
			continue;
		if (oct >= 4 && !octaveActive45)
			continue;
		
		float wavelengthM = userWavelengths[oct];
		float cellSizeGame = wavelengthM * M_TO_GAME_UNIT * 6.0f;
		
		float octaveAmp = userAmplitudes[oct] * baseAmplitude;
		if (oct < 3) {
			octaveAmp *= largeWaveBlend;
		}
		float octaveSteep = userSteepness[oct] * steepnessMult;
		
		CellWaveData cellData = BlendCellWaves(
			worldPos,
			cellSizeGame,
			wavelengthM,
			octaveAmp,
			octaveSteep,
			speedMult,
			timeSeconds,
			uint(oct)
		);
		
		totalDisp += cellData.displacement;
		totalTangent += cellData.tangentAccum;
		totalBinormal += cellData.binormalAccum;

		geoTangent += cellData.tangentAccum;
		geoBinormal += cellData.binormalAccum;
	}
	
	float2 shoreDirEff = shoreDirection;
	float shoreDirLenSq = dot(shoreDirEff, shoreDirEff);
	if (shoreGradientMag <= 0.001f || shoreDirLenSq < 1e-8f) {
		float flowLenSq = dot(flowBiasDir, flowBiasDir);
		if (flowBiasWeight > 0.01f && flowLenSq > 1e-8f)
			shoreDirEff = flowBiasDir * rsqrt(flowLenSq);
		else
			shoreDirEff = normalize(float2(0.707f, 0.707f));
	} else {
		shoreDirEff = shoreDirEff * rsqrt(shoreDirLenSq);
	}

	if (hasShoreData) {
		float shoreAmpMult = waveIntensity * amplitudeMult * distanceFade;
		ShoreWaveResult shoreWaves = EvaluateShoreWaves(
			worldPos,
			shoreDirEff,
			shoreWaveInfluence,
			depthMeters * M_TO_GAME_UNIT,
			timeSeconds,
			shoreAmpMult,
			steepnessMult,
			shoreGradientMag
		);

		totalDisp += shoreWaves.displacement;
		totalTangent += shoreWaves.tangentAccum;
		totalBinormal += shoreWaves.binormalAccum;
		geoTangent += shoreWaves.tangentAccum;
		geoBinormal += shoreWaves.binormalAccum;
	}

	const float maxTangentPerturbation = 0.8f;
	totalTangent = clamp(totalTangent, -maxTangentPerturbation, maxTangentPerturbation);
	totalBinormal = clamp(totalBinormal, -maxTangentPerturbation, maxTangentPerturbation);
	geoTangent = clamp(geoTangent, -maxTangentPerturbation, maxTangentPerturbation);
	geoBinormal = clamp(geoBinormal, -maxTangentPerturbation, maxTangentPerturbation);
	
	float3 tangent = float3(1.0f - totalTangent.x, -totalTangent.y, totalTangent.z);
	float3 binormal = float3(-totalBinormal.x, 1.0f - totalBinormal.y, totalBinormal.z);
	
	tangent = normalize(tangent);
	binormal = normalize(binormal);
	
	float3 rawNormal = cross(binormal, tangent);
	float normalLen = length(rawNormal);
	
	float3 waveNormal;
	if (normalLen < 0.5f) {
		waveNormal = normalize(lerp(float3(0, 0, 1), rawNormal / max(normalLen, 0.001f), normalLen * 2.0f));
	} else {
		waveNormal = rawNormal / normalLen;
	}
	
	if (waveNormal.z < 0.0f) {
		waveNormal = -waveNormal;
	}
	
	const float maxSlope = 2.0f;
	float2 normalXY = waveNormal.xy;
	float xyLen = length(normalXY);
	if (xyLen > maxSlope) {
		normalXY = normalXY * (maxSlope / xyLen);
		waveNormal.xy = normalXY;
		waveNormal.z = sqrt(max(1.0f - dot(normalXY, normalXY), 0.05f));
		waveNormal = normalize(waveNormal);
	}
	
	float3 geoTan = float3(1.0f - geoTangent.x, -geoTangent.y, geoTangent.z);
	float3 geoBin = float3(-geoBinormal.x, 1.0f - geoBinormal.y, geoBinormal.z);
	
	geoTan = normalize(geoTan);
	geoBin = normalize(geoBin);
	
	float3 rawGeoNormal = cross(geoBin, geoTan);
	float geoNormalLen = length(rawGeoNormal);
	
	float3 geoNormal;
	if (geoNormalLen < 0.5f) {
		geoNormal = normalize(lerp(float3(0, 0, 1), rawGeoNormal / max(geoNormalLen, 0.001f), geoNormalLen * 2.0f));
	} else {
		geoNormal = rawGeoNormal / geoNormalLen;
	}
	
	if (geoNormal.z < 0.0f) {
		geoNormal = -geoNormal;
	}
	
	float2 geoNormalXY = geoNormal.xy;
	float geoXYLen = length(geoNormalXY);
	if (geoXYLen > maxSlope) {
		geoNormalXY = geoNormalXY * (maxSlope / geoXYLen);
		geoNormal.xy = geoNormalXY;
		geoNormal.z = sqrt(max(1.0f - dot(geoNormalXY, geoNormalXY), 0.05f));
		geoNormal = normalize(geoNormal);
	}
	
	const float maxHorizDisp = 25.0f;
	const float maxVertDisp = 100.0f;
	
	totalDisp.xy = clamp(totalDisp.xy, -maxHorizDisp, maxHorizDisp);
	totalDisp.z = clamp(totalDisp.z, -maxVertDisp, maxVertDisp);
	
	result.displacement = totalDisp;
	result.normal = waveNormal;
	result.geometricNormal = geoNormal;

	if (hasShoreData) {
		float2 defaultDir = normalize(float2(0.707f, 0.707f));
		float blendT = saturate(shoreWaveInfluence * 2.0f);
		result.primaryDirection = normalize(lerp(defaultDir, shoreDirEff, blendT));
	}

	return result;
}

// ============================================================================
// WAVE SELF-SHADOWING — Ray-march against dominant waves
// ============================================================================
// Marches 4 steps along the sun's horizontal projection and evaluates
// the two largest waves (wave 1 & 2) to check whether a nearby crest
// occludes the light reaching this pixel.  Cost: 2 sincos + 8 sin.

float CalculateWaveSelfShadow(
	float2 worldPos,
	float currentHeight,
	float3 lightDir,
	float waveIntensity,
	float amplitudeMult,
	float timeSeconds,
	float dayPhase)
{
	if (waveIntensity <= 0.01f)
		return 1.0f;

	// FFT normals already encode all frequency content; skip expensive ray march
	if (FFTWavesEnabled > 0.5f)
		return 1.0f;

	// Sun nearly overhead → no meaningful wave-to-wave occlusion
	float lightHorizLen = length(lightDir.xy);
	if (lightHorizLen < 0.05f)
		return 1.0f;

	float2 marchDir = lightDir.xy / lightHorizLen;
	float slopeRatio = lightDir.z / lightHorizLen;

	// Precompute wave 1 direction
	static const float2 baseDir = float2(-0.70710678f, 0.70710678f);
	float sinA1, cosA1;
	sincos(Wave1AngleOffset, sinA1, cosA1);
	float2 dir1 = float2(baseDir.x * cosA1 - baseDir.y * sinA1,
	                      baseDir.x * sinA1 + baseDir.y * cosA1);
	float wl1 = max(Wave1Wavelength * M_TO_GAME_UNIT, 1.0f);
	float k1 = UW_TWO_PI / wl1;
	float omega1 = sqrt(UW_GRAVITY * M_TO_GAME_UNIT * k1) * WaveSpeed;
	float amp1 = Wave1Amplitude * M_TO_GAME_UNIT * amplitudeMult * waveIntensity;

	// Precompute wave 2 direction
	float sinA2, cosA2;
	sincos(Wave2AngleOffset, sinA2, cosA2);
	float2 dir2 = float2(baseDir.x * cosA2 - baseDir.y * sinA2,
	                      baseDir.x * sinA2 + baseDir.y * cosA2);
	float wl2 = max(Wave2Wavelength * M_TO_GAME_UNIT, 1.0f);
	float k2 = UW_TWO_PI / wl2;
	float omega2 = sqrt(UW_GRAVITY * M_TO_GAME_UNIT * k2) * WaveSpeed;
	float amp2 = Wave2Amplitude * M_TO_GAME_UNIT * amplitudeMult * waveIntensity;

	// March distance spans ~1 wavelength of wave 1
	float marchLength = wl1;

	float shadow = 1.0f;

	[unroll]
	for (int i = 0; i < 4; i++) {
		float t = (float(i) + 0.5f) * 0.25f;
		float dist = marchLength * t;
		float2 samplePos = worldPos + marchDir * dist;

		float sampleHeight = amp1 * sin(k1 * dot(dir1, samplePos) - omega1 * timeSeconds)
		                   + amp2 * sin(k2 * dot(dir2, samplePos) - omega2 * timeSeconds);

		// Height of the shadow ray at this horizontal distance
		float rayHeight = currentHeight + dist * slopeRatio;

		// Soft occlusion: penumbra proportional to how far the crest exceeds the ray
		float occlusion = saturate((sampleHeight - rayHeight) * rcp(max(amp1, 0.01f)) + 0.3f);
		shadow = min(shadow, 1.0f - occlusion);
	}

	// Ambient floor: even fully shadowed areas get some indirect light
	return lerp(0.35f, 1.0f, shadow);
}

#endif // __GERSTNER_WAVES_HLSLI__
