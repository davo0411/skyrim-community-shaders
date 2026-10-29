#ifndef __PBR_WATER_FOAM_HLSLI__
#define __PBR_WATER_FOAM_HLSLI__

// ============================================================================
// PBR Water - drawing foam from a coverage.
//
// Foam is a coverage (0..1, how much of the surface is foam) turned into a pattern by the "black point fade" of Crest
// (wave-harmonic/crest, MIT, OceanFoam.hlsl WhiteFoamTexture): the foam texture is thresholded at 1 - coverage, so as
// the coverage falls the bubbles in the foam grow and merge and dense foam thins into lace and then specks. The
// texture (t125, baked at start-up: src/Features/PBRWater/FoamPattern.cpp) stores in R the coverage at which each
// texel whitens, equalised, so the thresholded share equals the coverage and the mip chain converges to it far away.
// The pattern is moved by Fluid Flux's three-phase flow advection (MF_AdvectionData, DaiZiLing's HLSL port of
// Fluid Flux 2's MF_FluidFoam): three copies slide along the flow, each fading out before it has slid a whole cycle,
// so the foam churns and moves without stretching. G holds bubble specks for the aerated water just under the
// surface, Crest's bubble foam layer (BubbleFoamTexture), seen with parallax through the surface.
// ============================================================================

namespace PBRWaterFoam
{
	Texture2D<float2> FoamTexture : register(t125);

	static const float BubblesPerTile = 12.0;  // PBRWaterFoam::BubblesPerTile (FoamPattern.h)

	/**
	 * Reflectance of foam (linear RGB, Lambertian: Dupuy & Bruneton 2012 after Koepke 1984). Fresh, dense foam reflects
	 * about 55 % of visible light (Whitlock, Bartlett & Gurganus 1982), a little less towards the red, where the water
	 * between the bubbles absorbs (Frouin, Schwindling & Deschamps 1996). It drains and thins as it ages and reflects
	 * less: about 22 % averaged over a whitecap's life (Koepke 1984), which the thinning coverage and this together give.
	 * @param freshness 1 being made now, 0 aged
	 */
	float3 Albedo(float freshness)
	{
		return float3(0.47, 0.53, 0.56) * lerp(0.6, 1.0, saturate(freshness));
	}

	/**
	 * Crest's black-point fade: the share `coverage` of the (uniform) texture values `value` that turns white, feathered.
	 * The feather is held within the values' range: a window reaching past 1 whitened far more than the coverage (a 2 %
	 * coverage feathered by 0.5 covered 11 % of the water).
	 */
	float BlackPoint(float value, float coverage, float feather)
	{
		coverage = saturate(coverage);
		float threshold = 1.0 - coverage;
		feather = max(min(feather, min(coverage, threshold)), 1e-3);
		return smoothstep(threshold - feather, threshold + feather, value);
	}

	/// Fluid Flux's advection phases: three copies of a pattern at time offsets a third of a cycle apart, each faded out
	/// (weight 0) as it wraps. x/y/z weights sum to 1; offsets in cycles.
	void AdvectionPhases(float time, out float3 weights, out float3 offsets)
	{
		float t = frac(time);
		offsets = frac(t + float3(0.0, 1.0, 2.0) / 3.0);
		weights = (1.0 - cos((float3(0.0, 1.0, 2.0) / 3.0 + t) * 6.2831853)) / 3.0;
	}

	/// How much larger the foam's patches are than its bubbles: the texture tiled this much larger gives the metre-scale
	/// structure of whitecap and surf foam (patches, streaks and holes) between the bubbles, as Crest draws its foam
	/// from two scales of one texture.
	static const float PatchScale = 8.0;

	/// The mean of two independent uniform values, made uniform again (its CDF), so a coverage C still whitens a share C.
	float UniformMean(float a, float b)
	{
		float m = 0.5 * (a + b);
		return m < 0.5 ? 2.0 * m * m : 1.0 - 2.0 * (1.0 - m) * (1.0 - m);
	}

