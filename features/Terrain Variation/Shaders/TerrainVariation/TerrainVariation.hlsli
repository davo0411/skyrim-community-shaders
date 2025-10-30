// Implements stochastic noise sampling for terrain textures to reduce tiling artifacts and improve visual quality.
// Based on paper "Procedural Stochastic Textures by Tiling and Blending" by Thomas Deliot & Eric Heitz.
// https://eheitzresearch.wordpress.com/722-2/

// Implements texture bombing to overlay small detail textures on terrain based on biome and texture type for added variety.
// Based on ideas from CPU Gems Chapter 20: "Texture Bombing" by R. Steven Glanville.
// https://developer.nvidia.com/gpugems/gpugems/part-iii-materials/chapter-20-texture-bombing

#ifndef TERRAIN_VARIATION_HLSLI
#define TERRAIN_VARIATION_HLSLI

#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

// --------------------- CONSTANTS AND STRUCTURES --------------------- //
// Height blend operator settings - DO NOT CHANGE THESE VALUES.
static const float HEIGHT_BLEND_CONTRAST = 12.0;  // Controls sharpness of height-based transitions (reduced from 16.0 for performance)
static const float HEIGHT_INFLUENCE = 0.3;        // How much height affects blending (0=pure stochastic, 1=pure height)
// Pre-computed constants to avoid runtime calculations
static const float2x2 SKEW_MATRIX = float2x2(1.0, 0.0, -0.57735027, 1.15470054);
static const float WORLD_SCALE = 332.54;
// Blending constants
static const float3 DEFAULT_WEIGHTS = float3(0.33, 0.33, 0.34);
static const float3 LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// Hash constants
static const float2 HASH_MULTIPLIER = float2(1271.5151, 3337.8237);
// Performance optimization constants
static const float MIP_LEVEL_INCREASE = 0.5;      // Additional mip level increase for distance optimization
static const float DISTANCE_SAMPLE_REDUCTION = 2.0; // Mip level where we reduce to 2 samples
static const float FAR_DISTANCE_THRESHOLD = 4.0;  // Mip level where we use single sample with higher mip level

// Structure to hold stochastic sampling offsets and weights
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float2 offset3;
	float3 weights;
};

struct TerrainBombSprite
{
	float4 averageColorMask; // rgb = average color, a = category mask bits
	float4 params;           // x = size scale, y = normal strength, z = height strength, w = flags
	float4 materialParams;   // x = roughness scale, y = metalness scale, z = ao scale, w = height scale
	float4 parallaxParams;   // x = parallax scale, remaining reserved
};

Texture2DArray<float4> TerrainBombTextures : register(t66);
StructuredBuffer<TerrainBombSprite> TerrainBombSpriteTable : register(t67);
Texture2DArray<float4> TerrainBombNormalTextures : register(t120);
Texture2DArray<float4> TerrainBombRMAOSTextures : register(t121);
Texture2DArray<float4> TerrainBombHeightTextures : register(t122);

// --------------------- FUNCTION DECLARATIONS --------------------- //
float4 StochasticSampleLOD(float rnd, Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsetsLOD, float2 dx, float2 dy);
float4 StochasticEffect(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, float2 dx, float2 dy);
float4 StochasticEffectParallax(Texture2D tex, SamplerState samp, float2 uv, float mipLevel, StochasticOffsets offsets, float2 dx, float2 dy);
void ApplyTerrainBombing(float3 worldPosition, float3 baseLinearColor, float3 flatWorldNormal, float3 viewDirWS, float snowCoverage, float viewDistance, inout float3 blendedColor, inout float3 blendedNormalRGB, inout float4 blendedRMAOS);

// --------------------- COMPUTE FUNCTIONS --------------------- //

// Hash function for stochastic sampling
inline float2 hash2D2D(float2 s)
{
	// More efficient hash using frac and multiply operations
	s = frac(s * HASH_MULTIPLIER);
	s += dot(s, s.yx + 19.19);
	return frac((s.xx + s.yy) * s.yx);
}

