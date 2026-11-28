// PBR Water's shore field: the coastline map the shore waves run on (BuildShoreField), and its queries.

#include "WaveModel.h"
#include "WaveModelMath.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace PBRWaterModel
{
	using namespace Math;

	// ========================================================================
	// Shore field
	// ========================================================================

	float Wavenumber(float omega, float depth)
	{
		const float k0 = omega * omega / G;
		const float t = std::tanh(std::min(k0 * std::max(depth, 1e-3f), 20.0f));
		return k0 / std::sqrt(std::max(t, 1e-6f));
	}

	float PhaseSpeed(float omega, float depth)
	{
		omega = std::max(omega, 1e-3f);
		return omega / Wavenumber(omega, depth);
	}

	float ShoalingCoefficient(float omega, float depth)
	{
		omega = std::max(omega, 1e-3f);
		depth = std::max(depth, 1e-3f);
		const float k = Wavenumber(omega, depth);
		const float kh = std::min(k * depth, 20.0f);
		const float n = 0.5f * (1.0f + 2.0f * kh / std::sinh(2.0f * kh));
		const float cg = n * omega / k;
		const float cg0 = 0.5f * G / omega;
		return std::sqrt(cg0 / cg);
	}

	namespace
	{
		constexpr float Far = 1.0e30f;

		/// Separable [1 4 6 4 1] / 16 blur, clamped at the edges.
		void Blur(std::vector<float>& v, uint32_t w, uint32_t h, int passes)
		{
			std::vector<float> tmp(v.size());
			auto clampX = [&](int x) { return static_cast<uint32_t>(std::clamp(x, 0, static_cast<int>(w) - 1)); };
			auto clampY = [&](int y) { return static_cast<uint32_t>(std::clamp(y, 0, static_cast<int>(h) - 1)); };
			for (int p = 0; p < passes; ++p) {
				for (uint32_t y = 0; y < h; ++y) {
					const float* row = &v[static_cast<size_t>(y) * w];
					for (uint32_t x = 0; x < w; ++x) {
						const int xi = static_cast<int>(x);
						tmp[static_cast<size_t>(y) * w + x] = (row[clampX(xi - 2)] + 4.0f * row[clampX(xi - 1)] + 6.0f * row[x] + 4.0f * row[clampX(xi + 1)] + row[clampX(xi + 2)]) * (1.0f / 16.0f);
					}
				}
				for (uint32_t y = 0; y < h; ++y) {
					const int yi = static_cast<int>(y);
					for (uint32_t x = 0; x < w; ++x) {
						auto at = [&](int yy) { return tmp[static_cast<size_t>(clampY(yy)) * w + x]; };
						v[static_cast<size_t>(y) * w + x] = (at(yi - 2) + 4.0f * at(yi - 1) + 6.0f * at(yi) + 4.0f * at(yi + 1) + at(yi + 2)) * (1.0f / 16.0f);
					}
				}
			}
		}

		/**
		 * Fast sweeping (Zhao 2005) for |grad T| = cost (per texel) on the `active` texels, with the Godunov
		 * upwind update. Texels outside `active` never pass values on.
		 */
		void FastSweep(std::vector<float>& T, const std::vector<float>& cost, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, int iterations, const std::atomic_bool* cancel)
		{
			const int W = static_cast<int>(w);
			const int H = static_cast<int>(h);
			auto at = [&](int x, int y) {
				if (x < 0 || y < 0 || x >= W || y >= H)
					return Far;
				const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
				return active[i] ? T[i] : Far;
			};
			for (int it = 0; it < iterations; ++it) {
				for (int sweep = 0; sweep < 4; ++sweep) {
					if (cancel && cancel->load(std::memory_order_relaxed))
						return;
					const int dx = (sweep & 1) ? -1 : 1;
					const int dy = (sweep & 2) ? -1 : 1;
					for (int y = dy > 0 ? 0 : H - 1; y >= 0 && y < H; y += dy) {
						for (int x = dx > 0 ? 0 : W - 1; x >= 0 && x < W; x += dx) {
							const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
							if (!active[i])
								continue;
							const float a = std::min(at(x - 1, y), at(x + 1, y));
							const float b = std::min(at(x, y - 1), at(x, y + 1));
							if (a >= Far && b >= Far)
								continue;
							const float f = cost[i];
							float t;
							if (std::abs(a - b) >= f)
								t = std::min(a, b) + f;
							else
								t = 0.5f * (a + b + std::sqrt(2.0f * f * f - (a - b) * (a - b)));
							T[i] = std::min(T[i], t);
						}
					}
				}
			}
		}

		/// Bilinear interpolation over the corners already computed (`done`). False when none is.
		bool InterpolateDone(const std::vector<float>& v, const std::vector<uint8_t>& done, uint32_t w, uint32_t h, float fx, float fy, float& out)
		{
			const int x0 = static_cast<int>(std::floor(fx));
			const int y0 = static_cast<int>(std::floor(fy));
			const float tx = fx - static_cast<float>(x0);
			const float ty = fy - static_cast<float>(y0);
			float sum = 0.0f, weights = 0.0f;
			for (int c = 0; c < 4; ++c) {
				const int x = x0 + (c & 1);
				const int y = y0 + (c >> 1);
				if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h))
					continue;
				const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
				if (!done[i])
					continue;
				const float weight = ((c & 1) ? tx : 1.0f - tx) * ((c >> 1) ? ty : 1.0f - ty);
				sum += v[i] * weight;
				weights += weight;
			}
			if (weights < 1e-4f)
				return false;
			out = sum / weights;
			return true;
		}

		/// Gradient of `v` at (x, y) per texel from the neighbours that are `active` and finite.
		bool Gradient(const std::vector<float>& v, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, int x, int y, float& gx, float& gy)
		{
			auto get = [&](int xx, int yy, float& out) {
				if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
					return false;
				const size_t i = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
				if (!active[i] || !(v[i] < Far))
					return false;
				out = v[i];
				return true;
			};
			float c = 0.0f, l = 0.0f, r = 0.0f, d = 0.0f, u = 0.0f;
			if (!get(x, y, c))
				return false;
			const bool hasL = get(x - 1, y, l), hasR = get(x + 1, y, r), hasD = get(x, y - 1, d), hasU = get(x, y + 1, u);
			gx = hasL && hasR ? 0.5f * (r - l) : (hasR ? r - c : (hasL ? c - l : 0.0f));
			gy = hasD && hasU ? 0.5f * (u - d) : (hasU ? u - c : (hasD ? c - d : 0.0f));
			return hasL || hasR || hasD || hasU;
		}
	}

	ShoreField::Sample ShoreField::At(float x, float y) const
	{
		Sample out;
		if (!Valid())
			return out;
		const float fx = (x - minX) / step;
		const float fy = (y - minY) / step;
		const float maxX = static_cast<float>(width - 1);
		const float maxY = static_cast<float>(height - 1);
		if (fx < 0.0f || fy < 0.0f || fx > maxX || fy > maxY)
			return out;
		const uint32_t x0 = std::min(static_cast<uint32_t>(fx), width - 2);
		const uint32_t y0 = std::min(static_cast<uint32_t>(fy), height - 2);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		auto lerp2 = [&](const std::vector<float>& v) {
			const size_t i = static_cast<size_t>(y0) * width + x0;
			const float a = v[i] + (v[i + 1] - v[i]) * tx;
			const float b = v[i + width] + (v[i + width + 1] - v[i + width]) * tx;
			return a + (b - a) * ty;
		};
		out.travelTime = lerp2(travelTime);
		out.phaseLag = omega * out.travelTime;
		out.distance = lerp2(distance);
		out.along = lerp2(along);
		out.slownessX = lerp2(slownessX);
		out.slownessY = lerp2(slownessY);
		out.slope = lerp2(slope);
		out.exposure = lerp2(exposure);
		out.room = lerp2(room);
		// Fade out over the outer tenth of the field, so its moving edge never pops, as PBRWater::SampleShore.
		const float edge = std::min(std::min(fx, maxX - fx) / maxX, std::min(fy, maxY - fy) / maxY);
		out.weight = Smoothstep(0.0f, 0.1f, edge);
		return out;
	}

	float ShoreField::CrestDepthAt(float x, float y, float fallback) const
	{
		const float fx = (x - minX) / step;
		const float fy = (y - minY) / step;
		if (!Valid() || crestDepth.size() != level.size() || fx < 0.0f || fy < 0.0f || fx > static_cast<float>(width - 1) || fy > static_cast<float>(height - 1))
			return fallback;
		const uint32_t x0 = std::min(static_cast<uint32_t>(fx), width - 2);
		const uint32_t y0 = std::min(static_cast<uint32_t>(fy), height - 2);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const size_t i = static_cast<size_t>(y0) * width + x0;
		const float a = crestDepth[i] + (crestDepth[i + 1] - crestDepth[i]) * tx;
		const float b = crestDepth[i + width] + (crestDepth[i + width + 1] - crestDepth[i + width]) * tx;
		return a + (b - a) * ty;
	}

	namespace
	{
		/**
		 * First-order fast sweeping leaves faint kinks where the upwind direction switches between neighbours; one
		 * [1 4 6 4 1] pass over the texels whose whole kernel is `reached` smooths them without moving the isolines (the
		 * filter is symmetric, so it leaves a locally linear field unchanged and only rounds corners of about a texel).
		 * Texels near the edge of the reached area keep their own value.
		 */
		std::vector<float> SmoothReached(const std::vector<float>& v, const std::vector<uint8_t>& reached, uint32_t w, uint32_t h)
		{
			const int W = static_cast<int>(w);
			const int H = static_cast<int>(h);
			auto ok = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && reached[static_cast<size_t>(y) * w + static_cast<size_t>(x)]; };
			constexpr float Kernel[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
			std::vector<float> rows(v.size(), 0.0f), out(v);
			std::vector<uint8_t> full(v.size(), 0);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					bool all = true;
					float sum = 0.0f;
					for (int t = -2; t <= 2 && all; ++t) {
						all = ok(x + t, y);
						if (all)
							sum += Kernel[t + 2] * v[static_cast<size_t>(y) * w + static_cast<size_t>(x + t)];
					}
					const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
					rows[i] = sum;
					full[i] = all;
				}
			}
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					if (!ok(x, y))
						continue;
					bool all = true;
					float sum = 0.0f;
					for (int t = -2; t <= 2 && all; ++t) {
						all = y + t >= 0 && y + t < H && full[static_cast<size_t>(y + t) * w + static_cast<size_t>(x)];
						if (all)
							sum += Kernel[t + 2] * rows[static_cast<size_t>(y + t) * w + static_cast<size_t>(x)];
					}
					if (all)
						out[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = sum;
				}
			}
			return out;
		}

		/// Unit direction of the gradient of `v` (lightly smoothed, 3 x 3) on the `active` texels; `sign` -1 points it downhill.
		void GradientDirections(const std::vector<float>& v, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, float sign, std::vector<float>& outX, std::vector<float>& outY)
		{
			const size_t n = v.size();
			std::vector<float> gx(n, 0.0f), gy(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					float dx = 0.0f, dy = 0.0f;
					if (!active[i] || !Gradient(v, active, w, h, static_cast<int>(x), static_cast<int>(y), dx, dy))
						continue;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len > 1e-12f) {
						gx[i] = sign * dx / len;
						gy[i] = sign * dy / len;
					}
				}
			}
			outX.assign(n, 0.0f);
			outY.assign(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					float sx = 0.0f, sy = 0.0f;
					for (int k = -1; k <= 1; ++k) {
						for (int l = -1; l <= 1; ++l) {
							const int xx = std::clamp(static_cast<int>(x) + l, 0, static_cast<int>(w) - 1);
							const int yy = std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(h) - 1);
							const size_t j = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
							sx += gx[j];
							sy += gy[j];
						}
					}
					const float len = std::sqrt(sx * sx + sy * sy);
					const size_t i = static_cast<size_t>(y) * w + x;
					if (len > 1e-3f) {
						outX[i] = sx / len;
						outY[i] = sy / len;
					}
				}
			}
		}
	}

	std::shared_ptr<ShoreField> BuildShoreField(const ShoreInputs& in, const std::atomic_bool* cancel)
	{
		const uint32_t w = in.width;
		const uint32_t h = in.height;
		const size_t n = static_cast<size_t>(w) * h;
		if (w < 8 || h < 8 || in.bed.size() != n || in.level.size() != n)
			return nullptr;
		auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

		const float metresPerUnit = static_cast<float>(MetresPerUnit);
		const float stepMetres = in.step * metresPerUnit;
		// The swash lens: waves never slow down below the speed in 15 cm of water.
		constexpr float MinDepth = 0.15f;

		// Bed, lightly smoothed: the terrain's vertices are 1.8 m apart, which would make depths and slopes step.
		std::vector<float> bed(n);
		for (size_t i = 0; i < n; ++i) {
			const float level = in.level[i];
			bed[i] = std::isfinite(in.bed[i]) ? in.bed[i] : (std::isfinite(level) ? level - 1.0e4f : 0.0f);
		}
		// What is water is decided on the unsmoothed bed, so a narrow stream between steep banks stays water. The
		// smoothed bed only sets the depths the waves feel.
		std::vector<uint8_t> water(n), land(n);
		for (size_t i = 0; i < n; ++i) {
			water[i] = std::isfinite(in.level[i]) && in.level[i] - bed[i] > 0.0f;
			land[i] = !water[i];
		}
		Blur(bed, w, h, 2);

		std::vector<float> depth(n);
		for (size_t i = 0; i < n; ++i) {
			const float smoothed = std::isfinite(in.level[i]) ? in.level[i] - bed[i] : -1.0e4f;
			depth[i] = water[i] ? std::max(smoothed, 1.0f) : std::min(smoothed, -1.0f);
		}

		// Distance to the waterline on either side (units), seeded with the zero crossing of the depth between
		// neighbours: the beach slope and, up the beach, the travel times are carried along it.
		std::vector<float> waterDistance(n, Far), landDistance(n, Far), unit(n, in.step);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (!water[i])
					continue;
				const int nx[4] = { -1, 1, 0, 0 };
				const int ny[4] = { 0, 0, -1, 1 };
				for (int k = 0; k < 4; ++k) {
					const int xx = static_cast<int>(x) + nx[k];
					const int yy = static_cast<int>(y) + ny[k];
					if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
						continue;
					const size_t j = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
					if (!land[j])
						continue;
					const float frac = std::clamp(depth[i] / std::max(depth[i] - depth[j], 1e-3f), 0.0f, 1.0f);
					waterDistance[i] = std::min(waterDistance[i], frac * in.step);
					landDistance[j] = std::min(landDistance[j], (1.0f - frac) * in.step);
				}
			}
		}
		FastSweep(waterDistance, unit, water, w, h, 2, cancel);
		FastSweep(landDistance, unit, land, w, h, 2, cancel);
		if (cancelled())
			return nullptr;

		auto field = std::make_shared<ShoreField>();
		field->width = w;
		field->height = h;
		field->minX = in.minX;
		field->minY = in.minY;
		field->step = in.step;
		field->swellDirection = in.swellDirection;
		field->level = in.level;

		// Rays across the waterline: towards it in the water, away from it on land.
		std::vector<float> rayX, rayY, inlandX, inlandY;
		GradientDirections(waterDistance, water, w, h, -1.0f, rayX, rayY);
		GradientDirections(landDistance, land, w, h, 1.0f, inlandX, inlandY);
		for (size_t i = 0; i < n; ++i) {
			if (land[i]) {
				rayX[i] = inlandX[i];
				rayY[i] = inlandY[i];
			}
		}
		if (cancelled())
			return nullptr;

		std::vector<uint32_t> seaward, inland;
		seaward.reserve(n);
		inland.reserve(n);
		for (uint32_t i = 0; i < n; ++i) {
			if (water[i] && waterDistance[i] < Far)
				seaward.push_back(i);
			if (land[i] && landDistance[i] < Far)
				inland.push_back(i);
		}
		std::sort(seaward.begin(), seaward.end(), [&](uint32_t a, uint32_t b) { return waterDistance[a] < waterDistance[b]; });
		std::sort(inland.begin(), inland.end(), [&](uint32_t a, uint32_t b) { return landDistance[a] < landDistance[b]; });

		// The time tau the waves take to the nearest waterline: Fluid Flux's coastline distance, as a travel time along
		// the straight ray from the shore at the phase speed over the real depths. The crests stay parallel to the
		// coast, slow and close up as the water shoals, and surf runs into every beach nearly head-on whatever the sea
		// offshore does. (A true first-arrival eikonal races through deep holes and leaves Skyrim's uneven beds without
		// coherent fronts.)
		const float omega = std::max(in.omega, 0.05f);
		field->omega = omega;
		std::vector<float> tau(n, 0.0f), speed(n, 1.0f);
		std::vector<uint8_t> reached(n, 0);
		for (const uint32_t i : seaward) {
			speed[i] = PhaseSpeed(omega, std::max(depth[i] * metresPerUnit, MinDepth));
			const float cost = stepMetres / speed[i];
			float behind = 0.0f;
			if (waterDistance[i] <= in.step) {
				tau[i] = waterDistance[i] / in.step * cost;
				reached[i] = 1;
			} else if (InterpolateDone(tau, reached, w, h, static_cast<float>(i % w) + rayX[i], static_cast<float>(i / w) + rayY[i], behind)) {
				tau[i] = behind + cost;
				reached[i] = 1;
			}
		}
		if (cancelled())
			return nullptr;
		field->travelTime = SmoothReached(tau, reached, w, h);

		// The direction of travel, smoothed over ~7 m of water. Where rays from different stretches of shore meet (mid-channel,
		// off a headland) they disagree and the average shortens: its length is how coherent the fronts are.
		std::vector<float> fronts(n, 0.0f);
		{
			std::vector<float> gx(n, 0.0f), gy(n, 0.0f), weight(n, 0.0f);
			for (size_t i = 0; i < n; ++i) {
				if (reached[i]) {
					gx[i] = rayX[i];
					gy[i] = rayY[i];
					weight[i] = 1.0f;
				}
			}
			Blur(gx, w, h, 8);
			Blur(gy, w, h, 8);
			Blur(weight, w, h, 8);
			auto& sx = field->slownessX;
			auto& sy = field->slownessY;
			sx.assign(n, 0.0f);
			sy.assign(n, 0.0f);
			for (size_t i = 0; i < n; ++i) {
				if (!reached[i])
					continue;
				const float length = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]);
				fronts[i] = length / std::max(weight[i], 1e-4f);
				if (length > 1e-4f) {
					// grad tau: seaward, its length 1 / c.
					sx[i] = -gx[i] / length / speed[i];
					sy[i] = -gy[i] / length / speed[i];
				}
			}
		}

		// The depth a wave answers to as its crest passes (m). Two things make it differ from the depth under the
		// point: - A wave feels the bottom over a stretch of its crest: the depth is averaged along the crest (across
		// the ray), a LAND
		//    quad at least and a quarter wave at most either way.
		// - Breaking is not undone: a wave that has broken over a shoal or bar crosses the deeper water behind it as a
		// bore. So
		//    the depth is the least the wave has crossed on its way in, carried shorewards along the ray, and its stage only
		//    advances.
		field->crestDepth.assign(n, 0.0f);
		// Water the rays did not reach (far from any shore) answers to its own depth.
		for (size_t i = 0; i < n; ++i)
			field->crestDepth[i] = std::max(depth[i], 0.0f) * metresPerUnit;
		{
			auto depthAt = [&](float fx, float fy) {
				fx = std::clamp(fx, 0.0f, static_cast<float>(w - 1));
				fy = std::clamp(fy, 0.0f, static_cast<float>(h - 1));
				const uint32_t x0 = std::min(static_cast<uint32_t>(fx), w - 2);
				const uint32_t y0 = std::min(static_cast<uint32_t>(fy), h - 2);
				const float tx = fx - static_cast<float>(x0);
				const float ty = fy - static_cast<float>(y0);
				const size_t i = static_cast<size_t>(y0) * w + x0;
				const float a = depth[i] + (depth[i + 1] - depth[i]) * tx;
				const float b = depth[i + w] + (depth[i + w + 1] - depth[i + w]) * tx;
				return a + (b - a) * ty;
			};
			constexpr float offsets[5] = { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f };
			constexpr float weights[5] = { 1.0f, 2.0f, 3.0f, 2.0f, 1.0f };
			std::vector<float> alongCrest(n, 0.0f);
			for (const uint32_t i : seaward) {
				const float x = static_cast<float>(i % w);
				const float y = static_cast<float>(i / w);
				const float wavelength = speed[i] * static_cast<float>(TwoPi) / omega * static_cast<float>(UnitsPerMetre);
				const float half = std::max(128.0f, 0.25f * wavelength) / in.step;
				float sum = 0.0f;
				for (int k = 0; k < 5; ++k)
					sum += weights[k] * depthAt(x - rayY[i] * offsets[k] * half, y + rayX[i] * offsets[k] * half);
				alongCrest[i] = std::max(sum / 9.0f, 0.0f) * metresPerUnit;
			}
			// From the open water in: each texel after the ones seaward of it.
			std::vector<uint8_t> done(n, 0);
			for (auto it = seaward.rbegin(); it != seaward.rend(); ++it) {
				const uint32_t i = *it;
				float crossed = 0.0f;
				float value = alongCrest[i];
				if (InterpolateDone(field->crestDepth, done, w, h, static_cast<float>(i % w) - rayX[i], static_cast<float>(i / w) - rayY[i], crossed))
					value = std::min(value, crossed);
				field->crestDepth[i] = value;
				done[i] = 1;
			}
			// Neighbouring rays that crossed different shoals carry different minima: the stage, and with it a breaker's
			// curl, would jump along the crest where they meet. Smoothed over ~5 m, as the slope is.
			Blur(field->crestDepth, w, h, 3);
		}
		if (cancelled())
			return nullptr;

		// Signed distance to the waterline (Fluid Flux's coastline distance): it finds the nearest point of the coast, where
		// the shaders read the variation along it and the swash its timing.
		field->distance.assign(n, 0.0f);
		for (size_t i = 0; i < n; ++i) {
			if (water[i])
				field->distance[i] = std::min(waterDistance[i], 1.0e7f);
			else
				field->distance[i] = -std::min(landDistance[i], 1.0e7f);
		}
		if (cancelled())
			return nullptr;

		// Exposure to the swell: open water upwind along its direction. Shores facing away from it, and water behind
		// islands, reefs and headlands, see little of it (diffraction fills in over a few wavelengths, hence the soft ramp
		// from 300 m to 3 km of open water). Measured on the map's own distances, not the fetch's compressed ones (Fetch
		// Scale): counted 15 times over, a 100 m gap between two islands let 70 % of the swell into the cove behind them.
		field->exposure.assign(n, 0.0f);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (!reached[i])
					continue;
				// The swell lives on the sea: a lake or a river above it (however long its open water) never sees it.
				if (std::isfinite(in.seaLevel) && std::abs(in.level[i] - in.seaLevel) > 256.0f)
					continue;
				const float wx = in.minX + static_cast<float>(x) * in.step;
				const float wy = in.minY + static_cast<float>(y) * in.step;
				const float fetchMetres = in.fetch && in.fetch->Valid() ? in.fetch->SampleMetres(wx, wy, in.swellDirection) : 5.0e5f;
				field->exposure[i] = Smoothstep(2.5f, 3.5f, std::log10(std::max(fetchMetres, 1.0f)));
			}
		}
		// Diffraction fills the shadow behind a headland over tens of metres (sqrt(lambda r)), where the ray-marched
		// fetch has razor edges along the swell direction: averaged over the water within ~50 m (on a
		// quarter-resolution copy), a lee shore close to open water still sees some of the swell; water it can never
		// reach (a lake, a stream) sees none.
		{
			constexpr uint32_t Down = 4;
			const uint32_t lw = (w + Down - 1) / Down;
			const uint32_t lh = (h + Down - 1) / Down;
			std::vector<float> sum(static_cast<size_t>(lw) * lh, 0.0f), weight(static_cast<size_t>(lw) * lh, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!reached[i])
						continue;
					const size_t j = static_cast<size_t>(y / Down) * lw + x / Down;
					sum[j] += field->exposure[i];
					weight[j] += 1.0f;
				}
			}
			Blur(sum, lw, lh, 48);
			Blur(weight, lw, lh, 48);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!reached[i])
						continue;
					// Bilinear between the quarter-resolution texel centres.
					const float fx = std::clamp((static_cast<float>(x) + 0.5f) / Down - 0.5f, 0.0f, static_cast<float>(lw - 1));
					const float fy = std::clamp((static_cast<float>(y) + 0.5f) / Down - 0.5f, 0.0f, static_cast<float>(lh - 1));
					const uint32_t x0 = std::min(static_cast<uint32_t>(fx), lw > 1 ? lw - 2 : 0u);
					const uint32_t y0 = std::min(static_cast<uint32_t>(fy), lh > 1 ? lh - 2 : 0u);
					const float tx = fx - static_cast<float>(x0);
					const float ty = fy - static_cast<float>(y0);
					auto at = [&](const std::vector<float>& v, uint32_t xx, uint32_t yy) { return v[static_cast<size_t>(yy) * lw + xx]; };
					auto bilinear = [&](const std::vector<float>& v) {
						const float a = at(v, x0, y0) + (at(v, x0 + 1, y0) - at(v, x0, y0)) * tx;
						const float b = at(v, x0, y0 + 1) + (at(v, x0 + 1, y0 + 1) - at(v, x0, y0 + 1)) * tx;
						return a + (b - a) * ty;
					};
					const float average = bilinear(sum) / std::max(bilinear(weight), 1e-4f);
					field->exposure[i] = std::clamp(average, 0.0f, 1.0f);
				}
			}
		}

		// Room for the wave train: the fronts run from mid-water to each shore, so the water must be a fair part of a
		// wavelength across for them to look like waves at all. Only a ditch narrower than that has none (the FFT sea
		// carries what waves it has); a stream keeps a train, as small as its fetch-limited sea (no swell reaches it). The width is that of the largest
		// disc of water around each point (twice the distance to the waterline at its centre: the width of a channel, the
		// size of a pond), propagated by raster sweeps; the wavelength that of the shore waves in the local depth.
		field->room.assign(n, 0.0f);
		{
			std::vector<float> radius(n, 0.0f), reach(n, -1.0f);
			for (size_t i = 0; i < n; ++i) {
				if (water[i]) {
					radius[i] = std::min(waterDistance[i], 1.0e7f);
					reach[i] = radius[i];
				}
			}
			const float diagonal = in.step * std::numbers::sqrt2_v<float>;
			auto relax = [&](size_t i, size_t j, float d) {
				const float r = reach[j] - d;
				if (r >= 0.0f && radius[j] > radius[i]) {
					radius[i] = radius[j];
					reach[i] = r;
				}
			};
			for (int pass = 0; pass < 2; ++pass) {
				for (uint32_t y = 0; y < h; ++y) {
					for (uint32_t x = 0; x < w; ++x) {
						const size_t i = static_cast<size_t>(y) * w + x;
						if (!water[i])
							continue;
						if (x > 0)
							relax(i, i - 1, in.step);
						if (y > 0) {
							relax(i, i - w, in.step);
							if (x > 0)
								relax(i, i - w - 1, diagonal);
							if (x + 1 < w)
								relax(i, i - w + 1, diagonal);
						}
					}
				}
				for (uint32_t y = h; y-- > 0;) {
					for (uint32_t x = w; x-- > 0;) {
						const size_t i = static_cast<size_t>(y) * w + x;
						if (!water[i])
							continue;
						if (x + 1 < w)
							relax(i, i + 1, in.step);
						if (y + 1 < h) {
							relax(i, i + w, in.step);
							if (x + 1 < w)
								relax(i, i + w + 1, diagonal);
							if (x > 0)
								relax(i, i + w - 1, diagonal);
						}
					}
				}
				if (cancelled())
					return nullptr;
			}
			for (size_t i = 0; i < n; ++i) {
				if (!water[i])
					continue;
				const float wavelength = static_cast<float>(TwoPi) / Wavenumber(omega, std::max(depth[i] * metresPerUnit, 0.5f));
				field->room[i] = Smoothstep(0.1f, 0.6f, 2.0f * radius[i] * metresPerUnit / wavelength);
			}
		}

		// The train fades out where the fronts disagree: fronts meeting head-on average to ~0, at right angles to 0.71.
		// It is gone well before the line where the directions flip, since its orbits run along the direction. Water no
		// front reached has none either (it would rise and fall in step everywhere).
		for (size_t i = 0; i < n; ++i) {
			if (water[i])
				field->room[i] *= reached[i] ? Smoothstep(0.75f, 0.95f, fronts[i]) : 0.0f;
		}

		// Up the beach the travel times, directions and exposure are those of the waterline the inland ray leaves from,
		// so the swash on a stretch of beach follows the crests that reach it.
		{
			std::vector<uint8_t> done(reached);
			std::vector<std::vector<float>*> carried = { &field->travelTime, &field->slownessX, &field->slownessY };
			carried.push_back(&field->exposure);
			carried.push_back(&field->room);
			for (const uint32_t i : inland) {
				const float x = static_cast<float>(i % w) - rayX[i];
				const float y = static_cast<float>(i / w) - rayY[i];
				bool any = false;
				for (auto* v : carried) {
					float value = 0.0f;
					if (InterpolateDone(*v, done, w, h, x, y, value)) {
						(*v)[i] = value;
						any = true;
					}
				}
				done[i] = any;
			}
			// Neighbouring rays leave from different stretches of the waterline: where they meet, the carried times and
			// directions jump from texel to texel, which put hard corners in the swash. Smoothed over the carried texels.
			for (int pass = 0; pass < 2; ++pass) {
				field->travelTime = SmoothReached(field->travelTime, done, w, h);
				field->slownessX = SmoothReached(field->slownessX, done, w, h);
				field->slownessY = SmoothReached(field->slownessY, done, w, h);
			}
		}

		// Beach slope at the waterline (bed smoothed over ~10 m), carried out along the rays and up the beach, so the
		// whole surf and swash zone of a beach shares one slope.
		{
			std::vector<float> smooth(bed);
			Blur(smooth, w, h, 4);
			std::vector<float> local(n, 1.0f);
			const std::vector<uint8_t> all(n, 1);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					float gx = 0.0f, gy = 0.0f;
					if (Gradient(smooth, all, w, h, static_cast<int>(x), static_cast<int>(y), gx, gy))
						local[static_cast<size_t>(y) * w + x] = std::sqrt(gx * gx + gy * gy) / in.step;
				}
			}
			field->slope.assign(n, 1.0f);
			std::vector<uint8_t> done(n, 0);
			for (size_t i = 0; i < n; ++i) {
				if ((water[i] && waterDistance[i] <= in.step) || (land[i] && landDistance[i] <= in.step)) {
					field->slope[i] = local[i];
					done[i] = 1;
				}
			}
			auto carry = [&](const std::vector<uint32_t>& order, float sign) {
				for (const uint32_t i : order) {
					if (done[i])
						continue;
					float value = 0.0f;
					const float x = static_cast<float>(i % w) + sign * rayX[i];
					const float y = static_cast<float>(i / w) + sign * rayY[i];
					field->slope[i] = InterpolateDone(field->slope, done, w, h, x, y, value) ? value : local[i];
					done[i] = 1;
				}
			};
			carry(seaward, 1.0f);
			carry(inland, -1.0f);
			for (size_t i = 0; i < n; ++i) {
				if (!done[i])
					field->slope[i] = local[i];
			}
		}

		// The slope, the exposure and the room are copied along rays and propagated texel by texel, so they jump where
		// rays from different stretches of shore meet (and the room's disc sweep draws octagons). They gate the surf,
		// the swash and the foam, so they are smoothed over ~4 m.
		Blur(field->slope, w, h, 4);
		Blur(field->exposure, w, h, 4);
		Blur(field->room, w, h, 4);

		// The crests' offset along the coast: smooth noise at the nearest point of the waterline, a few wavelengths across and
		// less than a wave of offset, so each stretch of shore sees them at its own time (Fluid Flux's MF_CoastlineOffsets;
		// a sea's crests are short, and break at different times along a beach). Blurred along with the seams between rays.
		const float L0 = G * static_cast<float>(TwoPi) / (in.omega * in.omega) * static_cast<float>(UnitsPerMetre);
		field->along.assign(n, 0.0f);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				const float sx = field->slownessX[i], sy = field->slownessY[i];
				const float slowness = std::sqrt(sx * sx + sy * sy);
				const float reach = slowness > 1e-6f ? field->distance[i] / slowness : 0.0f;
				const float coastX = in.minX + x * in.step - sx * reach;
				const float coastY = in.minY + y * in.step - sy * reach;
				const float coarse = ValueNoise(coastX / (4.0f * L0), coastY / (4.0f * L0));
				const float fine = ValueNoise(coastX / (1.7f * L0) + 17.1f, coastY / (1.7f * L0) + 17.1f);
				field->along[i] = 0.6f * (coarse * 0.67f + fine * 0.33f - 0.5f);
			}
		}
		Blur(field->along, w, h, 4);
		field->PackForGpu();
		return field;
	}

	void ShoreField::PackForGpu()
	{
		const size_t count = static_cast<size_t>(width) * height;
		if (!Valid())
			return;
		gpuTimes.resize(count * 4);
		gpuRays.resize(count * 4);
		gpuRoom.resize(count * 2);
		auto half = [](float v) { return DirectX::PackedVector::XMConvertFloatToHalf(v); };
		const bool crest = crestDepth.size() == count;
		for (size_t i = 0; i < count; ++i) {
			gpuTimes[i * 4 + 0] = travelTime[i];
			gpuTimes[i * 4 + 1] = distance[i];
			gpuTimes[i * 4 + 2] = along[i];
			// No water in reach: a sentinel the shaders test for (NaN would spread through the bilinear filter).
			gpuTimes[i * 4 + 3] = std::isfinite(level[i]) ? level[i] : -1.0e9f;
			gpuRays[i * 4 + 0] = half(slownessX[i]);
			gpuRays[i * 4 + 1] = half(slownessY[i]);
			gpuRays[i * 4 + 2] = half(slope[i]);
			gpuRays[i * 4 + 3] = half(exposure[i]);
			gpuRoom[i * 2 + 0] = half(room[i]);
			gpuRoom[i * 2 + 1] = half(crest ? crestDepth[i] : 0.0f);
		}
	}
}
