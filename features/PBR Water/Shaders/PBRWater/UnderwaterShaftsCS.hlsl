// ============================================================================
// PBR Water - the underwater light shafts at half resolution (PBRWater::DrawUnderwaterVolume), denoised as Effects 11's
// volumetric rays are (BlurVolumetricRaysCS, ApplyVolumetricRaysPS), with a temporal pass before the blur:
//   (march)   UnderwaterVolume.hlsl MarchPS, a pixel shader (the shadow maps are bound to the pixel stage): one pixel
//             of every 2x2 (a different one each frame) marches the in-scattering (UnderwaterLight.hlsli), rgb the light
//             (linear, unencoded), a the distance of what it shows (units).
//   TEMPORAL  reprojects last frame's result with the camera (the water moves little in a frame; the waves' focusing
//             does, and the blend lets it), rejecting history whose distance disagrees (an edge moved over it), and
//             blends: the jitter of the steps and the sub-pixel add up over frames. Kept as next frame's history.
//   BLUR      separable depth-aware blur (compile with HORIZONTAL for the horizontal pass), taps rejected by their
//             relative distance: Effects 11's kernel, on rgb with the distance in a.
// The composite upsamples the result jointly with the full-resolution depth (UnderwaterVolume.hlsl).
// ============================================================================

#include "PBRWater/UnderwaterLight.hlsli"

#if defined(TEMPORAL)

Texture2D<float4> Current : register(t0);
Texture2D<float4> History : register(t1);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (any(id.xy >= (uint2)Half.xy))
		return;
	float4 current = Current[id.xy];
	float4 result = current;
	[branch] if (Shafts.w > 0.5)
	{
		// Where this pixel's point was on last frame's screen.
		float2 pixel = float2(id.xy) * 2.0 + 1.0;
		float2 uv = pixel / Viewport.xy;
		float4 ray = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0));
		float3 position = normalize(ray.xyz / ray.w) * current.a;
		float3 previousPosition = position + FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz;
		float4 clip = mul(FrameBuffer::CameraPreviousViewProjUnjittered, float4(previousPosition, 1.0));
		float2 previousUv = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		if (clip.w > 0.0 && all(previousUv > 0.0) && all(previousUv < 1.0)) {
			float4 history = History.SampleLevel(PBRWater::LinearClampSampler, previousUv, 0);
			float expected = length(previousPosition);
			// An edge moved over it: last frame's light belongs to another surface.
			float agreement = 1.0 - smoothstep(0.05, 0.2, abs(history.a - expected) / max(expected, 1e-3));
			result.rgb = lerp(current.rgb, history.rgb, 0.9 * agreement);
		}
	}
	Output[id.xy] = result;
}

#else  // the blur

Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);

#	define TG_DIM 256
#	define WINDOW 12

groupshared float4 Samples[TG_DIM];

static const int TapOffsets[5] = { -12, -6, 0, 6, 12 };
static const float TapWeights[5] = { 0.178400, 0.210431, 0.222338, 0.210431, 0.178400 };

#	if defined(HORIZONTAL)
[numthreads(TG_DIM, 1, 1)] void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
	int idx = groupThreadId.x;
	int base = idx - WINDOW;
	int2 pixel = int2(groupId.x * (TG_DIM - WINDOW * 2) + base, groupId.y);
#	else
[numthreads(1, TG_DIM, 1)] void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID) {
	int idx = groupThreadId.y;
	int base = idx - WINDOW;
	int2 pixel = int2(groupId.x, groupId.y * (TG_DIM - WINDOW * 2) + base);
#	endif
	int2 last = int2(Half.xy) - 1;
	Samples[idx] = Input[clamp(pixel, 0, last)];
	GroupMemoryBarrierWithGroupSync();
	if (base < 0 || base >= TG_DIM - WINDOW * 2 || any(pixel > last))
		return;
	float centre = Samples[idx].a;
	float rcpCentre = rcp(max(centre, 1e-4));
	float3 sum = 0.0;
	float weightSum = 0.0;
	[unroll] for (uint i = 0; i < 5; i++)
	{
		float4 tap = Samples[idx + TapOffsets[i]];
		float weight = TapWeights[i] * (1.0 - smoothstep(0.1, 0.3, abs(tap.a - centre) * rcpCentre));
		sum += weight * tap.rgb;
		weightSum += weight;
	}
	Output[pixel] = float4(sum / weightSum, centre);
}

#endif
