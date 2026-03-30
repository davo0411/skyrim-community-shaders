#ifndef __FFT_COMMON_HLSLI__
#define __FFT_COMMON_HLSLI__

// Ocean FFT pipeline aligned with Tessendorf + Horvath TMA/JONSWAP + Stockham FFT,
// same overall structure as GodotOceanWaves (compute spectrum → modulate → 2D FFT → unpack).
// Reference: https://github.com/2Retr0/GodotOceanWaves

#define FFT_PI 3.14159265358979323846f
#define FFT_TWO_PI 6.28318530717958647692f
#define FFT_GRAVITY 9.81f
#define FFT_MAP_SIZE 256
#define FFT_NUM_STAGES 8
#define FFT_NUM_CASCADES 3
#define FFT_NUM_SPECTRA 4

cbuffer FFTParams : register(b6)
{
	uint FFTMapSize;
	uint FFTCascadeIndex;
	float FFTTime;
	float FFTDeltaTime;

	float2 FFTTileLength;
	float FFTDepth;
	float FFTAlpha;

	float FFTPeakFrequency;
	float FFTWindSpeed;
	float FFTWindDirection;
	float FFTSwell;

	float FFTDetail;
	float FFTSpread;
	float FFTWhitecap;
	float FFTFoamGrowRate;

	float FFTFoamDecayRate;
	float FFTChoppiness;
	float FFTDisplacementScale;
	float FFTNormalScale;

	int2 FFTSpectrumSeed;
	float FFTPad0;
	float FFTPad1;
};

float2 ComplexMul(float2 a, float2 b)
{
	return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 ComplexConj(float2 z)
{
	return float2(z.x, -z.y);
}

float2 ComplexExp(float x)
{
	float s, c;
	sincos(x, s, c);
	return float2(c, s);
}

#endif
