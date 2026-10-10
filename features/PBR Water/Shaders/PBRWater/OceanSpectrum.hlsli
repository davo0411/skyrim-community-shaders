#ifndef __PBR_WATER_OCEAN_SPECTRUM_HLSLI__
#define __PBR_WATER_OCEAN_SPECTRUM_HLSLI__

// ============================================================================
// PBR Water - FFT ocean spectrum, shared by the ocean compute shaders.
//
// Mirrors the CPU code in src/Features/PBRWater/WaveModel.cpp (OceanSpectrum::Amplitude, OceanNoise,
// DispersionSteps): the CPU re-synthesises the long-wave cascades for gameplay from the same random
// coefficients, so the two must stay in sync.
//
//   Spectrum   : JONSWAP (Hasselmann et al. 1973) for the wind sea plus a narrow JONSWAP swell
//   Spreading  : cos^2s(theta / 2) (Longuet-Higgins 1963) with the Mitsuyasu et al. (1975) exponent
//   Synthesis  : Tessendorf, "Simulating Ocean Water" (2001), with frequencies quantised to a loop period
// ============================================================================

cbuffer OceanCB : register(b0)
{
	float4 Wind;         // xy wind direction, z open-sea peak omega (rad/s), w height scale
	float4 Spread;       // x Mitsuyasu exponent at the peak, y choppiness (lambda), z swell alpha, w swell peak omega (rad/s)
	float4 Swell;        // xy swell direction, z gravity (m/s^2), w omega0 = 2 pi / loop period (rad/s)
	float4 Time;         // x loop fraction (0..1), y fft size, z units per metre, w unused
	float4 Band[4];      // x tile size (m), y band start, z band end, w anti-aliasing fade start (rad/m, 0 = none)
	float4 Foam0;        // x dt (s), y foam trail retained this step, z crest foam threshold (Jacobian), w unused
	float4 FoamBand[4];  // x 1 / std of the other bands' compression, yz foam drift this step (uv), w unused
};

static const float OceanPi = 3.14159265;
static const float OceanJonswapAlpha = 0.0081;
static const float OceanJonswapGamma = 3.3;
static const float OceanSwellGamma = 7.0;
static const float OceanSwellSpread = 30.0;

uint OceanHash(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

/// ln(Gamma(x)) for x >= 1 (Stirling series after shifting x above 7).
float OceanLogGamma(float x)
{
	float product = 1.0;
	[unroll] for (int i = 0; i < 6; i++)
	{
		if (x < 7.0) {
			product *= x;
			x += 1.0;
		}
	}
	float z = 1.0 / (x * x);
	return (x - 0.5) * log(x) - x + 0.9189385 + (0.0833333 - z * (0.00277778 - z * 0.000793651)) / x - log(product);
}

/// Normalisation of cos^2s(theta / 2) over [-pi, pi].
float OceanSpreadingNorm(float s)
{
	return exp((2.0 * s - 1.0) * 0.6931472 - 1.1447299 + 2.0 * OceanLogGamma(s + 1.0) - OceanLogGamma(2.0 * s + 1.0));
}

float OceanSpreading(float cosAngle, float s)
{
	return OceanSpreadingNorm(s) * pow(max(0.5 + 0.5 * cosAngle, 0.0), s);
}

float OceanMitsuyasuExponent(float omega, float peak, float spreadPeak)
{
	float x = omega / peak;
	float s = x <= 1.0 ? spreadPeak * x * x * x * x * x : spreadPeak * pow(abs(x), -2.5);
	return clamp(s, 1.0, 60.0);
}

float OceanJonswap(float omega, float peak, float alpha, float gamma)
{
	float g = Swell.z;
	float sigma = omega <= peak ? 0.07 : 0.09;
	float d = omega - peak;
	float r = exp(-d * d / (2.0 * sigma * sigma * peak * peak));
	float p = peak / omega;
	float p2 = p * p;
	float o2 = omega * omega;
	return alpha * g * g / (o2 * o2 * omega) * exp(-1.25 * p2 * p2) * pow(gamma, r);
}

/// Amplitude (m) of the wave travelling along k (rad/m) on a lattice of spacing dk, before the random coefficient.
float OceanAmplitude(float2 k, float dk, uint cascade)
{
	float4 band = Band[cascade];
	float kl = length(k);
	// Points exactly on a band start must fall on the same side as on the CPU: test with a small margin.
	if (kl <= 0.0 || kl < band.y * 0.9999 || kl >= band.z * 0.9999)
		return 0.0;
	float g = Swell.z;
	float omega = sqrt(g * kl);
	float cosWind = dot(k, Wind.xy) / kl;
	float density = OceanJonswap(omega, Wind.z, OceanJonswapAlpha, OceanJonswapGamma) * Wind.w * Wind.w *
	                OceanSpreading(cosWind, OceanMitsuyasuExponent(omega, Wind.z, Spread.x));
	if (Spread.z > 0.0) {
		float cosSwell = dot(k, Swell.xy) / kl;
		density += OceanJonswap(omega, Spread.w, Spread.z, OceanSwellGamma) * OceanSpreading(cosSwell, OceanSwellSpread);
	}
	float energy = density * g / (2.0 * omega * kl);
	float amplitude = sqrt(max(energy, 0.0) * dk * dk * 0.5);
	if (band.w > 0.0)
		amplitude *= 1.0 - smoothstep(band.w, band.z, kl);
	return amplitude;
}

/// Standard complex Gaussian (E|xi|^2 = 1) for lattice index (m, n) of a cascade.
float2 OceanNoise(int m, int n, uint cascade)
{
	uint key = ((uint)(m + 32768) & 0xFFFFu) | (((uint)(n + 32768) & 0xFFFFu) << 16);
	uint h1 = OceanHash(key ^ (cascade * 0x68E31DA4u));
	uint h2 = OceanHash(h1 + 0x9E3779B9u);
	float u1 = ((float)(h1 >> 8) + 0.5) * (1.0 / 16777216.0);
	float u2 = (float)(h2 >> 8) * (1.0 / 16777216.0);
	float r = sqrt(-2.0 * log(u1)) * 0.70710678;
	float s, c;
	sincos(6.2831853 * u2, s, c);
	return float2(r * c, r * s);
}

/// Quantised angular frequency of wavenumber k (rad/m), in multiples of omega0.
float OceanDispersionSteps(float k)
{
	return max(floor(sqrt(Swell.z * k) / Swell.w + 0.5), 1.0);
}

float2 CMul(float2 a, float2 b)
{
	return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

#endif  // __PBR_WATER_OCEAN_SPECTRUM_HLSLI__
