// ============================================================================
// PBR Water - FFT ocean, step 3: unpack the transforms into the textures the water shaders sample.
//
//   Displacement : xyz Lagrangian displacement (units), w dDx/dy
//   Derivatives  : xy height slopes dDz/dx, dDz/dy; zw compression dDx/dx, dDy/dy
//   Surface      : squared slopes (LEAN moments: mip-filtered, they keep the variance of the slopes a pixel averages
//                  away)
//
//   Foam         : one persistent value per cascade texel, accumulated from the Jacobian of the cascade's own horizontal
//                  displacement. A direct port of CS_AssembleMaps' foam in GarrettGunnell/Water (Acerola,
//                  https://github.com/GarrettGunnell/Water, MIT License, Copyright (c) 2023 Garrett Gunnell):
//                      foam *= exp(-decay); foam = saturate(foam);
//                      biased = max(0, -(jacobian - bias)); if (biased > threshold) foam += add * biased;
//                  The rates are per 1/60 s there (one step per frame); here they scale by the frame time, so 60 fps is
//                  identical and a paused game (frame time 0) neither grows nor decays the foam.
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

Texture2DArray<float4> Spatial : register(t0);  // after the inverse FFT, two slices per cascade

RWTexture2DArray<float4> Displacement : register(u0);
RWTexture2DArray<float4> Derivatives : register(u1);
RWTexture2DArray<float2> Surface : register(u2);
RWTexture2DArray<float> FoamField : register(u3);  // persistent: read, decayed and added to in place

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

	// Jacobian of x -> x + D(x) from this cascade's own gradient (choppiness is already in the spectrum).
	float steps = Time.w;  // frame time in 1/60 s: 0 while paused
	[branch] if (steps > 0.0)
	{
		float jacobian = (1.0 + compression.x) * (1.0 + compression.y) - shear * shear;
		float foam = FoamField[id] * exp(-Foam.w * steps);
		foam = saturate(foam);
		float biasedJacobian = max(0.0, -(jacobian - Foam.x));
		if (biasedJacobian > Foam.y)
			foam += Foam.z * steps * biasedJacobian;
		FoamField[id] = foam;
	}
}
