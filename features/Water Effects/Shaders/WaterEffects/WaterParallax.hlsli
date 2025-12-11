namespace WaterEffects
{
	// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
	// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h

	// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
	// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
	// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

	float GetMipLevel(float2 coords, Texture2D<float4> tex)
	{
		// Compute the current gradients:
		float2 textureDims;
		tex.GetDimensions(textureDims.x, textureDims.y);

#if defined(VR)
		textureDims /= 16.0;
#else
		textureDims /= 8.0;
#endif

		float2 texCoordsPerSize = coords * textureDims;

		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		// Find min of change in u and v across quad: compute du and dv magnitude across quad
		float2 dTexCoords = dxSize * dxSize + dySize * dySize;

		// Standard mipmapping uses max here
		float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);

		// Compute the current mip level  (* 0.5 is effectively computing a square root before )
		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

		return mipLevel;
	}

	float GetHeight(PS_INPUT input, float2 currentOffset, float3 normalScalesRcp, float3 mipLevels)
	{
		float3 heights;
		heights.x = Normals01Tex.SampleLevel(Normals01Sampler, input.TexCoord1.xy + currentOffset * normalScalesRcp.x, mipLevels.x).w;
		heights.y = Normals02Tex.SampleLevel(Normals02Sampler, input.TexCoord1.zw + currentOffset * normalScalesRcp.y, mipLevels.y).w;
		heights.z = Normals03Tex.SampleLevel(Normals03Sampler, input.TexCoord2.xy + currentOffset * normalScalesRcp.z, mipLevels.z).w;
		heights *= NormalsAmplitude.xyz;
		return 1.0 - (heights.x + heights.y + heights.z);
	}

	float2 GetParallaxOffset(PS_INPUT input, float3 normalScalesRcp)
	{
		float3 viewDirection = normalize(input.WPosition.xyz);
		float2 parallaxOffsetTS = viewDirection.xy / -viewDirection.z;

		// Parallax scale is also multiplied by normalScalesRcp
		parallaxOffsetTS *= 20.0;

		float3 mipLevels;
		mipLevels.x = GetMipLevel(input.TexCoord1.xy, Normals01Tex);
		mipLevels.y = GetMipLevel(input.TexCoord1.zw, Normals02Tex);
		mipLevels.z = GetMipLevel(input.TexCoord2.xy, Normals03Tex);

#if defined(VR)
		mipLevels = mipLevels + 4;
#else
		mipLevels = mipLevels + 3;
#endif

		float stepSize = rcp(16.0);
		float currBound = 0.0;
		float currHeight = 1.0;
		float prevHeight = 1.0;

		[loop] while (currHeight > currBound)
		{
			prevHeight = currHeight;
			currBound += stepSize;
			currHeight = GetHeight(input, currBound * parallaxOffsetTS.xy, normalScalesRcp, mipLevels);
		}

		float prevBound = currBound - stepSize;

		float delta2 = prevBound - prevHeight;
		float delta1 = currBound - currHeight;
		float denominator = delta2 - delta1;
		float parallaxAmount = (currBound * delta2 - prevBound * delta1) / denominator;

		return parallaxOffsetTS.xy * parallaxAmount;
	}

#if defined(FLOWMAP)
	float GetFlowmapHeight(PS_INPUT input, float2 uvShift, float multiplier, float offset, float mipLevel)
	{
		FlowmapData flowData = GetFlowmapDataUV(input, uvShift);
		float2 baseUV = offset + (flowData.flowVector - float2(multiplier * ((0.001 * ReflectionColor.w) * flowData.color.w), 0));
		return FlowMapNormalsTex.SampleLevel(FlowMapNormalsSampler, baseUV, mipLevel).w;
	}

	float GetFlowmapBlendedHeight(PS_INPUT input, float2 normalMul, float2 uvShift, float mipLevel)
	{
		float height0 = GetFlowmapHeight(input, uvShift, 9.92, 0, mipLevel);
		float height1 = GetFlowmapHeight(input, float2(0, uvShift.y), 10.64, 0.27, mipLevel);
		float height2 = GetFlowmapHeight(input, 0.0.xx, 8, 0, mipLevel);
		float height3 = GetFlowmapHeight(input, float2(uvShift.x, 0), 8.48, 0.62, mipLevel);
		
		float blendedHeight =
			normalMul.y * (normalMul.x * height2 + (1 - normalMul.x) * height3) +
			(1 - normalMul.y) * (normalMul.x * height1 + (1 - normalMul.x) * height0);
		
		return blendedHeight;
	}

	float GetFlowmapParallaxAmount(PS_INPUT input, float2 flowmapDims, float3 viewDirection)
	{
		float viewDotUp = -viewDirection.z;
		
		if (viewDotUp < 0.05)
			return 0.0;
		
		float2 parallaxDir = viewDirection.xy / -viewDirection.z;
		parallaxDir.y = -parallaxDir.y;
		
		float parallaxScale = 0.008 * saturate(viewDotUp * 2.0);
		parallaxDir *= parallaxScale;
		
		float2 uvShiftPx = 1 / (128 * flowmapDims);
		
		int numSteps = (int)lerp(32.0, 8.0, viewDotUp);
		float stepSize = rcp((float)numSteps);
		
		float currBound = 0.0;
		float currHeight = 1.0;
		float prevHeight = 1.0;
		
		[loop] for (int i = 0; i < numSteps && currHeight > currBound; i++)
		{
			prevHeight = currHeight;
			currBound += stepSize;
			
			PS_INPUT offsetInput = input;
			offsetInput.TexCoord3.xy = input.TexCoord3.xy + currBound * parallaxDir;
			
			float2 cellBlend = 0.5 + -(-0.5 + abs(frac(offsetInput.TexCoord2.zw * (64 * flowmapDims)) * 2 - 1));
			currHeight = 1.0 - GetFlowmapBlendedHeight(offsetInput, cellBlend, uvShiftPx, 0);
		}
		
		float prevBound = currBound - stepSize;
		float delta2 = prevBound - prevHeight;
		float delta1 = currBound - currHeight;
		float denominator = delta2 - delta1;
		
		return denominator != 0.0 ? (currBound * delta2 - prevBound * delta1) / denominator : currBound;
	}

	float GetFlowmapParallaxHeight(PS_INPUT input, float2 currentOffset, float3 normalScalesRcp, float mipLevel)
	{
		float height = Normals01Tex.SampleLevel(Normals01Sampler, input.TexCoord1.xy + currentOffset * normalScalesRcp.x, mipLevel).w;
		height *= NormalsAmplitude.x;
		return 1.0 - height;
	}

	float2 GetFlowmapParallaxUVOffset(PS_INPUT input, float3 viewDirection, float3 normalScalesRcp)
	{
		float2 parallaxOffsetTS = viewDirection.xy / -viewDirection.z;
		parallaxOffsetTS *= 80.0;
		
		float2 textureDims;
		Normals01Tex.GetDimensions(textureDims.x, textureDims.y);
#if defined(VR)
		textureDims /= 16.0;
#else
		textureDims /= 8.0;
#endif
		float2 texCoordsPerSize = input.TexCoord1.xy * textureDims;
		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);
		float2 dTexCoords = dxSize * dxSize + dySize * dySize;
		float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);
		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);
