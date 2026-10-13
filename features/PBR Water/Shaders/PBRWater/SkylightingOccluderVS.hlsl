// ============================================================================
// PBR Water - the displaced water surface in Skylighting's occlusion maps.
//
// Skylighting finds how much sky each probe sees from depth maps rendered from the sky, one random direction a
// frame, averaged over many frames. Drawing the waves into those maps makes them occlude the sky like any other
// geometry: a point low in a trough sees less sky past the crests around it than a crest does. The water reads
// Skylighting just above its own surface (Water.hlsl); anything under the still water reads it at the surface
// (Skylighting::LiftOutOfWater), as its sky light comes in through the water.
//
// A camera-centred grid over the still water level of the cells around the eye, displaced like the water mesh
// (waves, swash), drawn depth-only with Skylighting's occlusion transform. Grid points in cells without water go
// far below everything, so they occlude nothing.
// ============================================================================

#include "PBRWater/PBRWater.hlsli"

cbuffer SkylightingOccluderCB : register(b0)
{
	row_major float4x4 OcclusionViewProj;  // Skylighting's: eye-relative position -> occlusion clip space
	float4 Eye;                            // xyz absolute eye position, w grid extent (units)
	float4 Grid;                           // x quads per side, y spacing (units), zw absolute cell of the eye
	float4 CellLevels[25];                 // x absolute still water level of cell (eye cell + (i - 2, j - 2)) at i + 5 j, < -1e8: no water
};

Texture2D<float4> FlowMap : register(t8);  // Unified Water's worldspace flowmap (PBRWater::Flow0)

/// River current calms the waves, as on the water mesh (Water.hlsl GetWaveFlowDamping).
float FlowDamping(float2 absXY)
{
	float damping = 1.0;
	if (PBRWater::Flow0.x != 0.0) {
		float4 flow = FlowMap.SampleLevel(PBRWater::LinearClampSampler, absXY * PBRWater::Flow0.xz + PBRWater::Flow0.yw, 0);
		damping = 1.0 - PBRWater::Foam1.w * saturate(flow.z * flow.w);
	}
	return damping;
}

struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
};

VS_OUTPUT main(uint id : SV_VertexID)
{
	uint verts = (uint)Grid.x + 1;
	float2 g = float2(id % verts, id / verts);
	// Whole grid steps in world space, so the grid does not swim as the eye moves.
	float2 absXY = (floor(Eye.xy / Grid.y) + g - 0.5 * Grid.x) * Grid.y;

	int2 cell = (int2)floor(absXY / 4096.0) - (int2)Grid.zw + 2;
	float level = all(cell >= 0) && all(cell < 5) ? CellLevels[clamp(cell.x, 0, 4) + 5 * clamp(cell.y, 0, 4)].x : -1e9;
	bool water = level > -1e8;

	float3 absPos = float3(absXY, water ? level : Eye.z);
	float3 positionWS = absPos - FrameBuffer::CameraPosAdjust.xyz;
	float damping = PBRWater::DisplacementDistanceFade(length(absPos - Eye.xyz)) * FlowDamping(absXY);
	PBRWater::WaveContext ctx = PBRWater::BuildWaveContext(positionWS, damping);
	float3 displacement = PBRWater::EvaluateWaves(positionWS, ctx, Grid.y, false).displacement;
	if (ctx.beach > 0.0)
		displacement.z = max(displacement.z, PBRWater::SwashTop(ctx.elevation, PBRWater::SwashTip(positionWS, ctx, false).x));
	float limit = PBRWater::DisplacementLimit();
	displacement = clamp(displacement, -limit, limit);

	// No water: far below anything a probe could be.
	float3 eyeRelative = water ? absPos + displacement - Eye.xyz : float3(absXY - Eye.xy, -1e6);
	VS_OUTPUT o;
	o.Position = mul(OcclusionViewProj, float4(eyeRelative, 1.0));
	return o;
}