inline float2 hashLOD(float2 p)
{
	p = frac(p * 0.318);
	return frac(p.x + p.y * float2(17.0, 23.0));
}

inline float3 NormalizeWeights(float3 weights)
{
	float weightSum = weights.x + weights.y + weights.z;
	// Skip expensive division if already normalized
	if (abs(weightSum - 1.0) < 0.01) return weights;
	float rcpWeightSum = rcp(max(weightSum, 1e-6));
	return weights * rcpWeightSum;
}

// Common barycentric coordinate calculation for stochastic sampling
inline float4x3 ComputeBarycentricVerts(float2 landscapeUV)
{
    float2 scaledUV = landscapeUV * (WORLD_SCALE);
    float2 skewUV = mul(SKEW_MATRIX, scaledUV);
    float2 vxID = floor(skewUV);
    float2 frac_uv = frac(skewUV);

    float barry_z = 1.0 - frac_uv.x - frac_uv.y;
    float3 barry = float3(frac_uv, barry_z);

    return (barry.z > 0) ?
        float4x3(float3(vxID, 0), float3(vxID + float2(0, 1), 0), float3(vxID + float2(1, 0), 0), barry.zyx) :
        float4x3(float3(vxID + float2(1, 1), 0), float3(vxID + float2(1, 0), 0), float3(vxID + float2(0, 1), 0), float3(-barry.z, 1.0 - barry.y, 1.0 - barry.x));
}

inline StochasticOffsets ComputeStochasticOffsets(float2 landscapeUV)
{
    float4x3 BW_vx = ComputeBarycentricVerts(landscapeUV);

    StochasticOffsets offsets;
    offsets.offset1 = hash2D2D(BW_vx[0].xy);
    offsets.offset2 = hash2D2D(BW_vx[1].xy);
    offsets.offset3 = hash2D2D(BW_vx[2].xy);
    offsets.weights = BW_vx[3];

    return offsets;
}

inline StochasticOffsets ComputeStochasticOffsetsLOD(float2 landscapeUV)
{
	// Precomputed scaling: (WORLD_SCALE / 0.010416667) * 8.0 = ~255437
	static const float LOD_SCALE = 255437.0;

	float2 scaledUV = landscapeUV * LOD_SCALE;
	float2 cellID = floor(scaledUV);

	StochasticOffsets offsetsLOD;
	// Generate both offsets from single hash to reduce calls
	float2 hash1 = hashLOD(cellID);
	float2 hash2 = hashLOD(cellID + 127.0);

	offsetsLOD.offset1 = hash1 * 0.08;
	offsetsLOD.offset2 = hash2 * 0.08;

	// Simplified weights since we only use 2 samples now
	offsetsLOD.weights = float3(0.65, 0.35, 0.0);

	return offsetsLOD;
}

// --------------------- TERRAIN BOMBING HELPERS --------------------- //

static const uint TERRAIN_BOMB_CATEGORY_SNOW = 1u << 0;
static const uint TERRAIN_BOMB_CATEGORY_DIRT = 1u << 1;
static const uint TERRAIN_BOMB_CATEGORY_ROCK = 1u << 2;
static const uint TERRAIN_BOMB_CATEGORY_MUD = 1u << 3;
static const uint TERRAIN_BOMB_CATEGORY_MOSS = 1u << 4;
static const uint TERRAIN_BOMB_CATEGORY_GRAVEL = 1u << 5;
static const uint TERRAIN_BOMB_CATEGORY_SHARED = 1u << 6;
static const uint TERRAIN_BOMB_FLAG_HAS_NORMAL = 1u << 0;
static const uint TERRAIN_BOMB_FLAG_HAS_RMA = 1u << 1;
static const uint TERRAIN_BOMB_FLAG_HAS_HEIGHT = 1u << 2;

inline float SmoothFalloff(float t)
{
	float s = saturate(t);
	return s * s * (3.0 - 2.0 * s);
}

