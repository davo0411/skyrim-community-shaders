#include "WaveModel.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace PBRWaterModel
{
	namespace
	{
		constexpr float G = static_cast<float>(Gravity);
		constexpr float Omega0 = static_cast<float>(TwoPi / LoopPeriod);
		constexpr float JonswapAlpha = 0.0081f;  // Pierson-Moskowitz Phillips constant (open sea)
		constexpr float JonswapGamma = 3.3f;
		constexpr float SwellGamma = 7.0f;    // narrow-banded, long-travelled swell
		constexpr float SwellSpread = 30.0f;  // Mitsuyasu exponent of the swell: long, parallel crests
		constexpr float BandMargin = 0.9999f;

		double Wrap(double phase)
		{
			phase = std::fmod(phase, TwoPi);
			return phase < 0.0 ? phase + TwoPi : phase;
		}

		float Smoothstep(float e0, float e1, float v)
		{
			const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// ---- Shared with OceanSpectrum.hlsli: keep the two in sync ----

		uint32_t HashU(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352du;
			x ^= x >> 15;
			x *= 0x846ca68bu;
			x ^= x >> 16;
			return x;
		}

		/// ln(Gamma(x)) for x >= 1 (Stirling series after shifting x above 7).
		float LogGamma(float x)
		{
			float product = 1.0f;
			for (int i = 0; i < 6 && x < 7.0f; ++i) {
				product *= x;
				x += 1.0f;
			}
			const float z = 1.0f / (x * x);
			return (x - 0.5f) * std::log(x) - x + 0.9189385f + (0.0833333f - z * (0.00277778f - z * 0.000793651f)) / x - std::log(product);
		}

		/// Normalisation of the cos^2s(theta/2) spreading function over [-pi, pi] (Longuet-Higgins 1963).
		float SpreadingNorm(float s)
		{
			return std::exp((2.0f * s - 1.0f) * 0.6931472f - 1.1447299f + 2.0f * LogGamma(s + 1.0f) - LogGamma(2.0f * s + 1.0f));
		}

		/// D(theta) = Q(s) cos^2s(theta / 2), written with cos(theta) = cosAngle.
		float Spreading(float cosAngle, float s)
		{
			return SpreadingNorm(s) * std::pow(std::max(0.5f + 0.5f * cosAngle, 0.0f), s);
		}

		/// Mitsuyasu et al. (1975) spreading exponent: narrowest at the spectral peak, wider away from it.
		float MitsuyasuExponent(float omega, float peak, float spreadPeak)
		{
			const float x = omega / peak;
			const float s = x <= 1.0f ? spreadPeak * x * x * x * x * x : spreadPeak * std::pow(x, -2.5f);
			return std::clamp(s, 1.0f, 60.0f);
		}

		/// JONSWAP spectral density S(omega) in m^2 s (Hasselmann et al. 1973).
		float Jonswap(float omega, float peak, float alpha, float gamma)
		{
			const float sigma = omega <= peak ? 0.07f : 0.09f;
			const float d = omega - peak;
			const float r = std::exp(-d * d / (2.0f * sigma * sigma * peak * peak));
			const float p = peak / omega;
			const float p2 = p * p;
			const float o2 = omega * omega;
			return alpha * G * G / (o2 * o2 * omega) * std::exp(-1.25f * p2 * p2) * std::pow(gamma, r);
		}

		// ---- end of shared code ----

		/// Height spectrum along wavenumber magnitude k (rad/m), directions integrated: S(omega) d(omega)/dk.
		float BandDensity(const OceanSpectrum& s, float k)
		{
			const float omega = std::sqrt(G * k);
			float density = Jonswap(omega, s.peakOmega, JonswapAlpha, JonswapGamma) * s.heightScale * s.heightScale;
			if (s.swellAlpha > 0.0f)
				density += Jonswap(omega, s.swellPeak, s.swellAlpha, SwellGamma);
			return density * G / (2.0f * omega);
		}

		// ---- CPU FFT (same Stockham radix-2 kernel as OceanFFTCS.hlsl) ----

		struct Complex
		{
			float re, im;
		};

		Complex Mul(Complex a, Complex b) { return { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re }; }
		Complex Add(Complex a, Complex b) { return { a.re + b.re, a.im + b.im }; }
		Complex Sub(Complex a, Complex b) { return { a.re - b.re, a.im - b.im }; }
		Complex Scale(Complex a, float s) { return { a.re * s, a.im * s }; }
		Complex Conj(Complex a) { return { a.re, -a.im }; }

		/// Unnormalised inverse DFT x_j = sum X_m e^(2 pi i m j / N), N = MirrorSize, natural order in and out.
		class InverseFFT
		{
		public:
			InverseFFT()
			{
				for (uint32_t j = 0; j < MirrorSize / 2; ++j) {
					const double a = TwoPi * j / MirrorSize;
					twiddle[j] = { static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)) };
				}
			}

			void Run(Complex* data, Complex* scratch) const
			{
				constexpr uint32_t half = MirrorSize / 2;
				Complex* a = data;
				Complex* b = scratch;
				for (uint32_t p = 1; p < MirrorSize; p <<= 1) {
					const uint32_t twiddleStride = half / p;  // exp(i pi k / p) = twiddle[k * N / (2p)]
					for (uint32_t t = 0; t < half; ++t) {
						const uint32_t k = t & (p - 1);
						const Complex u0 = a[t];
						const Complex u1 = Mul(a[t + half], twiddle[k * twiddleStride]);
						const uint32_t j = (t << 1) - k;
						b[j] = Add(u0, u1);
						b[j + p] = Sub(u0, u1);
					}
					std::swap(a, b);
				}
				if (a != data)
					std::copy(a, a + MirrorSize, data);
			}

		private:
			std::array<Complex, MirrorSize / 2> twiddle{};
		};

		const InverseFFT& GetInverseFFT()
		{
			static const InverseFFT fft;
			return fft;
		}

		/// 2D inverse transform in place; only rows |n| <= MirrorBandIndex can be non-zero in the spectrum.
		void InverseFFT2D(std::vector<Complex>& grid)
		{
			const auto& fft = GetInverseFFT();
			std::array<Complex, MirrorSize> line{};
			std::array<Complex, MirrorSize> scratch{};
			for (int32_t n = -MirrorBandIndex; n <= MirrorBandIndex; ++n) {
				Complex* row = &grid[static_cast<size_t>((n + MirrorSize) % MirrorSize) * MirrorSize];
				fft.Run(row, scratch.data());
			}
			for (uint32_t x = 0; x < MirrorSize; ++x) {
				for (uint32_t y = 0; y < MirrorSize; ++y)
					line[y] = grid[static_cast<size_t>(y) * MirrorSize + x];
				fft.Run(line.data(), scratch.data());
				for (uint32_t y = 0; y < MirrorSize; ++y)
					grid[static_cast<size_t>(y) * MirrorSize + x] = line[y];
			}
		}

		/// Catmull-Rom taps of a periodic grid along one axis.
		struct Taps
		{
			uint32_t index[4];
			float weight[4];
		};

		Taps CubicTaps(double texel)
		{
			const double base = std::floor(texel);
			const float t = static_cast<float>(texel - base);
			const int64_t i0 = static_cast<int64_t>(base);
			Taps taps;
			for (int i = 0; i < 4; ++i) {
				const int64_t idx = (i0 - 1 + i) % static_cast<int64_t>(MirrorSize);
				taps.index[i] = static_cast<uint32_t>(idx < 0 ? idx + MirrorSize : idx);
			}
			const float t2 = t * t;
			const float t3 = t2 * t;
			taps.weight[0] = 0.5f * (-t3 + 2.0f * t2 - t);
			taps.weight[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
			taps.weight[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
			taps.weight[3] = 0.5f * (t3 - t2);
			return taps;
		}

		/// Accumulates one mirror grid sampled at absolute (x, y) units, scaled by the given weights.
		void SampleMirror(const MirrorFrame::Grid& grid, uint32_t cascade, double x, double y, float vertical, float horizontal, WaveSnapshot::Displacement& out)
		{
			const double texelsPerUnit = MirrorSize / (CascadeLength[cascade] * UnitsPerMetre);
			const Taps tx = CubicTaps(x * texelsPerUnit);
			const Taps ty = CubicTaps(y * texelsPerUnit);
			float dx = 0.0f, dy = 0.0f, dz = 0.0f, vx = 0.0f, vy = 0.0f, vz = 0.0f;
			for (int j = 0; j < 4; ++j) {
				const size_t row = static_cast<size_t>(ty.index[j]) * MirrorSize;
				for (int i = 0; i < 4; ++i) {
					const float w = ty.weight[j] * tx.weight[i];
					const size_t idx = row + tx.index[i];
					dx += grid.dx[idx] * w;
					dy += grid.dy[idx] * w;
					dz += grid.dz[idx] * w;
					vx += grid.vx[idx] * w;
					vy += grid.vy[idx] * w;
					vz += grid.vz[idx] * w;
				}
			}
			out.dx += dx * horizontal;
			out.dy += dy * horizontal;
			out.dz += dz * vertical;
			out.vx += vx * horizontal;
			out.vy += vy * horizontal;
			out.vz += vz * vertical;
		}

		/**
		 * Computes mirror frames ahead of time on one worker thread. Deliberately never destroyed: joining or
		 * destroying a worker during DLL unload, after the process has already killed it, can hang the game.
		 */
		class MirrorWorker
		{
		public:
			static MirrorWorker& Get()
			{
				static MirrorWorker* instance = new MirrorWorker();
				return *instance;
			}

			void Request(int64_t step, const std::shared_ptr<const OceanSpectrum>& spectrum)
			{
				std::scoped_lock lock(mutex);
				if (done.contains(step) || running.contains(step))
					return;
				for (const auto& job : queue) {
					if (job.step == step)
						return;
				}
				queue.push_back({ step, spectrum });
				signal.notify_all();
			}

			/// The frame for `step`; computes it on the calling thread when the worker has not started it.
			std::shared_ptr<const MirrorFrame> Acquire(int64_t step, const std::shared_ptr<const OceanSpectrum>& spectrum)
			{
				std::unique_lock lock(mutex);
				std::shared_ptr<const OceanSpectrum> source = spectrum;
				for (auto it = queue.begin(); it != queue.end(); ++it) {
					if (it->step == step) {
						source = it->spectrum;
						queue.erase(it);
						break;
					}
				}
				if (running.contains(step))
					signal.wait(lock, [&] { return done.contains(step); });
				if (const auto it = done.find(step); it != done.end())
					return it->second;

				lock.unlock();
				auto frame = SafeCompute(*source, step);
				lock.lock();
				done[step] = frame;
				return frame;
			}

			/// Forgets every frame outside [first, last].
			void Trim(int64_t first, int64_t last)
			{
				std::scoped_lock lock(mutex);
				std::erase_if(done, [&](const auto& entry) { return entry.first < first || entry.first > last; });
				std::erase_if(queue, [&](const Job& job) { return job.step < first || job.step > last; });
			}

		private:
			struct Job
			{
				int64_t step;
				std::shared_ptr<const OceanSpectrum> spectrum;
			};

			MirrorWorker()
			{
				std::thread([this] { Run(); }).detach();
			}

			static std::shared_ptr<const MirrorFrame> SafeCompute(const OceanSpectrum& spectrum, int64_t step)
			{
				try {
					return ComputeMirrorFrame(spectrum, static_cast<double>(step) * MirrorStep);
				} catch (...) {
					return nullptr;
				}
			}

			void Run()
			{
				std::unique_lock lock(mutex);
				for (;;) {
					signal.wait(lock, [&] { return !queue.empty(); });
					Job job = std::move(queue.front());
					queue.pop_front();
					running.insert(job.step);
					lock.unlock();
					auto frame = SafeCompute(*job.spectrum, job.step);
					lock.lock();
					running.erase(job.step);
					done[job.step] = std::move(frame);
					signal.notify_all();
				}
			}

			std::mutex mutex;
			std::condition_variable signal;
			std::deque<Job> queue;
			std::set<int64_t> running;
			std::map<int64_t, std::shared_ptr<const MirrorFrame>> done;
		};
	}

	// ========================================================================
	// Spectrum
	// ========================================================================

	void OceanNoise(int32_t m, int32_t n, uint32_t cascade, float& re, float& im)
	{
		const uint32_t key = (static_cast<uint32_t>(m + 32768) & 0xFFFFu) | ((static_cast<uint32_t>(n + 32768) & 0xFFFFu) << 16);
		const uint32_t h1 = HashU(key ^ (cascade * 0x68E31DA4u));
		const uint32_t h2 = HashU(h1 + 0x9E3779B9u);
		const float u1 = (static_cast<float>(h1 >> 8) + 0.5f) * (1.0f / 16777216.0f);
		const float u2 = static_cast<float>(h2 >> 8) * (1.0f / 16777216.0f);
		// Box-Muller; E|xi|^2 = 1.
		const float r = std::sqrt(-2.0f * std::log(u1)) * 0.70710678f;
		const float a = 6.2831853f * u2;
		re = r * std::cos(a);
		im = r * std::sin(a);
	}

	float DispersionSteps(float k)
	{
		return std::max(std::floor(std::sqrt(G * k) / Omega0 + 0.5f), 1.0f);
	}

	float OceanSpectrum::Amplitude(float kx, float ky, float dk, uint32_t cascade) const
	{
		const CascadeBand& band = cascades[cascade];
		const float k = std::sqrt(kx * kx + ky * ky);
		// Lattice points exactly on a band start (|index| = CascadeBandStart) must land on the same side on
		// the GPU and the CPU whatever the rounding: the margin is far below the gap to the next lattice radius.
		if (k <= 0.0f || k < band.kLow * BandMargin || k >= band.kHigh * BandMargin)
			return 0.0f;
		const float omega = std::sqrt(G * k);
		const float cosWind = (kx * windDirX + ky * windDirY) / k;
		float density = Jonswap(omega, peakOmega, JonswapAlpha, JonswapGamma) * heightScale * heightScale *
		                Spreading(cosWind, MitsuyasuExponent(omega, peakOmega, spreadPeak));
		if (swellAlpha > 0.0f) {
			const float cosSwell = (kx * swellDirX + ky * swellDirY) / k;
			density += Jonswap(omega, swellPeak, swellAlpha, SwellGamma) * Spreading(cosSwell, SwellSpread);
		}
		// E(kx, ky) = S(omega) D(theta) (d omega / dk) / k; each travelling wave holds half of its cell's variance.
		const float energy = density * G / (2.0f * omega * k);
		float amplitude = std::sqrt(std::max(energy, 0.0f) * dk * dk * 0.5f);
		if (band.kFadeStart > 0.0f)
			amplitude *= 1.0f - Smoothstep(band.kFadeStart, band.kHigh, k);
		return amplitude;
	}

	OceanSpectrum GenerateSpectrum(const SpectrumParams& params)
	{
		OceanSpectrum s;
		const float U = std::max(params.windSpeed, 0.5f);
		s.windSpeed = U;
		s.windDirX = std::cos(params.windDirection);
		s.windDirY = std::sin(params.windDirection);
		s.peakOmega = 0.877f * G / U;  // Pierson-Moskowitz fully developed peak
		s.heightScale = std::max(params.heightScale, 0.0f);
		const float narrow = 1.0f - std::clamp(params.spread, 0.0f, 1.0f);
		s.spreadPeak = 2.0f + 38.0f * narrow * narrow;
		s.resolution = std::clamp(params.resolution, 64u, 1024u);

		// Swell: a narrow JONSWAP from a distant storm, scaled to the requested significant height.
		if (params.swellHeight > 0.01f && params.swellPeriod > 1.0f) {
			s.swellPeak = static_cast<float>(TwoPi) / params.swellPeriod;
			s.swellDirX = std::cos(params.swellDirection);
			s.swellDirY = std::sin(params.swellDirection);
			double m0 = 0.0;
			constexpr int Samples = 512;
			const double lo = std::log(0.3 * s.swellPeak), hi = std::log(8.0 * s.swellPeak);
			for (int i = 0; i < Samples; ++i) {
				const double w0 = std::exp(lo + (hi - lo) * i / Samples);
				const double w1 = std::exp(lo + (hi - lo) * (i + 1) / Samples);
				m0 += Jonswap(static_cast<float>(0.5 * (w0 + w1)), s.swellPeak, 1.0f, SwellGamma) * (w1 - w0);
			}
			const double target = params.swellHeight * 0.25;
			s.swellAlpha = m0 > 0.0 ? static_cast<float>(target * target / m0) : 0.0f;
		}

		// Bands: each cascade from CascadeBandStart of its own lattice steps to the start of the next one;
		// the smallest fades out before its Nyquist frequency.
		for (uint32_t c = 0; c < NumCascades; ++c) {
			CascadeBand& band = s.cascades[c];
			band.length = static_cast<float>(CascadeLength[c]);
			const float dk = static_cast<float>(TwoPi / CascadeLength[c]);
			band.kLow = c == 0 ? 0.0f : static_cast<float>(CascadeBandStart) * dk;
			if (c + 1 < NumCascades) {
				band.kHigh = static_cast<float>(CascadeBandStart * TwoPi / CascadeLength[c + 1]);
			} else {
				const float nyquist = 3.14159265f * static_cast<float>(s.resolution) / band.length;
				band.kHigh = 0.85f * nyquist;
				band.kFadeStart = 0.6f * nyquist;
			}
		}

		// Statistics per band (directions integrate to one).
		double totalVariance = 0.0, totalSlope = 0.0;
		for (uint32_t c = 0; c < NumCascades; ++c) {
			CascadeBand& band = s.cascades[c];
			const double dk = TwoPi / CascadeLength[c];
			const double lo = std::log(std::max<double>(band.kLow, 0.5 * dk));
			const double hi = std::log(band.kHigh);
			double variance = 0.0, slope = 0.0, omegaSum = 0.0, kSum = 0.0;
			constexpr int Samples = 256;
			for (int i = 0; i < Samples; ++i) {
				const double k0 = std::exp(lo + (hi - lo) * i / Samples);
				const double k1 = std::exp(lo + (hi - lo) * (i + 1) / Samples);
				const float k = static_cast<float>(0.5 * (k0 + k1));
				double e = BandDensity(s, k) * (k1 - k0);
				if (band.kFadeStart > 0.0f) {
					const float fade = 1.0f - Smoothstep(band.kFadeStart, band.kHigh, k);
					e *= fade * fade;
				}
				variance += e;
				slope += e * k * k;
				omegaSum += e * std::sqrt(G * k);
				kSum += e * k;
			}
			band.variance = static_cast<float>(variance);
			band.meanSquareSlope = static_cast<float>(slope);
			if (variance > 1e-12) {
				band.meanOmega = static_cast<float>(omegaSum / variance);
				band.meanK = static_cast<float>(kSum / variance);
			} else {
				band.meanK = std::sqrt(std::max(band.kLow, static_cast<float>(dk)) * band.kHigh);
				band.meanOmega = std::sqrt(G * band.meanK);
			}
			const float r = s.peakOmega / band.meanOmega;
			band.pmWeightOpenSea = std::exp(-1.25f * r * r * r * r);
			totalVariance += variance;
			totalSlope += slope;
		}

		// Choppiness: keep the surface from folding over itself (Jacobian < 0) more than ~3 sigma out.
		// The divergence of the horizontal displacement has a standard deviation of lambda * sqrt(mss).
		const float slopeStd = static_cast<float>(std::sqrt(totalSlope));
		s.choppiness = std::max(params.choppiness, 0.0f) * std::min(1.0f, slopeStd > 1e-6f ? (1.0f / 3.0f) / slopeStd : 1.0f);

		const float sigma = static_cast<float>(std::sqrt(totalVariance));
		s.meanSquareSlope = static_cast<float>(totalSlope);
		s.significantHeight = 4.0f * sigma * static_cast<float>(UnitsPerMetre);
		s.displacementBound = 5.0f * sigma * std::max(1.0f, s.choppiness) * static_cast<float>(UnitsPerMetre) + 1.0f;
		s.whitecapCoverage = std::clamp(3.84e-6f * std::pow(U, 3.41f), 0.0f, 1.0f);
		s.calm = 4.0f * sigma < 0.01f;
		return s;
	}

	// ========================================================================
	// CPU mirror
	// ========================================================================

	namespace
	{
		/// The time-independent part of a mirror frame: the wave coefficients of every component in the mirrored bands.
		struct MirrorLattice
		{
			struct Point
			{
				uint32_t index;    ///< row-major position in the mirror grid
				float dirX, dirY;  ///< unit wavenumber direction
				float steps;       ///< quantised frequency, multiples of 2 pi / LoopPeriod
				Complex h0;        ///< wave travelling along +k
				Complex h0mConj;   ///< conj of the wave travelling along -k
			};
			std::array<std::vector<Point>, MirroredCascades> points;
			OceanSpectrum source;
		};

		bool Close(float a, float b, float tolerance)
		{
			return std::abs(a - b) <= tolerance * std::max(std::abs(a), std::abs(b)) + 1e-7f;
		}

		/// The sea state changes slowly (the wind is smoothed over seconds), so most frames reuse the coefficients.
		bool SameShape(const OceanSpectrum& a, const OceanSpectrum& b)
		{
			constexpr float t = 1e-3f;
			return Close(a.peakOmega, b.peakOmega, t) && Close(a.heightScale, b.heightScale, t) && Close(a.spreadPeak, b.spreadPeak, t) &&
			       Close(a.swellAlpha, b.swellAlpha, t) && Close(a.swellPeak, b.swellPeak, t) && std::abs(a.windDirX - b.windDirX) < t &&
			       std::abs(a.windDirY - b.windDirY) < t && std::abs(a.swellDirX - b.swellDirX) < t && std::abs(a.swellDirY - b.swellDirY) < t;
		}

		std::unique_ptr<MirrorLattice> BuildLattice(const OceanSpectrum& spectrum)
		{
			auto lattice = std::make_unique<MirrorLattice>();
			lattice->source = spectrum;
			for (uint32_t c = 0; c < MirroredCascades; ++c) {
				auto& points = lattice->points[c];
				const float dk = static_cast<float>(TwoPi / CascadeLength[c]);
				for (int32_t n = -MirrorBandIndex; n <= MirrorBandIndex; ++n) {
					for (int32_t m = -MirrorBandIndex; m <= MirrorBandIndex; ++m) {
						if (m == 0 && n == 0)
							continue;
						const float kx = static_cast<float>(m) * dk;
						const float ky = static_cast<float>(n) * dk;
						const float a = spectrum.Amplitude(kx, ky, dk, c);
						const float am = spectrum.Amplitude(-kx, -ky, dk, c);
						if (a <= 0.0f && am <= 0.0f)
							continue;
						const float k = std::sqrt(kx * kx + ky * ky);
						Complex xi, xim;
						OceanNoise(m, n, c, xi.re, xi.im);
						OceanNoise(-m, -n, c, xim.re, xim.im);
						MirrorLattice::Point point;
						point.index = static_cast<uint32_t>((n + static_cast<int32_t>(MirrorSize)) % MirrorSize) * MirrorSize + static_cast<uint32_t>((m + static_cast<int32_t>(MirrorSize)) % MirrorSize);
						point.dirX = kx / k;
						point.dirY = ky / k;
						point.steps = DispersionSteps(k);
						point.h0 = Scale(xi, a);
						point.h0mConj = Conj(Scale(xim, am));
						points.push_back(point);
					}
				}
			}
			return lattice;
		}
	}

	std::shared_ptr<const MirrorFrame> ComputeMirrorFrame(const OceanSpectrum& spectrum, double time)
	{
		thread_local std::unique_ptr<MirrorLattice> lattice;
		if (!lattice || !SameShape(lattice->source, spectrum))
			lattice = BuildLattice(spectrum);

		auto frame = std::make_shared<MirrorFrame>();
		frame->time = time;
		double tau = std::fmod(time, LoopPeriod) / LoopPeriod;
		if (tau < 0.0)
			tau += 1.0;
		const float lambda = spectrum.choppiness;
		const float units = static_cast<float>(UnitsPerMetre);
		constexpr size_t Count = static_cast<size_t>(MirrorSize) * MirrorSize;

		std::vector<Complex> p0(Count), p1(Count), p2(Count);
		for (uint32_t c = 0; c < MirroredCascades; ++c) {
			std::fill(p0.begin(), p0.end(), Complex{ 0.0f, 0.0f });
			std::fill(p1.begin(), p1.end(), Complex{ 0.0f, 0.0f });
			std::fill(p2.begin(), p2.end(), Complex{ 0.0f, 0.0f });

			for (const auto& point : lattice->points[c]) {
				const double cycles = point.steps * tau;
				const double phase = TwoPi * (cycles - std::floor(cycles));
				const float cp = static_cast<float>(std::cos(phase));
				const float sp = static_cast<float>(std::sin(phase));
				const float omega = point.steps * Omega0;

				// Travelling along +k: h0 e^(-i w t); along -k: conj(h0(-k)) e^(+i w t).
				const Complex forward = Mul(point.h0, { cp, -sp });
				const Complex backward = Mul(point.h0mConj, { cp, sp });
				const Complex H = Add(forward, backward);
				const Complex Gv = Sub(forward, backward);  // dH/dt = -i omega Gv

				// Dx + i Dy = lambda H (i kx - ky) / k
				p0[point.index] = Scale(Mul(H, { -point.dirY, point.dirX }), lambda);
				// Dz + i Vz with Vz = -i omega Gv
				p1[point.index] = Add(H, Scale(Gv, omega));
				// Vx + i Vy = lambda omega Gv (kx + i ky) / k
				p2[point.index] = Scale(Mul(Gv, { point.dirX, point.dirY }), lambda * omega);
			}

			InverseFFT2D(p0);
			InverseFFT2D(p1);
			InverseFFT2D(p2);

			auto& grid = frame->grids[c];
			grid.dx.resize(Count);
			grid.dy.resize(Count);
			grid.dz.resize(Count);
			grid.vx.resize(Count);
			grid.vy.resize(Count);
			grid.vz.resize(Count);
			for (size_t i = 0; i < Count; ++i) {
				grid.dx[i] = p0[i].re * units;
				grid.dy[i] = p0[i].im * units;
				grid.dz[i] = p1[i].re * units;
				grid.vz[i] = p1[i].im * units;
				grid.vx[i] = p2[i].re * units;
				grid.vy[i] = p2[i].im * units;
			}
		}
		return frame;
	}

	void OceanMirror::Update(const std::shared_ptr<const OceanSpectrum>& spectrum, double time)
	{
		if (!spectrum || spectrum->calm) {
			frame0.reset();
			frame1.reset();
			alpha = 0.0f;
			return;
		}
		auto& worker = MirrorWorker::Get();
		const int64_t step = static_cast<int64_t>(std::floor(time / MirrorStep));
		worker.Trim(step, step + 2);
		// The frame after next is computed ahead, so the worker is always one step in front.
		worker.Request(step + 1, spectrum);
		worker.Request(step + 2, spectrum);
		frame0 = worker.Acquire(step, spectrum);
		frame1 = worker.Acquire(step + 1, spectrum);
		alpha = static_cast<float>(std::clamp((time - static_cast<double>(step) * MirrorStep) / MirrorStep, 0.0, 1.0));
		if (!frame0 || !frame1) {
			frame0.reset();
			frame1.reset();
		}
	}

	void OceanMirror::Reset()
	{
		frame0.reset();
		frame1.reset();
		alpha = 0.0f;
	}

	// ========================================================================
	// Environment
	// ========================================================================

	float FetchPeakOmega(float fetchMetres, float windSpeed, float openSeaPeak)
	{
		const float U = std::max(windSpeed, 0.5f);
		const float F = std::max(fetchMetres, 10.0f);
		const float omega = 22.0f * std::pow(G * G / (U * F), 1.0f / 3.0f);
		return std::max(omega, openSeaPeak);
	}

	float FetchHeightRatio(float fetchMetres, float windSpeed)
	{
		// Hs(F) = 0.0016 sqrt(g F) U / g  versus  Hs(inf) = 0.21 U^2 / g
		const float U = std::max(windSpeed, 0.5f);
		return std::clamp(0.0016f / 0.21f * std::sqrt(G * std::max(fetchMetres, 0.0f)) / U, 0.0f, 1.0f);
	}

	float FetchEnergyGain(float fetchMetres, float windSpeed)
	{
		// JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against the Pierson-Moskowitz 0.0081 of the open sea.
		const float U = std::max(windSpeed, 0.5f);
		const float chi = G * std::max(fetchMetres, 10.0f) / (U * U);
		const float alpha = 0.076f * std::pow(chi, -0.22f);
		return std::sqrt(std::clamp(alpha / 0.0081f, 1.0f, 4.0f));
	}

	bool Bathymetry::Contains(float x, float y) const
	{
		if (!Valid())
			return false;
		const float fx = (x - minX) / stepX;
		const float fy = (y - minY) / stepY;
		return fx >= 0.0f && fy >= 0.0f && fx <= static_cast<float>(width - 1) && fy <= static_cast<float>(height - 1);
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

	// ========================================================================
	// Snapshot queries
	// ========================================================================

	float WaveSnapshot::VerticalBound() const
	{
		const float waves = ocean && !ocean->calm ? ocean->displacementBound : 0.0f;
		return 2.0f * waves + 2.0f * shore.amplitude;
	}

	WaveSnapshot::Context WaveSnapshot::BuildContext(double x, double y, float waterZ) const
	{
		Context ctx{};
		const float windSpeed = ocean ? ocean->windSpeed : 0.5f;
		const float openPeak = ocean ? ocean->peakOmega : 1.0f;
		const float fetchMetres = fetch ? fetch->SampleMetres(static_cast<float>(x), static_cast<float>(y), windDirection) : (exterior ? 500000.0f : 50.0f);
		const float peakOmega = FetchPeakOmega(fetchMetres, windSpeed, openPeak);
		const float energyGain = FetchEnergyGain(fetchMetres, windSpeed);
		ctx.fetchRatio = FetchHeightRatio(fetchMetres, windSpeed);
		ctx.horizontalScale = 1.0f / energyGain;
		ctx.depth = 1e6f;
		ctx.hasTerrain = false;

		const float fx = static_cast<float>(x);
		const float fy = static_cast<float>(y);
		const Bathymetry* bed = (land && land->Contains(fx, fy)) ? land.get() : ((bathymetry && bathymetry->Valid()) ? bathymetry.get() : nullptr);
		if (bed) {
			const float step = std::abs(bed->stepX);
			const float z0 = bed->Sample(fx, fy);
			ctx.depth = std::max(waterZ - z0, 0.0f);
			ctx.gradX = (bed->Sample(fx + step, fy) - z0) / step;
			ctx.gradY = (bed->Sample(fx, fy + step) - z0) / step;
			ctx.hasTerrain = true;
		}

		// Same weighting as PBRWater::CascadeWeight in the shader (minus the render-only distance fade).
		for (uint32_t c = 0; c < NumCascades; ++c) {
			if (!ocean) {
				ctx.weight[c] = 0.0f;
				continue;
			}
			const CascadeBand& band = ocean->cascades[c];
			const float r = peakOmega / band.meanOmega;
			const float r2 = r * r;
			float weight = std::clamp(std::exp(-1.25f * r2 * r2) / std::max(band.pmWeightOpenSea, 1e-4f), 0.0f, 1.0f) * energyGain;
			const float kh = band.meanK * static_cast<float>(MetresPerUnit) * ctx.depth;
			weight *= 1.0f - 2.0f / (std::exp(2.0f * std::clamp(kh, 0.0f, 20.0f)) + 1.0f);
			ctx.weight[c] = weight;
		}
		return ctx;
	}

	WaveSnapshot::Displacement WaveSnapshot::Evaluate(double x, double y, const Context& ctx) const
	{
		Displacement out;
		if (mirror0 && mirror1) {
			Displacement a, b;
			for (uint32_t c = 0; c < MirroredCascades; ++c) {
				const float vertical = ctx.weight[c];
				if (vertical <= 0.0f)
					continue;
				const float horizontal = vertical * ctx.horizontalScale;
				SampleMirror(mirror0->grids[c], c, x, y, vertical, horizontal, a);
				SampleMirror(mirror1->grids[c], c, x, y, vertical, horizontal, b);
			}
			const float t = mirrorAlpha;
			out.dx = a.dx + (b.dx - a.dx) * t;
			out.dy = a.dy + (b.dy - a.dy) * t;
			out.dz = a.dz + (b.dz - a.dz) * t;
			out.vx = a.vx + (b.vx - a.vx) * t;
			out.vy = a.vy + (b.vy - a.vy) * t;
			out.vz = a.vz + (b.vz - a.vz) * t;
		}

		if (ctx.hasTerrain && shore.amplitude > 0.0f && ctx.depth < shore.onsetDepth) {
			const float gradLen = std::sqrt(ctx.gradX * ctx.gradX + ctx.gradY * ctx.gradY);
			if (gradLen > 1e-4f) {
				const float h = std::max(ctx.depth, 1.0f);
				const float onset = shore.onsetDepth;
				float A = shore.amplitude * ctx.fetchRatio * std::sqrt(std::sqrt(onset / h));
				A = std::min(A, 0.39f * h);
				A *= Smoothstep(onset, onset * 0.5f, h) * Smoothstep(0.05f * static_cast<float>(UnitsPerMetre), 0.25f * static_cast<float>(UnitsPerMetre), h);
				const float hm = std::max(h * static_cast<float>(MetresPerUnit), 0.02f);
				// Same slope limit as the shader: k A at most 0.4 where the wavelength collapses in the swash.
				const float dTdh = 1.0f / (std::max(shore.slope, 0.005f) * std::sqrt(G * hm)) * static_cast<float>(MetresPerUnit);
				const float kLocal = shore.omega * dTdh * gradLen;
				A = std::min(A, 0.4f / std::max(kLocal, 1e-5f));
				const float travel = 2.0f * std::sqrt(hm) / (std::max(shore.slope, 0.005f) * std::sqrt(G));
				const double theta = -shore.omega * travel - shorePhase;
				out.dz += A * static_cast<float>(std::sin(theta));
				out.vz += -shore.omega * A * static_cast<float>(std::cos(theta));
			}
		}
		return out;
	}

	void WaveSnapshot::ParcelDisplacements(const double* xs, const double* ys, size_t count, float waterZ, Displacement* out) const
	{
		for (size_t i = 0; i < count; ++i)
			out[i] = {};
		if (count == 0 || VerticalBound() <= 1.0f)
			return;
		const Context ctx = BuildContext(xs[0], ys[0], waterZ);
		for (size_t i = 0; i < count; ++i)
			out[i] = Evaluate(xs[i], ys[i], ctx);
	}

	WaveSnapshot::Sample WaveSnapshot::SampleAt(double x, double y, float waterZ) const
	{
		Sample out;
		out.originX = x;
		out.originY = y;
		if (VerticalBound() <= 1.0f)
			return out;
		const Context ctx = BuildContext(x, y, waterZ);

		// Find the rest position of the water that the horizontal displacement has carried above (x, y).
		double x0 = x, y0 = y;
		Displacement d;
		for (int it = 0; it < 2; ++it) {
			d = Evaluate(x0, y0, ctx);
			x0 = x - d.dx;
			y0 = y - d.dy;
		}
		d = Evaluate(x0, y0, ctx);

		out.height = d.dz;
		out.verticalVelocity = d.vz;
		out.velocityX = d.vx;
		out.velocityY = d.vy;
		out.originX = x0;
		out.originY = y0;
		return out;
	}

	// ========================================================================
	// Clock
	// ========================================================================

	void WaveClock::Advance(const ShoreParams& shore, double dt, double x, double y, double z)
	{
		if (!initialised) {
			shorePhase = 0.0;
			shorePhasePrev = 0.0;
			initialised = true;
		}
		time += std::max(dt, 0.0);
		shorePhasePrev = shorePhase;
		shorePhase = Wrap(shorePhase + shore.omega * dt);
		camX = x;
		camY = y;
		camZ = z;
	}

	void WaveClock::Fill(WaveSnapshot& snapshot) const
	{
		snapshot.time = time;
		snapshot.shorePhase = shorePhase;
		snapshot.shorePhasePrev = shorePhasePrev;
		snapshot.refX = camX;
		snapshot.refY = camY;
		snapshot.refZ = camZ;
	}
}
