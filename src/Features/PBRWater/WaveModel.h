#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
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
	inline constexpr double Pi = std::numbers::pi;
	inline constexpr double TwoPi = 2.0 * std::numbers::pi;
	inline constexpr double CellSize = 4096.0;  // units per exterior cell

	inline constexpr uint32_t NumCascades = 4;
	/// Tile size of each cascade (m). Ratios of ~6.17 keep every band at 7+ texels per wavelength.
	inline constexpr std::array<double, NumCascades> CascadeLength = { 1000.0, 162.0, 26.3, 4.27 };
	/// A cascade carries wavenumbers from this many of its own lattice steps up; shorter waves belong to the next one.
	inline constexpr double CascadeBandStart = 6.0;
	/// The sea repeats exactly after this many seconds (Tessendorf's frequency quantisation).
	inline constexpr double LoopPeriod = 1024.0;

	/// Cascades the CPU mirror reproduces for gameplay (all but the smallest).
	inline constexpr uint32_t MirroredCascades = 3;
	/// Cascades whose crests whiten: all but the smallest. Its waves (shorter than 0.7 m) only break on the microscale,
	/// without entraining air (Banner & Phillips 1974; Jessup et al. 1997), so they make no whitecaps.
	inline constexpr uint32_t FoamCascades = 3;
	/// Whitecaps break on the crests: the surface's height over its local standard deviation from which a compressed
	/// crest can break, and from which it fully can (a smoothstep between them). Mirrors PBRWater::WhitecapCrest.
	inline constexpr float WhitecapCrestLow = 0.8f;
	inline constexpr float WhitecapCrestHigh = 1.8f;
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
		float shoreScale = 1.0f;      ///< scale on the height of the sea arriving at the shore (shore wave train)
		float swellPeriod = 10.0f;    ///< peak period of the swell (s)
		float swellDirection = 0.0f;  ///< radians, direction the swell travels towards
		uint32_t resolution = 256;    ///< GPU FFT size; sets the shortest wave of the smallest cascade
		float foamAmount = 1.0f;      ///< multiplier on the measured whitecap coverage
	};

	/**
	 * @brief Where the open sea is breaking: the smallest eigenvalue of the foam Jacobian below `threshold`.
	 *
	 * Tessendorf (2001, sec. 4.6) shows that the horizontal displacement folds the surface where the Jacobian of
	 * x -> x + D(x) turns negative, its smallest eigenvalue first; whitecaps are taken where that eigenvalue drops below
	 * a threshold (Tessendorf, Reinhard & Gao 2023, after Crest and others). The threshold is set from the wind as in
	 * their Algorithm 1: the whitecap coverage measured by Monahan & O'Muircheartaigh (1980), W = 3.84e-6 U^3.41, is the
	 * crests breaking now plus the foam they leave behind, so only W (1 - g) / (1 - g W) of the sea breaks at any
	 * instant (g = e^-0.15), and the threshold is that quantile of the eigenvalue over the sea.
	 *
	 * Monahan's W depends on the local wind, not on how big the waves have grown, so the shaders apply the threshold
	 * relative to the local spread of the Jacobian: the sea under the same wind breaks as much in a fetch-limited bay or
	 * over the shoals (where the shore wave train carries the long waves) as in the open sea.
	 */
	struct Whitecaps
	{
		float threshold = -1.0f;  ///< smallest eigenvalue below which a crest of the open sea is breaking (-1: never)
		float width = 0.1f;       ///< how far below the threshold the foam source reaches its full strength
		float spread = 0.0f;      ///< standard deviation of the Jacobian's trace (the divergence of D) over the open sea
		float eigenRatio = 1.0f;  ///< the smallest eigenvalue's standard deviation per spread of the trace
		/// Threshold for a footprint that averages many crests, as a deficit below 1 in spreads of the trace: the share of
		/// a Gaussian eigenvalue (of the measured spread) past it is the breaking share (Dupuy & Bruneton 2012).
		float averagedDeficit = 1.0e3f;
		float activeShare = 0.0f;  ///< share of the sea breaking at any instant
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
		float windSeaHeight = 0.0f;      ///< Hs of the wind sea alone in units (open sea, unlimited fetch)
		float swellHeight = 0.0f;        ///< Hs of the swell alone in units
		float shoreScale = 1.0f;         ///< scale on the sea's height where it reaches the shore
		float meanSquareSlope = 0.0f;    ///< all cascades
		float displacementBound = 0.0f;  ///< units; no displacement component exceeds this (open sea)
		float whitecapCoverage = 0.0f;   ///< fraction of the sea covered by whitecaps (Monahan 1980), times the foam amount
		Whitecaps whitecaps;             ///< where the crests break (ComputeWhitecaps)
		/// Joint covariance of the foam Jacobian's entries (dDx/dx, dDy/dy, dDx/dy; foam cascades only) and the height h
		/// (all cascades, m) over the open sea: xx, yy, shear (= cov(xx, yy)), cov(xx, shear), cov(yy, shear), var(h),
		/// cov(h, xx), cov(h, yy), cov(h, shear).
		std::array<float, 9> foamDeformation{};
		bool calm = true;  ///< nothing worth displacing

		/** @brief Amplitude (m) of the wave travelling along (kx, ky) rad/m on a lattice of spacing dk, before the random coefficient. */
		float Amplitude(float kx, float ky, float dk, uint32_t cascade) const;
	};

	/** @brief Builds the sea state for the given wind and swell. */
	OceanSpectrum GenerateSpectrum(const SpectrumParams& params);

	/**
	 * @brief The whitecap threshold for a sea whose foam Jacobian entries and height have the joint covariance
	 * `deformation` (OceanSpectrum::foamDeformation) and whose whitecap coverage is `coverage` (Whitecaps).
	 *
	 * The entries and the height are linear in the Gaussian wave amplitudes, so they are jointly Gaussian (the height is
	 * high where the surface compresses: crests sharpen); the smallest eigenvalue is not, and its lower tail matters
	 * (Tessendorf et al. 2023 measure it on the simulated surface). It is sampled from a fixed set of Gaussian draws, so
	 * the threshold is deterministic and moves smoothly with the sea state, and it is set for the breaking the shaders
	 * draw: the compression past the threshold on a crest (WhitecapCrestLow/High).
	 */
	Whitecaps ComputeWhitecaps(const std::array<float, 9>& deformation, float coverage);

	/** @brief Deterministic standard complex Gaussian for lattice index (m, n) of a cascade. Mirrors OceanSpectrum.hlsli. */
	void OceanNoise(int32_t m, int32_t n, uint32_t cascade, float& re, float& im);
	/** @brief Quantised angular frequency of wavenumber k (rad/m), as a multiple of 2 pi / LoopPeriod. Mirrors the shader. */
	float DispersionSteps(float k);

	/// The rms height of a random sea in the surf zone stays below 0.42 h (Thornton & Guza 1982; the shore train's
	/// regular waves carry the rms height). Mirrored by PBRWater::BreakerIndex.
	inline constexpr float BreakerIndex = 0.42f;
	/// Height of the regular waves that carry a random sea's energy: Hrms = Hs / sqrt(2). Mirrored by PBRWater::ShoreHeightRatio.
	inline constexpr float ShoreHeightRatio = 0.7071f;
	/// The shore waves' horizontal orbits are held to k ax <= this, so the surface never folds. Mirrors PBRWater::ShoreOrbitLimit.
	inline constexpr float ShoreOrbitLimit = 0.7f;

	/**
	 * @brief Shoreline waves: the wave train that runs up the coastline map (ShoreField) and the swash it throws up the
	 * beach, after Fluid Flux 2's coastline waves (see PBRWater::ShoreWaves). All of it is derived from the sea state
	 * (UpdateShoreParams): the dominant sea, swell or wind sea, sets the period and the heights; the coastline map sets
	 * where the crests are and how they slow down, steepen and break.
	 */
	struct ShoreParams
	{
		float omega = 1.0f;           ///< angular frequency of the dominant waves (rad/s), eased: the clock's rate
		float swellDirection = 0.0f;  ///< direction the swell travels towards (rad): sets which shores it reaches
		float swellHeight = 0.0f;     ///< significant height of the swell arriving from the open sea (units)
		float windSeaHeight = 0.0f;   ///< significant height of the open-sea wind sea (units), before the local fetch limit
		float period = 10.0f;         ///< peak period 2 pi / omega (s)
		float deepLength = 1.0f;      ///< deep-water wavelength g T^2 / (2 pi) (units)
		float maxRunup = 0.0f;        ///< how far above the still water the swash can reach (units), for bounds
		float variation = 0.5f;       ///< 0..1 how much the crests' timing varies along the coast (setting)
		float variationTime = 0.0f;   ///< clock of the slow drift of that variation (s, wrapped daily; Shore3.y)
		/// Wave clock (rad, wrapped): integrated frame by frame (phase += omega dt), so a changing period never moves the crests.
		double phase = 0.0;
		double groupTime = 0.0;  ///< clock of the wave groups (s, wrapped at an hour; Shore0.z)
		bool enabled = false;

		/** @brief Largest open-sea height the train can bring to a shore (units): swell and wind sea at full fetch. */
		float MaxDeepHeight() const { return std::sqrt(swellHeight * swellHeight + windSeaHeight * windSeaHeight); }
	};

	/**
	 * @brief The shore wave train for this frame's sea state (`enabled`, `variation` and `variationTime` are left for
	 * the caller to set).
	 *
	 * The dominant sea (swell or wind sea, whichever is higher) sets the period; the run-up bound follows Stockdon et
	 * al. (2006). The period eases towards the sea state's over a few seconds and the clock is integrated
	 * (phase += omega dt), so a changing sea never makes the waves jump.
	 * @param previous last frame's parameters (nullptr on the first frame)
	 * @param dt       seconds of wave time since then
	 */
	ShoreParams UpdateShoreParams(const OceanSpectrum& spectrum, const ShoreParams* previous, double dt);

	/** @brief Depth-limited breaking: the significant height of random waves in the surf zone stays below this share of the depth (Thornton & Guza 1983). */
	inline constexpr float SurfHeightRatio = 0.6f;

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

	/**
	 * @brief Wavenumber (rad/m) of waves of angular frequency `omega` (rad/s) in water `depth` metres deep: Eckart
	 * (1952), k = k0 / sqrt(tanh(k0 h)), within a few percent of omega^2 = g k tanh(k h). Mirrors the shader.
	 */
	float Wavenumber(float omega, float depth);
	/** @brief Phase speed (m/s) of waves of angular frequency `omega` (rad/s) in water `depth` metres deep. */
	float PhaseSpeed(float omega, float depth);
	/**
	 * @brief Shoaling coefficient sqrt(cg0 / cg) (energy flux conservation, Green's law in shallow water) of waves
	 * of angular frequency `omega` (rad/s) in water `depth` metres deep. Mirrors the shader.
	 */
	float ShoalingCoefficient(float omega, float depth);

	struct FetchField;

	/** @brief Terrain, water level and the waves to trace around the player, input of BuildShoreField. */
	struct ShoreInputs
	{
		uint32_t width = 0, height = 0;
		float minX = 0.0f, minY = 0.0f;                            ///< world position of texel (0, 0)'s centre
		float step = 128.0f;                                       ///< units per texel
		std::vector<float> bed;                                    ///< absolute terrain z (NaN: unknown, taken as deep water)
		std::vector<float> level;                                  ///< absolute still water z (NaN: no water in reach)
		float omega = 1.0f;                                        ///< angular frequency of the shore waves (rad/s)
		float swellDirection = 0.0f;                               ///< direction the swell travels towards (rad), for the exposure
		std::shared_ptr<const FetchField> fetch;                   ///< open water upwind (null: every shore is open to the sea)
		float fetchScale = 1.0f;                                   ///< multiplies the baked fetch (Skyrim's compressed distances)
		float seaLevel = std::numeric_limits<float>::quiet_NaN();  ///< the worldspace's sea level (absolute z; NaN: unknown): only water at it sees the swell
	};

	/**
	 * @brief The coastline map around the player (CPU, worker thread; uploaded to the GPU): Fluid Flux 2's coastline map
	 * (distance to the coast, direction, intensity), built from the real terrain and water levels instead of baked.
	 *
	 * The shore waves' phase runs with the time tau they take to reach the waterline: tau solves the eikonal
	 * |grad tau| = 1 / c(h) (fast sweeping, Zhao 2005) from the waterline outwards, with c the waves' phase speed in the
	 * local depth, so the crests run parallel to the shore, slow down and close up over the shoals, turn towards the depth
	 * contours and wrap into bays. grad tau gives the direction they travel in (down it) and, times omega, their
	 * wavenumber. Where fronts from different stretches of shore meet the room drops and the open sea carries the waves
	 * (Fluid Flux's coastline mask), as it does where the water is too narrow for them. The signed distance to the
	 * waterline finds the nearest point of the coast, the beach slope there decides between breaking and surging, and the
	 * exposure says how open the water is to the swell. Up the beach the direction, exposure and room of the waterline the
	 * inland ray leaves from are carried, for the swash.
	 */
	struct ShoreField
	{
		uint32_t width = 0, height = 0;
		float minX = 0.0f, minY = 0.0f, step = 128.0f;
		float omega = 1.0f;             ///< angular frequency the travel times were traced for (rad/s)
		float swellDirection = 0.0f;    ///< direction the exposure was measured along (rad)
		std::vector<float> travelTime;  ///< time tau to the waterline (s); up the beach the waterline's (~0)
		std::vector<float> distance;    ///< signed distance to the waterline (units): > 0 in the water, < 0 up the beach
		std::vector<float> slownessX;   ///< grad tau (s/m, seaward): the waves travel down it, |grad tau| = 1 / c; up the beach the waterline's
		std::vector<float> slownessY;
		std::vector<float> slope;     ///< beach slope (tan) at the waterline, carried out along the rays and up the beach
		std::vector<float> exposure;  ///< 0..1 how open the water is to the swell (open water upwind along its direction)
		std::vector<float> room;      ///< 0..1 room for the wave train: the water's width against the wavelength, and coherent fronts
		std::vector<float> level;     ///< absolute still water z (NaN: no water in reach)

		bool Valid() const { return width > 1 && height > 1 && level.size() == static_cast<size_t>(width) * height && travelTime.size() == level.size(); }

		struct Sample
		{
			float travelTime = 0.0f;  ///< s
			float phaseLag = 0.0f;    ///< omega tau (rad) at the frequency the field was traced for; blended from the field being replaced (WaveSnapshot::ShoreAt)
			float distance = 0.0f;    ///< units
			float slownessX = 0.0f;   ///< s/m
			float slownessY = 0.0f;
			float slope = 1.0f;
			float exposure = 0.0f;
			float room = 0.0f;
			float weight = 0.0f;  ///< 0 outside the field (and fading out towards its border)
		};
		/** @brief Bilinear sample at absolute (x, y). */
		Sample At(float x, float y) const;
	};

	/** @brief Builds the shore field. `cancel` (optional) aborts the build when set. */
	std::shared_ptr<ShoreField> BuildShoreField(const ShoreInputs& inputs, const std::atomic_bool* cancel = nullptr);

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

	/** @brief The shore wave train's local, time-independent inputs at one point (as PBRWater::ShoreWaves gets them). */
	struct ShoreTrainContext
	{
		float weight = 0.0f;      ///< hand-over from the FFT, 0..1 (0: no train)
		float deepHeight = 0.0f;  ///< deep-water significant height H0 arriving here (m)
		float depth = 0.0f;       ///< still water depth (m)
		float shoaling = 1.0f;    ///< shoaling coefficient Ks of the dominant waves
		float beach = 0.0f;       ///< 0..1 the waves break on this shore rather than surge against it
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
		float fetchScale = 1.0f;  ///< multiplies the baked fetch (Skyrim's compressed distances)
		ShoreParams shore;
		std::shared_ptr<const ShoreField> shoreField;     ///< the coastline map around the player
		std::shared_ptr<const ShoreField> shorePrevious;  ///< the map it replaces, while shoreBlend < 1
		float shoreBlend = 1.0f;                          ///< 0..1 from shorePrevious's travel times to shoreField's
		double time = 0.0;                                ///< wave time (s), drives the GPU spectrum
		double refX = 0.0, refY = 0.0, refZ = 0.0;        ///< reference camera (absolute units)
		float windDirection = 0.0f;
		bool exterior = true;
		float bubbleDrift[2]{};        ///< drift of the bubbles under the surface (units, wrapped)
		float foamDriftVelocity[2]{};  ///< surface drift that carries the wakes' churn (units/s)

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

		/** @brief Largest vertical reach of the waves (units), shoaling, fetch gain, shore train and run-up included. */
		float VerticalBound() const;

	private:
		struct Context
		{
			std::array<float, NumCascades> weight;  ///< vertical amplitude multiplier per cascade (fetch, shoaling, breaking, train hand-over)
			float horizontalScale;                  ///< horizontal motion relative to vertical (energy gain taken out)
			float fetchRatio;
			float depth;  ///< units
			bool hasTerrain;
			ShoreTrainContext train;  ///< the shore wave train here
		};
		Context BuildContext(double x, double y, float waterZ) const;
		Displacement Evaluate(double x, double y, const Context& ctx) const;
		/** @brief The coastline map at (x, y), travel times blended from the map being replaced as the shaders do. */
		ShoreField::Sample ShoreAt(float x, float y) const;
	};

	/**
	 * @brief Wave time and the reference camera. Owned by the main thread.
	 */
	class WaveClock
	{
	public:
		/** @brief Advances by `dt` seconds of game time with the camera now at (camX, camY, camZ). */
		void Advance(double dt, double camX, double camY, double camZ);
		double Time() const { return time; }

		void Fill(WaveSnapshot& snapshot) const;

	private:
		double time = 0.0;
		double camX = 0.0, camY = 0.0, camZ = 0.0;
	};
}
