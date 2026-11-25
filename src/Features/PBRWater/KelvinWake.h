#pragma once

/**
 * @brief Kelvin wakes: the wave pattern a body moving through deep water leaves behind it (the chevron of divergent waves
 * inside +-19.47 degrees, and the transverse waves across it), for the actors swimming or wading through the water.
 *
 * The pattern is the far-field Kelvin integral for a moving pressure point (Lamb, Hydrodynamics, 1932; Havelock 1908),
 * evaluated once in dimensionless coordinates (k0 x, k0 y with k0 = g / U^2) into a texture: height and its two slopes.
 * Each source scales it by its own speed (the wavelength of the transverse waves is 2 pi U^2 / g) and lays it behind
 * itself along its heading (PBRWater.hlsli KelvinWakes): the vertices take the height, the pixels the slopes.
 */
namespace PBRWaterWake
{
	inline constexpr float Length = 30.0f;  ///< the pattern's reach behind the source (in 1 / k0): about five transverse waves
	inline constexpr float Width = 15.0f;   ///< and to either side (the wedge's 19.47 degrees reaches k0 y = 10.6 at the end)
	inline constexpr uint32_t MaxSources = 8;

	/** @brief One wake this frame. */
	struct Source
	{
		float x = 0.0f;  ///< absolute position (units)
		float y = 0.0f;
		float dirX = 1.0f;  ///< heading
		float dirY = 0.0f;
		float speed = 0.0f;      ///< units/s
		float amplitude = 0.0f;  ///< the pattern's peak height (units)
	};

	/** @brief Evaluates the pattern (a few hundred milliseconds once) into an R32G32B32A32 texture: height, d/dX, d/dY. */
	winrt::com_ptr<ID3D11ShaderResourceView> CreatePattern();
}
