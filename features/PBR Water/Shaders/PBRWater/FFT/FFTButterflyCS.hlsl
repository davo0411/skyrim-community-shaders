#include "PBRWater/FFT/FFTCommon.hlsli"

RWStructuredBuffer<float4> ButterflyFactors : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint col = DTid.x;
	uint stage = DTid.y;
	uint mapSize = FFTMapSize;

	uint stride = 1u << stage;
	uint mid = mapSize >> (stage + 1u);
	uint i = col >> stage;
	uint j = col % stride;

	float2 twiddleFactor = ComplexExp(FFT_PI / float(stride) * float(j));

	uint r0 = stride * (i + 0u) + j;
	uint r1 = stride * (i + mid) + j;
	uint w0 = stride * (2u * i + 0u) + j;
	uint w1 = stride * (2u * i + 1u) + j;

	float2 readIndices = float2(float(r0), float(r1));

	ButterflyFactors[stage * mapSize + w0] = float4(readIndices, twiddleFactor);
	ButterflyFactors[stage * mapSize + w1] = float4(readIndices, -twiddleFactor);
}
