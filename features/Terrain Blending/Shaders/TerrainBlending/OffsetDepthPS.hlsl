// Second prepass draw of nearby terrain, into the main depth buffer. Pushing the surface back a
// fixed distance lets objects buried just under it survive the prepass so terrain can blend over
// them, while anything deeper stays depth-culled. Depth only ever moves away from the camera,
// so SV_DepthGreaterEqual keeps early depth rejection.

#include "Common/SharedData.hlsli"

// Larger than the 10 unit blend range in Lighting.hlsl, with headroom for depth quantisation,
// so bare terrain still resolves to a blend factor of exactly 1
static const float DepthOffset = 20.0;

struct PS_INPUT
{
	float4 Position : SV_POSITION0;
};

float main(PS_INPUT input) :
	SV_DepthGreaterEqual
{
	// Inverse of SharedData::GetScreenDepth
	float linearDepth = SharedData::GetScreenDepth(input.Position.z) + DepthOffset;
	float offsetDepth = (SharedData::CameraData.x - SharedData::CameraData.w / linearDepth) / SharedData::CameraData.z;
	return max(offsetDepth, input.Position.z);
}
