#ifndef __WATER_DEPTH_ESTIMATION_HLSLI__
#define __WATER_DEPTH_ESTIMATION_HLSLI__

// ============================================================================
// WATER DEPTH ESTIMATION FROM TERRAIN HEIGHTMAP
// ============================================================================
//
// This system estimates water depth by sampling the terrain heightmap
// in the vertex shader using the raw terrain elevation data.
// This enables depth-aware wave modulation in worldspace.
//
// Key principles:
// 1. Use raw terrain heightmap texture (t61) - NOT the shadow-processed data (t60)
// 2. Sample heightmap at water surface XY world position
// 3. Get terrain Z from normalized heightmap value using RawHeightmapZRange
// 4. Calculate water depth = water surface Z - terrain Z
// 5. Modulate wave amplitude based on depth
//
// Advantages over depth buffer approach:
// - Works in worldspace (not screen-dependent)
// - Works for off-screen water vertices
// - No projection/inverse projection needed
// - More stable and predictable results
// - Independent of shadow calculation (no contamination)

// Raw terrain heightmap texture (slot 61) - separate from shadow data (slot 60)
// Format: Single-channel float with normalized elevation [0,1]
// This is the pure terrain elevation data, NOT processed with shadow information
#if defined(VSHADER) || defined(DSHADER)
Texture2D<float> TerrainElevationTexture : register(t61);

// Sampler for terrain heightmap - using linear filtering for smooth interpolation
SamplerState TerrainHeightSampler : register(s12);
#endif

// ============================================================================
// TERRAIN HEIGHTMAP SAMPLING
// ============================================================================

/**
 * Converts world XY to terrain heightmap UV coordinates
 * Uses the same transformation as TerrainShadows
 * @param worldXY World position XY coordinates
 * @param scaleXY Terrain heightmap scale from PerFrame buffer
 * @param offsetXY Terrain heightmap offset from PerFrame buffer
 * @return Heightmap UV coordinates [0,1]
 */
float2 GetTerrainHeightmapUV(float2 worldXY, float2 scaleXY, float2 offsetXY)
{
	return worldXY * scaleXY + offsetXY;
}

/**
 * Converts normalized heightmap Z value to world Z coordinate
 * Uses the raw heightmap Z range (pos0.z to pos1.z)
 * @param normZ Normalized Z value from heightmap [0,1]
 * @param rawZMin Raw heightmap minimum Z (pos0.z)
 * @param rawZMax Raw heightmap maximum Z (pos1.z)
 * @return World Z coordinate in game units
 */
float GetTerrainWorldZ(float normZ, float rawZMin, float rawZMax)
{
	return lerp(rawZMin, rawZMax, normZ);
}

/**
 * Samples terrain height at a world XY position using raw elevation texture
 * @param worldXY World position XY coordinates
 * @param scaleXY Terrain heightmap scale
 * @param offsetXY Terrain heightmap offset
 * @param rawZMin Raw heightmap minimum Z (pos0.z)
 * @param rawZMax Raw heightmap maximum Z (pos1.z)
 * @return Terrain world Z coordinate, or very large negative value if unavailable
 */
float SampleTerrainHeight(float2 worldXY, float2 scaleXY, float2 offsetXY, float rawZMin, float rawZMax)
{
#if defined(VSHADER) || defined(DSHADER)
	// Get heightmap UV coordinates
	float2 heightmapUV = GetTerrainHeightmapUV(worldXY, scaleXY, offsetXY);
	
	// Check if UV is in valid range [0,1]
	if (heightmapUV.x < 0.0f || heightmapUV.x > 1.0f || 
	    heightmapUV.y < 0.0f || heightmapUV.y > 1.0f) {
		return -1e6f;  // Outside heightmap coverage
	}
	
	// Sample raw terrain elevation texture (t61) - single channel normalized height
	// This is pure terrain elevation, NOT contaminated with shadow data
	float normalizedHeight = TerrainElevationTexture.SampleLevel(TerrainHeightSampler, heightmapUV, 0);
	
	// DEBUG: If sample is zero, texture may not be bound
	if (abs(normalizedHeight) < 0.0001f) {
		// Could be valid (sea level terrain) or unbound texture
		// Allow it through - sea level is a valid height
	}
	
	// Convert normalized height [0,1] to world Z using raw heightmap range
	float terrainWorldZ = GetTerrainWorldZ(normalizedHeight, rawZMin, rawZMax);
	
	return terrainWorldZ;
#else
	return -1e6f;  // Pixel shader doesn't need this
#endif
}

// Debug output structure for depth estimation visualization
struct DepthEstimationDebug
{
	float depth;           // Calculated water depth in game units
	float debugCode;       // Diagnostic code:
	                       // 0 = success
	                       // 1 = outside heightmap coverage
	                       // 2 = negative depth (terrain above water)
	float terrainZ;        // Sampled terrain world Z
	float waterZ;          // Water surface world Z
};

