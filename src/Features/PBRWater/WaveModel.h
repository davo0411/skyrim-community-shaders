#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

/**
 * @file WaveModel.h
 * @brief CPU side of PBR Water's FFT ocean.
 *
 * The open water is a sum of four FFT cascades (Tessendorf 2001): periodic tiles of 1000, 162, 26.3 and
 * 4.27 m, each carrying one band of a wind (JONSWAP) plus swell spectrum with directional spreading. The
 * tiles are anchored in world space; each band belongs to exactly one cascade, and the tile ratios are
 * not integers, so the sum never visibly repeats.
 *
 * The GPU (OceanSimulation, OceanSpectrumCS.hlsl) synthesises every cascade each frame. Gameplay needs the
 * same surface without reading anything back, so OceanMirror runs the long-wave cascades again on the CPU
 * from the *same* random coefficients (a hash of the wavenumber index, see OceanSpectrum.hlsli) at
 * 20 Hz on a worker, and WaveSnapshot interpolates between those frames. What the mirror leaves out is the
 * smallest cascade (waves shorter than 0.7 m, ~1 cm rms).
 *
 * Frequencies are quantised to multiples of 2 pi / LoopPeriod, so the whole sea repeats exactly every
 * LoopPeriod seconds and the GPU only ever sees time as a fraction of that period.
 */
namespace PBRWaterModel
{
	inline constexpr double Gravity = 9.81;        // m/s^2
	inline constexpr double UnitsPerMetre = 70.0;  // matches METRES_TO_UNITS in Common/Game.hlsli
	inline constexpr double MetresPerUnit = 1.0 / UnitsPerMetre;
	inline constexpr double TwoPi = 6.283185307179586;

	inline constexpr uint32_t NumCascades = 4;
	/// Tile size of each cascade (m). Ratios of ~6.17 keep every band at 7+ texels per wavelength.
	inline constexpr std::array<double, NumCascades> CascadeLength = { 1000.0, 162.0, 26.3, 4.27 };
	/// A cascade carries wavenumbers from this many of its own lattice steps up; shorter waves belong to the next one.
	inline constexpr double CascadeBandStart = 6.0;
	/// The sea repeats exactly after this many seconds (Tessendorf's frequency quantisation).
	inline constexpr double LoopPeriod = 1024.0;

	/// Cascades the CPU mirror reproduces for gameplay (all but the smallest).
	inline constexpr uint32_t MirroredCascades = 3;
	/// Mirror grid size; every mirrored band fits in |index| <= MirrorBandIndex with 3.5+ samples per wavelength.
	inline constexpr uint32_t MirrorSize = 128;
	inline constexpr int32_t MirrorBandIndex = 40;
	/// Mirror frame interval (s); frames are interpolated in between.
	inline constexpr double MirrorStep = 1.0 / 20.0;

	/** @brief Inputs of the spectrum. Everything is physical; nothing is per-location. */
	struct SpectrumParams
	{
		float windSpeed = 8.0f;       ///< m/s at 10 m above the sea
		float windDirection = 0.0f;   ///< radians, direction the wind blows towards
		float heightScale = 1.0f;     ///< artistic scale on the wind sea
		float choppiness = 1.0f;      ///< horizontal displacement scale (1 = linear theory)
		float spread = 0.6f;          ///< directional spread, 0 = all waves aligned with the wind
		float swellHeight = 0.0f;     ///< significant height of the swell (m)
		float swellPeriod = 10.0f;    ///< peak period of the swell (s)
		float swellDirection = 0.0f;  ///< radians, direction the swell travels towards
		uint32_t resolution = 256;    ///< GPU FFT size; sets the shortest wave of the smallest cascade
	};

