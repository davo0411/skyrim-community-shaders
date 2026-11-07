#ifndef __PBR_WATER_FOAM_HLSLI__
#define __PBR_WATER_FOAM_HLSLI__

// ============================================================================
// PBR Water - drawing foam from a coverage.
//
// Foam is a coverage (0..1, how much of the surface is foam) turned into a pattern by the "black point fade" of Crest
// (wave-harmonic/crest, MIT, OceanFoam.hlsl WhiteFoamTexture): the foam texture is thresholded at 1 - coverage, so as
// the coverage falls the bubbles in the foam grow and merge and dense foam thins into lace and then specks. The
// texture (t125, baked at start-up: src/Features/PBRWater/FoamPattern.cpp) stores the coverage at which each texel
// whitens, equalised, so the thresholded share equals the coverage. The water shader samples it still, where the surface
// is, projected from the side as well on steep faces (triplanar): at the rest position the waves' horizontal displacement
// drew it out into streaks over every breaker's face, and from above alone it ran down a rising face.
// ============================================================================

namespace PBRWaterFoam
{
	Texture2D<float> FoamTexture : register(t125);

	static const float BubblesPerTile = 12.0;  // PBRWaterFoam::BubblesPerTile (FoamPattern.h)

	/// Reflectance of dense foam (linear RGB, Lambertian: Dupuy & Bruneton 2012 after Koepke 1984): about 55 % of visible
	/// light (Whitlock, Bartlett & Gurganus 1982), a little less towards the red, where the water between the bubbles
	/// absorbs (Frouin, Schwindling & Deschamps 1996).
	static const float3 Albedo = float3(0.47, 0.53, 0.56);

	/**
	 * Crest's black-point fade: the share `coverage` of the (uniform) texture values `value` that turns white, feathered.
	 * The feather is held within the values' range: a window reaching past 1 whitened far more than the coverage.
	 */
	float BlackPoint(float value, float coverage, float Feather)
	{
		float threshold = 1.0 - coverage;
		float feather = max(min(Feather, min(coverage, threshold)), 1e-3);
		return smoothstep(threshold - feather, threshold + feather, value);
	}

	/**
	 * Surface foam in [0, 1] (water pixel shader: needs screen derivatives). The texture at three fixed scales, each three
	 * times the last (the bubbles, and lace a few and ten bubbles across), so the foam is a microfoam with a ragged edge;
	 * the larger structure comes from the coverage itself (the simulated foam). Each scale drops out as its bubbles shrink
	 * below a few pixels, and with none left the foam is drawn as the coverage it stands for: a pattern held at any
	 * distance by moving to coarser scales drew distant foam as metre-sized blotches, and a thresholded mip of the pattern
	 * would turn into a hard-edged mask.
	 * @param p    absolute position (units)
	 * @param foam the foam here, 0..1
	 * @param tile size of the foam texture's tile (units): BubblesPerTile bubbles across
	 * @param Feather how far the white share of the pattern is feathered, in the texture's values (0.12 by default)
	 * @param s    sampler (wrap, anisotropic)
	 */
	float SurfaceFoam(float2 p, float foam, float tile, float Feather, SamplerState s)
	{
		float amount = saturate(foam);
		// The gradients are taken first, in uniform control flow; most of the water has no foam and skips the rest.
		float2 gx = ddx_fine(p);
		float2 gy = ddy_fine(p);
		float result = 0.0;
		[branch] if (amount > 0.002)
		{
			// Pixels across one bubble of the finest scale, along the footprint's long axis (anisotropic filtering keeps the short one).
			float footprint = max(sqrt(max(dot(gx, gx), dot(gy, gy))), 1e-4);
			float bubblePixels = tile / BubblesPerTile / footprint;
			float3 scales = tile * float3(1.0, 3.0, 9.0);
			float3 shown = saturate((bubblePixels * float3(1.0, 3.0, 9.0) - 2.0) / 4.0);
			float3 values;
			[unroll] for (uint i = 0; i < 3; i++)
				values[i] = FoamTexture.SampleGrad(s, p / scales[i] + i * float2(0.41, 0.17), gx / scales[i], gy / scales[i]);
			// The mix of uniform values is narrower than any of them: back to their contrast, or the foam would thin out.
			float total = dot(shown, 1.0.xxx);
			float value = saturate(0.5 + dot(values - 0.5, shown) * rsqrt(max(dot(shown, shown), 1e-4)));
			float pattern = BlackPoint(value, amount, Feather);
			result = lerp(amount, pattern, saturate(total)) * (0.35 + 0.65 * amount);
		}
		return result;
	}
}

#endif  // __PBR_WATER_FOAM_HLSLI__
