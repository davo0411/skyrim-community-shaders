// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h
// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

// Extended Materials: split for faster compiles on non-landscape Lighting permutations.
// - Terrain helpers: ExtendedMaterialsTerrain.hlsli (only when LANDSCAPE)
// - Parallax core: ExtendedMaterialsParallaxCore.hlsli (GetParallaxCoords + mesh soft shadows)

#ifndef EXTENDED_MATERIALS_HLSLI
#define EXTENDED_MATERIALS_HLSLI

// Terrain variation: optional feature pack — include only when macro + headers ship together.
// When absent, stub `StochasticOffsets`, `SampleTerrain`, and `ComputeStochasticOffsets` so the
// LANDSCAPE pixel path compiles to a single permutation that mirrors the vanilla sampling behaviour.
#	if defined(LANDSCAPE)
#		if defined(TERRAIN_VARIATION)
#			include "TerrainVariation/TerrainVariation.hlsli"
#		else
struct StochasticOffsets
{
	float2 offset1;
	float2 offset2;
	float2 offset3;
	float3 weights;
};
inline StochasticOffsets ComputeStochasticOffsets(float2 landscapeUV) { return (StochasticOffsets)0; }
inline float4 SampleTerrain(Texture2D tex, SamplerState samp, float2 uv, StochasticOffsets offsets, float extraLandMipBias)
{
	return tex.SampleBias(samp, uv, SharedData::MipBias + extraLandMipBias);
}
#		endif
#	endif

struct DisplacementParams
{
	float DisplacementScale;
	float DisplacementOffset;
	float HeightScale;
	float FlattenAmount;
};

namespace ExtendedMaterials
{
	static const float ShadowIntensity = 2.0;
	static const float ParallaxCheapDistance = 1024.0;
	static const float ParallaxNearShadowQuality = 1.0;
	static const float ParallaxFarShadowQuality = 0.76;
	static const float TerrainParallaxShadowMaxMipLevel = 0.5;

	inline uint ParallaxShadowTapCount(float quality)
	{
		uint taps = 1;
		if (quality > 0.25)
			taps++;
		if (quality > 0.5)
			taps++;
		if (quality > 0.75)
			taps++;
		return taps;
	}

	float ScaleDisplacement(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.HeightScale;
	}

	float AdjustDisplacementNormalized(float displacement, DisplacementParams params)
	{
		return (displacement - 0.5) * params.DisplacementScale + 0.5 + params.DisplacementOffset;
	}

	float4 AdjustDisplacementNormalized(float4 displacement, DisplacementParams params)
	{
		return float4(AdjustDisplacementNormalized(displacement.x, params), AdjustDisplacementNormalized(displacement.y, params), AdjustDisplacementNormalized(displacement.z, params), AdjustDisplacementNormalized(displacement.w, params));
	}

	float GetMipLevel(float2 coords, Texture2D<float4> tex)
	{
		float2 textureDims;
		tex.GetDimensions(textureDims.x, textureDims.y);

#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		textureDims /= 2.0;
#	endif

#	if defined(VR)
		textureDims /= 2.0;
#	endif

		float2 texCoordsPerSize = coords * textureDims;

		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		float minTexCoordDelta = min(dot(dxSize, dxSize), dot(dySize, dySize));

		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

#	if !defined(PARALLAX) && !defined(TRUE_PBR)
		mipLevel++;
#	endif

#	if defined(VR)
		mipLevel++;
#	endif

		return floor(mipLevel);
	}

#	if defined(LANDSCAPE)
#		include "ExtendedMaterials/ExtendedMaterialsTerrain.hlsli"
#	endif
#	include "ExtendedMaterials/ExtendedMaterialsParallaxCore.hlsli"
}

#endif  // EXTENDED_MATERIALS_HLSLI
