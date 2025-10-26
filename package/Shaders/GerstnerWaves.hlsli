/**
 * Gerstner Wave System
 * Based on GPU Gems Chapter 1 by Mark Finch and Cyan Worlds
 * Provides realistic wave simulation with directionality and steepness control
 * 
 * NOTE: This file contains standalone Gerstner wave functions. The main wave displacement
 * system (CalculateWaterDisplacement, CalculateGerstnerNormals) remains in Water.hlsl because
 * it depends on shoreline detection functions that cannot be extracted.
 */

#ifndef GERSTNER_WAVES_HLSLI
#define GERSTNER_WAVES_HLSLI

/**
 * Calculate Gerstner wave values for a given position
 * @param position 2D world position
 * @param direction Normalized wave direction vector
 * @param amplitude Wave height multiplier
 * @param wavelength Distance between wave peaks
 * @param steepness Wave steepness factor (0-1, higher = more peaked)
 * @param timer Time value for animation
 * @return float3 (cos(phase), sin(phase), frequency)
 */
float3 GerstnerWaveValues(float2 position, float2 direction, float amplitude, float wavelength, float steepness, float timer)
{
	float w = 2.0 * 3.14159265 / wavelength;
	float dotD = dot(position, direction);
	float phase = w * dotD + timer;
	return float3(cos(phase), sin(phase), w);
}

/**
 * Calculate Gerstner wave normal contribution
 * @param direction Wave direction
 * @param amplitude Wave amplitude  
 * @param steepness Wave steepness (Q factor)
 * @param vals Wave values from GerstnerWaveValues
 * @return Normal contribution
 */
float3 GerstnerWaveNormal(float2 direction, float amplitude, float steepness, float3 vals)
{
	float C = vals.x;
	float S = vals.y; 
	float w = vals.z;
	float WA = w * amplitude;
	float WAC = WA * C;
	float3 normal = float3(-direction.x * WAC, 1.0 - steepness * WA * S, -direction.y * WAC);
	return normalize(normal);
}

/**
 * Calculate Gerstner wave displacement
 * @param direction Wave direction
 * @param amplitude Wave amplitude
 * @param steepness Wave steepness
 * @param vals Wave values from GerstnerWaveValues
 * @return 3D displacement vector
 */
float3 GerstnerWaveDisplacement(float2 direction, float amplitude, float steepness, float3 vals)
{
	float C = vals.x;
	float S = vals.y;
	float Q = steepness / (2.0 * 3.14159265 / 60.0 * amplitude);
	return float3(Q * amplitude * direction.x * C, amplitude * S, Q * amplitude * direction.y * C);
}

/**
 * Compute enhanced wave normal with Gerstner wave contribution
 * @param worldPos World space position
 * @param baseNormal Existing normal from texture sampling
 * @param timer Time for animation
 * @param waveIntensity Overall wave intensity multiplier
 * @return Enhanced normal with wave displacement
 */
float3 ComputeEnhancedWaveNormal(float3 worldPos, float3 baseNormal, float timer, float waveIntensity)
{
	if (waveIntensity < 0.01) return baseNormal;
	
	float2 waveDir1 = normalize(float2(1.0, 0.3));
	float2 waveDir2 = normalize(float2(-0.5, 1.0));
	
	float3 combinedNormal = baseNormal;
	
	float3 vals1 = GerstnerWaveValues(worldPos.xz * 0.01, waveDir1, 0.8, 120.0, 0.3, timer * 0.5);
	float3 normal1 = GerstnerWaveNormal(waveDir1, 0.8, 0.3, vals1);
	
	float3 vals2 = GerstnerWaveValues(worldPos.xz * 0.015, waveDir2, 0.5, 80.0, 0.4, timer * 0.7);
	float3 normal2 = GerstnerWaveNormal(waveDir2, 0.5, 0.4, vals2);
	
	float3 vals3 = GerstnerWaveValues(worldPos.xz * 0.03, waveDir1, 0.2, 40.0, 0.5, timer * 1.2);
	float3 normal3 = GerstnerWaveNormal(waveDir1, 0.2, 0.5, vals3);
	
	combinedNormal = normalize(baseNormal + waveIntensity * (normal1 * 0.5 + normal2 * 0.3 + normal3 * 0.2));

	return combinedNormal;
}

#endif // GERSTNER_WAVES_HLSLI
