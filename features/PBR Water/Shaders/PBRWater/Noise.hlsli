#ifndef __PBR_WATER_NOISE_HLSLI__
#define __PBR_WATER_NOISE_HLSLI__

// ============================================================================
// PBR Water - procedural noise shared by foam, water clarity and wind.
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
}

#endif  // __PBR_WATER_NOISE_HLSLI__
