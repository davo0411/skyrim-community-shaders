#ifndef __PBR_WATER_NOISE_HLSLI__
#define __PBR_WATER_NOISE_HLSLI__

// ============================================================================
// PBR Water - procedural noise shared by foam, water clarity and wind, and the foam patterns (on the water, and on
// the sand the swash leaves, drawn by the lit terrain).
// ============================================================================

namespace PBRWater
{
	// Hash without Sine, Dave Hoskins (MIT): stable across GPUs, unlike sin-based hashes.
	float Hash12(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * 0.1031);
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.x + p3.y) * p3.z);
	}

	float2 Hash22(float2 p)
	{
		float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac((p3.xx + p3.yz) * p3.zy);
	}

	float ValueNoise(float2 p)
	{
		float2 i = floor(p);
		float2 f = frac(p);
		float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(Hash12(i), Hash12(i + float2(1, 0)), u.x),
			lerp(Hash12(i + float2(0, 1)), Hash12(i + float2(1, 1)), u.x), u.y);
	}

	/// Three-octave fBm with rotated octaves (no grid-aligned artefacts), in [0, 1].
	float Fbm(float2 p)
	{
		const float2x2 rotation = float2x2(0.8, 0.6, -0.6, 0.8);
		float sum = 0.0;
		float amplitude = 0.5;
		[unroll] for (int i = 0; i < 3; i++)
		{
			sum += amplitude * ValueNoise(p);
			p = mul(rotation, p) * 2.03 + 7.1;
			amplitude *= 0.5;
		}
		return sum / 0.875;
	}

	/// Two-octave variant for terms evaluated many times per pixel.
	float Fbm2(float2 p)
	{
		const float2x2 rotation = float2x2(0.8, 0.6, -0.6, 0.8);
		return (ValueNoise(p) * 2.0 + ValueNoise(mul(rotation, p) * 2.03 + 7.1)) / 3.0;
	}

	/// Distance to the nearest bubble centre, normalised by that bubble's radius and merged with a
	/// smooth minimum: 0 at a bubble centre, ~1 at its rim, with rounded walls between bubbles.
	float BubbleField(float2 p, float time)
	{
		float2 cell = floor(p);
		float2 f = frac(p);
		float result = 0.0;
		float weightSum = 0.0;
		[unroll] for (int y = -1; y <= 1; y++)
		{
			[unroll] for (int x = -1; x <= 1; x++)
			{
				float2 o = float2(x, y);
				// Each bubble drifts on its own small orbit and breathes, so the lace keeps churning.
				float2 h = Hash22(cell + o);
				float phase = Hash12(cell + o + 5.31) * 6.2831853;
				float speed = 0.6 + 0.8 * h.x;
				float2 orbit = float2(sin(time * speed + phase), cos(time * speed * 0.83 + phase * 1.7));
				float2 centre = o + 0.5 + (h - 0.5) * 0.6 + orbit * 0.18;
				float radius = (0.55 + 0.45 * Hash12(cell + o + 17.17)) * (0.9 + 0.1 * sin(time * 1.3 * speed + phase));
				float d = length(centre - f) / radius;
				// Exponential smooth minimum: neighbouring bubbles merge softly instead of meeting at an edge.
				float w = exp2(-8.0 * d);
				result += w * d;
				weightSum += w;
			}
		}
		return result / max(weightSum, 1e-6);
	}

	/**
	 * Foam pattern in [0, 1] for a local coverage, shared by the foam on the water and the foam the swash leaves
	 * on the sand. As coverage drops the bubbles grow and merge, so dense foam thins into lace and then into specks.
	 * @param q          position in foam cells
	 * @param coverage   fraction of the surface that is foam, 0..1
	 * @param pixelCells size of a pixel in foam cells (anti-aliasing)
	 * @param time       animation time (bubbles slowly churn and pop)
	 */
	float FoamLacePattern(float2 q, float coverage, float pixelCells, float time)
	{
		// Large-scale density variation: foam gathers in clumps and streaks.
		float clumps = Fbm(q * 0.21 + float2(time * 0.011, -time * 0.007));
		coverage = saturate(coverage * (0.45 + 1.1 * clumps));
		float foam = 0.0;
		[branch] if (coverage > 0.002)
		{
			// Domain warp bends the bubble walls into organic, curved filaments; it flows over time.
			float2 warp = float2(Fbm(q * 0.6 + float2(time * 0.17, time * 0.05)), Fbm(q * 0.6 + 31.7 - float2(time * 0.06, time * 0.15))) - 0.5;
			float2 w = q + warp * 1.6;

			float large = BubbleField(w, time);
			float small = BubbleField(w * 2.7 + 9.3, time * 1.7);

			// Bubbles grow as coverage falls; at zero coverage they swallow everything.
			float radius = lerp(1.45, 0.05, sqrt(coverage));
			float feather = 0.12 + pixelCells * 2.0;
			float lace = smoothstep(radius - feather, radius + feather, large);
			lace *= smoothstep(radius * 0.85 - feather * 2.7, radius * 0.85 + feather * 2.7, small);

			// Far away the pattern is sub-pixel: converge to its average instead of shimmering.
			float distanceBlend = saturate(pixelCells * 4.0 - 0.5);
			foam = lerp(lace, coverage * coverage, distanceBlend);
		}
		return foam;
	}

	/**
	 * The foam the backwash leaves on the sand, in [0, 1]: rafts of bubbles lying in clumps where the water dropped
	 * them, with holes where bubbles have burst. On the moving water foam is drawn out into lace; on the sand nothing
	 * stretches it, so as it ages the rafts shrink and break into patches and specks instead of thinning into filaments.
	 * @param q          position in foam cells
	 * @param amount     0..1 how much foam is left (fresh = 1)
	 * @param pixelCells size of a pixel in foam cells (anti-aliasing)
	 */
	float BeachFoamPattern(float2 q, float amount, float pixelCells)
	{
		amount = saturate(amount);
		// Clumps, their edges torn by a finer noise.
		float2 warp = float2(ValueNoise(q * 0.9 + 3.1), ValueNoise(q * 0.9 + 17.7)) - 0.5;
		float clumps = 0.7 * Fbm(q * 0.35 + warp * 0.6) + 0.3 * ValueNoise(q * 2.4 + warp * 2.0);
		float feather = 0.04 + pixelCells * 1.5;
		float threshold = lerp(0.9, 0.3, amount);
		float rafts = smoothstep(threshold - feather, threshold + feather, clumps);
		// Burst bubbles: round holes that grow as the foam ages.
		float holeRadius = lerp(0.55, 0.15, amount);
		float holes = 1.0 - smoothstep(holeRadius - feather, holeRadius + feather, BubbleField(q * 2.2 + 5.3, 0.0));
		float foam = rafts * (1.0 - 0.8 * holes);
		// Far away the pattern is sub-pixel: converge to its average instead of shimmering.
		return lerp(foam, amount * amount * 0.6, saturate(pixelCells * 4.0 - 0.5));
	}
}

#endif  // __PBR_WATER_NOISE_HLSLI__