	/** @brief One cascade's share of the spectrum (open sea). */
	struct CascadeBand
	{
		float length = 1.0f;           ///< tile size (m)
		float kLow = 0.0f;             ///< band start (rad/m)
		float kHigh = 0.0f;            ///< band end (rad/m)
		float kFadeStart = 0.0f;       ///< anti-aliasing fade towards the tile's Nyquist (rad/m), smallest cascade only
		float variance = 0.0f;         ///< height variance (m^2)
		float meanSquareSlope = 0.0f;  ///< slope variance
		float meanOmega = 1.0f;        ///< energy-weighted angular frequency (rad/s)
		float meanK = 0.0f;            ///< energy-weighted wavenumber (rad/m)
		float pmWeightOpenSea = 1.0f;  ///< Pierson-Moskowitz low-frequency weight of meanOmega at unlimited fetch
	};

	/**
	 * @brief The sea state of one frame: everything the GPU spectrum shader and the CPU mirror need to build
	 * the same surface, plus statistics. Amplitude() is mirrored by WaveAmplitude() in OceanSpectrum.hlsli.
	 */
	struct OceanSpectrum
	{
		float windSpeed = 8.0f;  ///< m/s
		float windDirX = 1.0f, windDirY = 0.0f;
		float peakOmega = 1.0f;  ///< fully developed (Pierson-Moskowitz) peak, rad/s
		float heightScale = 1.0f;
		float spreadPeak = 8.0f;  ///< Mitsuyasu spreading exponent at the peak
		float choppiness = 1.0f;  ///< horizontal displacement scale actually used (folding-limited)
		float swellAlpha = 0.0f;  ///< swell spectrum level, normalised to the swell height
		float swellPeak = 0.6f;   ///< rad/s
		float swellDirX = 1.0f, swellDirY = 0.0f;
		uint32_t resolution = 256;

		std::array<CascadeBand, NumCascades> cascades{};
		float significantHeight = 0.0f;  ///< Hs in units (open sea)
		float meanSquareSlope = 0.0f;    ///< all cascades
		float displacementBound = 0.0f;  ///< units; no displacement component exceeds this (open sea)
		float whitecapCoverage = 0.0f;   ///< fraction of the sea covered by whitecaps (Monahan 1980)
		bool calm = true;                ///< nothing worth displacing

		/** @brief Amplitude (m) of the wave travelling along (kx, ky) rad/m on a lattice of spacing dk, before the random coefficient. */
		float Amplitude(float kx, float ky, float dk, uint32_t cascade) const;
	};

	/** @brief Builds the sea state for the given wind and swell. */
	OceanSpectrum GenerateSpectrum(const SpectrumParams& params);

	/** @brief Deterministic standard complex Gaussian for lattice index (m, n) of a cascade. Mirrors OceanSpectrum.hlsli. */
	void OceanNoise(int32_t m, int32_t n, uint32_t cascade, float& re, float& im);
	/** @brief Quantised angular frequency of wavenumber k (rad/m), as a multiple of 2 pi / LoopPeriod. Mirrors the shader. */
	float DispersionSteps(float k);

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

	/** @brief Local fetch-limited JONSWAP peak (rad/s), never below the open-sea peak. Mirrors the shader. */
	float FetchPeakOmega(float fetchMetres, float windSpeed, float openSeaPeak);
	/** @brief Fetch-limited significant height relative to the open sea (JONSWAP growth law). */
	float FetchHeightRatio(float fetchMetres, float windSpeed);
	/** @brief Amplitude gain of the fetch-limited spectrum (JONSWAP vs Pierson-Moskowitz alpha). Mirrors the shader. */
	float FetchEnergyGain(float fetchMetres, float windSpeed);

	/** @brief One instant of the CPU copy of the mirrored cascades. */
	struct MirrorFrame
	{
		double time = 0.0;  ///< wave time (s)
		struct Grid
		{
			// Lagrangian displacement (units) and its velocity (units/s), row-major MirrorSize^2.
			std::vector<float> dx, dy, dz, vx, vy, vz;
		};
		std::array<Grid, MirroredCascades> grids;
	};

