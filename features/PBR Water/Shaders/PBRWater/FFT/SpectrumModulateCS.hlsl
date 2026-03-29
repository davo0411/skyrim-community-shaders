#include "PBRWater/FFT/FFTCommon.hlsli"

// Time-evolves the ocean spectrum and packs displacement + gradient derivatives
// into 4 complex pairs for real-valued FFT. Because h respects the conjugation
// property, we can pack two real signals per complex FFT (Matusiak 2001).

Texture2DArray<float4> SpectrumTex : register(t0);

RWStructuredBuffer<float2> FFTBuffer : register(u0);

float DispersionRelation(float k)
{
	return sqrt(FFT_GRAVITY * k * tanh(k * FFTDepth));
}

uint FFTIndex(uint3 id, uint layer)
{
	uint mapSize = FFTMapSize;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint mapSize = FFTMapSize;
	int2 dims = int2(mapSize, mapSize);
	uint3 id = uint3(DTid.xy, FFTCascadeIndex);

	float2 kVec = (float2(int2(id.xy) - dims / 2)) * FFT_TWO_PI / FFTTileLength;
	float k = length(kVec) + 1e-6f;
	float2 kUnit = kVec / k;

	float4 h0 = SpectrumTex[int3(id.xy, id.z)];
	float dispersion = DispersionRelation(k) * FFTTime;
	float2 modulation = ComplexExp(dispersion);

	float2 h = ComplexMul(h0.xy, modulation) + ComplexMul(h0.zw, ComplexConj(modulation));
	float2 hInv = float2(-h.y, h.x);

	// Displacement components
	float2 hx = hInv * kUnit.y;
	float2 hy = h;
	float2 hz = hInv * kUnit.x;

	// Gradient derivatives for normals and Jacobian
	float2 dhyDx = hInv * kVec.y;
	float2 dhyDz = hInv * kVec.x;
	float2 dhxDx = -h * kVec.y * kUnit.y;
	float2 dhzDz = -h * kVec.x * kUnit.x;
	float2 dhzDx = -h * kVec.y * kUnit.x;

	// Pack two real signals per complex slot (conjugation symmetry)
	FFTBuffer[FFTIndex(id, 0)] = float2(hx.x - hy.y, hx.y + hy.x);
	FFTBuffer[FFTIndex(id, 1)] = float2(hz.x - dhyDx.y, hz.y + dhyDx.x);
	FFTBuffer[FFTIndex(id, 2)] = float2(dhyDz.x - dhxDx.y, dhyDz.y + dhxDx.x);
	FFTBuffer[FFTIndex(id, 3)] = float2(dhzDz.x - dhzDx.y, dhzDz.y + dhzDx.x);
}
