// ============================================================================
// PBR Water - FFT ocean, step 2: inverse FFT along rows (or columns with VERTICAL).
//
// One thread group per line of one slice; the line lives in group shared memory and goes through
// log2(N) radix-2 Stockham stages (no bit reversal; natural order in and out). Each thread performs one
// butterfly per stage on a float4, i.e. on two packed complex fields at once. Unnormalised:
//     x_j = sum_m X_m e^(+2 pi i m j / N)
// so the spectrum amplitudes are the wave amplitudes. Reads one texture and writes another, because
// typed UAV loads of four-component formats are not guaranteed on D3D11 hardware.
//
// FFT_SIZE and FFT_LOG2 are compile-time defines (128, 256 or 512).
// ============================================================================

#ifndef FFT_SIZE
#	define FFT_SIZE 256
#	define FFT_LOG2 8
#endif

#define FFT_HALF (FFT_SIZE / 2)

Texture2DArray<float4> Source : register(t0);
RWTexture2DArray<float4> Destination : register(u0);

groupshared float4 Line[2][FFT_SIZE];

/// Two complex numbers (xy, zw) times the same twiddle w.
float4 TwiddleMul(float4 a, float2 w)
{
	return float4(a.x * w.x - a.y * w.y, a.x * w.y + a.y * w.x, a.z * w.x - a.w * w.y, a.z * w.y + a.w * w.x);
}

uint3 Address(uint index, uint lineIndex, uint slice)
{
#if defined(VERTICAL)
	return uint3(lineIndex, index, slice);
#else
	return uint3(index, lineIndex, slice);
#endif
}

[numthreads(FFT_HALF, 1, 1)] void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
	uint t = thread.x;
	uint lineIndex = group.y;
	uint slice = group.z;

	Line[0][t] = Source.Load(int4(Address(t, lineIndex, slice), 0));
	Line[0][t + FFT_HALF] = Source.Load(int4(Address(t + FFT_HALF, lineIndex, slice), 0));
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stage = 0; stage < FFT_LOG2; stage++)
	{
		uint src = stage & 1;
		uint p = 1u << stage;
		uint k = t & (p - 1);
		float2 w;
		sincos(3.14159265 * (float)k / (float)p, w.y, w.x);
		float4 u0 = Line[src][t];
		float4 u1 = TwiddleMul(Line[src][t + FFT_HALF], w);
		uint j = (t << 1) - k;
		Line[src ^ 1][j] = u0 + u1;
		Line[src ^ 1][j + p] = u0 - u1;
		GroupMemoryBarrierWithGroupSync();
	}

	uint result = FFT_LOG2 & 1;
	Destination[Address(t, lineIndex, slice)] = Line[result][t];
	Destination[Address(t + FFT_HALF, lineIndex, slice)] = Line[result][t + FFT_HALF];
}
