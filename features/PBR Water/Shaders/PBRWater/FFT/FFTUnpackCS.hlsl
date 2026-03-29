#include "PBRWater/FFT/FFTCommon.hlsli"

// Unpacks the IFFT outputs into displacement and normal/foam textures.
// Foam is computed from the Jacobian of horizontal displacement:
// negative Jacobian = wave folding = whitecap generation (Tessendorf).

#define UNPACK_TILE_SIZE 16

RWTexture2DArray<float4> DisplacementMap : register(u0);
RWTexture2DArray<float4> NormalFoamMap : register(u1);

RWStructuredBuffer<float2> FFTBuffer : register(u2);

uint FFTDataIndex(uint3 id, uint layer)
{
	uint mapSize = FFTMapSize;
	return id.z * mapSize * mapSize * FFT_NUM_SPECTRA * 2 +
		FFT_NUM_SPECTRA * mapSize * mapSize +
		layer * mapSize * mapSize +
		id.y * mapSize + id.x;
}

groupshared float2 localTile[FFT_NUM_SPECTRA][UNPACK_TILE_SIZE][UNPACK_TILE_SIZE];

[numthreads(UNPACK_TILE_SIZE, UNPACK_TILE_SIZE, 1)]
void main(uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID)
{
	uint3 id = uint3(DTid.xy, FFTCascadeIndex);

	// ifftshift: multiply by (-1)^(x+y) to center the spectrum
	float signShift = -2.0f * float((id.x & 1u) ^ (id.y & 1u)) + 1.0f;

	[unroll]
	for (uint s = 0; s < FFT_NUM_SPECTRA; ++s)
		localTile[s][GTid.y][GTid.x] = FFTBuffer[FFTDataIndex(id, s)];

	GroupMemoryBarrierWithGroupSync();

	// Displacement: hx, hy, hz (packed as real parts of paired signals)
	float hx = localTile[0][GTid.y][GTid.x].x;
	float hy = localTile[0][GTid.y][GTid.x].y;
	float hz = localTile[1][GTid.y][GTid.x].x;

	DisplacementMap[id] = float4(hx, hy, hz, 0.0f) * signShift;

	// Gradient / Jacobian terms
	float dhyDx = localTile[1][GTid.y][GTid.x].y * signShift;
	float dhyDz = localTile[2][GTid.y][GTid.x].x * signShift;
	float dhxDx = localTile[2][GTid.y][GTid.x].y * signShift;
	float dhzDz = localTile[3][GTid.y][GTid.x].x * signShift;
	float dhzDx = localTile[3][GTid.y][GTid.x].y * signShift;

	// Jacobian determinant of the horizontal displacement field
	float jacobian = (1.0f + dhxDx) * (1.0f + dhzDz) - dhzDx * dhzDx;

	// Foam: negative Jacobian = wave crest folding over
	float foamFactor = -min(0.0f, jacobian - FFTWhitecap);

	float4 existingNF = NormalFoamMap[id];
	float foam = existingNF.w;
	foam *= exp(-FFTFoamDecayRate);
	foam += foamFactor * FFTFoamGrowRate;
	foam = saturate(foam);

	// Gradient corrected for horizontal displacement
	float2 gradient = float2(dhyDx, dhyDz) / (1.0f + abs(float2(dhxDx, dhzDz)));

	NormalFoamMap[id] = float4(gradient, dhxDx, foam);
}
