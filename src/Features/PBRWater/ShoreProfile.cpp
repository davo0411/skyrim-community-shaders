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

	/// Where the bore's front stands in the wave, in phase. Mirrored by PBRWater::ShoreBoreFront.
	constexpr float BoreFront = 0.16f;
	/// The least the surface may be stretched along a wave by its forward motion (1 = unchanged, 0 = folded over).
	constexpr float MinStretch = 0.35f;

	struct Shape
	{
		float forward, up, foam;
	};

	/**
	 * The wave at phase x and stage v: one hump throughout its life, whose crest, faces and height change with the stage
	 * (the same family as PBRWater::ShoreShapeAt and ShoreHump, which the breaker's curl is drawn over).
	 *  - shoaling (to 0.5): a sinusoid narrows into a peaked crest over long flat troughs, the crest moving towards the
	 *    front of the wave as its front face steepens (cnoidal, then solitary, as Ursell's number grows);
	 *  - breaking (0.5 to 0.72): the crest runs ahead of the wave, to where a plunger's lip comes down and the splash-up
	 *    rises (Galvin 1969; Peregrine 1983), and loses height;
	 *  - the bore (from 0.58, formed by 0.80): the hump turns into a turbulent front with a long sloping back (Svendsen
	 *    1984's roller, about a wave height long), a good deal lower than the wave that broke.
	 * The crest only ever moves forward. (The bore used to be a second hump faded in ahead of the sinking crest, while the
	 * forward displacement that had carried the crest towards it was taken back: the crest stood still, sank, and a new
	 * one rose 0.16 of a wavelength ahead.) The overturning lip itself is no part of the table: a surface drawn from a
	 * vertex every metre cannot hold it. Where the mesh can, the shaders turn the crest over (PBRWater::ShoreCurlShape).
	 */
	Shape Evaluate(float x, float v)
	{
		const float pi = std::numbers::pi_v<float>;
		const float s = Smoothstep(0.0f, 0.5f, v);
		const float b = Smoothstep(0.58f, 0.80f, v);
		const float crest = 0.5f - 0.18f * s - (0.32f - BoreFront) * Smoothstep(0.50f, 0.72f, v);
		const float front = (0.5f * (1.0f - s) + 0.075f * s) * (1.0f - b) + 0.105f * b;
		const float back = 0.5f * (1.0f - s) + 0.30f * s;
		const float power = 1.0f + 1.2f * s;
		const float xi = x - crest;

		// Either face falls from the crest: a raised cosine narrowed towards it while the wave is whole; as a bore a
		// smooth ramp ahead (a slope of about 30 degrees, not a step) and a long back that has gone by the next wave.
		float hump;
		if (xi < 0.0f) {
			const float a = std::clamp(-xi / front, 0.0f, 1.0f);
			const float wave = std::pow(0.5f * (1.0f + std::cos(pi * a)), power);
			const float bore = 1.0f - a * a * (3.0f - 2.0f * a);
			hump = wave + (bore - wave) * b;
		} else {
			const float wave = std::pow(0.5f * (1.0f + std::cos(pi * std::clamp(xi / back, 0.0f, 1.0f))), power);
			const float bore = std::exp(-xi / 0.34f) * (1.0f - Smoothstep(0.82f, 1.0f, x));
			hump = wave + (bore - wave) * b;
		}

		// White water: the crown of the crest as it breaks, then a line riding the bore's front and trailing off quickly
		// along its back. The bore keeps it to the waterline, where the surf simulation carries its water on.
		const float crown = Smoothstep(0.46f, 0.56f, v) * std::exp(-(xi / 0.06f) * (xi / 0.06f));
		const float line = xi < 0.0f ? Smoothstep(-(2.0f / 3.0f) * front, 0.0f, xi) : std::exp(-xi / 0.09f);

		Shape o;
		o.up = (1.0f - 0.42f * Smoothstep(0.58f, 0.72f, v)) * hump;
		// The crest leans forward as it steepens; the bore carries a little of its water with it.
		o.forward = (0.10f * Smoothstep(0.25f, 0.55f, v) * (1.0f - b) + 0.05f * b) * hump;
		o.foam = std::max((1.0f - b) * crown, b * line);
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
			// The steep front face carries its water forward faster than the surface ahead of it moves, and past a point
			// the surface folds over itself. The mesh carries the wave at a vertex every metre or so, so a fold draws as a
			// flat top over a vertical wall (a step with right-angled edges) that jumps between vertices as it moves. The
			// forward displacement is at most a wavelength (ShoreWaves), so 1 + d(forward)/d(phase) bounds the surface's
			// stretch along the wave: each stage is scaled so it never falls below MinStretch. The shaders' own copy of
			// the shape (PBRWater::ShoreCurlBackground) takes the same scale from the table.
			float steepest = 0.0f;
			for (uint32_t i = 1; i < W; ++i)
				steepest = std::min(steepest, (row[i].forward - row[i - 1].forward) / dx);
			float scale = 1.0f;
			if (1.0f + steepest < MinStretch) {
				scale = (1.0f - MinStretch) / -steepest;
				for (auto& point : row)
					point.forward *= scale;
			}
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
				s1[2] = scale;
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