	/** @brief Synthesises the mirrored cascades at `time` (any thread). */
	std::shared_ptr<const MirrorFrame> ComputeMirrorFrame(const OceanSpectrum& spectrum, double time);

	/**
	 * @brief Keeps CPU frames of the mirrored cascades around the current wave time, computed one step ahead
	 * on a worker thread. Owned by the main thread.
	 */
	class OceanMirror
	{
	public:
		/** @brief Makes the frames bracketing `time` available (blocks only if they were not computed ahead). */
		void Update(const std::shared_ptr<const OceanSpectrum>& spectrum, double time);
		void Reset();

		std::shared_ptr<const MirrorFrame> frame0;
		std::shared_ptr<const MirrorFrame> frame1;
		float alpha = 0.0f;  ///< interpolation from frame0 to frame1
	};

	/**
	 * @brief Immutable, thread-safe description of the surface at one instant. Published by the main
	 * thread, read by the render thread (to fill the GPU constants) and by gameplay hooks.
	 */
	struct WaveSnapshot
	{
		std::shared_ptr<const OceanSpectrum> ocean;
		std::shared_ptr<const MirrorFrame> mirror0;
		std::shared_ptr<const MirrorFrame> mirror1;
		float mirrorAlpha = 0.0f;
		ShoreParams shore;
		double time = 0.0;  ///< wave time (s), drives the GPU spectrum
		double shorePhase = 0.0;
		double shorePhasePrev = 0.0;
		double refX = 0.0, refY = 0.0, refZ = 0.0;  ///< reference camera (absolute units)
		float windDirection = 0.0f;
		bool exterior = true;
		float foamDrift[4]{};          ///< xy foam drift, zw extra bubble drift (units, wrapped)
		float foamDriftVelocity[2]{};  ///< units/s

		std::shared_ptr<const FetchField> fetch;
		std::shared_ptr<const Bathymetry> bathymetry;
		std::shared_ptr<const Bathymetry> land;  ///< exact terrain of the loaded cells; preferred where it covers

		struct Sample
		{
			float height = 0.0f;            ///< vertical displacement at the queried (displaced) point
			float verticalVelocity = 0.0f;  ///< units/s
			float velocityX = 0.0f;         ///< horizontal orbital velocity, units/s
			float velocityY = 0.0f;
			double originX = 0.0;  ///< rest (Lagrangian) position of the water now at the queried point
			double originY = 0.0;
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
		 * Inverts the horizontal displacement with a short fixed-point iteration, so the returned height
		 * is the surface directly above (x, y), as seen in game.
		 */
		Sample SampleAt(double x, double y, float waterZ) const;

		/** @brief Largest vertical reach of the waves (units), shoaling and fetch gain included. */
		float VerticalBound() const;

	private:
		struct Context
		{
			std::array<float, NumCascades> weight;  ///< vertical amplitude multiplier per cascade
			float horizontalScale;                  ///< horizontal motion relative to vertical (energy gain taken out)
			float fetchRatio;
			float depth;
			float gradX, gradY;
			bool hasTerrain;
		};
		Context BuildContext(double x, double y, float waterZ) const;
		Displacement Evaluate(double x, double y, const Context& ctx) const;
	};

	/**
	 * @brief Wave time and the shoreline train's phase. Owned by the main thread.
	 */
	class WaveClock
	{
	public:
		/** @brief Advances by `dt` seconds of game time with the camera now at (camX, camY, camZ). */
		void Advance(const ShoreParams& shore, double dt, double camX, double camY, double camZ);
		void Reset() { initialised = false; }
		double Time() const { return time; }

		void Fill(WaveSnapshot& snapshot) const;

	private:
		double time = 0.0;
		double shorePhase = 0.0;
		double shorePhasePrev = 0.0;
		double camX = 0.0, camY = 0.0, camZ = 0.0;
		bool initialised = false;
	};
}
