#pragma once

#include <cstdint>
#include <vector>

/**
 * @file FoamPattern.h
 * @brief The tileable foam texture PBR Water draws all of its foam with (CPU bake, no game dependencies).
 *
 * Foam is a coverage (how much of the surface is foam) turned into a pattern by thresholding a foam texture at
 * 1 - coverage, the "black point fade" of Crest (wave-harmonic/crest, MIT, OceanFoam.hlsl): as the coverage falls the
 * texture's lowest values drop out first, so dense foam thins into lace and then into specks instead of fading
 * uniformly. The texture is baked once at start-up instead of shipped as an asset:
 *  - R: the foam itself. Bubbles of random size on a jittered grid, merged with a smooth minimum, two sizes, walls
 *    bent by a domain warp, density varied by large-scale clumps. Each texel stores the coverage at which it turns
 *    white, histogram-equalised, so a coverage C whitens a share C of the texture: thresholding is exact on average
 *    and the mip-mapped texture converges to the coverage far away.
 *  - G: bubble specks for the aerated water just under the surface (Crest's bubble foam layer), equalised alike.
 * Every noise is periodic over the tile, so the texture wraps seamlessly.
 */
namespace PBRWaterFoam
{
	/// Texels per side of the foam texture.
	inline constexpr uint32_t TextureSize = 512;
	/// Large foam bubbles per side of the tile: the Foam Pattern Size setting is the size of one of them.
	inline constexpr uint32_t BubblesPerTile = 12;

	/** @brief One mip level, RG8 texels (R foam, G bubbles), row-major. */
	struct Level
	{
		uint32_t size = 0;
		std::vector<uint8_t> texels;  ///< 2 bytes per texel
	};

	/** @brief Bakes the foam texture and its full mip chain (box filtered). Deterministic. */
	std::vector<Level> Bake(uint32_t size = TextureSize);
}
