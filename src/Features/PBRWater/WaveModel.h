#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @file WaveModel.h
 * @brief CPU side of PBR Water's analytic wave model.
 *
 * The ocean is a sum of Gerstner waves whose amplitudes follow a JONSWAP spectrum driven by the
 * current wind. The exact same function is evaluated by the GPU (features/PBR Water/Shaders/PBRWater/
 * PBRWater.hlsli) and by WaveSnapshot::Sample() here, so gameplay (swimming, buoyancy) agrees with
 * what is rendered without reading anything back from the GPU.
 *
 * Phases are integrated per wave *at the camera* in double precision:
 *     psi_i += -omega_i * dt + k_i * dot(d_i, deltaCamera)
 * and the GPU adds k_i * dot(d_i, x - camera). This keeps the surface continuous while the wind
 * changes the spectrum, freezes it while the game is paused, and avoids large-coordinate float error.
 */
namespace PBRWaterModel
{
	inline constexpr uint32_t MaxWaves = 16;
	inline constexpr double Gravity = 9.81;        // m/s^2
	inline constexpr double UnitsPerMetre = 70.0;  // matches METRES_TO_UNITS in Common/Game.hlsli
	inline constexpr double MetresPerUnit = 1.0 / UnitsPerMetre;
	inline constexpr double TwoPi = 6.283185307179586;

	/** @brief Inputs of the spectrum. Everything is physical; nothing is per-location. */
	struct SpectrumParams
	{
		float windSpeed = 8.0f;           ///< m/s at 10 m above the sea
		float windDirection = 0.0f;       ///< radians, direction the wind blows towards
		float heightScale = 1.0f;         ///< artistic scale on the spectrum amplitude
		float choppiness = 0.8f;          ///< total Gerstner steepness budget, 0..1
		float spread = 0.6f;              ///< directional spread, 0 = all waves aligned with the wind
		float shortestWavelength = 1.0f;  ///< metres; shorter detail is left to the normal maps
	};

	/** @brief One spectral component. Units: game units, seconds, radians. */
	struct Wave
	{
		float dirX = 1.0f;
		float dirY = 0.0f;
		float k = 0.0f;                ///< wavenumber (rad/unit)
		float omega = 0.0f;            ///< angular frequency (rad/s)
		float amplitude = 0.0f;        ///< units
		float steepness = 0.0f;        ///< Gerstner Q
		float wavelength = 0.0f;       ///< units
		float pmWeightOpenSea = 1.0f;  ///< Pierson-Moskowitz low-frequency weight at unlimited fetch
	};

	struct Spectrum
	{
		std::array<Wave, MaxWaves> waves{};
		uint32_t count = 0;
		float peakOmega = 1.0f;                     ///< rad/s, fully developed sea
		float windSpeed = 8.0f;                     ///< m/s
		float amplitudeSum = 0.0f;                  ///< units, upper bound of the vertical displacement
		float shortestDisplacedWavelength = 70.0f;  ///< units, shortest component that matters for geometry
		float significantHeight = 0.0f;             ///< Hs in units (open sea)
		float whitecapCoverage = 0.0f;              ///< fraction of the sea covered by whitecaps (Monahan 1980)
	};

	/** @brief Builds a JONSWAP-weighted set of Gerstner waves for the given wind. */
	Spectrum GenerateSpectrum(const SpectrumParams& params);

	/** @brief Shoreline wave train parameters derived from the spectrum. */
	struct ShoreParams
	{
		float amplitude = 0.0f;   ///< units at the onset depth (open sea, before fetch scaling)
		float omega = 1.0f;       ///< rad/s
		float slope = 0.04f;      ///< nominal beach slope
		float onsetDepth = 0.0f;  ///< units; the train starts where it feels the bottom
		float steepness = 0.6f;
	};

	/** @brief Bathymetry used by the CPU evaluator (downsampled copy of the terrain heightmap). */
	struct Bathymetry
	{
		std::vector<float> heights;  ///< absolute terrain z, row-major
		uint32_t width = 0;
		uint32_t height = 0;
		float minX = 0.0f, minY = 0.0f;    ///< world position of texel (0, 0) centre
		float stepX = 1.0f, stepY = 1.0f;  ///< world units per texel (stepY may be negative)

		bool Valid() const { return width > 1 && height > 1 && !heights.empty(); }
		/** @brief True when (x, y) lies inside the sampled grid (no edge clamping needed). */
		bool Contains(float x, float y) const;
		float Sample(float x, float y) const;
	};

