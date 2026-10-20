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

	/// Crest's black-point fade: the share `coverage` of the texture values `value` that turns white, feathered.
	float BlackPoint(float value, float coverage, float feather)
	{
		float threshold = 1.0 - saturate(coverage);
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

	/**
	 * Surface foam in [0, 1] for a coverage (water pixel shader: needs screen derivatives).
	 * @param p        Lagrangian absolute position (units), drift already applied
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
			// Texels per pixel along the footprint's long axis: the thresholding holds while the texture is magnified,
			// and the foam converges to its coverage once the texels are sub-pixel (the mips average the values).
			uint w, h;
			FoamTexture.GetDimensions(w, h);
			float texels = sqrt(max(dot(gx, gx), dot(gy, gy))) * w;
			float feather = 0.03 + 0.5 * saturate(texels);
			float foam = weights.x * BlackPoint(v0, coverage, feather) + weights.y * BlackPoint(v1, coverage, feather) + weights.z * BlackPoint(v2, coverage, feather);
			result = lerp(foam, coverage, saturate(log2(max(texels, 1e-4)) * 0.75 - 0.25));
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
