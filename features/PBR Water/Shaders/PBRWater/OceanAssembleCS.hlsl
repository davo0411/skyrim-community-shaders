// ============================================================================
// PBR Water - FFT ocean, step 3: unpack the transforms into the textures the water shaders sample.
//
//   Displacement : xyz Lagrangian displacement (units), w dDx/dy
//   Derivatives  : xy height slopes dDz/dx, dDz/dy; zw compression dDx/dx, dDy/dy
//   Surface      : xy squared slopes (LEAN moments: mip-filtered, they keep the variance of the slopes a
//                  pixel averages away), z crest foam trail, w |laplacian of the height| (1/unit, tessellation)
//
// Crest foam: where a crest folds (Jacobian below the threshold) it leaves foam that thins over the trail
// time and drifts with the surface current. Each cascade only knows its own band, so it stores the
// probability that the *whole* surface folds there, given its own compression and the spread of all the
// other bands (Gaussian): sharp where one band dominates, a uniform low level where it is minor. The foam
// lives in the cascade's tile, i.e. in Lagrangian coordinates, so it rides the waves it came from.
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

Texture2DArray<float4> Spatial : register(t0);      // after the inverse FFT, two slices per cascade
Texture2DArray<float> FoamPrevious : register(t1);  // trail of the previous frame
SamplerState WrapSampler : register(s0);

RWTexture2DArray<float4> Displacement : register(u0);
RWTexture2DArray<float4> Derivatives : register(u1);
RWTexture2DArray<float4> Surface : register(u2);
RWTexture2DArray<float> FoamCurrent : register(u3);

float2 SlopesAt(int2 p, uint size, uint cascade)
{
	uint2 q = uint2(p + (int)size) % size;
	return Spatial.Load(int4(q, cascade * 2 + 1, 0)).xy;
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint size = (uint)Time.y;
	if (any(id.xy >= size))
		return;
	uint cascade = id.z;
	float4 first = Spatial.Load(int4(id.xy, cascade * 2, 0));
	float4 second = Spatial.Load(int4(id.xy, cascade * 2 + 1, 0));

	float unitsPerMetre = Time.z;
	float3 displacement = float3(first.x, first.y, first.z) * unitsPerMetre;
	float shear = first.w;
	float2 slopes = second.xy;
	float2 compression = second.zw;
	float jacobian = (1.0 + compression.x) * (1.0 + compression.y) - shear * shear;

	// Laplacian of the height from the neighbouring slopes (periodic tile).
	float texel = Band[cascade].x * unitsPerMetre / (float)size;
	int2 p = int2(id.xy);
	float dxx = SlopesAt(p + int2(1, 0), size, cascade).x - SlopesAt(p - int2(1, 0), size, cascade).x;
	float dyy = SlopesAt(p + int2(0, 1), size, cascade).y - SlopesAt(p - int2(0, 1), size, cascade).y;
	float curvature = abs(dxx + dyy) / (2.0 * texel);

	// Probability that the total surface folds here (logistic approximation of the normal CDF).
	float x = (Foam0.z - jacobian) * FoamBand[cascade].x;
	float folding = 1.0 / (1.0 + exp(-1.702 * clamp(x, -20.0, 20.0)));
	float2 uv = (float2(id.xy) + 0.5) / (float)size - FoamBand[cascade].yz;
	float trail = FoamPrevious.SampleLevel(WrapSampler, float3(uv, cascade), 0) * Foam0.y;
	float foam = saturate(max(trail, folding));

	FoamCurrent[id] = foam;
	Displacement[id] = float4(displacement, shear);
	Derivatives[id] = float4(slopes, compression);
	Surface[id] = float4(slopes * slopes, foam, curvature);
}
