#include "PBRWater/FFT/FFTCommon.hlsli"

// Memory-efficient coalesced matrix transpose with shared-memory tiling.
// Uses TILE_SIZE+1 column stride to avoid bank conflicts.
// Source: NVIDIA "Efficient Matrix Transpose in CUDA C/C++"

#define TRANSPOSE_TILE_SIZE 32

StructuredBuffer<float4> ButterflyFactors : register(t0);

RWStructuredBuffer<float2> FFTBuffer : register(u0);

groupshared float2 tile[TRANSPOSE_TILE_SIZE][TRANSPOSE_TILE_SIZE + 1];

uint DataInIndex(uint3 id, uint layer)
{
	uint mapSize = FFTMapSize;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		FFT_NUM_SPECTRA * mapSize * mapSize +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

uint DataOutIndex(uint3 id, uint layer)
{
	uint mapSize = FFTMapSize;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		0 +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

[numthreads(TRANSPOSE_TILE_SIZE, TRANSPOSE_TILE_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID, uint3 Gid : SV_GroupID)
{
	uint spectrum = DTid.z;
	uint3 readId = uint3(DTid.xy, FFTCascadeIndex);

	tile[GTid.y][GTid.x] = FFTBuffer[DataInIndex(readId, spectrum)];

	GroupMemoryBarrierWithGroupSync();

	uint3 writeId = uint3(Gid.yx * TRANSPOSE_TILE_SIZE + GTid.xy, FFTCascadeIndex);
	FFTBuffer[DataOutIndex(writeId, spectrum)] = tile[GTid.x][GTid.y];
}