/**
 * Estimates water depth at a world position using terrain heightmap (with debug info)
 * 
 * @param waterWorldPos Absolute world XYZ position of water surface
 * @param scaleXY Terrain heightmap scale
 * @param offsetXY Terrain heightmap offset  
 * @param rawZMin Raw heightmap minimum Z (TerrainRawZMin from PerFrame)
 * @param rawZMax Raw heightmap maximum Z (TerrainRawZMax from PerFrame)
 * @param debugOut Debug information output (optional)
 * @return Estimated water depth in game units (positive = water above terrain)
 *         Returns large value (1e5) if depth cannot be determined
 */
float EstimateWaterDepthFromTerrain(float3 waterWorldPos, float2 scaleXY, float2 offsetXY, float rawZMin, float rawZMax, out DepthEstimationDebug debugOut)
{
	debugOut.depth = 1e5f;
	debugOut.debugCode = 0.0f;
	debugOut.terrainZ = 0.0f;
	debugOut.waterZ = waterWorldPos.z;
	
#if !defined(VSHADER) && !defined(DSHADER)
	// Pixel shader doesn't need depth estimation
	return 1e5f;
#else
	// Sample terrain height at water XY position using raw elevation texture
	float terrainZ = SampleTerrainHeight(waterWorldPos.xy, scaleXY, offsetXY, rawZMin, rawZMax);
	debugOut.terrainZ = terrainZ;
	
	// Check if sample was valid
	if (terrainZ < -1e5f) {
		debugOut.debugCode = 1.0f;  // Outside heightmap coverage
		return 1e5f;
	}
	
	// Calculate depth as vertical distance from terrain to water surface
	float waterDepth = waterWorldPos.z - terrainZ;
	
	// Handle negative depth (terrain above water - shouldn't normally happen)
	if (waterDepth < 0.0f) {
		debugOut.debugCode = 2.0f;  // Negative depth
		debugOut.depth = 0.0f;
		return 0.0f;
	}
	
	debugOut.depth = waterDepth;
	debugOut.debugCode = 0.0f;  // Success
	return waterDepth;
#endif
}

/**
 * Simplified version without debug output for production use
 */
float EstimateWaterDepthFromTerrain(float3 waterWorldPos, float2 scaleXY, float2 offsetXY, float rawZMin, float rawZMax)
{
	DepthEstimationDebug debugOut;
	return EstimateWaterDepthFromTerrain(waterWorldPos, scaleXY, offsetXY, rawZMin, rawZMax, debugOut);
}
/**
 * Visualizes depth estimation debug information as a color
 * @param debugInfo Debug information from EstimateWaterDepthFromTerrain
 * @return RGB color for visualization
 */
float3 VisualizeDepthEstimation(DepthEstimationDebug debugInfo)
{
	// Error visualization (bright colors for easy spotting)
	if (debugInfo.debugCode == 1.0f) {
		// Blue = outside heightmap coverage
		return float3(0.0f, 0.0f, 1.0f);
	}
	if (debugInfo.debugCode == 2.0f) {
		// Yellow = negative depth (terrain above water)
		return float3(1.0f, 1.0f, 0.0f);
	}
	
	float depth = debugInfo.depth;
	
	if (depth > 1000.0f) {
		// Very deep water - white
		return float3(1.0f, 1.0f, 1.0f);
	} else if (depth > 200.0f) {
		// Medium depth - cyan to blue gradient
		float t = saturate((depth - 200.0f) / 800.0f);
		return lerp(float3(0.0f, 1.0f, 1.0f), float3(1.0f, 1.0f, 1.0f), t);
	} else if (depth > 50.0f) {
		// Shallow - green to cyan gradient
		float t = saturate((depth - 50.0f) / 150.0f);
		return lerp(float3(0.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 1.0f), t);
	} else {
		// Very shallow - yellow to green gradient
		float t = saturate(depth / 50.0f);
		return lerp(float3(1.0f, 0.0f, 0.0f), float3(0.0f, 1.0f, 0.0f), t);
	}
}

/**
 * Multi-sample depth estimation with filtering for more stable results
 * Samples depth at multiple points around the query position and averages
 * This reduces noise from terrain geometry edges
 * 
 * @param waterWorldPos Absolute world position of water surface
 * @param scaleXY Terrain heightmap scale
 * @param offsetXY Terrain heightmap offset
 * @param rawZMin Raw heightmap minimum Z
 * @param rawZMax Raw heightmap maximum Z
 * @param sampleRadius Radius in world units to sample (default 64 units)
 * @return Averaged water depth estimate
 */
float EstimateWaterDepthFiltered(float3 waterWorldPos, float2 scaleXY, float2 offsetXY, float rawZMin, float rawZMax, float sampleRadius = 64.0f)
{
	// just center sample for now
	float centerDepth = EstimateWaterDepthFromTerrain(waterWorldPos, scaleXY, offsetXY, rawZMin, rawZMax);
	
	// Can add more samples here later if needed (davo reminder)
		// float3 offset1 = waterWorldPos + float3(sampleRadius, 0, 0);
		// float3 offset2 = waterWorldPos + float3(0, sampleRadius, 0);
		// float3 offset3 = waterWorldPos + float3(-sampleRadius, 0, 0);
		// float3 offset4 = waterWorldPos + float3(0, -sampleRadius, 0);
		// Average valid samples
	
	return centerDepth;
}

#endif // __WATER_DEPTH_ESTIMATION_HLSLI__
