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
	static const float SHADOW_THICKNESS = 0.1;

	// Calculate contact shadow for a point light
	// Returns shadow term [0 = shadowed, 1 = lit]
	float CalculateContactShadow(
		Texture2D<float4> depthTexture,
		SamplerState samplerState,
		float3 worldPosition,
		float3 lightPosition,
		float lightDistance,
		SharedData::ContactShadowSettings settings,
		uint eyeIndex,
		float shadowMapValue = 1.0,
		float2 screenPosition = float2(0, 0),
		float3 surfaceNormal = float3(0, 0, 1))
	{
		if (!settings.Enabled)
			return 1.0;

		// Fix first-person lights - skip contact shadows when looking at player model
		if (!FrameBuffer::FrameParams.y)
			return 1.0;

		// Skip if already fully shadowed by shadow map
		if (shadowMapValue < 0.01)
			return 0.0;

		// Early out if light is too far
		float maxTraceDist = min(settings.MaxDistance, lightDistance);
		if (maxTraceDist < 0.01)
			return 1.0;

		// Ray direction from surface TOWARD light
		float3 worldRayDir = normalize(lightPosition - worldPosition);
		
		// Start raymarching slightly offset from surface
		float startOffset = 0.05;  // 5cm offset
		
		// Calculate step size with minimum to prevent floating-point precision issues
		float stepSize = max(0.02, (maxTraceDist - startOffset) / float(settings.MaxSteps));
		
		// Track shadow accumulation for soft shadows
		float shadowAccum = 1.0;
		bool foundOcclusion = false;
		
		[loop]
		for (uint step = 1; step <= settings.MaxSteps; step++)
		{
			// March from surface toward light
			float marchDist = startOffset + stepSize * float(step);
			
			// Stop if we've reached the light
			if (marchDist >= lightDistance)
				break;
			
			float3 sampleWorldPos = worldPosition + worldRayDir * marchDist;
			
			// Transform sample position to clip space using UNJITTERED projection
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
			
			// Adjust UV for dynamic resolution
			sampleUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(sampleUV);
			
			// Sample depth buffer at half-resolution for performance
			// Use mip level 1 (half-res) for distant samples
			float mipLevel = (lightDistance > settings.MaxDistance * 0.3) ? 1.0 : 0.0;
			float sceneDepth = depthTexture.SampleLevel(samplerState, sampleUV, mipLevel).x;
			
			// Skip stencil/sky
			if (sceneDepth == 0.0 || sceneDepth == 1.0)
				continue;
			
			// Get ray depth
			float rayDepth = sampleNDC.z;
			
			// Check if scene geometry is closer (in front of ray)
			float depthDiff = rayDepth - sceneDepth;
			
			// Check for occlusion
			if (depthDiff > 0.0 && depthDiff < SHADOW_THICKNESS)
			{
				// Edge detection: Check neighboring depths to detect geometry boundaries
				// Sample a small cross pattern around the occlusion point
				float2 texelSize = 1.0 / float2(1920.0, 1080.0); // Approximate screen resolution
				float depthRight = depthTexture.SampleLevel(samplerState, sampleUV + float2(texelSize.x * 2.0, 0), mipLevel).x;
				float depthLeft = depthTexture.SampleLevel(samplerState, sampleUV + float2(-texelSize.x * 2.0, 0), mipLevel).x;
				float depthUp = depthTexture.SampleLevel(samplerState, sampleUV + float2(0, -texelSize.y * 2.0), mipLevel).x;
				float depthDown = depthTexture.SampleLevel(samplerState, sampleUV + float2(0, texelSize.y * 2.0), mipLevel).x;
				
				// Calculate max depth variation in neighborhood
				float maxDepthVariation = max(
					max(abs(sceneDepth - depthRight), abs(sceneDepth - depthLeft)),
					max(abs(sceneDepth - depthUp), abs(sceneDepth - depthDown))
				);
				
				// If there's a significant depth discontinuity, this is likely a geometry edge
				// Skip shadowing at edges to prevent cross-geometry artifacts
				float edgeThreshold = 0.002; // 0.2% depth variation threshold
				if (maxDepthVariation > edgeThreshold)
					continue;
				
				foundOcclusion = true;
				
				// Calculate normalized depth difference (0 = at occluder, 1 = at edge)
				float normalizedDepth = saturate(depthDiff / SHADOW_THICKNESS);
				
				// Apply contrast boost for softness control
				// Higher contrast = sharper shadows, lower = softer
				// Softness 0.0 = contrast 8.0 (very sharp)
				// Softness 1.0 = contrast 1.0 (very soft)
				float contrast = lerp(8.0, 1.0, settings.Softness);
				
				// Apply contrast formula (same as screen-space shadows)
				// This creates a remapped shadow value with adjustable falloff
				float shadowValue = saturate(normalizedDepth * contrast + (1.0 - contrast));
				
				// Accumulate darkest shadow (invert so 0 = shadow, 1 = light)
				shadowAccum = min(shadowAccum, shadowValue);
			}
		}
		
		// Return accumulated shadow value
		// 1.0 = fully lit, 0.0 = fully shadowed
		return foundOcclusion ? shadowAccum : 1.0;
	}
}

#endif  // __CONTACT_SHADOWS_HLSLI__
