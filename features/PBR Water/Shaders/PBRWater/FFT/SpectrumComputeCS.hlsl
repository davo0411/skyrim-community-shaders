#include "PBRWater/FFT/FFTCommon.hlsli"

// Generates 2D JONSWAP/TMA wave spectrum with Hasselmann directional spreading.
// Adapted from Tessendorf "Simulating Ocean Water" and
// Horvath "Empirical Directional Wave Spectra for Computer Graphics".

RWTexture2DArray<float4> SpectrumTex : register(u0);

// --- Hash / RNG ---

uint WangHash(uint seed)
{
	seed = (seed ^ 61u) ^ (seed >> 16u);
	seed *= 9u;
	seed = seed ^ (seed >> 4u);
	seed *= 0x27d4eb2du;
	seed = seed ^ (seed >> 15u);
	return seed;
}

float2 HashToUniform(uint2 coord)
{
	uint h = WangHash(coord.y + 374761393u + coord.x * 3266489917u);
	h = 2246822519u * (h ^ (h >> 15u));
	h = 3266489917u * (h ^ (h >> 13u));
	uint n = h ^ (h >> 16u);
	uint2 rz = uint2(n, n * 48271u);
	return float2((rz.xy >> 1u) & 0x7FFFFFFFu) / float(0x7FFFFFFF);
}

float2 BoxMullerGaussian(float2 u)
{
	float r = sqrt(-2.0f * log(max(u.x, 1e-10f)));
	float theta = FFT_TWO_PI * u.y;
	float s, c;
	sincos(theta, s, c);
	return float2(r * c, r * s);
}

// --- Dispersion ---

float2 DispersionRelation(float k)
{
	float a = k * FFTDepth;
	float th = tanh(a);
	float omega = sqrt(FFT_GRAVITY * k * th);
	float dOmega = 0.5f * FFT_GRAVITY * (th + a * (1.0f - th * th)) / omega;
	return float2(omega, dOmega);
}

// --- Directional spreading (Hasselmann + swell) ---

float LHNormalization(float s)
{
	float a = sqrt(s);
	return (s < 0.4f)
		? (0.5f / FFT_PI) + s * (0.220636f + s * (-0.109f + s * 0.090f))
		: rsqrt(FFT_PI) * (a * 0.5f + (1.0f / a) * 0.0625f);
}

float LonguetHiggins(float s, float theta)
{
	return LHNormalization(s) * pow(abs(cos(theta * 0.5f)), 2.0f * s);
}

float HasselmannSpread(float w, float wPeak, float windSpeed, float theta)
{
	float p = w / wPeak;
	float s;
	if (w <= wPeak)
		s = 6.97f * pow(abs(p), 4.06f);
	else
		s = 9.77f * pow(abs(p), -2.33f - 1.45f * (windSpeed * wPeak / FFT_GRAVITY - 1.17f));

	float sXi = 16.0f * tanh(wPeak / w) * FFTSwell * FFTSwell;
	return LonguetHiggins(s + sXi, theta - FFTWindDirection);
}

// --- TMA spectrum (JONSWAP + depth attenuation) ---

float TMASpectrum(float w, float wPeak, float alpha)
{
	const float beta = 1.25f;
	const float gamma = 3.3f;

	float sigma = (w <= wPeak) ? 0.07f : 0.09f;
	float r = exp(-(w - wPeak) * (w - wPeak) / (2.0f * sigma * sigma * wPeak * wPeak));
	float jonswap = (alpha * FFT_GRAVITY * FFT_GRAVITY) / pow(w, 5.0f) *
		exp(-beta * pow(wPeak / w, 4.0f)) * pow(gamma, r);

	float wH = min(w * sqrt(FFTDepth / FFT_GRAVITY), 2.0f);
	float kitaigorodskii = (wH <= 1.0f)
		? 0.5f * wH * wH
		: 1.0f - 0.5f * (2.0f - wH) * (2.0f - wH);

	return jonswap * kitaigorodskii;
}

float2 GetSpectrumAmplitude(int2 id, int2 mapDims)
{
	// Zero-mean sea surface: DC bin blows up TMA/JONSWAP (~1/w^5) and adds a huge constant offset.
	int2 halfDims = mapDims / 2;
	if (id.x == halfDims.x && id.y == halfDims.y)
		return float2(0.0f, 0.0f);

	float2 dk = FFT_TWO_PI / FFTTileLength;
	float2 kVec = (float2(id) - float2(mapDims) * 0.5f) * dk;
	float k = length(kVec) + 1e-6f;
	float theta = atan2(kVec.x, kVec.y);

	float2 disp = DispersionRelation(k);
	float w = disp.x;
	float wNorm = disp.y / k * dk.x * dk.y;

	float S = TMASpectrum(w, FFTPeakFrequency, FFTAlpha);
	float D = lerp(0.5f / FFT_PI,
		HasselmannSpread(w, FFTPeakFrequency, FFTWindSpeed, theta),
		1.0f - FFTSpread) *
		exp(-(1.0f - FFTDetail) * (1.0f - FFTDetail) * k * k);

	return BoxMullerGaussian(HashToUniform(uint2(id + FFTSpectrumSeed))) * sqrt(2.0f * S * D * wNorm);
}

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	int2 dims = int2(FFTMapSize, FFTMapSize);
	int3 id = int3(DTid.xy, FFTCascadeIndex);
	int2 id0 = id.xy;
	int2 id1 = int2(((uint2)(-id0)) % (uint2)dims);

	float2 h0k = GetSpectrumAmplitude(id0, dims);
	float2 h0mk = ComplexConj(GetSpectrumAmplitude(id1, dims));

	SpectrumTex[id] = float4(h0k, h0mk);
}
