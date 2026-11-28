#pragma once

#include "WaveModel.h"

#include <algorithm>
#include <cmath>

/// Helpers the wave model's sources share (WaveModel.cpp, ShoreField.cpp).
namespace PBRWaterModel::Math
{
	constexpr float G = static_cast<float>(Gravity);

	inline float Smoothstep(float e0, float e1, float v)
	{
		const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// ---- Shared with Noise.hlsli (Hash12, ValueNoise): keep the two in sync ----

	inline float Frac(float x) { return x - std::floor(x); }

	inline float Hash12(float x, float y)
	{
		float p3x = Frac(x * 0.1031f), p3y = Frac(y * 0.1031f), p3z = Frac(x * 0.1031f);
		const float d = p3x * (p3y + 33.33f) + p3y * (p3z + 33.33f) + p3z * (p3x + 33.33f);
		p3x += d;
		p3y += d;
		p3z += d;
		return Frac((p3x + p3y) * p3z);
	}

	inline float ValueNoise(float x, float y)
	{
		const float ix = std::floor(x), iy = std::floor(y);
		const float fx = x - ix, fy = y - iy;
		const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
		const float a = Hash12(ix, iy) + (Hash12(ix + 1.0f, iy) - Hash12(ix, iy)) * ux;
		const float b = Hash12(ix, iy + 1.0f) + (Hash12(ix + 1.0f, iy + 1.0f) - Hash12(ix, iy + 1.0f)) * ux;
		return a + (b - a) * uy;
	}
}