#if defined(VR)
		mipLevel += 4;
#else
		mipLevel += 3;
#endif
		
		float stepSize = rcp(16.0);
		float currBound = 0.0;
		float currHeight = 1.0;
		float prevHeight = 1.0;
		
		[loop] while (currHeight > currBound)
		{
			prevHeight = currHeight;
			currBound += stepSize;
			currHeight = GetFlowmapParallaxHeight(input, currBound * parallaxOffsetTS.xy, normalScalesRcp, mipLevel);
		}
		
		float prevBound = currBound - stepSize;
		float delta2 = prevBound - prevHeight;
		float delta1 = currBound - currHeight;
		float denominator = delta2 - delta1;
		float parallaxAmount = (currBound * delta2 - prevBound * delta1) / denominator;
		
		return parallaxOffsetTS.xy * parallaxAmount;
	}

	float2 GetFlowmapParallaxOffset(PS_INPUT input, float2 flowmapDimensions, float3 viewDirection, float3 normalScalesRcp)
	{
		return GetFlowmapParallaxUVOffset(input, viewDirection, normalScalesRcp);
	}
#endif

	// Water parallax shadowing using ExtendedMaterials approach
	// This ensures shadows properly match the height displacement
	
	float GetWaterParallaxShadow(PS_INPUT input, float3 sunDirTS, float3 normalScalesRcp, float3 mipLevels, float2 parallaxOffset)
	{
		if (sunDirTS.z <= 0.05)
			return 1.0;
		
		// Use normals for much more detail than height maps
		// Sample with parallax offset so shadows move with animated waves
		float2 rayOffset = sunDirTS.xy * 0.02;
		float2 baseUV = input.TexCoord1.xy + parallaxOffset * normalScalesRcp.x;
		
		// Sample normals along sun direction with animation
		float3 n0 = Normals01Tex.SampleLevel(Normals01Sampler, baseUV, mipLevels.x).xyz * 2.0 - 1.0;
		float3 n1 = Normals01Tex.SampleLevel(Normals01Sampler, baseUV + rayOffset * normalScalesRcp.x, mipLevels.x).xyz * 2.0 - 1.0;
		
		// Calculate how much the surface slopes away from sun
		float slope0 = saturate(dot(normalize(n0), sunDirTS));
		float slope1 = saturate(dot(normalize(n1), sunDirTS));
		
		// If forward slope is higher, we're in shadow
		float shadowAmount = saturate((slope1 - slope0) * 8.0);
		return 1.0 - shadowAmount * 0.7;
	}
	
	float GetWaveCrestShading(float height, float3 normal, float3 sunDir)
	{
		// Enhance wave crests (high points) and darken troughs (low points)
		float heightShading = (height - 0.5) * 1.0;
		float normalShading = (saturate(dot(normal, sunDir)) - 0.5) * 0.6;
		return 1.0 + heightShading + normalShading;
	}

#if defined(FLOWMAP)
	float GetFlowmapParallaxShadow(PS_INPUT input, float2 flowmapDimensions, float3 sunDirTS, float2 normalMul, float2 uvShift, float2 flowmapParallaxOffset, float3 normalScalesRcp)
	{
		if (sunDirTS.z <= 0.05)
			return 1.0;
			
		// Use base normals with parallax offset for animated shadows
		float2 rayOffset = sunDirTS.xy * 0.02;
		float2 baseUV = input.TexCoord1.xy + flowmapParallaxOffset * normalScalesRcp.x;
		
		// Sample normals along sun direction with animation
		float3 n0 = Normals01Tex.SampleLevel(Normals01Sampler, baseUV, 0).xyz * 2.0 - 1.0;
		float3 n1 = Normals01Tex.SampleLevel(Normals01Sampler, baseUV + rayOffset * normalScalesRcp.x, 0).xyz * 2.0 - 1.0;
		
		// Calculate shadow from normal slope difference
		float slope0 = saturate(dot(normalize(n0), sunDirTS));
		float slope1 = saturate(dot(normalize(n1), sunDirTS));
		
		float shadowAmount = saturate((slope1 - slope0) * 8.0);
		return 1.0 - shadowAmount * 0.7;
	}
#endif
}
