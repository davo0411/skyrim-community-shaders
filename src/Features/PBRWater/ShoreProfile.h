#pragma once

#include <cstdint>
#include <vector>

/**
 * @file ShoreProfile.h
 * @brief The shape of a shore wave through its life (CPU bake, no game dependencies).
 *
 * Fluid Flux 2 draws its coastline waves from a canned profile animation: a texture indexed by the position across the
 * wave and by the wave's life, holding how far the water at that point is carried forward and up, and its foam. The same
 * idea, computed at start-up: a wave's whole shape comes from one lookup, so it can roll, pitch over its own front and
 * collapse into a bore, which no height field or sum of orbits does. Both the shaders and the CPU evaluator read this
 * table, so swimming and floating objects ride the same wave.
 *
 * The phase runs over one wave: 0 at the trough ahead of it, 1 at the trough behind. The surface and its displacement
 * vanish at both, so consecutive waves in different stages of their lives meet without a step. The stage runs from
 * 0 (a sinusoid in deep water) through 0.5 (the wave reaches its breaking limit) to 1 (the bore reaching the shore).
 */
namespace PBRWaterModel
{
	/// The wave at one phase and stage.
	struct ShoreProfilePoint
	{
		float forward = 0.0f;   ///< displacement along the direction of travel, in wavelengths
		float up = 0.0f;        ///< surface above the trough, in wave heights
		float dForward = 0.0f;  ///< d forward / d phase
		float dUp = 0.0f;       ///< d up / d phase
		float foam = 0.0f;      ///< 0..1 white water carried by the wave
		float mean = 0.0f;      ///< mean of `up` over the wave at this stage
	};

	/** @brief The table, in texture layout: Phases x Stages texels of two float4 slices, row-major. */
	struct ShoreProfileTable
	{
		static constexpr uint32_t Phases = 256;
		static constexpr uint32_t Stages = 64;

		std::vector<float> slice0;  ///< forward, up, dForward, dUp
		std::vector<float> slice1;  ///< foam, mean, unused, unused

		/** @brief Bilinear lookup, clamped: mirrors PBRWater::ShoreProfileAt. */
		ShoreProfilePoint At(float phase, float stage) const;
	};

	/** @brief The baked table, computed on first use and immutable after that. */
	const ShoreProfileTable& GetShoreProfile();
}
