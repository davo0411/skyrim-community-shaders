#include "FoamPattern.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace PBRWaterFoam
{
	namespace
	{
		uint32_t Hash(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352du;
			x ^= x >> 15;
			x *= 0x846ca68bu;
			x ^= x >> 16;
			return x;
		}

		/// Uniform [0, 1) for lattice point (x, y) of a grid repeating every `period` cells.
		float Hash01(int32_t x, int32_t y, int32_t period, uint32_t seed)
		{
			const auto px = static_cast<uint32_t>(((x % period) + period) % period);
			const auto py = static_cast<uint32_t>(((y % period) + period) % period);
			return static_cast<float>(Hash(px * 0x8da6b343u ^ py * 0xd8163841u ^ seed * 0xcb1ab31fu) >> 8) * (1.0f / 16777216.0f);
		}

		/// Value noise in [0, 1], periodic over `period` cells.
		float ValueNoise(float x, float y, int32_t period, uint32_t seed)
		{
			const float fx = std::floor(x), fy = std::floor(y);
			const auto ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy);
			float u = x - fx, v = y - fy;
			u = u * u * (3.0f - 2.0f * u);
			v = v * v * (3.0f - 2.0f * v);
			const float a = Hash01(ix, iy, period, seed), b = Hash01(ix + 1, iy, period, seed);
			const float c = Hash01(ix, iy + 1, period, seed), d = Hash01(ix + 1, iy + 1, period, seed);
			return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
		}

		/// Three-octave fBm in [0, 1] over tile coordinate (tx, ty) in [0, 1), `period` cells per tile at the first octave.
		float Fbm(float tx, float ty, int32_t period, uint32_t seed)
		{
			float sum = 0.0f, amplitude = 0.5f;
			for (int octave = 0; octave < 3; ++octave) {
				sum += amplitude * ValueNoise(tx * period, ty * period, period, seed + octave);
				period *= 2;
				amplitude *= 0.5f;
			}
			return sum / 0.875f;
		}

		/// Distance to the nearest bubble centre over that bubble's radius, merged with a smooth minimum: 0 at a centre,
		/// ~1 at its rim, rounded walls between neighbours. Bubbles of random size on a jittered grid, periodic over `period`.
		float BubbleField(float x, float y, int32_t period, uint32_t seed)
		{
			const float fx = std::floor(x), fy = std::floor(y);
			const auto cx = static_cast<int32_t>(fx), cy = static_cast<int32_t>(fy);
			float result = 0.0f, weights = 0.0f;
			for (int32_t j = -1; j <= 1; ++j) {
				for (int32_t i = -1; i <= 1; ++i) {
					const int32_t gx = cx + i, gy = cy + j;
					const float ox = static_cast<float>(gx) + 0.5f + (Hash01(gx, gy, period, seed) - 0.5f) * 0.6f;
					const float oy = static_cast<float>(gy) + 0.5f + (Hash01(gx, gy, period, seed + 101u) - 0.5f) * 0.6f;
					const float radius = 0.55f + 0.45f * Hash01(gx, gy, period, seed + 202u);
					const float d = std::sqrt((ox - x) * (ox - x) + (oy - y) * (oy - y)) / radius;
					const float w = std::exp2(-8.0f * d);
					result += w * d;
					weights += w;
				}
			}
			return result / std::max(weights, 1e-12f);
		}

		/// Distance to the nearest speck centre over its radius (no merging): bubbles in the water are separate.
		float SpeckDistance(float x, float y, int32_t period, uint32_t seed)
		{
			const float fx = std::floor(x), fy = std::floor(y);
			const auto cx = static_cast<int32_t>(fx), cy = static_cast<int32_t>(fy);
			float nearest = 1e9f;
			for (int32_t j = -1; j <= 1; ++j) {
				for (int32_t i = -1; i <= 1; ++i) {
					const int32_t gx = cx + i, gy = cy + j;
					const float ox = static_cast<float>(gx) + 0.5f + (Hash01(gx, gy, period, seed) - 0.5f) * 0.8f;
					const float oy = static_cast<float>(gy) + 0.5f + (Hash01(gx, gy, period, seed + 7u) - 0.5f) * 0.8f;
					const float radius = 0.1f + 0.2f * Hash01(gx, gy, period, seed + 13u);
					nearest = std::min(nearest, std::sqrt((ox - x) * (ox - x) + (oy - y) * (oy - y)) / radius);
				}
			}
			return nearest;
		}

		/// Replaces every value by its rank in [0, 1): a threshold at 1 - C then keeps exactly a share C.
		void Equalise(std::vector<float>& values)
		{
			std::vector<uint32_t> order(values.size());
			std::iota(order.begin(), order.end(), 0u);
			std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return values[a] < values[b]; });
			const float scale = 1.0f / static_cast<float>(values.size());
			for (size_t rank = 0; rank < order.size(); ++rank)
				values[order[rank]] = (static_cast<float>(rank) + 0.5f) * scale;
		}

		uint8_t ToUnorm(float v)
		{
			return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
		}
	}

	std::vector<Level> Bake(uint32_t size)
	{
		size = std::max(size, 4u);
		const size_t count = static_cast<size_t>(size) * size;
		std::vector<float> foam(count), bubbles(count);
		constexpr int32_t Large = static_cast<int32_t>(BubblesPerTile);
		constexpr int32_t Small = 32;  // ~2.7 times smaller bubbles in the walls between the large ones
		constexpr int32_t Specks = 40;

		for (uint32_t y = 0; y < size; ++y) {
			for (uint32_t x = 0; x < size; ++x) {
				const float tx = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
				const float ty = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
				// Domain warp (in large-bubble cells): bends the bubble walls into curved filaments.
				const float warpX = (Fbm(tx, ty, 6, 11u) - 0.5f) * 1.6f;
				const float warpY = (Fbm(tx, ty, 6, 23u) - 0.5f) * 1.6f;
				const float large = BubbleField(tx * Large + warpX, ty * Large + warpY, Large, 37u);
				const float fineField = BubbleField(tx * Small + warpX * (static_cast<float>(Small) / Large) + 9.3f,
					ty * Small + warpY * (static_cast<float>(Small) / Large) + 9.3f, Small, 59u);
				// Foam gathers in clumps and streaks: denser foam survives to lower coverage there.
				const float clumps = Fbm(tx, ty, 3, 71u);
				// The coverage at which this texel turns white: the bubbles shrink from radius 1.45 (nothing but the
				// densest walls) to 0.05 (everything but the bubble centres) as the coverage rises, the fineField bubbles a
				// little inside the large ones.
				const float wall = std::min(large, fineField / 0.85f);
				const float r = std::clamp((1.45f - wall) / 1.4f, 0.0f, 1.0f);
				const float onset = r * r / (0.45f + 1.1f * clumps);
				foam[static_cast<size_t>(y) * size + x] = 1.0f - onset;
				bubbles[static_cast<size_t>(y) * size + x] = -SpeckDistance(tx * Specks, ty * Specks, Specks, 83u);
			}
		}
		Equalise(foam);
		Equalise(bubbles);

		std::vector<Level> levels;
		Level top;
		top.size = size;
		top.texels.resize(count * 2);
		for (size_t i = 0; i < count; ++i) {
			top.texels[i * 2 + 0] = ToUnorm(foam[i]);
			top.texels[i * 2 + 1] = ToUnorm(bubbles[i]);
		}
		levels.push_back(std::move(top));

		// Box-filtered mips: far away the texture averages to the coverage it is thresholded at.
		std::vector<float> fr(foam), fg(bubbles);
		uint32_t s = size;
		while (s > 1) {
			const uint32_t n = s / 2;
			std::vector<float> nr(static_cast<size_t>(n) * n), ng(static_cast<size_t>(n) * n);
			for (uint32_t y = 0; y < n; ++y) {
				for (uint32_t x = 0; x < n; ++x) {
					const size_t a = static_cast<size_t>(2 * y) * s + 2 * x;
					nr[static_cast<size_t>(y) * n + x] = 0.25f * (fr[a] + fr[a + 1] + fr[a + s] + fr[a + s + 1]);
					ng[static_cast<size_t>(y) * n + x] = 0.25f * (fg[a] + fg[a + 1] + fg[a + s] + fg[a + s + 1]);
				}
			}
			Level level;
			level.size = n;
			level.texels.resize(static_cast<size_t>(n) * n * 2);
			for (size_t i = 0; i < nr.size(); ++i) {
				level.texels[i * 2 + 0] = ToUnorm(nr[i]);
				level.texels[i * 2 + 1] = ToUnorm(ng[i]);
			}
			levels.push_back(std::move(level));
			fr.swap(nr);
			fg.swap(ng);
			s = n;
		}
		return levels;
	}
}
