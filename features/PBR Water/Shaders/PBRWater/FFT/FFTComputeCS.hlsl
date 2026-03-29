#include "PBRWater/FFT/FFTCommon.hlsli"

// log2(FFT_MAP_SIZE); must match FFTButterflyCS dispatch (Y groups) and shared row size.
#ifndef FFT_NUM_STAGES
#define FFT_NUM_STAGES 8
#endif

// Stockham FFT kernel — coalesced decimation-in-time using shared memory.
// Applied row-wise. A transpose pass + second row-wise pass gives the 2D FFT.
// Based on Okahisa's OTFFT DIT Stockham algorithm.

StructuredBuffer<float4> ButterflyFactors : register(t0);

RWStructuredBuffer<float2> FFTBuffer : register(u0);

groupshared float2 rowShared[2 * FFT_MAP_SIZE];

uint DataInIndex(uint3 id, uint layer)
{
	uint mapSize = FFT_MAP_SIZE;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		0 +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

uint DataOutIndex(uint3 id, uint layer)
{
	uint mapSize = FFT_MAP_SIZE;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		FFT_NUM_SPECTRA * mapSize * mapSize +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

[numthreads(FFT_MAP_SIZE, 1, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	uint col = GTid.x;
	uint3 id = uint3(col, Gid.y, FFTCascadeIndex);
	uint spectrum = Gid.z;

	rowShared[col] = FFTBuffer[DataInIndex(id, spectrum)];

	[unroll]
	for (uint stage = 0; stage < FFT_NUM_STAGES; stage++) {
		GroupMemoryBarrierWithGroupSync();

		uint2 bufIdx = uint2(stage & 1u, (stage + 1u) & 1u);
		float4 bf = ButterflyFactors[stage * FFT_MAP_SIZE + col];

		uint2 readIdx = uint2(uint(bf.x), uint(bf.y));
		float2 twiddle = bf.zw;

		float2 upper = rowShared[bufIdx.x * FFT_MAP_SIZE + readIdx.x];
		float2 lower = rowShared[bufIdx.x * FFT_MAP_SIZE + readIdx.y];

		rowShared[bufIdx.y * FFT_MAP_SIZE + col] = upper + ComplexMul(lower, twiddle);
	}

	FFTBuffer[DataOutIndex(id, spectrum)] = rowShared[(FFT_NUM_STAGES & 1u) * FFT_MAP_SIZE + col];
}
