// ============================================================================
// PBR Water - the low-frequency surface noise around the camera, baked once per frame.
//
// Drifting sediment patches and wind gusts are tens of metres across, but their noise (TurbidityPatches, WindGust: some
// thirty value-noise evaluations) was evaluated by every vertex, which with tessellation is every vertex the tessellator
// generates, in every pass. Here each texel of a camera-centred grid evaluates it once; the vertices read it back with
// one bilinear lookup (PBRWater::SurfaceNoise), and beyond the grid evaluate it themselves.
//
// Output: Noise (x TurbidityPatches, y WindGust).
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

RWTexture2D<float2> Noise : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint w, h;
	Noise.GetDimensions(w, h);
	if (any(id.xy >= uint2(w, h)))
		return;
	// The texel centre, as the vertices' bilinear lookups see it.
	float2 absXY = PBRWater::Noise0.xy + (float2(id.xy) + 0.5) / (PBRWater::Noise0.z * float2(w, h));
	Noise[id.xy] = float2(PBRWater::TurbidityPatches(absXY), PBRWater::WindGust(absXY));
}