	/**
	 * Surface foam in [0, 1] for a coverage (water pixel shader: needs screen derivatives). Two scales of the foam
	 * texture are thresholded together: bubbles and lace close up, and the metre-scale patches the foam breaks into, so
	 * thin foam stays a scatter of crisp patches further off instead of an even grey veil (the bubbles' mips average
	 * them away well before the patches'); once the patches are sub-pixel too it converges to its coverage.
	 * @param p        Lagrangian absolute position (units)
	 * @param coverage share of the surface that is foam
	 * @param tile     size of the foam texture's tile (units)
	 * @param flow     advection: xy distance the pattern slides per cycle (units), z cycles per second
	 * @param time     animation time (s)
	 * @param s        sampler (wrap, anisotropic)
	 */
	float SurfaceFoam(float2 p, float coverage, float tile, float3 flow, float time, SamplerState s)
	{
		coverage = saturate(coverage);
		float2 uv = p / tile;
		float2 gx = ddx_fine(uv);
		float2 gy = ddy_fine(uv);
		// Most of the water has no foam and skips the texture (the gradients are taken first, in uniform control flow).
		float result = 0.0;
		[branch] if (coverage > 0.002)
		{
			float3 weights, offsets;
			AdvectionPhases(time * flow.z, weights, offsets);
			float2 slide = flow.xy / tile;
			float v0 = FoamTexture.SampleGrad(s, uv + offsets.x * slide, gx, gy).x;
			float v1 = FoamTexture.SampleGrad(s, uv + offsets.y * slide + float2(0.33, 0.66), gx, gy).x;
			float v2 = FoamTexture.SampleGrad(s, uv + offsets.z * slide + float2(0.66, 0.33), gx, gy).x;
			// The patches ride with the water: they are not churned.
			float patch = FoamTexture.SampleGrad(s, uv / PatchScale + float2(0.41, 0.17), gx / PatchScale, gy / PatchScale).x;
			// The streaks and fields the patches gather into, a scale up again: far off the foam keeps a pattern instead of
			// fading into an even grey veil once the patches are sub-pixel.
			const float fieldScale = PatchScale * PatchScale;
			float field = FoamTexture.SampleGrad(s, uv / fieldScale + float2(0.73, 0.29), gx / fieldScale, gy / fieldScale).x;
			// Texels per pixel along the footprint's long axis: the thresholding holds while a scale is magnified; the
			// bubbles hand over to the patches as they turn sub-pixel, the patches to the fields, and only those to the
			// coverage.
			uint w, h;
			FoamTexture.GetDimensions(w, h);
			float texels = sqrt(max(dot(gx, gx), dot(gy, gy))) * w;
			float patchTexels = texels / PatchScale;
			float fieldTexels = texels / fieldScale;
			float bubbles = 1.0 - saturate(log2(max(texels, 1e-4)) * 0.75 - 0.25);
			float patches = 1.0 - saturate(log2(max(patchTexels, 1e-4)) * 0.75 - 0.25);
			float coarse = lerp(field, UniformMean(patch, field), patches);
			float feather = 0.03 + 0.5 * saturate(lerp(lerp(fieldTexels, patchTexels, patches), texels, bubbles));
			float3 values = lerp(coarse.xxx, float3(UniformMean(v0, patch), UniformMean(v1, patch), UniformMean(v2, patch)), bubbles);
			float foam = weights.x * BlackPoint(values.x, coverage, feather) + weights.y * BlackPoint(values.y, coverage, feather) + weights.z * BlackPoint(values.z, coverage, feather);
			result = lerp(foam, coverage, saturate(log2(max(fieldTexels, 1e-4)) * 0.75 - 0.25));
		}
		return result;
	}

	/**
	 * Bubble specks in the water just under the surface (Crest's bubble foam), 0..1.
	 * @param p       Lagrangian absolute position (units)
	 * @param density how aerated the water is (foam, wakes, breaking waves)
	 * @param parallax horizontal offset of the bubble layer seen through the surface (units)
	 */
	float Bubbles(float2 p, float density, float tile, float2 parallax, SamplerState s)
	{
		density = saturate(density);
		float2 uv = (p + parallax) / (0.6 * tile);
		float2 gx = ddx_fine(uv);
		float2 gy = ddy_fine(uv);
		float result = 0.0;
		[branch] if (density > 0.002)
		{
			float v = FoamTexture.SampleGrad(s, uv, gx, gy).y;
			uint w, h;
			FoamTexture.GetDimensions(w, h);
			float texels = sqrt(max(dot(gx, gx), dot(gy, gy))) * w;
			// Only the densest third of the specks shows even in fully aerated water; sub-pixel specks fade out rather
			// than averaging into a milky haze.
			float specks = BlackPoint(v, 0.3 * density, 0.03 + 0.5 * saturate(texels));
			result = specks * saturate(1.5 - 0.5 * log2(max(texels, 1e-4)));
		}
		return result;
	}
}

#endif  // __PBR_WATER_FOAM_HLSLI__
