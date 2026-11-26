// ============================================================================
// PBR Water - the water around the camera under the surface (PBRWater::DrawUnderwaterVolume), in place of the game's
// underwater fog (PBRWater zeroes each water form's underwater fog amount): the composite over the scene.
//
// Drawn once over the screen before the water's own passes while the camera is under water. Every pixel's light is
// attenuated over its distance through the water (exact, per pixel) and the water's in-scattering is added: the shafts'
// half-resolution result (UnderwaterShaftsCS.hlsl) upsampled jointly with the depth (Effects 11's ApplyVolumetricRaysPS:
// bilinear taps weighted by distance similarity, so edges take the nearest surface's light), or in closed form without
// shafts. Dual-source blending: SV_Target0 is added, SV_Target1 multiplies what is there. The surface seen from below
// draws over its own pixels afterwards with the same water (Water.hlsl, UNDERWATER).
// ============================================================================

struct VSOutput
{
	float4 position : SV_Position;
};

#if defined(VSHADER)
// The full-screen triangle (the vertex stage takes none of the lighting headers).
VSOutput VS(uint id : SV_VertexID)
{
	VSOutput o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}
#else

#include "PBRWater/UnderwaterLight.hlsli"

Texture2D<float> Depth : register(t0);
Texture2D<float4> ShaftLight : register(t1);  // UnderwaterShaftsCS: rgb in-scattered light, a distance (units)

struct PSOutput
{
	float4 added : SV_Target0;
	float4 multiplied : SV_Target1;
};

/// The shafts' light at full-resolution pixel `pixel` of distance `distance`: the four nearest half-resolution texels,
/// bilinear weights times their distances' similarity.
float3 UpsampleShafts(float2 pixel, float distance)
{
	float2 halfPixel = pixel * 0.5 - 0.5;
	int2 base = int2(floor(halfPixel));
	float2 f = halfPixel - base;
	const int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
	float4 bilinear = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
	int2 last = int2(Half.xy) - 1;
	float3 sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		float4 tap = ShaftLight[clamp(base + offsets[i], 0, last)];
		float weight = bilinear[i] * rcp(0.01 + abs(distance - tap.a) / max(distance, 1e-4));
		sum += weight * tap.rgb;
		weightSum += weight;
	}
	return sum / max(weightSum, 1e-6);
}

PSOutput PS(VSOutput input)
{
	float raw = Depth.Load(int3(input.position.xy, 0));
	float3 scene = UnderwaterScenePoint(input.position.xy, raw);
	float distance = min(length(scene), Viewport.z);
	float3 transmittance = exp(-Extinction.xyz * distance);
	float3 scattered = Shafts.x > 0.5 ? UpsampleShafts(input.position.xy, distance) : UnderwaterScatteringClosed(distance);

	PSOutput o;
	o.added = float4(scattered * SharedData::PreExposure, 0.0);
	o.multiplied = float4(transmittance, 1.0);
	return o;
}

/// The shafts' march at half resolution (drawn into UnderwaterShaftsCS's input, half-size viewport): one pixel of every
/// 2x2, a different one each frame (Half.zw).
float4 MarchPS(VSOutput input) : SV_Target0
{
	float2 halfPixel = floor(input.position.xy);
	float2 pixel = min(halfPixel * 2.0 + Half.zw * 2.0 + 0.5, Viewport.xy - 0.5);
	float raw = Depth.Load(int3(pixel, 0));
	float3 scene = UnderwaterScenePoint(pixel, raw);
	float distance = min(length(scene), Viewport.z);
	float3 viewDirection = scene / max(length(scene), 1e-4);
	float jitter = Random::InterleavedGradientNoise(halfPixel, SharedData::FrameCount);
	return float4(UnderwaterScatteringMarched(viewDirection, distance, jitter), distance);
}
#endif
