/**
 * @file ISTemporalAA_UI.hlsl
 * @brief Temporal anti-aliasing for UI/post-tonemapping pass.
 * 
 * Decompiled from vanilla Skyrim shader bytecode.
 * Modified to remove saturate clamps for HDR compatibility.
 */

#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
	float4 History : SV_Target1;
};

#if defined(PSHADER)

SamplerState SourceSampler : register(s0);
SamplerState HistorySampler : register(s1);
SamplerState MotionVectorsSampler : register(s2);
SamplerState DepthSampler : register(s3);
SamplerState MaskSampler : register(s4);

Texture2D<float4> SourceTex : register(t0);
Texture2D<float4> HistoryTex : register(t1);
Texture2D<float4> MotionVectorsTex : register(t2);
Texture2D<float4> DepthTex : register(t3);
Texture2D<float4> MaskTex : register(t4);

cbuffer PerGeometry : register(b2)
{
	float4 BlendParams : packoffset(c0);     // .x = motion blend factor
	float4 Unused1 : packoffset(c1);
	float4 NeighborWeights : packoffset(c2); // weights for neighbor sampling
	float4 TexelOffset : packoffset(c3);     // .xy = texel size offset
	float4 ClampParams : packoffset(c4);     // .xy = blend range, .zw = sharpen
};

float3 RGBToYCoCg(float3 rgb)
{
	float Y = dot(rgb, float3(0.25, 0.5, 0.25));
	float Co = dot(rgb, float3(0.5, 0.0, -0.5));
	float Cg = dot(rgb, float3(-0.25, 0.5, -0.25));
	return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg)
{
	float Y = ycocg.x;
	float Co = ycocg.y;
	float Cg = ycocg.z;
	return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float Luminance(float3 color)
{
	return dot(color.rgb, float3(0.5, 0.25, 0.25));
}

float2 GetDynamicResolutionUV(float2 uv)
{
	float2 drUV = uv * FrameBuffer::DynamicResolutionParams1.xy;
	drUV = max(drUV, 0.0);
	float2 maxUV = float2(FrameBuffer::DynamicResolutionParams2.z, FrameBuffer::DynamicResolutionParams1.y);
	return min(drUV, maxUV);
}

float2 GetPreviousDynamicResolutionUV(float2 uv)
{
	float2 drUV = uv * FrameBuffer::DynamicResolutionParams1.zw;
	drUV = max(drUV, 0.0);
	float2 maxUV = float2(FrameBuffer::DynamicResolutionParams2.w, FrameBuffer::DynamicResolutionParams1.w);
	return min(drUV, maxUV);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	
	float2 texCoord = input.TexCoord;
	float2 texelSize = TexelOffset.xy;
	
	// Sample 3x3 neighborhood for color clamping
	float2 offsets[9] = {
		float2(-1, -1), float2(0, -1), float2(1, -1),
		float2(-1,  0), float2(0,  0), float2(1,  0),
		float2(-1,  1), float2(0,  1), float2(1,  1)
	};
	
	float3 neighborColors[9];
	float neighborDepths[9];
	float neighborMasks[9];
	
	float3 minColor = 999999.0;
	float3 maxColor = -999999.0;
	float3 avgColor = 0.0;
	float minDepth = 999999.0;
	float2 closestUV = texCoord;
	
	[unroll]
	for (int i = 0; i < 9; i++)
	{
		float2 sampleUV = texCoord + offsets[i] * texelSize;
		float2 drUV = GetDynamicResolutionUV(sampleUV);
		
		neighborColors[i] = SourceTex.Sample(SourceSampler, drUV).rgb;
		neighborDepths[i] = DepthTex.Sample(DepthSampler, drUV).r;
		neighborMasks[i] = MaskTex.Sample(MaskSampler, drUV).r;
		
		// Find closest depth for velocity
		if (neighborDepths[i] < minDepth)
		{
			minDepth = neighborDepths[i];
			closestUV = sampleUV;
		}
		
		// Build AABB in YCoCg space for neighborhood clamping
		float3 ycocg = RGBToYCoCg(neighborColors[i]);
		minColor = min(minColor, ycocg);
		maxColor = max(maxColor, ycocg);
		avgColor += neighborColors[i];
	}
	
	avgColor /= 9.0;
	float3 centerColor = neighborColors[4];
	float centerMask = neighborMasks[4];
	
	// Sample motion vectors at closest depth location
	float2 motionUV = GetDynamicResolutionUV(closestUV);
	float2 motion = MotionVectorsTex.Sample(MotionVectorsSampler, motionUV).xy;
	float motionLength = length(motion);
	
	// Sample history
	float2 historyUV = texCoord + motion;
	float2 historyDRUV = GetPreviousDynamicResolutionUV(historyUV);
	float4 historyRaw = HistoryTex.Sample(HistorySampler, historyDRUV);
	float3 historyColor = historyRaw.rgb;
	float historyLuma = historyRaw.a;
	
	// Clamp history to neighborhood AABB (variance clipping)
	float3 historyYCoCg = RGBToYCoCg(historyColor);
	float3 clampedYCoCg = clamp(historyYCoCg, minColor, maxColor);
	float3 clampedHistory = YCoCgToRGB(clampedYCoCg);
	
	// Check for disocclusion via mask
	bool hasDisocclusion = false;
	[unroll]
	for (int j = 0; j < 9; j++)
	{
		if (neighborMasks[j] < 1.0)
		{
			hasDisocclusion = true;
			break;
		}
	}
	
	// Compute blend factor
	float blendFactor = 0.95;
	
	// Reduce blend when there's motion
	float motionBlend = saturate(motionLength * BlendParams.x * 128.0);
	blendFactor = lerp(blendFactor, 0.5, motionBlend);
	
	// Reduce blend when history was clamped significantly
	float clampAmount = length(historyYCoCg - clampedYCoCg);
	blendFactor = lerp(blendFactor, 0.0, saturate(clampAmount * 20.0));
	
	// Reset on disocclusion
	if (hasDisocclusion || centerMask < 0.5)
	{
		blendFactor = 0.0;
	}
	
	// Linearly blend between ranges
	float minBlend = ClampParams.y;
	float maxBlend = ClampParams.x;
	blendFactor = lerp(minBlend, maxBlend, blendFactor);
	
	// Final blend - NO SATURATE for HDR compatibility
	float3 result = lerp(centerColor, clampedHistory, blendFactor);
	
	// Optional sharpening 
	float3 sharpenDelta = centerColor - avgColor;
	result = result + sharpenDelta * ClampParams.z;
	
	// Anti-ringing: don't let sharpening push outside original bounds
	float3 antiRingDelta = result - clampedHistory;
	result = clampedHistory + antiRingDelta * ClampParams.w;
	
	// Output - (Vanilla saturates here, HDR need to allow values >1.0)
	psout.Color = float4(result, 1.0);
	
	// Store luminance in history alpha for next frame
	float newLuma = Luminance(result);
	float historyBlendLuma = lerp(newLuma, historyLuma, blendFactor);
	
	// Clamp history luminance update 
	if (abs(newLuma - historyLuma) * blendFactor < 0.01)
	{
		historyBlendLuma = newLuma;
	}
	
	psout.History = float4(blendFactor, blendFactor, motionBlend, 1.0);
	psout.History.x = saturate(historyBlendLuma);
	
	return psout;
}

#endif
