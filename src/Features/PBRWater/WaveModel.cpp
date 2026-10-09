#include "WaveModel.h"

#include <algorithm>
#include <cmath>

namespace PBRWaterModel
{
	namespace
	{
		constexpr float Pi = 3.14159265358979f;

		double Wrap(double phase)
		{
			phase = std::fmod(phase, TwoPi);
			return phase < 0.0 ? phase + TwoPi : phase;
		}

		/// Deterministic value in [-1, 1] per wave index; keeps the sea pattern stable across sessions.
		float Hash(uint32_t i)
		{
			uint32_t h = i * 0x9E3779B1u + 0x7F4A7C15u;
			h ^= h >> 16;
			h *= 0x85EBCA6Bu;
			h ^= h >> 13;
			h *= 0xC2B2AE35u;
			h ^= h >> 16;
			return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF) * 2.0f - 1.0f;
		}

		/// JONSWAP spectral density S(omega) in m^2 s (Hasselmann et al. 1973), PM alpha, gamma = 3.3.
		double Jonswap(double omega, double peak)
		{
			constexpr double alpha = 0.0081;
			constexpr double gamma = 3.3;
			const double sigma = omega <= peak ? 0.07 : 0.09;
			const double r = std::exp(-(omega - peak) * (omega - peak) / (2.0 * sigma * sigma * peak * peak));
			const double pm = alpha * Gravity * Gravity / std::pow(omega, 5.0) * std::exp(-1.25 * std::pow(peak / omega, 4.0));
			return pm * std::pow(gamma, r);
		}
	}

	float FetchPeakOmega(float fetchMetres, float windSpeed, float openSeaPeak)
	{
		const float U = std::max(windSpeed, 0.5f);
		const float F = std::max(fetchMetres, 10.0f);
		const float omega = 22.0f * std::pow(static_cast<float>(Gravity * Gravity) / (U * F), 1.0f / 3.0f);
		return std::max(omega, openSeaPeak);
	}

	float FetchHeightRatio(float fetchMetres, float windSpeed)
	{
		// Hs(F) = 0.0016 sqrt(g F) U / g  versus  Hs(inf) = 0.21 U^2 / g
		const float U = std::max(windSpeed, 0.5f);
		return std::clamp(0.0016f / 0.21f * std::sqrt(static_cast<float>(Gravity) * std::max(fetchMetres, 0.0f)) / U, 0.0f, 1.0f);
	}

	float FetchEnergyGain(float fetchMetres, float windSpeed)
	{
		// JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against the Pierson-Moskowitz 0.0081 of the open sea.
		const float U = std::max(windSpeed, 0.5f);
		const float chi = static_cast<float>(Gravity) * std::max(fetchMetres, 10.0f) / (U * U);
		const float alpha = 0.076f * std::pow(chi, -0.22f);
		return std::sqrt(std::clamp(alpha / 0.0081f, 1.0f, 4.0f));
	}

	Spectrum GenerateSpectrum(const SpectrumParams& params)
	{
		Spectrum s;
		const double U = std::max(static_cast<double>(params.windSpeed), 0.5);
		s.windSpeed = static_cast<float>(U);
		const double peak = 0.877 * Gravity / U;  // Pierson-Moskowitz fully developed peak
		s.peakOmega = static_cast<float>(peak);

		// Frequency band: from just below the peak up to the shortest wavelength the geometry should carry.
		const double kMax = TwoPi / std::max(static_cast<double>(params.shortestWavelength), 0.1);
		const double omegaMax = std::max(std::sqrt(Gravity * kMax), peak * 2.5);
		const double omegaMin = peak * 0.7;
		const double ratio = std::pow(omegaMax / omegaMin, 1.0 / (MaxWaves - 1));

		float maxAmplitude = 0.0f;
		double variance = 0.0;

		for (uint32_t i = 0; i < MaxWaves; ++i) {
			const double omega = omegaMin * std::pow(ratio, i);
			const double bandwidth = omega * (std::sqrt(ratio) - 1.0 / std::sqrt(ratio));
			const double density = Jonswap(omega, peak);
			const double amplitudeMetres = std::sqrt(2.0 * density * bandwidth) * params.heightScale;
			variance += density * bandwidth;

			Wave& w = s.waves[i];
			const double kMetres = omega * omega / Gravity;  // deep water dispersion
			w.k = static_cast<float>(kMetres * MetresPerUnit);
			w.omega = static_cast<float>(omega);
			w.amplitude = static_cast<float>(amplitudeMetres * UnitsPerMetre);
			w.wavelength = static_cast<float>(TwoPi / kMetres * UnitsPerMetre);
			w.pmWeightOpenSea = static_cast<float>(std::exp(-1.25 * std::pow(peak / omega, 4.0)));

			// Short waves follow the wind less closely than the dominant swell.
			const float t = static_cast<float>(i) / (MaxWaves - 1);
			const float spread = params.spread * (0.35f + 0.65f * t) * (Pi * 0.5f);
			const float angle = params.windDirection + Hash(i) * spread;
			w.dirX = std::cos(angle);
			w.dirY = std::sin(angle);

			maxAmplitude = std::max(maxAmplitude, w.amplitude);
			s.amplitudeSum += w.amplitude;
		}

		// Steepness: share the choppiness budget so that sum(Q k A) stays below 1 (no loops), and never
		// let a weak component move further sideways than up (Q <= 1).
		const float budget = std::clamp(params.choppiness, 0.0f, 0.95f) / MaxWaves;
		for (uint32_t i = 0; i < MaxWaves; ++i) {
			Wave& w = s.waves[i];
			const float kA = w.k * w.amplitude;
			w.steepness = kA > 1e-6f ? std::min(budget / kA, 1.0f) : 0.0f;
		}

		s.shortestDisplacedWavelength = s.waves[0].wavelength;
		for (uint32_t i = 0; i < MaxWaves; ++i) {
			if (s.waves[i].amplitude >= 0.02f * maxAmplitude)
				s.shortestDisplacedWavelength = std::min(s.shortestDisplacedWavelength, s.waves[i].wavelength);
		}

		s.count = MaxWaves;
		s.significantHeight = static_cast<float>(4.0 * std::sqrt(variance) * params.heightScale * UnitsPerMetre);
		s.whitecapCoverage = std::clamp(3.84e-6f * std::pow(static_cast<float>(U), 3.41f), 0.0f, 1.0f);
		return s;
	}

	float Bathymetry::Sample(float x, float y) const
	{
		const float fx = std::clamp((x - minX) / stepX, 0.0f, static_cast<float>(width - 1));
		const float fy = std::clamp((y - minY) / stepY, 0.0f, static_cast<float>(height - 1));
		const uint32_t x0 = std::min(static_cast<uint32_t>(fx), width - 2);
		const uint32_t y0 = std::min(static_cast<uint32_t>(fy), height - 2);
		const float tx = fx - x0;
		const float ty = fy - y0;
		const float* row0 = &heights[y0 * width];
		const float* row1 = row0 + width;
		const float a = row0[x0] + (row0[x0 + 1] - row0[x0]) * tx;
		const float b = row1[x0] + (row1[x0 + 1] - row1[x0]) * tx;
		return a + (b - a) * ty;
	}

	float FetchField::SampleMetres(float x, float y, float windDirection) const
	{
		if (!Valid())
			return openSeaKm * 1000.0f;

		constexpr float cellSize = 4096.0f;
		const float cx = x / cellSize - minCellX - 0.5f;
		const float cy = y / cellSize - minCellY - 0.5f;
		if (cx < -0.5f || cy < -0.5f || cx > width - 0.5f || cy > height - 0.5f)
			return openSeaKm * 1000.0f;

		float slice = static_cast<float>(Wrap(windDirection) / TwoPi) * Directions;
		const uint32_t s0 = static_cast<uint32_t>(slice) % Directions;
		const uint32_t s1 = (s0 + 1) % Directions;
		const float ts = slice - std::floor(slice);

		auto fetchAt = [&](uint32_t s) {
			const float fx = std::clamp(cx, 0.0f, static_cast<float>(width - 1));
			const float fy = std::clamp(cy, 0.0f, static_cast<float>(height - 1));
			const uint32_t x0 = std::min(static_cast<uint32_t>(fx), width > 1 ? width - 2 : 0);
			const uint32_t y0 = std::min(static_cast<uint32_t>(fy), height > 1 ? height - 2 : 0);
			const uint32_t x1 = std::min(x0 + 1, width - 1);
			const uint32_t y1 = std::min(y0 + 1, height - 1);
			const float tx = fx - x0;
			const float ty = fy - y0;
			const float* base = &kilometres[static_cast<size_t>(s) * width * height];
			const float a = base[y0 * width + x0] + (base[y0 * width + x1] - base[y0 * width + x0]) * tx;
			const float b = base[y1 * width + x0] + (base[y1 * width + x1] - base[y1 * width + x0]) * tx;
			return a + (b - a) * ty;
		};

		return (fetchAt(s0) + (fetchAt(s1) - fetchAt(s0)) * ts) * 1000.0f;
	}

	WaveSnapshot::Context WaveSnapshot::BuildContext(double x, double y, float waterZ) const
	{
		Context ctx{};
		const float fetchMetres = fetch ? fetch->SampleMetres(static_cast<float>(x), static_cast<float>(y), windDirection) : (exterior ? 500000.0f : 50.0f);
		const float peakOmega = FetchPeakOmega(fetchMetres, spectrum.windSpeed, spectrum.peakOmega);
		ctx.fetchRatio = FetchHeightRatio(fetchMetres, spectrum.windSpeed);
		ctx.energyGain = FetchEnergyGain(fetchMetres, spectrum.windSpeed);
		ctx.depth = 1e6f;
		ctx.hasTerrain = false;

		if (bathymetry && bathymetry->Valid()) {
			const float fx = static_cast<float>(x);
			const float fy = static_cast<float>(y);
			const float step = std::abs(bathymetry->stepX);
			const float z0 = bathymetry->Sample(fx, fy);
			ctx.depth = std::max(waterZ - z0, 0.0f);
			ctx.gradX = (bathymetry->Sample(fx + step, fy) - z0) / step;
			ctx.gradY = (bathymetry->Sample(fx, fy + step) - z0) / step;
			ctx.hasTerrain = true;
		}

		// Same weighting as PBRWater::WaveWeight in the shader (minus the render-only distance/Nyquist fades).
		for (uint32_t i = 0; i < spectrum.count; ++i) {
			const Wave& w = spectrum.waves[i];
			const float r = peakOmega / w.omega;
			float weight = std::clamp(std::exp(-1.25f * r * r * r * r) / std::max(w.pmWeightOpenSea, 1e-4f), 0.0f, 1.0f) * ctx.energyGain;
			weight *= std::tanh(w.k * ctx.depth);
			ctx.amplitude[i] = w.amplitude * weight;
		}
		return ctx;
	}

	void WaveSnapshot::Evaluate(double x, double y, const Context& ctx, float& dx, float& dy, float& dz, float& vz, float& vx, float& vy) const
	{
		dx = dy = dz = vz = vx = vy = 0.0f;
		const double px = x - refX;
		const double py = y - refY;

		for (uint32_t i = 0; i < spectrum.count; ++i) {
			const Wave& w = spectrum.waves[i];
			const float A = ctx.amplitude[i];
			if (A <= 0.0f)
				continue;

			const double theta = w.k * (w.dirX * px + w.dirY * py) + phase[i];
			const float s = static_cast<float>(std::sin(theta));
			const float c = static_cast<float>(std::cos(theta));
			// The energy gain only raises the surface; the steepness budget holds for the open-sea amplitudes.
			const float QA = w.steepness * A / ctx.energyGain;
			dx += w.dirX * QA * c;
			dy += w.dirY * QA * c;
			dz += A * s;
			// d/dt of theta is -omega
			vz += -w.omega * A * c;
			vx += w.dirX * QA * w.omega * s;
			vy += w.dirY * QA * w.omega * s;
		}

		if (ctx.hasTerrain && shore.amplitude > 0.0f && ctx.depth < shore.onsetDepth) {
			const float gradLen = std::sqrt(ctx.gradX * ctx.gradX + ctx.gradY * ctx.gradY);
			if (gradLen > 1e-4f) {
				const float h = std::max(ctx.depth, 1.0f);
				const float onset = shore.onsetDepth;
				float A = shore.amplitude * ctx.fetchRatio * std::sqrt(std::sqrt(onset / h));
				A = std::min(A, 0.39f * h);
				auto smooth = [](float e0, float e1, float v) {
					const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
					return t * t * (3.0f - 2.0f * t);
				};
				A *= smooth(onset, onset * 0.5f, h) * smooth(0.05f * static_cast<float>(UnitsPerMetre), 0.25f * static_cast<float>(UnitsPerMetre), h);
				const float hm = std::max(h * static_cast<float>(MetresPerUnit), 0.02f);
				const float travel = 2.0f * std::sqrt(hm) / (std::max(shore.slope, 0.005f) * std::sqrt(static_cast<float>(Gravity)));
				const double theta = -shore.omega * travel - shorePhase;
				dz += A * static_cast<float>(std::sin(theta));
				vz += -shore.omega * A * static_cast<float>(std::cos(theta));
			}
		}
	}

	WaveSnapshot::Sample WaveSnapshot::SampleAt(double x, double y, float waterZ) const
	{
		Sample out;
		if (spectrum.amplitudeSum + shore.amplitude <= 0.5f)
			return out;
		const Context ctx = BuildContext(x, y, waterZ);

		// Find the undisplaced point whose Gerstner orbit currently sits above (x, y).
		double x0 = x, y0 = y;
		float dx, dy, dz, vz, vx, vy;
		for (int it = 0; it < 2; ++it) {
			Evaluate(x0, y0, ctx, dx, dy, dz, vz, vx, vy);
			x0 = x - dx;
			y0 = y - dy;
		}
		Evaluate(x0, y0, ctx, dx, dy, dz, vz, vx, vy);

		out.height = dz;
		out.verticalVelocity = vz;
		out.velocityX = vx;
		out.velocityY = vy;
		return out;
	}

	void PhaseIntegrator::Advance(const Spectrum& spectrum, const ShoreParams& shore, double dt, double x, double y, double z)
	{
		if (!initialised) {
			for (uint32_t i = 0; i < MaxWaves; ++i)
				phase[i] = Wrap(Hash(i + 101) * TwoPi);
			shorePhase = 0.0;
			camX = x;
			camY = y;
			camZ = z;
			initialised = true;
		}

		const double moveX = x - camX;
		const double moveY = y - camY;
		for (uint32_t i = 0; i < spectrum.count; ++i) {
			const Wave& w = spectrum.waves[i];
			// Phase at the new camera position, at the current (pre-step) time ...
			const double moved = phase[i] + w.k * (w.dirX * moveX + w.dirY * moveY);
			// ... is where the previous frame's surface was; then step time forward.
			phasePrev[i] = Wrap(moved);
			phase[i] = Wrap(moved - w.omega * dt);
		}
		shorePhasePrev = shorePhase;
		shorePhase = Wrap(shorePhase + shore.omega * dt);
		camX = x;
		camY = y;
		camZ = z;
	}

	void PhaseIntegrator::Fill(WaveSnapshot& snapshot) const
	{
		snapshot.phase = phase;
		snapshot.phasePrev = phasePrev;
		snapshot.shorePhase = shorePhase;
		snapshot.shorePhasePrev = shorePhasePrev;
		snapshot.refX = camX;
		snapshot.refY = camY;
		snapshot.refZ = camZ;
	}
}
