#include "ShoreProfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace
{
	using Table = PBRWaterModel::ShoreProfileTable;

	float Smoothstep(float a, float b, float x)
	{
		const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/// Where the bore's front stands in the wave, in phase.
	constexpr float BoreFront = 0.16f;

	struct Shape
	{
		float forward, up, foam;
	};

	/**
	 * The wave at phase x and stage v, as three overlapping phases of its life:
	 *  - shoaling (to 0.5): a sinusoid narrows into a peaked crest over long flat troughs, with the crest moving towards the
	 *    front of the wave as its front face steepens (cnoidal, then solitary, as Ursell's number grows);
	 *  - plunging (0.4 to 0.74): the water at the crest outruns the wave and is thrown ahead of its front, so past 0.45 the
	 *    surface folds over itself into a lip, which then falls;
	 *  - the bore (from 0.58): what is left is a turbulent front with a long sloping back (Svendsen 2006), a good deal lower
	 *    than the wave that broke, rolling white water along its front.
	 */
	Shape Evaluate(float x, float v)
	{
		const float s = Smoothstep(0.0f, 0.5f, v);
		const float crest = 0.5f - 0.18f * s;
		const float front = 0.5f * (1.0f - s) + 0.075f * s;
		const float back = 0.5f * (1.0f - s) + 0.30f * s;
		const float xi = x - crest;
		const float a = xi < 0.0f ? std::clamp(-xi / front, 0.0f, 1.0f) : std::clamp(xi / back, 0.0f, 1.0f);
		// Raised cosine on either side of the crest, narrowed towards it as the wave peaks.
		const float wave = std::pow(0.5f * (1.0f + std::cos(std::numbers::pi_v<float> * a)), 1.0f + 1.2f * s);

		const float lip = Smoothstep(0.40f, 0.60f, v) * (1.0f - Smoothstep(0.60f, 0.74f, v));
		const float lipWidth = 0.11f + 0.05f * lip;
		const float thrown = std::exp(-(xi / lipWidth) * (xi / lipWidth));
		const float waveForward = 0.10f * Smoothstep(0.25f, 0.55f, v) * wave + 0.26f * lip * thrown;
		const float crown = std::exp(-(xi / 0.06f) * (xi / 0.06f));
		const float waveFoam = Smoothstep(0.46f, 0.56f, v) * crown;

		const float b = Smoothstep(0.58f, 0.80f, v);
		const float frontWidth = 0.022f + 0.02f * (1.0f - b);
		const float bore = x < BoreFront ? Smoothstep(BoreFront - 3.0f * frontWidth, BoreFront, x) :
		                                   std::exp(-(x - BoreFront) / 0.34f) * (1.0f - Smoothstep(0.82f, 1.0f, x));
		// A thin line of white water riding the front, trailing off quickly along the back; the rest of the wave stays clear.
		const float boreFoam = x < BoreFront ? Smoothstep(BoreFront - 2.0f * frontWidth, BoreFront, x) : std::exp(-(x - BoreFront) / 0.09f);
		const float collapse = 1.0f - 0.42f * Smoothstep(0.58f, 0.72f, v);

		Shape o;
		o.up = (1.0f - b) * wave + b * bore * collapse;
		o.forward = (1.0f - b) * waveForward + b * 0.05f * bore;
		o.foam = std::max((1.0f - b) * waveFoam, b * boreFoam * (1.0f - Smoothstep(0.92f, 1.0f, v)));
		return o;
	}

	Table Bake()
	{
		constexpr uint32_t W = Table::Phases;
		constexpr uint32_t H = Table::Stages;
		Table t;
		t.slice0.resize(static_cast<size_t>(W) * H * 4);
		t.slice1.resize(static_cast<size_t>(W) * H * 4);
		const float dx = 1.0f / static_cast<float>(W - 1);
		std::array<Shape, W> row{};
		for (uint32_t j = 0; j < H; ++j) {
			const float v = static_cast<float>(j) / static_cast<float>(H - 1);
			float mean = 0.0f;
			for (uint32_t i = 0; i < W; ++i) {
				row[i] = Evaluate(static_cast<float>(i) * dx, v);
				mean += row[i].up;
			}
			mean /= static_cast<float>(W);
			for (uint32_t i = 0; i < W; ++i) {
				const auto& a = row[i == 0 ? 0 : i - 1];
				const auto& b = row[i == W - 1 ? W - 1 : i + 1];
				const float span = dx * static_cast<float>((i == W - 1 ? W - 1 : i + 1) - (i == 0 ? 0 : i - 1));
				float* s0 = &t.slice0[(static_cast<size_t>(j) * W + i) * 4];
				float* s1 = &t.slice1[(static_cast<size_t>(j) * W + i) * 4];
				s0[0] = row[i].forward;
				s0[1] = row[i].up;
				s0[2] = (b.forward - a.forward) / span;
				s0[3] = (b.up - a.up) / span;
				s1[0] = row[i].foam;
				s1[1] = mean;
			}
		}
		return t;
	}
}

namespace PBRWaterModel
{
	ShoreProfilePoint ShoreProfileTable::At(float phase, float stage) const
	{
		const float fx = std::clamp(phase, 0.0f, 1.0f) * static_cast<float>(Phases - 1);
		const float fy = std::clamp(stage, 0.0f, 1.0f) * static_cast<float>(Stages - 1);
		const uint32_t x0 = std::min(static_cast<uint32_t>(fx), Phases - 2);
		const uint32_t y0 = std::min(static_cast<uint32_t>(fy), Stages - 2);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		auto lerp2 = [&](const std::vector<float>& s, uint32_t c) {
			const float* row0 = &s[(static_cast<size_t>(y0) * Phases + x0) * 4 + c];
			const float* row1 = row0 + static_cast<size_t>(Phases) * 4;
			const float a = row0[0] + (row0[4] - row0[0]) * tx;
			const float b = row1[0] + (row1[4] - row1[0]) * tx;
			return a + (b - a) * ty;
		};
		ShoreProfilePoint p;
		p.forward = lerp2(slice0, 0);
		p.up = lerp2(slice0, 1);
		p.dForward = lerp2(slice0, 2);
		p.dUp = lerp2(slice0, 3);
		p.foam = lerp2(slice1, 0);
		p.mean = lerp2(slice1, 1);
		return p;
	}

	const ShoreProfileTable& GetShoreProfile()
	{
		static const ShoreProfileTable table = Bake();
		return table;
	}
}
