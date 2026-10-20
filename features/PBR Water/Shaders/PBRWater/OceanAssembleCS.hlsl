// ============================================================================
// PBR Water - FFT ocean, step 3: unpack the transforms into the textures the water shaders sample.
//
//   Displacement : xyz Lagrangian displacement (units), w dDx/dy
//   Derivatives  : xy height slopes dDz/dx, dDz/dy; zw compression dDx/dx, dDy/dy
//   Surface      : xy squared slopes (LEAN moments: mip-filtered, they keep the variance of the slopes a
//                  pixel averages away), z whitecap foam, w |laplacian of the height| (1/unit, tessellation)
//
// Whitecap foam is GodotOceanWaves' Jacobian foam (2Retr0, MIT; after Tessendorf 2001): where the surface
// compresses past the whitecap threshold (the Jacobian of the horizontal displacement falls below it) foam grows,
// and everywhere it decays exponentially, so a breaking crest leaves a trail that thins out behind it. The growth is
// Crest's saturated form (wave-harmonic/crest, MIT, UpdateFoam.compute): it reaches its full rate a little past
// the threshold instead of growing without bound on folds. Each band accumulates its own foam in its own tile, i.e.
// in Lagrangian coordinates, so the foam rides the waves that made it, and drifts downwind with the surface current.
// The threshold is set on the CPU from the wind (PBRWaterModel::CascadeWhitecaps); the shaders weight the bands by
// how much they carry the dominant waves at a location.
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

Texture2DArray<float4> Spatial : register(t0);      // after the inverse FFT, two slices per cascade
Texture2DArray<float> FoamPrevious : register(t1);  // foam of the previous step
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

	// Jacobian foam: last step's foam (drifted downwind) decays, and grows where the crest is compressed past the
	// whitecap threshold.
	float2 uv = (float2(id.xy) + 0.5) / (float)size - FoamBand[cascade].yz;
	float previous = FoamPrevious.SampleLevel(WrapSampler, float3(uv, cascade), 0);
	float growth = saturate((FoamBand[cascade].x - jacobian) * FoamBand[cascade].w);
	float foam = saturate(previous * Foam0.y + growth * Foam0.z);

	FoamCurrent[id] = foam;
	Displacement[id] = float4(displacement, shear);
	Derivatives[id] = float4(slopes, compression);
	Surface[id] = float4(slopes * slopes, foam, curvature);
}