	/** @brief Upwind open-water distance per cell and direction, baked per worldspace. */
	struct FetchField
	{
		static constexpr uint32_t Directions = 16;
		std::vector<float> kilometres;  ///< [direction][y][x]
		int32_t minCellX = 0, minCellY = 0;
		uint32_t width = 0, height = 0;
		float openSeaKm = 500.0f;

		bool Valid() const { return width > 0 && height > 0 && !kilometres.empty(); }
		/** @brief Bilinear fetch in metres for wind blowing towards `windDirection`. */
		float SampleMetres(float x, float y, float windDirection) const;
	};

	/**
	 * @brief Immutable, thread-safe description of the surface at one instant. Published by the main
	 * thread, read by the render thread (to fill the GPU constants) and by gameplay hooks.
	 */
	struct WaveSnapshot
	{
		Spectrum spectrum;
		ShoreParams shore;
		std::array<double, MaxWaves> phase{};      ///< psi_i at the reference camera (wrapped)
		std::array<double, MaxWaves> phasePrev{};  ///< psi_i one frame earlier, at the same camera
		double shorePhase = 0.0;
		double shorePhasePrev = 0.0;
		double refX = 0.0, refY = 0.0, refZ = 0.0;  ///< reference camera
		float windDirection = 0.0f;
		bool exterior = true;
		float foamDrift[4]{};  ///< xy foam drift, zw extra bubble drift (units, wrapped)

		std::shared_ptr<const FetchField> fetch;
		std::shared_ptr<const Bathymetry> bathymetry;
		std::shared_ptr<const Bathymetry> land;  ///< exact terrain of the loaded cells; preferred where it covers

		struct Sample
		{
			float height = 0.0f;            ///< vertical displacement at the queried (displaced) point
			float verticalVelocity = 0.0f;  ///< units/s
			float velocityX = 0.0f;         ///< horizontal orbital velocity, units/s
			float velocityY = 0.0f;
		};

		/** @brief Motion of one water parcel (units, units/s). */
		struct Displacement
		{
			float dx = 0.0f, dy = 0.0f, dz = 0.0f;
			float vx = 0.0f, vy = 0.0f, vz = 0.0f;
		};

		/**
		 * @brief Displacement of the water parcels whose rest positions are (xs[i], ys[i]) above a flat plane
		 * at `waterZ`. A floating hull rides the parcels under it, so no inversion is needed. The spectrum
		 * weighting (fetch, depth) is evaluated once, at the first point.
		 */
		void ParcelDisplacements(const double* xs, const double* ys, size_t count, float waterZ, Displacement* out) const;

		/**
		 * @brief Surface displacement at world position (x, y) above a flat water plane at `waterZ`.
		 * Inverts the Gerstner horizontal displacement with a short fixed-point iteration, so the
		 * returned height is the surface directly above (x, y), as seen in game.
		 */
		Sample SampleAt(double x, double y, float waterZ) const;

	private:
		struct Context
		{
			std::array<float, MaxWaves> amplitude;  ///< per-wave amplitude after fetch and depth weighting
			float energyGain;                       ///< FetchEnergyGain at this point
			float fetchRatio;
			float depth;
			float gradX, gradY;
			bool hasTerrain;
		};
		Context BuildContext(double x, double y, float waterZ) const;
		void Evaluate(double x, double y, const Context& ctx, float& dx, float& dy, float& dz, float& vz, float& vx, float& vy) const;
	};

	/** @brief Local fetch-limited JONSWAP peak (rad/s), never below the open-sea peak. Mirrors the shader. */
	float FetchPeakOmega(float fetchMetres, float windSpeed, float openSeaPeak);
	/** @brief Fetch-limited significant height relative to the open sea (JONSWAP growth law). */
	float FetchHeightRatio(float fetchMetres, float windSpeed);
	/** @brief Amplitude gain of the fetch-limited spectrum (JONSWAP vs Pierson-Moskowitz alpha). Mirrors the shader. */
	float FetchEnergyGain(float fetchMetres, float windSpeed);

	/**
	 * @brief Integrates the per-wave phases. Owned by the main thread.
	 */
	class PhaseIntegrator
	{
	public:
		/** @brief Advances by `dt` seconds of game time with the camera now at (camX, camY). */
		void Advance(const Spectrum& spectrum, const ShoreParams& shore, double dt, double camX, double camY, double camZ);
		void Reset() { initialised = false; }

		void Fill(WaveSnapshot& snapshot) const;

	private:
		std::array<double, MaxWaves> phase{};
		std::array<double, MaxWaves> phasePrev{};
		double shorePhase = 0.0;
		double shorePhasePrev = 0.0;
		double camX = 0.0, camY = 0.0, camZ = 0.0;
		bool initialised = false;
	};
}
