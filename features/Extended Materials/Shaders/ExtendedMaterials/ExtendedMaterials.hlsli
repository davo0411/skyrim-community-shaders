// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h
// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

// Extended Materials: split for faster compiles on non-landscape Lighting permutations.
// Lighting.hlsl includes this header only when EMAT is defined; LANDSCAPE further gates terrain-only code.
// - Terrain helpers: ExtendedMaterialsTerrain.hlsli (EMAT implied; included only when LANDSCAPE)
// - Parallax core: ExtendedMaterialsParallaxCore.hlsli (GetParallaxCoords + mesh soft shadows)

#ifndef EXTENDED_MATERIALS_HLSLI
#define EXTENDED_MATERIALS_HLSLI

// Terrain variation: optional feature pack — include only when macro + headers ship together.
// When absent, stub `StochasticOffsets` so EMAT terrain APIs stay unified (offsets ignored on SampleLevel path).
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

	// SSDM seed (Lobel 2008, §2.2): project the displacement into screen space as a duv vector.
	// POM-style tangent step (Vt.xy / |Vt.z|) -> world-space offset -> screen UV delta. The forward
	// Lighting pass writes this into the SSDM displacement RT; it is metadata for the screen-space
	// silhouette extrusion ONLY and never feeds the texture-space POM lookup (no double-apply).
	// Callers pass surface -> camera (Lighting `viewDirection`).
	void ComputeDisplacementDuvAndOffsetVS(float3 viewPosVS, float3 viewDirWorld, float3 tbnTr0, float3 tbnTr1, float3 tbnTr2,
		float height, float displacementScale, uint eyeIndex, out float2 duv)
	{
		float h = height;

		float3 Tw = normalize(tbnTr0);
		float3 Bw = normalize(tbnTr1);
		float3 Nw = normalize(tbnTr2);
		float3 Vw = -normalize(viewDirWorld);

		float3 Vt;
		Vt.x = dot(Vw, Tw);
		Vt.y = dot(Vw, Bw);
		Vt.z = dot(Vw, Nw);

		// Use |Vt.z| so parallaxDir does not flip sign when the view passes below the tangent plane (Vt.z < 0).
		float zn = max(abs(Vt.z), 1e-5);
		float2 parallaxDir = Vt.xy / zn;

		static const float kLegacyNormalPush = 32.0;
		static const float kDefaultDisplacementScale = 0.05;
		static const float kTangentParallaxAmpScale = 0.22;
		float amp = h * displacementScale * (kLegacyNormalPush / kDefaultDisplacementScale) * kTangentParallaxAmpScale;
		// Keep SSDM apparent height stable across dynamic resolution tiers (DLAA -> DLSS perf).
		// Without this, lower internal resolution over-amplifies the screen-space displacement footprint.
		float drScale = saturate(sqrt(FrameBuffer::DynamicResolutionParams1.x * FrameBuffer::DynamicResolutionParams1.y));
		amp *= drScale;

		float3 worldOff = -(Tw * parallaxDir.x + Bw * parallaxDir.y) * amp;
		float3 offsetFull = FrameBuffer::WorldToView(worldOff, false, eyeIndex);
		duv = FrameBuffer::ViewToUV(viewPosVS + offsetFull, true, eyeIndex) - FrameBuffer::ViewToUV(viewPosVS, true, eyeIndex);
	}

	float2 ComputeDisplacementVector(float3 viewPosVS, float3 viewDirWorld, float3 tbnTr0, float3 tbnTr1, float3 tbnTr2,
		float height, float displacementScale, uint eyeIndex)
	{
		float2 duv;
		ComputeDisplacementDuvAndOffsetVS(viewPosVS, viewDirWorld, tbnTr0, tbnTr1, tbnTr2, height, displacementScale, eyeIndex, duv);
		return duv;
	}

#	if defined(LANDSCAPE)
#		include "ExtendedMaterials/ExtendedMaterialsTerrain.hlsli"
#	endif
#	include "ExtendedMaterials/ExtendedMaterialsParallaxCore.hlsli"
}

#endif  // EXTENDED_MATERIALS_HLSLI
