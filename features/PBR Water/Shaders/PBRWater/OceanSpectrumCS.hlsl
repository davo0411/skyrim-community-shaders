// ============================================================================
// PBR Water - FFT ocean, step 1: the spectrum at the current time.
//
// For every lattice point k of every cascade, h(k, t) = h0(k) e^(-i w t) + conj(h0(-k)) e^(+i w t)
// (Tessendorf 2001), then the eight real fields the surface needs are packed two per complex IFFT:
//   slice 2c     : xy  Dx + i Dy      (horizontal displacement, choppy waves)
//                  zw  Dz + i dDx/dy  (height, shear)
//   slice 2c + 1 : xy  dDz/dx + i dDz/dy (slopes)
//                  zw  dDx/dx + i dDy/dy (compression)
// Every one of them is Hermitian, so each packed transform yields both fields as its real and imaginary
// parts. The lattice is in standard FFT order (index i >= N/2 is frequency i - N), so the inverse
// transform needs no shift. Units: metres; the assembly pass converts to game units.
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

RWTexture2DArray<float4> Spectrum : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint size = (uint)Time.y;
	if (any(id.xy >= size))
		return;
	uint cascade = id.z;
	int half = (int)(size >> 1);
	int m = id.x < (uint)half ? (int)id.x : (int)id.x - (int)size;
	int n = id.y < (uint)half ? (int)id.y : (int)id.y - (int)size;

	float4 first = 0.0;
	float4 second = 0.0;
	// The Nyquist row and column have no conjugate partner on the lattice; leave them (and k = 0) empty.
	if (m != -half && n != -half && (m != 0 || n != 0)) {
		float dk = 2.0 * OceanPi / Band[cascade].x;
		float2 k = float2(m, n) * dk;
		float a = OceanAmplitude(k, dk, cascade);
		float am = OceanAmplitude(-k, dk, cascade);
		if (a > 0.0 || am > 0.0) {
			float kl = length(k);
			float2 h0 = OceanNoise(m, n, cascade) * a;
			float2 h0m = OceanNoise(-m, -n, cascade) * am;

			// Frequency quantised to the loop period: the phase is a whole number of turns per loop.
			float phase = 2.0 * OceanPi * frac(OceanDispersionSteps(kl) * Time.x);
			float s, c;
			sincos(phase, s, c);
			float2 h = CMul(h0, float2(c, -s)) + CMul(float2(h0m.x, -h0m.y), float2(c, s));

			float lambda = Spread.y;
			float2 dir = k / kl;
			float2 dxy = lambda * CMul(h, float2(-dir.y, dir.x));                      // lambda H (i kx - ky) / k
			float2 dzShear = CMul(h, float2(1.0, -lambda * k.x * dir.y));              // H (1 - i lambda kx ky / k)
			float2 slopes = CMul(h, float2(-k.y, k.x));                                // H (i kx - ky)
			float2 compression = -lambda * CMul(h, float2(k.x * dir.x, k.y * dir.y));  // -lambda H (kx^2 + i ky^2) / k
			first = float4(dxy, dzShear);
			second = float4(slopes, compression);
		}
	}
	Spectrum[uint3(id.xy, cascade * 2)] = first;
	Spectrum[uint3(id.xy, cascade * 2 + 1)] = second;
}
