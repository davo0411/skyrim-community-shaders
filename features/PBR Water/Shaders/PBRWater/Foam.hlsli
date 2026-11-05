#ifndef __PBR_WATER_FOAM_HLSLI__
#define __PBR_WATER_FOAM_HLSLI__

// ============================================================================
// PBR Water - drawing foam from a coverage.
//
// Foam is a coverage (0..1, how much of the surface is foam) turned into a pattern by the "black point fade" of Crest
// (wave-harmonic/crest, MIT, OceanFoam.hlsl WhiteFoamTexture): the foam texture is thresholded at 1 - coverage, so as
// the coverage falls the bubbles in the foam grow and merge and dense foam thins into lace and then specks. The
// texture (t125, baked at start-up: src/Features/PBRWater/FoamPattern.cpp) stores the coverage at which each texel
// whitens, equalised, so the thresholded share equals the coverage. It is sampled at the Lagrangian (undisplaced) position,
// so the pattern moves and compresses with the water.
// ============================================================================

namespace PBRWaterFoam
{
	Texture2D<float> FoamTexture : register(t125);

	static const float BubblesPerTile = 12.0;  // PBRWaterFoam::BubblesPerTile (FoamPattern.h)

	/// Reflectance of dense foam (linear RGB, Lambertian: Dupuy & Bruneton 2012 after Koepke 1984): about 55 % of visible
	/// light (Whitlock, Bartlett & Gurganus 1982), a little less towards the red, where the water between the bubbles
	/// absorbs (Frouin, Schwindling & Deschamps 1996).
	static const float3 Albedo = float3(0.47, 0.53, 0.56);

	/// How far the white share of the pattern is feathered, in the texture's values.
	static const float Feather = 0.12;

	/**
	 * Crest's black-point fade: the share `coverage` of the (uniform) texture values `value` that turns white, feathered.
	 * The feather is held within the values' range: a window reaching past 1 whitened far more than the coverage.
	 */
	float BlackPoint(float value, float coverage)
	{
		float threshold = 1.0 - coverage;
		float feather = max(min(Feather, min(coverage, threshold)), 1e-3);
		return smoothstep(threshold - feather, threshold + feather, value);
	}

	/**
	 * Surface foam in [0, 1] (water pixel shader: needs screen derivatives). The texture at three scales, each four times the
	 * last, gives the foam an irregular edge at every size: patches, streaks and holes with lace around them, where one
	 * scale alone drew smooth spots. The scales follow the pixel: as the finest would turn sub-pixel it fades out and a new
	 * coarsest fades in, so foam stays a crisp pattern at any distance and never melts into a flat wash. Thin foam is a
	 * translucent lace, fresh foam dense.
	 * @param p    Lagrangian absolute position (units)
	 * @param foam the foam here, 0..1
	 * @param tile size of the foam texture's tile (units) up close
	 * @param s    sampler (wrap, anisotropic)
	 */
	float SurfaceFoam(float2 p, float foam, float tile, SamplerState s)
	{
		float amount = saturate(foam);
		// The gradients are taken first, in uniform control flow; most of the water has no foam and skips the rest.
		float2 gx = ddx_fine(p);
		float2 gy = ddy_fine(p);
		float result = 0.0;
		[branch] if (amount > 0.002)
		{
			uint size, unused;
			FoamTexture.GetDimensions(size, unused);
			// The finest bubbles (BubblesPerTile per tile) stay some five pixels across.
			float bubblePixels = float(size) / BubblesPerTile / max(sqrt(max(dot(gx, gx), dot(gy, gy))) * size / tile, 1e-4);
			float level = max(0.5 * log2(5.0 / bubblePixels) + 1.0, 0.0);
			float octave = floor(level);
			float blend = level - octave;
			float3 weights = float3(1.0 - blend, 1.0, blend);
			float3 scales = tile * pow(4.0, octave) * float3(1.0, 4.0, 16.0);
			float3 values;
			[unroll] for (uint i = 0; i < 3; i++)
				values[i] = FoamTexture.SampleGrad(s, p / scales[i] + (octave + i) * float2(0.41, 0.17), gx / scales[i], gy / scales[i]);
			// The mix of uniform values is narrower than any of them: back to their contrast, or the foam would thin out.
			float total = weights.x + weights.y + weights.z;
			float value = saturate(0.5 + (dot(values, weights) / total - 0.5) * total * rsqrt(dot(weights, weights)));
			result = BlackPoint(value, amount) * (0.35 + 0.65 * amount);
		}
		return result;
	}
}

#endif  // __PBR_WATER_FOAM_HLSLI__
