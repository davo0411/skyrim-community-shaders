// Contact Shadows Implementation
// Simple depth-buffer raymarching for local light occlusion
// Based on NVIDIA GPU Gems concepts adapted for per-light screen-space tracing

#ifndef __CONTACT_SHADOWS_HLSLI__
#define __CONTACT_SHADOWS_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

namespace ContactShadows
{
	// Sample depth buffer at screen position
	float SampleDepth(Texture2D<float4> depthTexture, SamplerState samplerState, float2 uv, uint eyeIndex)
	{
#if defined(VR)
		// VR: adjust UV for eye side
		if (eyeIndex == 1) {
			uv.x = uv.x * 0.5 + 0.5;
		} else {
			uv.x = uv.x * 0.5;
		}
#endif
		return depthTexture.SampleLevel(samplerState, uv, 0).x;
	}

	// Calculate contact shadow for a point light
	// Returns shadow term [0 = shadowed, 1 = lit]
	float CalculateContactShadow(
		Texture2D<float4> depthTexture,
		SamplerState samplerState,
		float3 worldPosition,
		float3 lightPosition,
		float lightDistance,
		SharedData::ContactShadowSettings settings,
		uint eyeIndex)
	{
		if (!settings.Enabled)
			return 1.0;

		// Early out if light is too far
		float maxTraceDist = min(settings.MaxDistance, lightDistance);
		if (maxTraceDist < 0.01)
			return 1.0;

		// Transform positions to view space
		float3 viewPos = FrameBuffer::WorldToView(worldPosition, true, eyeIndex);
		float3 lightViewPos = FrameBuffer::WorldToView(lightPosition, true, eyeIndex);
		
		// Direction from surface to light in view space
		float3 viewLightDir = lightViewPos - viewPos;
		float viewLightDist = length(viewLightDir);
		viewLightDir = normalize(viewLightDir);

		// Calculate step size based on distance and max steps
		float stepSize = maxTraceDist / float(settings.MaxSteps);
		
		// Start slightly offset from surface to avoid self-occlusion
		float3 currentViewPos = viewPos + viewLightDir * stepSize * 0.5;
		
		float shadow = 1.0;
		
		[unroll]
		for (uint step = 0; step < settings.MaxSteps; step++)
		{
			// Check if we've reached the light
			float distanceToLight = length(lightViewPos - currentViewPos);
			if (distanceToLight < stepSize)
				break;
			
			// Project to screen space
			float2 uv = FrameBuffer::ViewToUV(currentViewPos, true, eyeIndex);
			
			// Check bounds
			if (FrameBuffer::IsOutsideFrame(uv, true))
				break;
			
			// Sample depth at this screen position
			float sampledDepth = SampleDepth(depthTexture, samplerState, uv, eyeIndex);
			
			// Convert ray position to depth
			float4 clipPos = mul(FrameBuffer::CameraProj[eyeIndex], float4(currentViewPos, 1.0));
			float rayDepth = clipPos.z / clipPos.w;
			
			// In standard depth: 0=near, 1=far
			// If ray depth > sampled depth, ray is further away (behind geometry)
			float depthDiff = sampledDepth - rayDepth;
			
			// If ray is behind geometry (sampledDepth < rayDepth) and within thickness
			if (depthDiff < 0 && abs(depthDiff) < settings.Thickness)
			{
				// Apply softness for smoother transitions
				float occlusionStrength = saturate(abs(depthDiff) / settings.Thickness);
				shadow = lerp(shadow, 0.0, occlusionStrength * settings.Softness);
				
				// For hard shadows, exit early
				if (settings.Softness >= 1.0)
					break;
			}
			
			// March forward
			currentViewPos += viewLightDir * stepSize;
		}
		
		// Apply distance fade
		if (settings.DistanceFade > 0.0)
		{
			float fadeFactor = saturate(lightDistance / settings.DistanceFade);
			shadow = lerp(shadow, 1.0, fadeFactor);
		}
		
		return shadow;
	}

	// Optimized version with early termination
	float CalculateContactShadowFast(
		Texture2D<float4> depthTexture,
		SamplerState samplerState,
		float3 worldPosition,
		float3 lightPosition,
		float lightDistance,
		SharedData::ContactShadowSettings settings,
		uint eyeIndex)
	{
		if (!settings.Enabled)
			return 1.0;

		// Early out if light is too far
		float maxTraceDist = min(settings.MaxDistance, lightDistance);
		if (maxTraceDist < 0.01)
			return 1.0;

		// World space ray direction toward light
		float3 worldRayDir = normalize(lightPosition - worldPosition);
		
		// Start raymarching slightly offset from surface to avoid self-intersection
		float startOffset = 0.1;  // 10cm offset
		float stepSize = (maxTraceDist - startOffset) / float(settings.MaxSteps);
		
		float shadowTerm = 1.0;
		int hitCount = 0;
		
		[loop]
		for (uint step = 1; step <= settings.MaxSteps; step++)
		{
			// March in world space, starting from offset position
			float marchDist = startOffset + stepSize * float(step);
			float3 sampleWorldPos = worldPosition + worldRayDir * marchDist;
			
			// Transform sample position to clip space using UNJITTERED projection
			// This prevents TAA jitter from causing flickering shadows
			float4 sampleClipPos = mul(FrameBuffer::CameraViewProjUnjittered[eyeIndex], float4(sampleWorldPos, 1.0));
			
			// Check if behind camera
			if (sampleClipPos.w <= 0.0)
				break;
			
			// Perspective divide to NDC
			float3 sampleNDC = sampleClipPos.xyz / sampleClipPos.w;
			
			// Convert NDC to UV
			float2 sampleUV = sampleNDC.xy * float2(0.5, -0.5) + 0.5;
			
			// Check screen bounds
			if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
				break;
			
			// Adjust UV for dynamic resolution (DLSS/FSR/etc)
			// This ensures shadows work correctly at display resolution, not render resolution
			sampleUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(sampleUV);
			
			// Sample depth buffer at adjusted coordinates
			float sceneDepth = depthTexture.SampleLevel(samplerState, sampleUV, 0).x;
			
			// Get ray depth in [0,1] range
			float rayDepth = sampleNDC.z;
			
			// Depth comparison: rayDepth > sceneDepth means ray is behind geometry (occluded)
			float depthDiff = rayDepth - sceneDepth;
			
			// Check for occlusion - simple and clean like debug mode
			if (depthDiff > 0.0 && depthDiff < settings.Thickness)
			{
				// Found occlusion
				return 0.0;
			}
		}
		
		// No occlusion found - fully lit
		return 1.0;
	}
}

#endif  // __CONTACT_SHADOWS_HLSLI__
