// ============================================================================
// PBR Water - the water around the camera under the surface (PBRWater::DrawUnderwaterVolume), in place of the game's
// underwater fog (PBRWater zeroes each water form's underwater fog amount).
//
// Drawn once over the screen before the water's own passes while the camera is under water. What every pixel shows lies
// some distance d away through the water: its light reaches the camera attenuated per channel (Beer-Lambert,
// T = exp(-sigma d), with the surface's extinction: Optics.hlsli Extinction, pure water's absorption its floor), and the
// water along the way scatters in its own colour, lit by the sun and the sky through the surface down to the camera's
// depth: L = L_scene T + L_water (1 - T). Dual-source blending: SV_Target0 is added, SV_Target1 multiplies what is there.
// The surface seen from below draws over its own pixels afterwards with the same water (Water.hlsl, UNDERWATER).
// ============================================================================

#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

cbuffer UnderwaterCB : register(b0)
{
	float4 Extinction;  // xyz 1/unit, w the camera's depth under the surface (units)
	float4 Body;        // xyz the water's colour (linear, the water form's deep colour), w unused
	float4 Viewport;    // xy the target's size (pixels), z the farthest the water is crossed (units)
};

Texture2D<float> Depth : register(t0);

struct VSOutput
{
	float4 position : SV_Position;
};

VSOutput VS(uint id : SV_VertexID)
{
	VSOutput o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

struct PSOutput
{
	float4 added : SV_Target0;
	float4 multiplied : SV_Target1;
};

PSOutput PS(VSOutput input)
{
	float raw = Depth.Load(int3(input.position.xy, 0));
	float2 uv = input.position.xy / Viewport.xy;
	float4 p = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), raw, 1.0));
	float distance = min(length(p.xyz / p.w), Viewport.z);
	float3 transmittance = exp(-Extinction.xyz * distance);

	// The light in the water here: the sun's and the sky's through the surface, down to the camera's depth.
	float3 sun = Color::DirectionalLight(SharedData::DirLightColor.xyz) * saturate(SharedData::DirLightDirection.z);
	float3 sky = Color::Ambient(max(0.0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0))));
	float3 light = (sun + sky) * exp(-Extinction.xyz * Extinction.w);
	float3 scattered = Body.xyz * light * (1.0 - transmittance);

	PSOutput o;
	o.added = float4(scattered * SharedData::PreExposure, 0.0);
	o.multiplied = float4(transmittance, 1.0);
	return o;
}
