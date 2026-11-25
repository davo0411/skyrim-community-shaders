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
//                  It drifts: the wind drags the surface water along at a few percent of its speed, and the foam with it
//                  while the crests that made it run on. The field slides over the cascade by whole texels as the drift
//                  adds up (FoamShift): world texel id holds foam texel id - floor(shift), so each thread still owns one
//                  texel and nothing is resampled (no blur), only moved. The foam the pixels read (Surface.w) is then taken
//                  between texels at the drift's fraction (FOAM_DRIFT, a second dispatch once the field is updated): the
//                  longest cascade's texels are metres wide, and whole-texel steps were seen as jumps.
// ============================================================================

#include "PBRWater/OceanSpectrum.hlsli"

Texture2DArray<float4> Spatial : register(t0);  // after the inverse FFT, two slices per cascade

RWTexture2DArray<float4> Displacement : register(u0);
RWTexture2DArray<float4> Derivatives : register(u1);
RWTexture2DArray<float4> Surface : register(u2);  // x height (units), y shear dDx/dy, z |slope|^2 (LEAN), w foam: the pixels' one read
RWTexture2DArray<float> FoamField : register(u3);  // persistent: read, decayed and added to in place

#if !defined(FOAM_DRIFT)
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint size = (uint)Time.y;
	if (any(id.xy >= size))
		return;
	uint cascade = id.z;
	// Skipped cascades keep last frame's displacement, derivatives, surface and foam.
	if (((uint)SwellShape.z >> cascade & 1) == 0)
		return;
	float4 first = Spatial.Load(int4(id.xy, cascade * 2, 0));
	float4 second = Spatial.Load(int4(id.xy, cascade * 2 + 1, 0));

	float unitsPerMetre = Time.z;
	float3 displacement = float3(first.x, first.y, first.z) * unitsPerMetre;
	float shear = first.w;
	float2 slopes = second.xy;
	float2 compression = second.zw;

	Displacement[id] = float4(displacement, shear);
	Derivatives[id] = float4(slopes, compression);

	// Jacobian of x -> x + D(x) from this cascade's own gradient (choppiness is already in the spectrum).
	float steps = Time.w;  // frame time in 1/60 s: 0 while paused
	float4 shifts = FoamShift[cascade >> 1];
	int2 shift = int2(floor((cascade & 1) ? shifts.zw : shifts.xy));
	uint3 f = uint3((int2(id.xy) - shift + int(size)) % int(size), id.z);
	float foamNow = FoamField[f];
	[branch] if (steps > 0.0)
	{
		float jacobian = (1.0 + compression.x) * (1.0 + compression.y) - shear * shear;
		float foam = FoamField[f] * exp(-Foam.w * steps);
		foam = saturate(foam);
		float biasedJacobian = max(0.0, -(jacobian - Foam.x));
		if (biasedJacobian > Foam.y)
			foam += Foam.z * steps * biasedJacobian;
		FoamField[f] = foam;
		foamNow = foam;
	}
	Surface[id] = float4(displacement.z, shear, dot(slopes, slopes), foamNow);
}
#else
/// The foam the pixels read, at the drift's fraction between the field's texels (the field holds whole-texel steps).
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint size = (uint)Time.y;
	if (any(id.xy >= size))
		return;
	uint cascade = id.z;
	if (((uint)SwellShape.z >> cascade & 1) == 0)
		return;
	float4 shifts = FoamShift[cascade >> 1];
	float2 drift = (cascade & 1) ? shifts.zw : shifts.xy;
	float2 whole = floor(drift);
	float2 t = drift - whole;
	// World texel id shows the foam that was at id - drift: between field texels id - whole and one further upwind.
	int s = int(size);
	int2 base = int2(id.xy) - int2(whole) + s;
	float f00 = FoamField[uint3(base % s, cascade)];
	float f10 = FoamField[uint3((base - int2(1, 0)) % s, cascade)];
	float f01 = FoamField[uint3((base - int2(0, 1)) % s, cascade)];
	float f11 = FoamField[uint3((base - int2(1, 1)) % s, cascade)];
	float4 surface = Surface[id];
	surface.w = lerp(lerp(f00, f10, t.x), lerp(f01, f11, t.x), t.y);
	Surface[id] = surface;
}
#endif