inline float2 Rotate2D(float2 v, float angle)
{
	float s = sin(angle);
	float c = cos(angle);
	return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

inline uint DetermineBombCategoryMask(float3 baseColor, float slope, float snowCoverage)
{
	float luminance = dot(baseColor, LUMINANCE_WEIGHTS);
	float greenBias = baseColor.g - max(baseColor.r, baseColor.b);
	uint mask = TERRAIN_BOMB_CATEGORY_SHARED;

	if (snowCoverage > 0.3 || luminance > 0.7) {
		mask |= TERRAIN_BOMB_CATEGORY_SNOW;
	}

	if (luminance < 0.45) {
		mask |= TERRAIN_BOMB_CATEGORY_DIRT | TERRAIN_BOMB_CATEGORY_MUD;
	}

	if (slope > 0.6) {
		mask |= TERRAIN_BOMB_CATEGORY_ROCK | TERRAIN_BOMB_CATEGORY_GRAVEL;
	}

	if (greenBias > 0.05 && luminance < 0.7) {
		mask |= TERRAIN_BOMB_CATEGORY_MOSS;
	}

	if ((mask & ~TERRAIN_BOMB_CATEGORY_SHARED) == 0u) {
		mask |= TERRAIN_BOMB_CATEGORY_ROCK | TERRAIN_BOMB_CATEGORY_DIRT;
	}

	return mask;
}

inline float EvaluateCategoryWeight(uint spriteMask, uint desiredMask, float biomeAffinity)
{
	if ((spriteMask & desiredMask) != 0u) {
		return 1.0;
	}
	return 1.0 - saturate(biomeAffinity);
}

inline uint HashCell(int2 cell, uint seed)
{
	uint3 key = uint3(asuint(cell.x), asuint(cell.y), seed);
	return Random::murmur3(key, seed * 37u + 11u);
}

// --------------------- STOCHASTIC SAMPLING FUNCTIONS --------------------- //

// Stochastic sampling function for Terrain LOD & LOD Mask.
inline float4 StochasticSampleLOD(float rnd, Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsetsLOD, float2 dx, float2 dy)
{
	float offsetScale = 0.01;

	// Cheap pseudo-rotation using simple transforms
	float2 dir1 = float2(rnd - 0.5, frac(rnd * 1.618) - 0.5);
	float2 dir2 = float2(dir1.y, -dir1.x);

	// Apply simple scaled offsets
	float2 microOffset1 = (offsetsLOD.offset1 + dir1) * offsetScale;
	float2 microOffset2 = (offsetsLOD.offset2 + dir2) * offsetScale;
	float4 sample1 = tex.SampleBias(samp, uv + microOffset1, SharedData::MipBias);
	float4 sample2 = tex.SampleBias(samp, uv + microOffset2, SharedData::MipBias);

	// Simple 2-sample blend weighted toward first sample
	return lerp(sample2, sample1, 0.65);
}

// Main stochastic sampling function
inline float4 StochasticEffect(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, float2 dx, float2 dy)
{
	// Calculate custom mip level from original UVs.
	float mipLevel = tex.CalculateLevelOfDetail(samp, uv);
	float adjustedMipLevel = mipLevel + SharedData::MipBias;

	// 3 Sample Blend
	float4 sample1 = tex.SampleLevel(samp, uv + offsets.offset1, adjustedMipLevel);
	float4 sample2 = tex.SampleLevel(samp, uv + offsets.offset2, adjustedMipLevel);
	float4 sample3 = tex.SampleLevel(samp, uv + offsets.offset3, adjustedMipLevel);

	// Full height-based blending for terrain
	float contrastFactor = HEIGHT_BLEND_CONTRAST * (1.0 - HEIGHT_INFLUENCE);
	float3 blendWeights = pow(saturate(offsets.weights), contrastFactor);

	// Height calculation - use luminance for RGB data, alpha when available
	float3 luminanceHeights = float3(
		dot(sample1.rgb, LUMINANCE_WEIGHTS),
		dot(sample2.rgb, LUMINANCE_WEIGHTS),
		dot(sample3.rgb, LUMINANCE_WEIGHTS)
	);

	float3 alphaValues = float3(sample1.a, sample2.a, sample3.a);
	float3 alphaMask = step(0.001, alphaValues);
	float3 heights = lerp(luminanceHeights, alphaValues, alphaMask);

	// Combined weight calculation and normalization
	float3 weights = NormalizeWeights(blendWeights * (1.0 + HEIGHT_INFLUENCE * heights));

	// Final blend
	return sample1 * weights.x + sample2 * weights.y + sample3 * weights.z;
}

// Stochastic sampling function without height blending for better performance
// Disable X4000 warning: FXC incorrectly reports potentially uninitialized variables due to complex control flow with early returns and conditional sampling
#pragma warning(push)
#pragma warning(disable : 4000)
inline float4 StochasticEffectParallax(Texture2D tex, SamplerState samp, float2 uv, float mipLevel, StochasticOffsets offsets, float2 dx, float2 dy)
{
	// Early exit for disabled terrain variation - avoid all other computations
	if (!SharedData::terrainVariationSettings.enableTilingFix)
	{
		return tex.SampleLevel(samp, uv, mipLevel);
	}

	// Use progressive mip level increase for better performance in parallax
	float adjustedMipLevel = mipLevel;
	if (mipLevel > 1.0)
	{
		adjustedMipLevel = mipLevel + (MIP_LEVEL_INCREASE * 0.5);
	}

	// Take three samples for blending at the adjusted mip level
	float4 sample1 = tex.SampleLevel(samp, uv + offsets.offset1, adjustedMipLevel);
	float4 sample2 = tex.SampleLevel(samp, uv + offsets.offset2, adjustedMipLevel);
	float4 sample3 = tex.SampleLevel(samp, uv + offsets.offset3, adjustedMipLevel);

	// Simple barycentric blend without height influence
	float3 weights = NormalizeWeights(saturate(offsets.weights));
	return sample1 * weights.x + sample2 * weights.y + sample3 * weights.z;
}
#pragma warning(pop)

inline void ApplyTerrainBombing(float3 worldPosition, float3 baseLinearColor, float3 flatWorldNormal, float3 viewDirWS, float snowCoverage, float viewDistance, inout float3 blendedColor, inout float3 blendedNormalRGB, inout float4 blendedRMAOS)
{
	if (!SharedData::terrainVariationSettings.enableBombing)
	{
		return;
	}

	uint spriteCount = SharedData::terrainVariationSettings.bombSpriteCount;
	const bool debugMode = SharedData::terrainVariationSettings.debugDraw != 0;
	if (spriteCount == 0 && !debugMode)
	{
		return;
	}

	float density = SharedData::terrainVariationSettings.density;
	if (density <= 1e-4 && !debugMode)
	{
		return;
	}

	float cellSize = max(SharedData::terrainVariationSettings.cellSize, 4.0);
	float fadeStart = SharedData::terrainVariationSettings.fadeStart;
	float fadeEnd = max(SharedData::terrainVariationSettings.fadeEnd, fadeStart + 1.0);
	float fadeRange = max(fadeEnd - fadeStart, 1.0);
	float distanceFade = saturate(1.0 - (viewDistance - fadeStart) / fadeRange);

	float debugFade = 1.0;
	if (debugMode)
	{
		float dbgStart = SharedData::terrainVariationSettings.debugFadeStart;
		float dbgEnd = max(SharedData::terrainVariationSettings.debugFadeEnd, dbgStart + 1.0);
		float dbgRange = max(dbgEnd - dbgStart, 1.0);
		debugFade = saturate(1.0 - (viewDistance - dbgStart) / dbgRange);
	}

	if (distanceFade <= 0.0 && !debugMode)
	{
		return;
	}

	if (debugMode && debugFade <= 0.0)
	{
		return;
	}

	int2 cellCoord = int2(floor(worldPosition.xz / cellSize));
	float2 localCoord = frac(worldPosition.xz / cellSize);
	float3 baseColor = saturate(baseLinearColor);
	float slope = 1.0 - saturate(abs(flatWorldNormal.z));
	uint desiredMask = DetermineBombCategoryMask(baseColor, slope, snowCoverage);

	uint perCell = (debugMode && density <= 1e-4) ? 1u : clamp((uint)ceil(max(density, 0.0) * 4.0f), 1u, 8u);
	uint seedBase = SharedData::terrainVariationSettings.randomSeed;

	float3 accumulatedColor = 0.0;
	float4 accumulatedRMAOS = 0.0;
	float rmaAccumulatedWeight = 0.0;
	float accumulatedWeight = 0.0;

	float3 safeViewDir = normalize(viewDirWS);
	float3 upVector = (abs(flatWorldNormal.z) > 0.98) ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
	float3 tangent = cross(upVector, flatWorldNormal);
	float tangentLen = max(length(tangent), 1e-4);
	tangent /= tangentLen;
	float3 bitangent = normalize(cross(flatWorldNormal, tangent));
	float viewDenom = max(abs(dot(safeViewDir, flatWorldNormal)), 1e-4);

	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			int2 neighbor = cellCoord + int2(x, y);
			uint cellSeed = HashCell(neighbor, seedBase);

			for (uint i = 0; i < perCell; ++i)
			{
				uint state = cellSeed + i * 977u;

				float2 randomOffset = Random::f2(state) * 0.5 + 0.5;
				float radiusJitter = Random::f1(state);
				float radius = lerp(SharedData::terrainVariationSettings.radiusMin, SharedData::terrainVariationSettings.radiusMax, radiusJitter);
				radius = max(radius, 0.1);

				TerrainBombSprite spriteData = (TerrainBombSprite)0;
				uint spriteMask = TERRAIN_BOMB_CATEGORY_SHARED;
				uint spriteIndex = 0;
				float spriteNormalInfluence = 0.0;
				uint spriteFlags = 0;

				if (spriteCount > 0)
				{
					float spriteRand = Random::f1(state);
					spriteIndex = min(spriteCount - 1u, (uint)(spriteRand * spriteCount));
					spriteData = TerrainBombSpriteTable[spriteIndex];
					spriteMask = asuint(spriteData.averageColorMask.w);
					radius *= max(spriteData.params.x, 0.1);
					spriteNormalInfluence = saturate(spriteData.params.y);
					spriteFlags = asuint(spriteData.params.w);
				}

				float angle = (Random::f1(state) - 0.5f) * Math::TAU;
				float cosA = cos(angle);
				float sinA = sin(angle);
				float2 center = float2(neighbor) + randomOffset;
				float2 deltaCell = center - (float2(cellCoord) + localCoord);
				float2 delta = deltaCell * cellSize;
				float distance = length(delta);
				if (distance >= radius)
				{
					continue;
				}

				float2 local = delta / radius;
				float2 rotated = Rotate2D(local, angle);
				float2 uv = rotated * 0.5 + 0.5;

				float3 rotatedTangent = tangent * cosA + bitangent * sinA;
				float3 rotatedBitangent = bitangent * cosA - tangent * sinA;

				if (!debugMode && spriteCount > 0 && (spriteFlags & TERRAIN_BOMB_FLAG_HAS_HEIGHT) != 0)
				{
					float heightSample = TerrainBombHeightTextures.SampleLevel(SampColorSampler, float3(uv, spriteIndex), 0).r;
					heightSample = saturate(heightSample * spriteData.materialParams.w);
					float2 viewPlane = float2(dot(safeViewDir, rotatedTangent), dot(safeViewDir, rotatedBitangent));
					float parallaxScale = spriteData.parallaxParams.x * spriteData.params.z;
					float2 parallaxOffset = ((heightSample - 0.5) * parallaxScale) * (viewPlane / viewDenom);
					uv += parallaxOffset;
				}

				if (any(uv < 0.0 || uv > 1.0))
				{
					continue;
				}

				float4 sampled = float4(0, 0, 0, 0);
				if (!debugMode && spriteCount > 0)
				{
					sampled = TerrainBombTextures.SampleLevel(SampColorSampler, float3(uv, spriteIndex), 0);
				}
				else
				{
					float3 debugColor = Random::f3(state) * 0.5 + 0.5;
					sampled = float4(debugColor, 1.0);
				}

				float radial = SmoothFalloff(1.0 - (distance / radius));
				float influence = sampled.a * radial;
				if (influence <= 1e-4)
				{
					continue;
				}

				float maskWeight = EvaluateCategoryWeight(spriteMask, desiredMask, SharedData::terrainVariationSettings.biomeAffinity);
				if (maskWeight <= 0.0)
				{
					continue;
				}

				float3 spriteAverage = (spriteCount > 0) ? spriteData.averageColorMask.xyz : baseColor;
				float colourDelta = length(spriteAverage - baseColor);
				float colorWeight = exp2(-colourDelta * SharedData::terrainVariationSettings.colorMatchStrength * 4.0);

				float fadeWeight = debugMode ? debugFade : distanceFade;
				float totalWeight = influence * maskWeight * colorWeight * fadeWeight * SharedData::terrainVariationSettings.intensity;
				if (totalWeight <= 1e-4)
				{
					continue;
				}

				accumulatedColor += sampled.rgb * totalWeight;
				accumulatedWeight += totalWeight;

				if (!debugMode && spriteCount > 0)
				{
					if ((spriteFlags & TERRAIN_BOMB_FLAG_HAS_RMA) != 0)
					{
						float4 spriteRMAOS = TerrainBombRMAOSTextures.SampleLevel(SampColorSampler, float3(uv, spriteIndex), 0);
						spriteRMAOS.x *= spriteData.materialParams.x;
						spriteRMAOS.y *= spriteData.materialParams.y;
						spriteRMAOS.z *= spriteData.materialParams.z;
						spriteRMAOS.w = saturate(spriteRMAOS.w);
						accumulatedRMAOS += spriteRMAOS * totalWeight;
						rmaAccumulatedWeight += totalWeight;
					}

					if (spriteNormalInfluence > 0.0 && SharedData::terrainVariationSettings.normalBlend > 0.0 && (spriteFlags & TERRAIN_BOMB_FLAG_HAS_NORMAL) != 0)
					{
						float3 sampledNormal = TerrainBombNormalTextures.SampleLevel(SampColorSampler, float3(uv, spriteIndex), 0).xyz * 2.0 - 1.0;
						float3 rotatedNormal;
						rotatedNormal.x = cosA * sampledNormal.x - sinA * sampledNormal.y;
						rotatedNormal.y = sinA * sampledNormal.x + cosA * sampledNormal.y;
						rotatedNormal.z = sampledNormal.z;
						rotatedNormal = normalize(rotatedNormal);
						float3 spriteNormalWS = normalize(rotatedNormal.x * rotatedTangent + rotatedNormal.y * rotatedBitangent + rotatedNormal.z * flatWorldNormal);
						float flatten = saturate(totalWeight * SharedData::terrainVariationSettings.normalBlend * spriteNormalInfluence);
						float3 existingNormal = blendedNormalRGB * 2.0 - 1.0;
						existingNormal = normalize(existingNormal);
						float3 mixedNormal = normalize(lerp(existingNormal, spriteNormalWS, flatten));
						blendedNormalRGB = mixedNormal * 0.5 + 0.5;
					}
				}
			}
		}
	}

	if (accumulatedWeight > 1e-4)
	{
		float invWeight = rcp(accumulatedWeight);
		float3 overlayColor = accumulatedColor * invWeight;
		float mixFactor = saturate(accumulatedWeight);
		blendedColor = lerp(blendedColor, overlayColor, mixFactor);
		if (!debugMode && rmaAccumulatedWeight > 1e-4)
		{
			float invRmaWeight = rcp(rmaAccumulatedWeight);
			float4 overlayRMA = accumulatedRMAOS * invRmaWeight;
			float rmaMix = saturate(rmaAccumulatedWeight);
			blendedRMAOS = lerp(blendedRMAOS, overlayRMA, rmaMix);
		}
	}
}


#endif  // TERRAIN_VARIATION_HLSLI