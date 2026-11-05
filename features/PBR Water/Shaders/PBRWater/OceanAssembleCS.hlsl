// ============================================================================
// PBR Water - FFT ocean, step 3: unpack the transforms into the textures the water shaders sample.
//
//   Displacement : xyz Lagrangian displacement (units), w dDx/dy
//   Derivatives  : xy height slopes dDz/dx, dDz/dy; zw compression dDx/dx, dDy/dy
//   Surface      : squared slopes (LEAN moments: mip-filtered, they keep the variance of the slopes a pixel averages
//                  away)
//
// Foam is not made per band: a band's crests alone are not where the summed surface breaks. The shaders read the
// Jacobian of the bands together (PBRWater::BreakingFoam).
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

Texture2DArray<float4> Spatial : register(t0);  // after the inverse FFT, two slices per cascade

RWTexture2DArray<float4> Displacement : register(u0);
RWTexture2DArray<float4> Derivatives : register(u1);
RWTexture2DArray<float2> Surface : register(u2);

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

	Displacement[id] = float4(displacement, shear);
	Derivatives[id] = float4(slopes, compression);
	Surface[id] = slopes * slopes;
}
