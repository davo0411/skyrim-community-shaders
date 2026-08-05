namespace WaterEffects
{
	// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
	// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h

	// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
	// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
	// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

	// Extra softening of the view-Z denominator, matching DisplacementParams::FlattenAmount in
	// Extended Materials. Water has no per-material displacement params, so it is a constant here.
	static const float ParallaxFlatten = 0.0;

	// Water is a horizontal plane, so tangent space is world space with Z up. Softening the
	// denominator rather than dividing by the raw Z bounds the lateral sweep to roughly 3.3x the
	// march depth, which is what stops tall heights from smearing at grazing angles.
	//
	// viewDirection points from the camera to the surface, the opposite of the tangent-space view
	// vector the march expects, so the result is negated. Offsets are then applied as
	// parallaxDir * (((1 - t) * -maxHeight) + minHeight), which pushes deep hits along
	// viewDirection.xy, i.e. away from the camera.
	float2 GetParallaxDirection(float3 viewDirection)
	{
		float parallaxZ = max(abs(viewDirection.z) * 0.7 + 0.3 + ParallaxFlatten, 0.0625);
		return -viewDirection.xy / parallaxZ;
	}

	// Squared grazing factor; near head-on stays cheap.
	float GetParallaxGrazing(float ndotv)
	{
		float grazing = 1.0 - ndotv;
		return grazing * grazing;
	}

	uint GetParallaxStepCount(float ndotv, float grazing, uint stepCap)
	{
		float angleStepMul = clamp(0.5 * rcp(max(ndotv, 0.0625)), 0.5, 2.5);
		float grazingStepBoost = lerp(1.0, 1.65, grazing);
		uint numSteps = max(4u, (uint)(8.0 * angleStepMul * grazingStepBoost));
		numSteps = min(numSteps, stepCap);
		return (numSteps + 2) & ~3u;
	}

	float GetMipLevel(float2 coords, Texture2D<float4> tex, float screenNoise)
	{
		// Compute the current gradients:
		float2 actualTextureDims;
		tex.GetDimensions(actualTextureDims.x, actualTextureDims.y);

		// Use hardcoded 512x512 for mip calculation
		float2 textureDims = float2(512.0, 512.0);

		float2 texCoordsPerSize = coords * textureDims;

		float2 dxSize = ddx(texCoordsPerSize);
		float2 dySize = ddy(texCoordsPerSize);

		// Find min of change in u and v across quad: compute du and dv magnitude across quad
		float2 dTexCoords = dxSize * dxSize + dySize * dySize;

		// Standard mipmapping uses max here
		float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);

		// Compute the current mip level  (* 0.5 is effectively computing a square root before )
		float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

		// Offset mip level to sample as if texture were 512x512
		float mipOffset = log2(actualTextureDims.x / 512.0);

		mipLevel = max(mipLevel + mipOffset, 0.0);

		// Stochastic mip selection: use screen noise to select between adjacent mip levels
		mipLevel = floor(mipLevel) + (screenNoise < frac(mipLevel) ? 1.0 : 0.0);

		return mipLevel;
	}

	// Alpha stores height directly. Normalising by the amplitude sum keeps the result in [0,1] so
	// the bisection and secant refinement stay well conditioned when the amplitudes do not sum to 1.
	// currentOffset is in world units; normalScalesRcp converts it into each layer's UV space.
	float GetHeight(PS_INPUT input, float2 currentOffset, float3 normalScalesRcp, float3 mipLevels, float rcpAmpSum)
	{
		float3 heights;
		heights.x = Normals01Tex.SampleLevel(Normals01Sampler, input.TexCoord1.xy + currentOffset * normalScalesRcp.x, mipLevels.x).w;
		heights.y = Normals02Tex.SampleLevel(Normals02Sampler, input.TexCoord1.zw + currentOffset * normalScalesRcp.y, mipLevels.y).w;
		heights.z = Normals03Tex.SampleLevel(Normals03Sampler, input.TexCoord2.xy + currentOffset * normalScalesRcp.z, mipLevels.z).w;
		return saturate(dot(heights * NormalsAmplitude.xyz, float3(1, 1, 1)) * rcpAmpSum);
	}

	float2 GetParallaxOffset(PS_INPUT input, float3 normalScalesRcp)
	{
		float3 viewDirection = normalize(input.WPosition.xyz);
		float2 parallaxDir = GetParallaxDirection(viewDirection);

		// The march runs in world units, so the 20 unit depth and the amplitude sum together give
		// the same displacement the old depth-space march produced.
		float ampSum = NormalsAmplitude.x + NormalsAmplitude.y + NormalsAmplitude.z;
		float rcpAmpSum = rcp(max(ampSum, 1e-4));
		float maxHeight = 20.0 * ampSum;
		float minHeight = maxHeight * 0.5;

		// Gradients must be taken outside the branch below.
		float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);

		float3 mipLevels;
		mipLevels.x = GetMipLevel(input.TexCoord1.xy, Normals01Tex, screenNoise);
		mipLevels.y = GetMipLevel(input.TexCoord1.zw, Normals02Tex, screenNoise);
		mipLevels.z = GetMipLevel(input.TexCoord2.xy, Normals03Tex, screenNoise);

		float2 resultOffset = 0.0;

		[branch] if (maxHeight > 0.001) {
			float ndotv = saturate(-viewDirection.z);
			float grazing = GetParallaxGrazing(ndotv);

			uint numSteps = GetParallaxStepCount(ndotv, grazing, 16u);
			uint contactIters = grazing > 0.2 ? 4u : 2u;
			uint secantIters = grazing > 0.25 ? 2u : 1u;

			float stepSize = rcp((float)numSteps);
			float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
			float2 prevOffset = parallaxDir * minHeight;
			float prevBound = 1.0;
			float prevHeight = 1.0;

			float2 pt1 = 0;
			float2 pt2 = 0;
			bool intersectionFound = false;

			[loop] while (numSteps > 0) {
				float4 currentOffset[2];
				currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
				currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
				float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

				float4 currHeight;
				currHeight.x = GetHeight(input, currentOffset[0].xy, normalScalesRcp, mipLevels, rcpAmpSum);
				currHeight.y = GetHeight(input, currentOffset[0].zw, normalScalesRcp, mipLevels, rcpAmpSum);
				currHeight.z = GetHeight(input, currentOffset[1].xy, normalScalesRcp, mipLevels, rcpAmpSum);
				currHeight.w = GetHeight(input, currentOffset[1].zw, normalScalesRcp, mipLevels, rcpAmpSum);

				bool4 testResult = currHeight >= currentBound;
				[branch] if (any(testResult)) {
					intersectionFound = true;
					[branch] if (testResult.x) {
						pt1 = float2(currentBound.x, currHeight.x);
						pt2 = float2(prevBound, prevHeight);
					} else if (testResult.y) {
						pt1 = float2(currentBound.y, currHeight.y);
						pt2 = float2(currentBound.x, currHeight.x);
					} else if (testResult.z) {
						pt1 = float2(currentBound.z, currHeight.z);
						pt2 = float2(currentBound.y, currHeight.y);
					} else {
						pt1 = float2(currentBound.w, currHeight.w);
						pt2 = float2(currentBound.z, currHeight.z);
					}
					break;
				}

				prevOffset = currentOffset[1].zw;
				prevBound = currentBound.w;
				prevHeight = currHeight.w;
				numSteps -= 4;
			}

			float parallaxAmount = 0.0;
			[branch] if (intersectionFound) {
				float tNear = pt1.x;
				float fNear = pt1.y - tNear;
				float tFar = pt2.x;
				float fFar = pt2.y - tFar;

				// Binary search on f(t) = h(t) - t before secant.
				[loop] for (uint c = 0; c < contactIters; c++) {
					float tMid = 0.5 * (tNear + tFar);
					float2 midOffset = parallaxDir * (((1.0 - tMid) * -maxHeight) + minHeight);
					float fMid = GetHeight(input, midOffset, normalScalesRcp, mipLevels, rcpAmpSum) - tMid;
					[branch] if (fMid >= 0.0) {
						tNear = tMid;
						fNear = fMid;
					} else {
						tFar = tMid;
						fFar = fMid;
					}
				}

				// Secant iterations on f(t) = h(t) - t.
				[loop] for (uint i = 0; i < secantIters; i++) {
					float denominator = fNear - fFar;
					float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
					float tSecant = lerp(tNear, tFar, r);
					float2 secantOffset = parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);
					float fSecant = GetHeight(input, secantOffset, normalScalesRcp, mipLevels, rcpAmpSum) - tSecant;
					[branch] if (fSecant >= 0.0) {
						tNear = tSecant;
						fNear = fSecant;
					} else {
						tFar = tSecant;
						fFar = fSecant;
					}
				}

				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				parallaxAmount = lerp(tNear, tFar, r);
			}

			resultOffset = parallaxDir * (((1.0 - parallaxAmount) * -maxHeight) + minHeight);
		}

		return resultOffset;
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

	// The blended flowmap alpha is already a height in [0,1], so it needs no remapping.
	float GetFlowmapMarchHeight(PS_INPUT input, float2 texCoordOffset, float2 cellBlend, float2 uvShiftPx)
	{
		PS_INPUT offsetInput = input;
		offsetInput.TexCoord3.xy = input.TexCoord3.xy + texCoordOffset;
		return GetFlowmapBlendedHeight(offsetInput, cellBlend, uvShiftPx, 0);
	}

	// Marches the flowmap normals in TexCoord3 space, where flowVector is 64 * TexCoord3.xy before
	// rotation. That amplification is why the depth here is 0.008 rather than a world-unit value.
	// Returns the finished TexCoord3 offset so the direction is derived in exactly one place.
	float2 GetFlowmapParallaxTexCoordOffset(PS_INPUT input, float2 flowmapDims, float3 viewDirection)
	{
		float viewDotUp = -viewDirection.z;

		if (viewDotUp < 0.05)
			return 0.0;

		float2 parallaxDir = GetParallaxDirection(viewDirection);
		parallaxDir.y = -parallaxDir.y;

		const float maxHeight = 0.008;
		const float minHeight = maxHeight * 0.5;

		float2 uvShiftPx = 1 / (128 * flowmapDims);

		// The cell blend reads TexCoord2.zw, which the march never offsets, so it is loop invariant.
		float2 cellBlend = 0.5 + -(-0.5 + abs(frac(input.TexCoord2.zw * (64 * flowmapDims)) * 2 - 1));

		// Each height evaluation costs eight taps, so this path marches shorter and refines less
		// than the normal map paths.
		float ndotv = saturate(viewDotUp);
		float grazing = GetParallaxGrazing(ndotv);
		uint numSteps = GetParallaxStepCount(ndotv, grazing, 12u);
		const uint contactIters = 2u;
		const uint secantIters = 1u;

		float stepSize = rcp((float)numSteps);
		float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
		float2 prevOffset = parallaxDir * minHeight;
		float prevBound = 1.0;
		float prevHeight = 1.0;

		float2 pt1 = 0;
		float2 pt2 = 0;
		bool intersectionFound = false;

		[loop] while (numSteps > 0) {
			float4 currentOffset[2];
			currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
			currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
			float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

			float4 currHeight;
			currHeight.x = GetFlowmapMarchHeight(input, currentOffset[0].xy, cellBlend, uvShiftPx);
			currHeight.y = GetFlowmapMarchHeight(input, currentOffset[0].zw, cellBlend, uvShiftPx);
			currHeight.z = GetFlowmapMarchHeight(input, currentOffset[1].xy, cellBlend, uvShiftPx);
			currHeight.w = GetFlowmapMarchHeight(input, currentOffset[1].zw, cellBlend, uvShiftPx);

			bool4 testResult = currHeight >= currentBound;
			[branch] if (any(testResult)) {
				intersectionFound = true;
				[branch] if (testResult.x) {
					pt1 = float2(currentBound.x, currHeight.x);
					pt2 = float2(prevBound, prevHeight);
				} else if (testResult.y) {
					pt1 = float2(currentBound.y, currHeight.y);
					pt2 = float2(currentBound.x, currHeight.x);
				} else if (testResult.z) {
					pt1 = float2(currentBound.z, currHeight.z);
					pt2 = float2(currentBound.y, currHeight.y);
				} else {
					pt1 = float2(currentBound.w, currHeight.w);
					pt2 = float2(currentBound.z, currHeight.z);
				}
				break;
			}

			prevOffset = currentOffset[1].zw;
			prevBound = currentBound.w;
			prevHeight = currHeight.w;
			numSteps -= 4;
		}

		float parallaxAmount = 0.0;
		[branch] if (intersectionFound) {
			float tNear = pt1.x;
			float fNear = pt1.y - tNear;
			float tFar = pt2.x;
			float fFar = pt2.y - tFar;

			// Binary search on f(t) = h(t) - t before secant.
			[loop] for (uint c = 0; c < contactIters; c++) {
				float tMid = 0.5 * (tNear + tFar);
				float2 midOffset = parallaxDir * (((1.0 - tMid) * -maxHeight) + minHeight);
				float fMid = GetFlowmapMarchHeight(input, midOffset, cellBlend, uvShiftPx) - tMid;
				[branch] if (fMid >= 0.0) {
					tNear = tMid;
					fNear = fMid;
				} else {
					tFar = tMid;
					fFar = fMid;
				}
			}

			// Secant iterations on f(t) = h(t) - t.
			[loop] for (uint i = 0; i < secantIters; i++) {
				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				float tSecant = lerp(tNear, tFar, r);
				float2 secantOffset = parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);
				float fSecant = GetFlowmapMarchHeight(input, secantOffset, cellBlend, uvShiftPx) - tSecant;
				[branch] if (fSecant >= 0.0) {
					tNear = tSecant;
					fNear = fSecant;
				} else {
					tFar = tSecant;
					fFar = fSecant;
				}
			}

			float denominator = fNear - fFar;
			float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
			parallaxAmount = lerp(tNear, tFar, r);
		}

		return parallaxDir * (((1.0 - parallaxAmount) * -maxHeight) + minHeight);
	}

	// Single layer variant of GetHeight. The amplitude cancels against maxHeight below, so the
	// alpha is used directly.
	float GetFlowmapParallaxHeight(PS_INPUT input, float2 currentOffset, float3 normalScalesRcp, float mipLevel)
	{
		return Normals01Tex.SampleLevel(Normals01Sampler, input.TexCoord1.xy + currentOffset * normalScalesRcp.x, mipLevel).w;
	}

	float2 GetFlowmapParallaxUVOffset(PS_INPUT input, float3 viewDirection, float3 normalScalesRcp)
	{
		float2 parallaxDir = GetParallaxDirection(viewDirection);

		// World units again, this time against layer one only.
		float maxHeight = 80.0 * NormalsAmplitude.x;
		float minHeight = maxHeight * 0.5;

		// Gradients must be taken outside the branch below.
		float screenNoise = Random::InterleavedGradientNoise(input.HPosition.xy, SharedData::FrameCount);
		float mipLevel = GetMipLevel(input.TexCoord1.xy, Normals01Tex, screenNoise);

		float2 resultOffset = 0.0;

		[branch] if (maxHeight > 0.001) {
			float ndotv = saturate(-viewDirection.z);
			float grazing = GetParallaxGrazing(ndotv);

			uint numSteps = GetParallaxStepCount(ndotv, grazing, 16u);
			uint contactIters = grazing > 0.2 ? 4u : 2u;
			uint secantIters = grazing > 0.25 ? 2u : 1u;

			float stepSize = rcp((float)numSteps);
			float2 offsetPerStep = parallaxDir * maxHeight * stepSize;
			float2 prevOffset = parallaxDir * minHeight;
			float prevBound = 1.0;
			float prevHeight = 1.0;

			float2 pt1 = 0;
			float2 pt2 = 0;
			bool intersectionFound = false;

			[loop] while (numSteps > 0) {
				float4 currentOffset[2];
				currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
				currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
				float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

				float4 currHeight;
				currHeight.x = GetFlowmapParallaxHeight(input, currentOffset[0].xy, normalScalesRcp, mipLevel);
				currHeight.y = GetFlowmapParallaxHeight(input, currentOffset[0].zw, normalScalesRcp, mipLevel);
				currHeight.z = GetFlowmapParallaxHeight(input, currentOffset[1].xy, normalScalesRcp, mipLevel);
				currHeight.w = GetFlowmapParallaxHeight(input, currentOffset[1].zw, normalScalesRcp, mipLevel);

				bool4 testResult = currHeight >= currentBound;
				[branch] if (any(testResult)) {
					intersectionFound = true;
					[branch] if (testResult.x) {
						pt1 = float2(currentBound.x, currHeight.x);
						pt2 = float2(prevBound, prevHeight);
					} else if (testResult.y) {
						pt1 = float2(currentBound.y, currHeight.y);
						pt2 = float2(currentBound.x, currHeight.x);
					} else if (testResult.z) {
						pt1 = float2(currentBound.z, currHeight.z);
						pt2 = float2(currentBound.y, currHeight.y);
					} else {
						pt1 = float2(currentBound.w, currHeight.w);
						pt2 = float2(currentBound.z, currHeight.z);
					}
					break;
				}

				prevOffset = currentOffset[1].zw;
				prevBound = currentBound.w;
				prevHeight = currHeight.w;
				numSteps -= 4;
			}

			float parallaxAmount = 0.0;
			[branch] if (intersectionFound) {
				float tNear = pt1.x;
				float fNear = pt1.y - tNear;
				float tFar = pt2.x;
				float fFar = pt2.y - tFar;

				// Binary search on f(t) = h(t) - t before secant.
				[loop] for (uint c = 0; c < contactIters; c++) {
					float tMid = 0.5 * (tNear + tFar);
					float2 midOffset = parallaxDir * (((1.0 - tMid) * -maxHeight) + minHeight);
					float fMid = GetFlowmapParallaxHeight(input, midOffset, normalScalesRcp, mipLevel) - tMid;
					[branch] if (fMid >= 0.0) {
						tNear = tMid;
						fNear = fMid;
					} else {
						tFar = tMid;
						fFar = fMid;
					}
				}

				// Secant iterations on f(t) = h(t) - t.
				[loop] for (uint i = 0; i < secantIters; i++) {
					float denominator = fNear - fFar;
					float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
					float tSecant = lerp(tNear, tFar, r);
					float2 secantOffset = parallaxDir * (((1.0 - tSecant) * -maxHeight) + minHeight);
					float fSecant = GetFlowmapParallaxHeight(input, secantOffset, normalScalesRcp, mipLevel) - tSecant;
					[branch] if (fSecant >= 0.0) {
						tNear = tSecant;
						fNear = fSecant;
					} else {
						tFar = tSecant;
						fFar = fSecant;
					}
				}

				float denominator = fNear - fFar;
				float r = abs(denominator) > EPSILON_DIVISION ? saturate(fNear / denominator) : 0.5;
				parallaxAmount = lerp(tNear, tFar, r);
			}

			resultOffset = parallaxDir * (((1.0 - parallaxAmount) * -maxHeight) + minHeight);
		}

		return resultOffset;
	}

	float2 GetFlowmapParallaxOffset(PS_INPUT input, float2 flowmapDimensions, float3 viewDirection, float3 normalScalesRcp)
	{
		return GetFlowmapParallaxUVOffset(input, viewDirection, normalScalesRcp);
	}
#endif
}
