// Second z-prepass draw of nearby terrain, into the main depth buffer.
// Pushes the terrain back by a fixed linear distance so that objects buried just under the
// surface survive the prepass and get blended, while anything deeper is depth-culled as usual.
// Only ever pushes depth further away, so SV_DepthGreaterEqual keeps early depth rejection.

#include "Common/SharedData.hlsli"

// Must exceed the 10-unit blend range in Lighting.hlsl (blendFactorTerrain), with margin for
// depth-buffer quantisation, so open terrain still resolves to a blend factor of exactly 1.
static const float TerrainBlendDepthOffset = 20.0;

struct PS_INPUT
{
	float4 Position : SV_POSITION0;
};

float main(PS_INPUT input) :
	SV_DepthGreaterEqual
{
	float linearDepth = SharedData::GetScreenDepth(input.Position.z) + TerrainBlendDepthOffset;
	float offsetDepth = (SharedData::CameraData.x - SharedData::CameraData.w / linearDepth) / SharedData::CameraData.z;
	return max(offsetDepth, input.Position.z);
}
