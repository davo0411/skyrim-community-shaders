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

		// ---- Shared with Noise.hlsli (Hash12, ValueNoise): keep the two in sync ----

		float Frac(float x) { return x - std::floor(x); }

		float Hash12(float x, float y)
		{
			float p3x = Frac(x * 0.1031f), p3y = Frac(y * 0.1031f), p3z = Frac(x * 0.1031f);
			const float d = p3x * (p3y + 33.33f) + p3y * (p3z + 33.33f) + p3z * (p3x + 33.33f);
			p3x += d;
			p3y += d;
			p3z += d;
			return Frac((p3x + p3y) * p3z);
		}

		float ValueNoise(float x, float y)
		{
			const float ix = std::floor(x), iy = std::floor(y);
			const float fx = x - ix, fy = y - iy;
			const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
			const float a = Hash12(ix, iy) + (Hash12(ix + 1.0f, iy) - Hash12(ix, iy)) * ux;
			const float b = Hash12(ix, iy + 1.0f) + (Hash12(ix + 1.0f, iy + 1.0f) - Hash12(ix, iy + 1.0f)) * ux;
			return a + (b - a) * uy;
		}

		/// Wave groups: the factor on the shore waves' height at the waterline point (x, y) for waves reaching it at
		/// `arrival` (s, groupTime clock). Mirrors PBRWater::ShoreGroup.
		float ShoreGroup(const ShoreParams& p, float x, float y, float arrival)
		{
			const float qx = x / std::max(3.0f * p.deepLength, 1.0f), qy = y / std::max(3.0f * p.deepLength, 1.0f);
			const float s = arrival / std::max(7.0f * p.period, 1.0f);
			const float slice = std::floor(s);
			float t = s - slice;
			t = t * t * (3.0f - 2.0f * t);
			const float a = ValueNoise(qx + slice * 17.31f, qy + slice * 17.31f);
			const float b = ValueNoise(qx + (slice + 1.0f) * 17.31f, qy + (slice + 1.0f) * 17.31f);
			return 0.45f + 1.1f * (a + (b - a) * t);
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
		const float r = std::sqrt(-2.0f * std::log(u1)) / std::numbers::sqrt2_v<float>;
		const float a = static_cast<float>(TwoPi) * u2;
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
				const float nyquist = static_cast<float>(Pi) * static_cast<float>(s.resolution) / band.length;
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
		// The wind sea and the swell on their own (Hs = 4 sqrt(m0)), over the whole spectrum rather than the cascades'
		// bands: the shore waves take their heights from these. The swell was normalised to its height above.
		{
			double m0 = 0.0;
			constexpr int Samples = 512;
			const double lo = std::log(0.3 * s.peakOmega), hi = std::log(20.0 * s.peakOmega);
			for (int i = 0; i < Samples; ++i) {
				const double w0 = std::exp(lo + (hi - lo) * i / Samples);
				const double w1 = std::exp(lo + (hi - lo) * (i + 1) / Samples);
				m0 += Jonswap(static_cast<float>(0.5 * (w0 + w1)), s.peakOmega, JonswapAlpha, JonswapGamma) * (w1 - w0);
			}
			s.windSeaHeight = 4.0f * static_cast<float>(std::sqrt(std::max(m0, 0.0))) * s.heightScale * static_cast<float>(UnitsPerMetre);
			s.swellHeight = s.swellAlpha > 0.0f ? params.swellHeight * static_cast<float>(UnitsPerMetre) : 0.0f;
		}
		s.shoreScale = params.shoreScale;
		s.displacementBound = 5.0f * sigma * std::max(1.0f, s.choppiness) * static_cast<float>(UnitsPerMetre) + 1.0f;
		s.whitecapCoverage = std::clamp(3.84e-6f * std::pow(U, 3.41f), 0.0f, 1.0f);
		s.calm = 4.0f * sigma < 0.01f;
		return s;
	}

	WhitecapCriterion CascadeWhitecaps(const OceanSpectrum& spectrum, uint32_t cascade, float amount)
	{
		WhitecapCriterion out;
		if (cascade >= NumCascades || spectrum.calm || amount <= 0.0f)
			return out;
		// Crests breaking now: half of Monahan's whitecap coverage (the rest is the foam they leave behind).
		out.share = std::clamp(0.5f * spectrum.whitecapCoverage * amount, 1e-6f, 0.4f);
		// Upper-tail normal quantile of the share (Abramowitz & Stegun 26.2.23).
		const float t = std::sqrt(-2.0f * std::log(out.share));
		const float z = t - (2.515517f + 0.802853f * t + 0.010328f * t * t) / (1.0f + 1.432788f * t + 0.189269f * t * t + 0.001308f * t * t * t);
		const CascadeBand& band = spectrum.cascades[cascade];
		const float sigma = std::max(spectrum.choppiness * std::sqrt(std::max(band.meanSquareSlope, 0.0f)), 1e-4f);
		out.threshold = 1.0f - sigma * z;
		// Half a spread past the threshold the foam grows at its full rate, so a crest only just over it whitens slowly.
		out.width = 0.5f * sigma;
		return out;
	}

	ShoreParams UpdateShoreParams(const OceanSpectrum& spectrum, const ShoreParams* previous, double dt)
	{
		ShoreParams p;
		// The shore train carries the open sea's swell and wind sea; Shore Wave Height scales both where they reach the shore.
		p.swellHeight = spectrum.swellHeight * spectrum.shoreScale;
		p.windSeaHeight = spectrum.windSeaHeight * spectrum.shoreScale;

		// The dominant sea: the swell where it is higher than the open-sea wind sea, else the wind sea.
		const bool swell = spectrum.swellHeight > spectrum.windSeaHeight;
		const float peak = swell ? spectrum.swellPeak : spectrum.peakOmega;
		// Within the periods that make surf.
		const float targetOmega = std::clamp(peak, static_cast<float>(TwoPi / 14.0), static_cast<float>(TwoPi / 4.0));
		const float targetSwellDirection = std::atan2(spectrum.swellDirY, spectrum.swellDirX);

		if (previous && previous->omega > 0.0f) {
			// Ease over 8 s (as the wind): when the swell overtakes the wind sea the period changes, which would otherwise
			// close up or spread every crest at once. The frequency eases in log space, so up and down take as long.
			const float ease = 1.0f - static_cast<float>(std::exp(-std::max(dt, 0.0) / 8.0));
			p.omega = previous->omega * std::exp((std::log(targetOmega) - std::log(previous->omega)) * ease);
			p.swellDirection = previous->swellDirection + static_cast<float>(std::remainder(static_cast<double>(targetSwellDirection) - previous->swellDirection, TwoPi)) * ease;
		} else {
			p.omega = targetOmega;
			p.swellDirection = targetSwellDirection;
		}
		// The clock integrates the frequency, so a frequency that changes never moves the waves.
		p.phase = previous ? Wrap(previous->phase + static_cast<double>(p.omega) * dt) : 0.0;
		p.groupTime = previous ? std::fmod(previous->groupTime + dt, 3600.0) : 0.0;

		const float units = static_cast<float>(UnitsPerMetre);
		p.period = static_cast<float>(TwoPi) / p.omega;
		const float L0 = G * p.period * p.period / static_cast<float>(TwoPi);  // m
		p.deepLength = L0 * units;

		// Bound of the run-up: Stockdon et al. (2006), 2 % exceedance, R = 1.1 (<eta> + S / 2) with set-up
		// <eta> = 0.35 b sqrt(H0 L0) and swash S = sqrt(H0 L0 (0.563 b^2 + 0.004)), for the highest wave of the biggest set
		// (ShoreGroup at most 1.55) of the full open sea on a steep (b = 0.2) beach, with a margin.
		constexpr float Slope = 0.2f;
		const float highest = ShoreHeightRatio * 1.55f * p.MaxDeepHeight() / units;
		const float hl = std::sqrt(highest * L0);
		const float runup = ShoreRunupScale * 1.1f * (0.35f * Slope * hl + 0.5f * hl * std::sqrt(0.563f * Slope * Slope + 0.004f));
		p.maxRunup = runup * units + 20.0f;
		return p;
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

		constexpr float cellSize = static_cast<float>(CellSize);
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
	// Shore field
	// ========================================================================

	float Wavenumber(float omega, float depth)
	{
		const float k0 = omega * omega / G;
		const float t = std::tanh(std::min(k0 * std::max(depth, 1e-3f), 20.0f));
		return k0 / std::sqrt(std::max(t, 1e-6f));
	}

	float PhaseSpeed(float omega, float depth)
	{
		omega = std::max(omega, 1e-3f);
		return omega / Wavenumber(omega, depth);
	}

	float ShoalingCoefficient(float omega, float depth)
	{
		omega = std::max(omega, 1e-3f);
		depth = std::max(depth, 1e-3f);
		const float k = Wavenumber(omega, depth);
		const float kh = std::min(k * depth, 20.0f);
		const float n = 0.5f * (1.0f + 2.0f * kh / std::sinh(2.0f * kh));
		const float cg = n * omega / k;
		const float cg0 = 0.5f * G / omega;
		return std::sqrt(cg0 / cg);
	}

	namespace
	{
		constexpr float Far = 1.0e30f;

		/// Separable [1 4 6 4 1] / 16 blur, clamped at the edges.
		void Blur(std::vector<float>& v, uint32_t w, uint32_t h, int passes)
		{
			std::vector<float> tmp(v.size());
			auto clampX = [&](int x) { return static_cast<uint32_t>(std::clamp(x, 0, static_cast<int>(w) - 1)); };
			auto clampY = [&](int y) { return static_cast<uint32_t>(std::clamp(y, 0, static_cast<int>(h) - 1)); };
			for (int p = 0; p < passes; ++p) {
				for (uint32_t y = 0; y < h; ++y) {
					const float* row = &v[static_cast<size_t>(y) * w];
					for (uint32_t x = 0; x < w; ++x) {
						const int xi = static_cast<int>(x);
						tmp[static_cast<size_t>(y) * w + x] = (row[clampX(xi - 2)] + 4.0f * row[clampX(xi - 1)] + 6.0f * row[x] + 4.0f * row[clampX(xi + 1)] + row[clampX(xi + 2)]) * (1.0f / 16.0f);
					}
				}
				for (uint32_t y = 0; y < h; ++y) {
					const int yi = static_cast<int>(y);
					for (uint32_t x = 0; x < w; ++x) {
						auto at = [&](int yy) { return tmp[static_cast<size_t>(clampY(yy)) * w + x]; };
						v[static_cast<size_t>(y) * w + x] = (at(yi - 2) + 4.0f * at(yi - 1) + 6.0f * at(yi) + 4.0f * at(yi + 1) + at(yi + 2)) * (1.0f / 16.0f);
					}
				}
			}
		}

		/**
		 * Fast sweeping (Zhao 2005) for |grad T| = cost (per texel) on the `active` texels, with the Godunov
		 * upwind update. Texels outside `active` never pass values on.
		 */
		void FastSweep(std::vector<float>& T, const std::vector<float>& cost, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, int iterations, const std::atomic_bool* cancel)
		{
			const int W = static_cast<int>(w);
			const int H = static_cast<int>(h);
			auto at = [&](int x, int y) {
				if (x < 0 || y < 0 || x >= W || y >= H)
					return Far;
				const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
				return active[i] ? T[i] : Far;
			};
			for (int it = 0; it < iterations; ++it) {
				for (int sweep = 0; sweep < 4; ++sweep) {
					if (cancel && cancel->load(std::memory_order_relaxed))
						return;
					const int dx = (sweep & 1) ? -1 : 1;
					const int dy = (sweep & 2) ? -1 : 1;
					for (int y = dy > 0 ? 0 : H - 1; y >= 0 && y < H; y += dy) {
						for (int x = dx > 0 ? 0 : W - 1; x >= 0 && x < W; x += dx) {
							const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
							if (!active[i])
								continue;
							const float a = std::min(at(x - 1, y), at(x + 1, y));
							const float b = std::min(at(x, y - 1), at(x, y + 1));
							if (a >= Far && b >= Far)
								continue;
							const float f = cost[i];
							float t;
							if (std::abs(a - b) >= f)
								t = std::min(a, b) + f;
							else
								t = 0.5f * (a + b + std::sqrt(2.0f * f * f - (a - b) * (a - b)));
							T[i] = std::min(T[i], t);
						}
					}
				}
			}
		}

		/// Bilinear interpolation over the corners already computed (`done`). False when none is.
		bool InterpolateDone(const std::vector<float>& v, const std::vector<uint8_t>& done, uint32_t w, uint32_t h, float fx, float fy, float& out)
		{
			const int x0 = static_cast<int>(std::floor(fx));
			const int y0 = static_cast<int>(std::floor(fy));
			const float tx = fx - static_cast<float>(x0);
			const float ty = fy - static_cast<float>(y0);
			float sum = 0.0f, weights = 0.0f;
			for (int c = 0; c < 4; ++c) {
				const int x = x0 + (c & 1);
				const int y = y0 + (c >> 1);
				if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h))
					continue;
				const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
				if (!done[i])
					continue;
				const float weight = ((c & 1) ? tx : 1.0f - tx) * ((c >> 1) ? ty : 1.0f - ty);
				sum += v[i] * weight;
				weights += weight;
			}
			if (weights < 1e-4f)
				return false;
			out = sum / weights;
			return true;
		}

		/// Gradient of `v` at (x, y) per texel from the neighbours that are `active` and finite.
		bool Gradient(const std::vector<float>& v, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, int x, int y, float& gx, float& gy)
		{
			auto get = [&](int xx, int yy, float& out) {
				if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
					return false;
				const size_t i = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
				if (!active[i] || !(v[i] < Far))
					return false;
				out = v[i];
				return true;
			};
			float c = 0.0f, l = 0.0f, r = 0.0f, d = 0.0f, u = 0.0f;
			if (!get(x, y, c))
				return false;
			const bool hasL = get(x - 1, y, l), hasR = get(x + 1, y, r), hasD = get(x, y - 1, d), hasU = get(x, y + 1, u);
			gx = hasL && hasR ? 0.5f * (r - l) : (hasR ? r - c : (hasL ? c - l : 0.0f));
			gy = hasD && hasU ? 0.5f * (u - d) : (hasU ? u - c : (hasD ? c - d : 0.0f));
			return hasL || hasR || hasD || hasU;
		}
	}

	ShoreField::Sample ShoreField::At(float x, float y) const
	{
		Sample out;
		if (!Valid())
			return out;
		const float fx = (x - minX) / step;
		const float fy = (y - minY) / step;
		const float maxX = static_cast<float>(width - 1);
		const float maxY = static_cast<float>(height - 1);
		if (fx < 0.0f || fy < 0.0f || fx > maxX || fy > maxY)
			return out;
		const uint32_t x0 = std::min(static_cast<uint32_t>(fx), width - 2);
		const uint32_t y0 = std::min(static_cast<uint32_t>(fy), height - 2);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		auto lerp2 = [&](const std::vector<float>& v) {
			const size_t i = static_cast<size_t>(y0) * width + x0;
			const float a = v[i] + (v[i + 1] - v[i]) * tx;
			const float b = v[i + width] + (v[i + width + 1] - v[i + width]) * tx;
			return a + (b - a) * ty;
		};
		out.travelTime = lerp2(travelTime);
		out.phaseLag = omega * out.travelTime;
		out.distance = lerp2(distance);
		out.slownessX = lerp2(slownessX);
		out.slownessY = lerp2(slownessY);
		out.slope = lerp2(slope);
		out.exposure = lerp2(exposure);
		out.room = lerp2(room);
		// Fade out over the outer tenth of the field, so its moving edge never pops, as PBRWater::SampleShore.
		const float edge = std::min(std::min(fx, maxX - fx) / maxX, std::min(fy, maxY - fy) / maxY);
		out.weight = Smoothstep(0.0f, 0.1f, edge);
		return out;
	}

	namespace
	{
		/**
		 * First-order fast sweeping leaves faint kinks where the upwind direction switches between neighbours; one
		 * [1 4 6 4 1] pass over the texels whose whole kernel is `reached` smooths them without moving the isolines (the
		 * filter is symmetric, so it leaves a locally linear field unchanged and only rounds corners of about a texel).
		 * Texels near the edge of the reached area keep their own value.
		 */
		std::vector<float> SmoothReached(const std::vector<float>& v, const std::vector<uint8_t>& reached, uint32_t w, uint32_t h)
		{
			const int W = static_cast<int>(w);
			const int H = static_cast<int>(h);
			auto ok = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && reached[static_cast<size_t>(y) * w + static_cast<size_t>(x)]; };
			constexpr float Kernel[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
			std::vector<float> rows(v.size(), 0.0f), out(v);
			std::vector<uint8_t> full(v.size(), 0);
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					bool all = true;
					float sum = 0.0f;
					for (int t = -2; t <= 2 && all; ++t) {
						all = ok(x + t, y);
						if (all)
							sum += Kernel[t + 2] * v[static_cast<size_t>(y) * w + static_cast<size_t>(x + t)];
					}
					const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
					rows[i] = sum;
					full[i] = all;
				}
			}
			for (int y = 0; y < H; ++y) {
				for (int x = 0; x < W; ++x) {
					if (!ok(x, y))
						continue;
					bool all = true;
					float sum = 0.0f;
					for (int t = -2; t <= 2 && all; ++t) {
						all = y + t >= 0 && y + t < H && full[static_cast<size_t>(y + t) * w + static_cast<size_t>(x)];
						if (all)
							sum += Kernel[t + 2] * rows[static_cast<size_t>(y + t) * w + static_cast<size_t>(x)];
					}
					if (all)
						out[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = sum;
				}
			}
			return out;
		}

		/// Unit direction of the gradient of `v` (lightly smoothed, 3 x 3) on the `active` texels; `sign` -1 points it downhill.
		void GradientDirections(const std::vector<float>& v, const std::vector<uint8_t>& active, uint32_t w, uint32_t h, float sign, std::vector<float>& outX, std::vector<float>& outY)
		{
			const size_t n = v.size();
			std::vector<float> gx(n, 0.0f), gy(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					float dx = 0.0f, dy = 0.0f;
					if (!active[i] || !Gradient(v, active, w, h, static_cast<int>(x), static_cast<int>(y), dx, dy))
						continue;
					const float len = std::sqrt(dx * dx + dy * dy);
					if (len > 1e-12f) {
						gx[i] = sign * dx / len;
						gy[i] = sign * dy / len;
					}
				}
			}
			outX.assign(n, 0.0f);
			outY.assign(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					float sx = 0.0f, sy = 0.0f;
					for (int k = -1; k <= 1; ++k) {
						for (int l = -1; l <= 1; ++l) {
							const int xx = std::clamp(static_cast<int>(x) + l, 0, static_cast<int>(w) - 1);
							const int yy = std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(h) - 1);
							const size_t j = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
							sx += gx[j];
							sy += gy[j];
						}
					}
					const float len = std::sqrt(sx * sx + sy * sy);
					const size_t i = static_cast<size_t>(y) * w + x;
					if (len > 1e-3f) {
						outX[i] = sx / len;
						outY[i] = sy / len;
					}
				}
			}
		}
	}

	std::shared_ptr<ShoreField> BuildShoreField(const ShoreInputs& in, const std::atomic_bool* cancel)
	{
		const uint32_t w = in.width;
		const uint32_t h = in.height;
		const size_t n = static_cast<size_t>(w) * h;
		if (w < 8 || h < 8 || in.bed.size() != n || in.level.size() != n)
			return nullptr;
		auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

		const float metresPerUnit = static_cast<float>(MetresPerUnit);
		const float stepMetres = in.step * metresPerUnit;
		// The swash lens: waves never slow down below the speed in 15 cm of water.
		constexpr float MinDepth = 0.15f;

		// Bed, lightly smoothed: the BC4 heightmap is terraced in half-metre steps and the loaded cells' vertices
		// are 1.8 m apart, which would make depths and slopes step.
		std::vector<float> bed(n);
		for (size_t i = 0; i < n; ++i) {
			const float level = in.level[i];
			bed[i] = std::isfinite(in.bed[i]) ? in.bed[i] : (std::isfinite(level) ? level - 1.0e4f : 0.0f);
		}
		// What is water is decided on the bed as it is: smoothed, a narrow stream between steep banks took the banks'
		// height and turned to land (no train, no swash). The smoothed bed only sets the depths the waves feel.
		std::vector<uint8_t> water(n), land(n);
		for (size_t i = 0; i < n; ++i) {
			water[i] = std::isfinite(in.level[i]) && in.level[i] - bed[i] > 0.0f;
			land[i] = !water[i];
		}
		Blur(bed, w, h, 2);

		std::vector<float> depth(n);
		for (size_t i = 0; i < n; ++i) {
			const float smoothed = std::isfinite(in.level[i]) ? in.level[i] - bed[i] : -1.0e4f;
			depth[i] = water[i] ? std::max(smoothed, 1.0f) : std::min(smoothed, -1.0f);
		}

		// Distance to the waterline on either side (units), seeded with the zero crossing of the depth between
		// neighbours: the beach slope and, up the beach, the travel times are carried along it.
		std::vector<float> waterDistance(n, Far), landDistance(n, Far), unit(n, in.step);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (!water[i])
					continue;
				const int nx[4] = { -1, 1, 0, 0 };
				const int ny[4] = { 0, 0, -1, 1 };
				for (int k = 0; k < 4; ++k) {
					const int xx = static_cast<int>(x) + nx[k];
					const int yy = static_cast<int>(y) + ny[k];
					if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
						continue;
					const size_t j = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
					if (!land[j])
						continue;
					const float frac = std::clamp(depth[i] / std::max(depth[i] - depth[j], 1e-3f), 0.0f, 1.0f);
					waterDistance[i] = std::min(waterDistance[i], frac * in.step);
					landDistance[j] = std::min(landDistance[j], (1.0f - frac) * in.step);
				}
			}
		}
		FastSweep(waterDistance, unit, water, w, h, 2, cancel);
		FastSweep(landDistance, unit, land, w, h, 2, cancel);
		if (cancelled())
			return nullptr;

		auto field = std::make_shared<ShoreField>();
		field->width = w;
		field->height = h;
		field->minX = in.minX;
		field->minY = in.minY;
		field->step = in.step;
		field->swellDirection = in.swellDirection;
		field->level = in.level;

		// The time tau the waves take to the nearest waterline (eikonal from the waterline outwards over the real depths,
		// |grad tau| = 1 / c(h)): Fluid Flux's coastline distance, as a travel time. Over a shoaling bed refraction turns the
		// crests towards the depth contours, so surf runs into every beach nearly head-on whatever the sea offshore does;
		// traced from the open sea in its one direction instead, the fronts ran along the banks of an inlet and lee shores
		// got none. The crests move shorewards down tau, and slow down and close up as the water shoals.
		const float omega = std::max(in.omega, 0.05f);
		field->omega = omega;
		std::vector<float> tau(n, Far), cost(n, 0.0f);
		for (size_t i = 0; i < n; ++i) {
			if (!water[i])
				continue;
			cost[i] = stepMetres / PhaseSpeed(omega, std::max(depth[i] * metresPerUnit, MinDepth));
			// The waterline: the water's edge, at the sub-texel distance the zero crossing of the depth gives.
			if (waterDistance[i] <= in.step)
				tau[i] = waterDistance[i] / in.step * cost[i];
		}
		FastSweep(tau, cost, water, w, h, 3, cancel);
		if (cancelled())
			return nullptr;
		std::vector<uint8_t> reached(n, 0);
		for (size_t i = 0; i < n; ++i)
			reached[i] = water[i] && tau[i] < Far;
		field->travelTime = SmoothReached(tau, reached, w, h);

		// Slowness grad tau (s/m), lightly smoothed: the waves travel down it, at 1 / its length.
		{
			auto& sx = field->slownessX;
			auto& sy = field->slownessY;
			sx.assign(n, 0.0f);
			sy.assign(n, 0.0f);
			std::vector<float> gx(n, 0.0f), gy(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					float dx = 0.0f, dy = 0.0f;
					if (reached[i] && Gradient(field->travelTime, reached, w, h, static_cast<int>(x), static_cast<int>(y), dx, dy)) {
						gx[i] = dx / stepMetres;
						gy[i] = dy / stepMetres;
					}
				}
			}
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!reached[i])
						continue;
					float ax = 0.0f, ay = 0.0f, count = 0.0f;
					for (int k = -1; k <= 1; ++k) {
						for (int l = -1; l <= 1; ++l) {
							const int xx = static_cast<int>(x) + l;
							const int yy = static_cast<int>(y) + k;
							if (xx < 0 || yy < 0 || xx >= static_cast<int>(w) || yy >= static_cast<int>(h))
								continue;
							const size_t j = static_cast<size_t>(yy) * w + static_cast<size_t>(xx);
							if (!reached[j])
								continue;
							ax += gx[j];
							ay += gy[j];
							count += 1.0f;
						}
					}
					sx[i] = ax / count;
					sy[i] = ay / count;
				}
			}
		}
		// Water no wave reaches (cut off from every seed) gets no train.
		for (size_t i = 0; i < n; ++i) {
			if (!reached[i])
				field->travelTime[i] = 0.0f;
		}

		// Signed distance to the waterline (Fluid Flux's coastline distance): it finds the nearest point of the coast, where
		// the shaders read the variation along it and the swash its timing.
		field->distance.assign(n, 0.0f);
		for (size_t i = 0; i < n; ++i) {
			if (water[i])
				field->distance[i] = std::min(waterDistance[i], 1.0e7f);
			else
				field->distance[i] = -std::min(landDistance[i], 1.0e7f);
		}
		if (cancelled())
			return nullptr;

		// Exposure to the swell: open water upwind along its direction. Shores facing away from it, and water behind
		// islands and headlands, see little of it (diffraction fills in over a few wavelengths, hence the soft ramp from
		// 100 m to 10 km of open water).
		field->exposure.assign(n, 0.0f);
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (!reached[i])
					continue;
				// The swell lives on the sea: a lake or a river above it (however long its open water) never sees it.
				if (std::isfinite(in.seaLevel) && std::abs(in.level[i] - in.seaLevel) > 256.0f)
					continue;
				const float wx = in.minX + static_cast<float>(x) * in.step;
				const float wy = in.minY + static_cast<float>(y) * in.step;
				const float fetchMetres = in.fetch && in.fetch->Valid() ? in.fetch->SampleMetres(wx, wy, in.swellDirection) * in.fetchScale : 5.0e5f;
				field->exposure[i] = Smoothstep(2.0f, 4.0f, std::log10(std::max(fetchMetres, 1.0f)));
			}
		}
		// Diffraction fills the shadow behind a headland over tens of metres (sqrt(lambda r)); the ray-marched fetch has
		// razor edges along the swell direction instead, which lit half of an inlet and left the other half flat. Averaged
		// over the water within ~50 m (on a quarter-resolution copy): a lee shore close to open water still sees some of
		// the swell; water it can never reach (a lake, a stream) sees none.
		{
			constexpr uint32_t Down = 4;
			const uint32_t lw = (w + Down - 1) / Down;
			const uint32_t lh = (h + Down - 1) / Down;
			std::vector<float> sum(static_cast<size_t>(lw) * lh, 0.0f), weight(static_cast<size_t>(lw) * lh, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!reached[i])
						continue;
					const size_t j = static_cast<size_t>(y / Down) * lw + x / Down;
					sum[j] += field->exposure[i];
					weight[j] += 1.0f;
				}
			}
			Blur(sum, lw, lh, 48);
			Blur(weight, lw, lh, 48);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					if (!reached[i])
						continue;
					// Bilinear between the quarter-resolution texel centres.
					const float fx = std::clamp((static_cast<float>(x) + 0.5f) / Down - 0.5f, 0.0f, static_cast<float>(lw - 1));
					const float fy = std::clamp((static_cast<float>(y) + 0.5f) / Down - 0.5f, 0.0f, static_cast<float>(lh - 1));
					const uint32_t x0 = std::min(static_cast<uint32_t>(fx), lw > 1 ? lw - 2 : 0u);
					const uint32_t y0 = std::min(static_cast<uint32_t>(fy), lh > 1 ? lh - 2 : 0u);
					const float tx = fx - static_cast<float>(x0);
					const float ty = fy - static_cast<float>(y0);
					auto at = [&](const std::vector<float>& v, uint32_t xx, uint32_t yy) { return v[static_cast<size_t>(yy) * lw + xx]; };
					auto bilinear = [&](const std::vector<float>& v) {
						const float a = at(v, x0, y0) + (at(v, x0 + 1, y0) - at(v, x0, y0)) * tx;
						const float b = at(v, x0, y0 + 1) + (at(v, x0 + 1, y0 + 1) - at(v, x0, y0 + 1)) * tx;
						return a + (b - a) * ty;
					};
					const float average = bilinear(sum) / std::max(bilinear(weight), 1e-4f);
					field->exposure[i] = std::clamp(average, 0.0f, 1.0f);
				}
			}
		}

		// Room for the wave train: the fronts run from mid-water to each shore, so the water must be a fair part of a
		// wavelength across for them to look like waves at all. Only a ditch narrower than that has none (the FFT sea
		// carries what waves it has); a stream keeps a train, as small as its fetch-limited sea (no swell reaches it). The width is that of the largest
		// disc of water around each point (twice the distance to the waterline at its centre: the width of a channel, the
		// size of a pond), propagated by raster sweeps; the wavelength that of the shore waves in the local depth.
		field->room.assign(n, 0.0f);
		{
			std::vector<float> radius(n, 0.0f), reach(n, -1.0f);
			for (size_t i = 0; i < n; ++i) {
				if (water[i]) {
					radius[i] = std::min(waterDistance[i], 1.0e7f);
					reach[i] = radius[i];
				}
			}
			const float diagonal = in.step * std::numbers::sqrt2_v<float>;
			auto relax = [&](size_t i, size_t j, float d) {
				const float r = reach[j] - d;
				if (r >= 0.0f && radius[j] > radius[i]) {
					radius[i] = radius[j];
					reach[i] = r;
				}
			};
			for (int pass = 0; pass < 2; ++pass) {
				for (uint32_t y = 0; y < h; ++y) {
					for (uint32_t x = 0; x < w; ++x) {
						const size_t i = static_cast<size_t>(y) * w + x;
						if (!water[i])
							continue;
						if (x > 0)
							relax(i, i - 1, in.step);
						if (y > 0) {
							relax(i, i - w, in.step);
							if (x > 0)
								relax(i, i - w - 1, diagonal);
							if (x + 1 < w)
								relax(i, i - w + 1, diagonal);
						}
					}
				}
				for (uint32_t y = h; y-- > 0;) {
					for (uint32_t x = w; x-- > 0;) {
						const size_t i = static_cast<size_t>(y) * w + x;
						if (!water[i])
							continue;
						if (x + 1 < w)
							relax(i, i + 1, in.step);
						if (y + 1 < h) {
							relax(i, i + w, in.step);
							if (x + 1 < w)
								relax(i, i + w + 1, diagonal);
							if (x > 0)
								relax(i, i + w - 1, diagonal);
						}
					}
				}
				if (cancelled())
					return nullptr;
			}
			for (size_t i = 0; i < n; ++i) {
				if (!water[i])
					continue;
				const float wavelength = static_cast<float>(TwoPi) / Wavenumber(omega, std::max(depth[i] * metresPerUnit, 0.5f));
				field->room[i] = Smoothstep(0.1f, 0.6f, 2.0f * radius[i] * metresPerUnit / wavelength);
			}
		}

		// Where fronts from different stretches of shore meet (mid-channel, off a headland, in a bay reached from several
		// sides) tau kinks: the crests meet at an angle and the direction of travel flips across a line. Across such a
		// kink the averaged gradient of tau is shorter than 1 / c, so |<grad tau>| c measures how coherent the fronts are;
		// the train fades out where it drops (the FFT sea carries the waves there), and where no front arrived at all (it
		// would rise and fall in step everywhere). Averaged over ~3.5 m: enough to hide the bend, narrow enough that a
		// stream, whose fronts meet down its middle, keeps its banks' waves. Applied
		// to the water before the room is carried up the beach.
		{
			// Averaged over the water only: land around the waterline must not shorten the average there.
			std::vector<float> gx(n, 0.0f), gy(n, 0.0f), weight(n, 0.0f);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const size_t i = static_cast<size_t>(y) * w + x;
					float dx = 0.0f, dy = 0.0f;
					if (reached[i] && Gradient(tau, reached, w, h, static_cast<int>(x), static_cast<int>(y), dx, dy)) {
						gx[i] = dx / stepMetres;
						gy[i] = dy / stepMetres;
						weight[i] = 1.0f;
					}
				}
			}
			Blur(gx, w, h, 4);
			Blur(gy, w, h, 4);
			Blur(weight, w, h, 4);
			for (size_t i = 0; i < n; ++i) {
				if (!water[i])
					continue;
				const float norm = 1.0f / std::max(weight[i], 1e-4f);
				const float slowness = std::sqrt(gx[i] * gx[i] + gy[i] * gy[i]) * norm;
				if (!reached[i] || slowness <= 0.0f) {
					field->room[i] = 0.0f;
					continue;
				}
				const float coherence = slowness * PhaseSpeed(omega, std::max(depth[i] * metresPerUnit, MinDepth));
				field->room[i] *= Smoothstep(0.75f, 0.95f, coherence);
			}
			if (cancelled())
				return nullptr;
		}

		// Rays across the waterline: towards it in the water, away from it on land.
		std::vector<float> rayX, rayY, inlandX, inlandY;
		GradientDirections(waterDistance, water, w, h, -1.0f, rayX, rayY);
		GradientDirections(landDistance, land, w, h, 1.0f, inlandX, inlandY);
		for (size_t i = 0; i < n; ++i) {
			if (land[i]) {
				rayX[i] = inlandX[i];
				rayY[i] = inlandY[i];
			}
		}
		if (cancelled())
			return nullptr;

		std::vector<uint32_t> seaward, inland;
		seaward.reserve(n);
		inland.reserve(n);
		for (uint32_t i = 0; i < n; ++i) {
			if (water[i] && waterDistance[i] < Far)
				seaward.push_back(i);
			if (land[i] && landDistance[i] < Far)
				inland.push_back(i);
		}
		std::sort(seaward.begin(), seaward.end(), [&](uint32_t a, uint32_t b) { return waterDistance[a] < waterDistance[b]; });
		std::sort(inland.begin(), inland.end(), [&](uint32_t a, uint32_t b) { return landDistance[a] < landDistance[b]; });

		// Up the beach the travel times, directions and exposure are those of the waterline the inland ray leaves from,
		// so the swash on a stretch of beach follows the crests that reach it.
		{
			std::vector<uint8_t> done(reached);
			std::vector<std::vector<float>*> carried = { &field->travelTime, &field->slownessX, &field->slownessY };
			carried.push_back(&field->exposure);
			carried.push_back(&field->room);
			for (const uint32_t i : inland) {
				const float x = static_cast<float>(i % w) - rayX[i];
				const float y = static_cast<float>(i / w) - rayY[i];
				bool any = false;
				for (auto* v : carried) {
					float value = 0.0f;
					if (InterpolateDone(*v, done, w, h, x, y, value)) {
						(*v)[i] = value;
						any = true;
					}
				}
				done[i] = any;
			}
			// Neighbouring rays leave from different stretches of the waterline: where they meet, the carried times and
			// directions jump from texel to texel, which put hard corners in the swash. Smoothed over the carried texels.
			for (int pass = 0; pass < 2; ++pass) {
				field->travelTime = SmoothReached(field->travelTime, done, w, h);
				field->slownessX = SmoothReached(field->slownessX, done, w, h);
				field->slownessY = SmoothReached(field->slownessY, done, w, h);
			}
		}

		// Beach slope at the waterline (bed smoothed over ~10 m), carried out along the rays and up the beach, so the
		// whole surf and swash zone of a beach shares one slope.
		{
			std::vector<float> smooth(bed);
			Blur(smooth, w, h, 4);
			std::vector<float> local(n, 1.0f);
			const std::vector<uint8_t> all(n, 1);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					float gx = 0.0f, gy = 0.0f;
					if (Gradient(smooth, all, w, h, static_cast<int>(x), static_cast<int>(y), gx, gy))
						local[static_cast<size_t>(y) * w + x] = std::sqrt(gx * gx + gy * gy) / in.step;
				}
			}
			field->slope.assign(n, 1.0f);
			std::vector<uint8_t> done(n, 0);
			for (size_t i = 0; i < n; ++i) {
				if ((water[i] && waterDistance[i] <= in.step) || (land[i] && landDistance[i] <= in.step)) {
					field->slope[i] = local[i];
					done[i] = 1;
				}
			}
			auto carry = [&](const std::vector<uint32_t>& order, float sign) {
				for (const uint32_t i : order) {
					if (done[i])
						continue;
					float value = 0.0f;
					const float x = static_cast<float>(i % w) + sign * rayX[i];
					const float y = static_cast<float>(i / w) + sign * rayY[i];
					field->slope[i] = InterpolateDone(field->slope, done, w, h, x, y, value) ? value : local[i];
					done[i] = 1;
				}
			};
			carry(seaward, 1.0f);
			carry(inland, -1.0f);
			for (size_t i = 0; i < n; ++i) {
				if (!done[i])
					field->slope[i] = local[i];
			}
		}

		// The slope, the exposure and the room are copied along rays and propagated texel by texel, so they jump where rays
		// from different stretches of shore meet (and the room's disc sweep draws octagons): they gate the surf, the swash
		// and the foam, which showed those seams as hard corners. Smoothed over ~4 m.
		Blur(field->slope, w, h, 4);
		Blur(field->exposure, w, h, 4);
		Blur(field->room, w, h, 4);
		return field;
	}

	// ========================================================================
	// Snapshot queries
	// ========================================================================

	namespace
	{
		/// Mirrors PBRWater::ValueNoiseGrad: the value noise and its gradient.
		void ValueNoiseGrad(float x, float y, float& value, float& gx, float& gy)
		{
			const float ix = std::floor(x), iy = std::floor(y);
			const float fx = x - ix, fy = y - iy;
			const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
			const float dux = 6.0f * fx * (1.0f - fx), duy = 6.0f * fy * (1.0f - fy);
			const float a = Hash12(ix, iy), b = Hash12(ix + 1.0f, iy), c = Hash12(ix, iy + 1.0f), d = Hash12(ix + 1.0f, iy + 1.0f);
			const float k = a - b - c + d;
			value = a + (b - a) * ux + (c - a) * uy + k * ux * uy;
			gx = dux * (b - a + k * uy);
			gy = duy * (c - a + k * ux);
		}

		/**
		 * Mirrors PBRWater::ShorePhase (Fluid Flux's MF_CoastlineProfileUV with MF_CoastlineOffsets): the phase at (x, y)
		 * and its gradient (rad/unit) for the waves travelling along (dirX, dirY).
		 */
		double ShorePhase(const ShoreParams& p, float tracedOmega, float x, float y, float coastX, float coastY, const ShoreField::Sample& ray, float dirX, float dirY, float slowness, float& gradX, float& gradY)
		{
			const float L0 = std::max(p.deepLength, 1.0f);
			const float drift = p.variationTime * 0.004f;
			float coarse, cx, cy, fine, fx, fy, across, ax, ay;
			ValueNoiseGrad(coastX / (4.0f * L0) + drift, coastY / (4.0f * L0), coarse, cx, cy);
			ValueNoiseGrad(coastX / (1.7f * L0) + 17.1f, coastY / (1.7f * L0) + 17.1f, fine, fx, fy);
			ValueNoiseGrad(x / (1.3f * L0) + 5.3f, y / (1.3f * L0) + drift * 1.5f, across, ax, ay);
			const float along = coarse * 0.67f + fine * 0.33f;
			const float offshoreRamp = ray.distance / L0 * 0.35f - 0.05f;
			const float offshore = std::clamp(offshoreRamp, 0.0f, 1.0f);
			const float alongScale = static_cast<float>(TwoPi) * 1.2f * p.variation;
			const float acrossScale = static_cast<float>(TwoPi) * 0.35f;
			float alongX = cx * (0.67f / (4.0f * L0)) + fx * (0.33f / (1.7f * L0));
			float alongY = cy * (0.67f / (4.0f * L0)) + fy * (0.33f / (1.7f * L0));
			const float alongDir = alongX * dirX + alongY * dirY;
			alongX -= dirX * alongDir;
			alongY -= dirY * alongDir;
			const float ramp = offshoreRamp > 0.0f && offshoreRamp < 1.0f ? 0.35f / L0 : 0.0f;
			gradX = -tracedOmega * slowness * dirX + alongScale * alongX + acrossScale * (offshore * ax / (1.3f * L0) - (across - 0.5f) * ramp * dirX);
			gradY = -tracedOmega * slowness * dirY + alongScale * alongY + acrossScale * (offshore * ay / (1.3f * L0) - (across - 0.5f) * ramp * dirY);
			return p.phase + static_cast<double>(ray.phaseLag) + alongScale * (along - 0.5f) + acrossScale * offshore * (across - 0.5f);
		}

		/// Mirrors PBRWater::ShoreShape.
		struct ShoreShape
		{
			float x = 0.0f, z = 0.0f, dx = 0.0f, dz = 0.0f;
		};

		/// Mirrors PBRWater::ShoreProfile: Airy orbits tilted forward, blending into the bore.
		ShoreShape ShoreProfile(double theta, float a, float ax, float tilt, float bore, float frontWidth)
		{
			const float s = static_cast<float>(std::sin(theta)), c = static_cast<float>(std::cos(theta));
			const float st = std::sin(tilt), ct = std::cos(tilt);
			const float X = ax * s, Z = a * c, dX = ax * c, dZ = -a * s;
			ShoreShape o;
			o.x = X * ct + Z * st;
			o.z = Z * ct - X * st;
			o.dx = dX * ct + dZ * st;
			o.dz = dZ * ct - dX * st;
			if (bore > 0.0f) {
				double v = theta / TwoPi;
				v -= std::floor(v);
				const float back = 1.0f - frontWidth;
				const bool onBack = v < back;
				const float rate = onBack ? 1.0f / back : 1.0f / frontWidth;
				const float u = onBack ? static_cast<float>(v) * rate : 1.0f + (static_cast<float>(v) - back) * rate;
				const float su = std::sin(static_cast<float>(Pi) * u), cu = std::cos(static_cast<float>(Pi) * u);
				o.x += (0.3f * ax * s - o.x) * bore;
				o.z += (a * cu - o.z) * bore;
				o.dx += (0.3f * ax * c - o.dx) * bore;
				o.dz += (-a * static_cast<float>(Pi) * su * rate / static_cast<float>(TwoPi) - o.dz) * bore;
			}
			return o;
		}

		/**
		 * Displacement (units) and velocity (units/s) of the shore wave train at (x, y), times the hand-over weight.
		 * Mirrors PBRWater::ShoreWaves in PBRWater.hlsli at full resolution (scale 0): the phase from the coastline map's
		 * travel time with Fluid Flux's offsets, the wave set's height shoaled up to the breaking limit, Airy orbits
		 * pitching forward as the wave nears breaking, then the bore. The velocity is the displacement's rate of change at
		 * the clock's frequency.
		 */
		WaveSnapshot::Displacement ShoreTrain(const ShoreParams& p, float tracedOmega, const ShoreTrainContext& t, const ShoreField::Sample& ray, float x, float y)
		{
			WaveSnapshot::Displacement out;
			const float slowness = std::sqrt(ray.slownessX * ray.slownessX + ray.slownessY * ray.slownessY);
			if (slowness <= 1e-9f || t.deepHeight <= 0.0f)
				return out;
			const float dirX = -ray.slownessX / slowness;
			const float dirY = -ray.slownessY / slowness;
			const float coastX = x + dirX * ray.distance;
			const float coastY = y + dirY * ray.distance;
			const float omega = std::max(tracedOmega, 1e-3f);
			const float depth = std::max(t.depth, 0.05f);
			// Slowness in s/unit, as the shaders read it.
			float gradX = 0.0f, gradY = 0.0f;
			const double theta = ShorePhase(p, omega, x, y, coastX, coastY, ray, dirX, dirY, slowness * static_cast<float>(MetresPerUnit), gradX, gradY);

			const float shoaled = ShoreHeightRatio * t.deepHeight * ShoreGroup(p, coastX, coastY, static_cast<float>(p.groupTime) + ray.travelTime) * t.shoaling;
			const float limit = BreakerIndex * depth;
			const float stage = shoaled / std::max(limit, 1e-4f);
			const float a = 0.5f * std::min(shoaled, limit);
			const float k = std::max(std::sqrt(gradX * gradX + gradY * gradY) * static_cast<float>(UnitsPerMetre), omega * omega / G);
			const float breaking = Smoothstep(0.75f, 1.0f, stage) * t.beach;
			const float bore = Smoothstep(1.0f, 1.4f, stage) * t.beach;
			const float lean = breaking * (1.0f - bore);
			const float ax = std::min(a / std::max(std::tanh(std::min(k * depth, 20.0f)), 1e-3f), ShoreOrbitLimit / k);
			const float tilt = 0.5f * std::atan(ray.slope) + 0.6f * lean;
			const ShoreShape shape = ShoreProfile(theta, a, ax, tilt, bore, 0.2f);

			const float rate = std::max(p.omega, 1e-3f);
			const float units = static_cast<float>(UnitsPerMetre) * t.weight;
			out.dx = dirX * shape.x * units;
			out.dy = dirY * shape.x * units;
			out.dz = shape.z * units;
			out.vx = dirX * shape.dx * rate * units;
			out.vy = dirY * shape.dx * rate * units;
			out.vz = shape.dz * rate * units;
			return out;
		}
	}

	ShoreField::Sample WaveSnapshot::ShoreAt(float x, float y) const
	{
		ShoreField::Sample sample = shoreField ? shoreField->At(x, y) : ShoreField::Sample{};
		// As PBRWater::SampleShore: a new map takes over from its predecessor's travel times, each field's lag at the
		// frequency it was traced for.
		if (shoreField && shorePrevious && shoreBlend < 1.0f) {
			const auto previous = shorePrevious->At(x, y);
			if (previous.weight > 0.0f) {
				sample.travelTime = previous.travelTime + (sample.travelTime - previous.travelTime) * shoreBlend;
				sample.phaseLag = previous.phaseLag + (sample.phaseLag - previous.phaseLag) * shoreBlend;
				sample.distance = previous.distance + (sample.distance - previous.distance) * shoreBlend;
				sample.weight = previous.weight + (sample.weight - previous.weight) * shoreBlend;
			}
		}
		return sample;
	}

	float WaveSnapshot::VerticalBound() const
	{
		const float waves = ocean && !ocean->calm ? ocean->displacementBound : 0.0f;
		// The shore train stays within the open sea's height (shoaled, but held to the depth); the swash adds its run-up.
		return 2.0f * waves + (shore.enabled ? shore.MaxDeepHeight() + shore.maxRunup : 0.0f);
	}

	WaveSnapshot::Context WaveSnapshot::BuildContext(double x, double y, float waterZ) const
	{
		Context ctx{};
		const float windSpeed = ocean ? ocean->windSpeed : 0.5f;
		const float openPeak = ocean ? ocean->peakOmega : 1.0f;
		const float fetchMetres = fetch ? fetch->SampleMetres(static_cast<float>(x), static_cast<float>(y), windDirection) * fetchScale : (exterior ? 500000.0f : 50.0f);
		const float peakOmega = FetchPeakOmega(fetchMetres, windSpeed, openPeak);
		const float energyGain = FetchEnergyGain(fetchMetres, windSpeed);
		ctx.fetchRatio = FetchHeightRatio(fetchMetres, windSpeed);
		ctx.horizontalScale = 1.0f / energyGain;
		ctx.depth = 1e6f;
		ctx.hasTerrain = false;

		const float fx = static_cast<float>(x);
		const float fy = static_cast<float>(y);
		// The exact terrain grid, faded into the coarse heightmap over its outer 2048 units (as PBRWater::TerrainHeight).
		const bool inLand = land && land->Contains(fx, fy);
		const bool inCoarse = bathymetry && bathymetry->Valid();
		if (inLand || inCoarse) {
			float bedZ = 0.0f;
			float landWeight = 0.0f;
			if (inLand) {
				// Distance (units) to the grid's border; its steps are positive (WaterEnvironment::UpdateLoadedLand).
				const float u = fx - land->minX;
				const float v = fy - land->minY;
				const float spanX = land->stepX * static_cast<float>(land->width - 1);
				const float spanY = land->stepY * static_cast<float>(land->height - 1);
				const float edge = std::min(std::min(u, spanX - u), std::min(v, spanY - v));
				landWeight = inCoarse ? std::clamp(edge / 2048.0f, 0.0f, 1.0f) : 1.0f;
				bedZ = land->Sample(fx, fy);
			}
			if (landWeight < 1.0f)
				bedZ = bathymetry->Sample(fx, fy) + (bedZ - bathymetry->Sample(fx, fy)) * landWeight;
			ctx.depth = std::max(waterZ - bedZ, 0.0f);
			ctx.hasTerrain = true;
		}

		// Same weighting as PBRWater::CascadeWeight in the shader (minus the render-only distance fade): the fetch-
		// limited spectrum, shoaling (Green's law) and depth-limited breaking.
		const float depthMetres = ctx.depth * static_cast<float>(MetresPerUnit);
		float variance = 0.0f;
		float deepVariance = 0.0f;
		for (uint32_t c = 0; c < NumCascades; ++c) {
			if (!ocean) {
				ctx.weight[c] = 0.0f;
				continue;
			}
			const CascadeBand& band = ocean->cascades[c];
			const float r = peakOmega / band.meanOmega;
			const float r2 = r * r;
			float weight = std::clamp(std::exp(-1.25f * r2 * r2) / std::max(band.pmWeightOpenSea, 1e-4f), 0.0f, 1.0f) * energyGain;
			deepVariance += weight * weight * band.variance;
			if (ctx.hasTerrain)
				weight *= ShoalingCoefficient(band.meanOmega, depthMetres);
			ctx.weight[c] = weight;
			variance += weight * weight * band.variance;
		}
		if (ctx.hasTerrain) {
			const float significant = 4.0f * std::sqrt(variance) * static_cast<float>(UnitsPerMetre);
			const float breaking = std::min(1.0f, SurfHeightRatio * ctx.depth / std::max(significant, 1e-3f));
			for (auto& weight : ctx.weight)
				weight *= breaking;
		}

		// The shore wave train (PBRWater::BuildWaveContext and ShoreWaves): it takes over from the FFT sea in water
		// shallower than half the deep-water wavelength, and the cascades carrying the same waves fade out as it comes
		// in, so the energy stays about the same and the short chop stays on top.
		if (shore.enabled && shoreField && ctx.hasTerrain && ocean) {
			const auto ray = ShoreAt(fx, fy);
			const float L0 = shore.deepLength * static_cast<float>(MetresPerUnit);
			const float weight = Smoothstep(0.5f, 0.15f, depthMetres / std::max(L0, 1e-3f)) * ray.weight * ray.room;
			if (weight > 0.0f) {
				ShoreTrainContext& t = ctx.train;
				t.weight = weight;
				t.depth = depthMetres;
				// The swell where this water is open to it, the wind sea as far as the fetch here (upwind open water) lets it
				// grow (as PBRWater::BuildWaveContext).
				const float swell = shore.swellHeight * ray.exposure;
				const float wind = shore.windSeaHeight * ctx.fetchRatio;
				t.deepHeight = std::sqrt(swell * swell + wind * wind) * static_cast<float>(MetresPerUnit);
				t.shoaling = ShoalingCoefficient(shoreField->omega, depthMetres);
				// Breaking or surging: the Iribarren number tan(beta) / sqrt(H0 / L0) of the local sea (Battjes 1974),
				// as ctx.beach in the shader.
				const float deepHeight = 4.0f * std::sqrt(deepVariance) * static_cast<float>(UnitsPerMetre);
				const float steepness = deepHeight * static_cast<float>(MetresPerUnit) / (static_cast<float>(TwoPi) * G / std::max(peakOmega * peakOmega, 1e-4f));
				const float iribarren = ray.slope / std::sqrt(std::max(steepness, 1e-4f));
				t.beach = Smoothstep(3.3f, 2.0f, iribarren) * ray.weight * ray.room * std::clamp(deepHeight - 1.0f, 0.0f, 1.0f);
				for (uint32_t c = 0; c < NumCascades; ++c) {
					const float overlap = std::clamp(1.0f - std::abs(std::log2(ocean->cascades[c].meanOmega / shoreField->omega)), 0.0f, 1.0f);
					ctx.weight[c] *= 1.0f - weight * overlap;
				}
			}
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
		if (ctx.train.weight > 0.0f && shoreField) {
			// The phase varies over a hull: travel times and directions are read at each point, the amplitude is the context's.
			const Displacement train = ShoreTrain(shore, shoreField->omega, ctx.train, ShoreAt(static_cast<float>(x), static_cast<float>(y)), static_cast<float>(x), static_cast<float>(y));
			out.dx += train.dx;
			out.dy += train.dy;
			out.dz += train.dz;
			out.vx += train.vx;
			out.vy += train.vy;
			out.vz += train.vz;
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

	void WaveClock::Advance(double dt, double x, double y, double z)
	{
		time += std::max(dt, 0.0);
		camX = x;
		camY = y;
		camZ = z;
	}

	void WaveClock::Fill(WaveSnapshot& snapshot) const
	{
		snapshot.time = time;
		snapshot.refX = camX;
		snapshot.refY = camY;
		snapshot.refZ = camZ;
	}
}
