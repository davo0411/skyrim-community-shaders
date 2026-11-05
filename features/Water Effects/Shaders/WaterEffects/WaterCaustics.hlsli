Texture2D<float4> WaterCaustics : register(t65);

namespace WaterEffects
{
	float2 PanCausticsUV(float2 uv, float speed, float tiling)
	{
		return frac((float2(1, 0) * SharedData::Timer * speed) + (uv * tiling));
	}

	float SampleCaustics(float2 uv, float lod)
	{
		return WaterCaustics.SampleLevel(SampColorSampler, uv, lod).x;
	}

	// Approximate wavelength-dependent refraction by offsetting red/blue around green.
	float3 SampleCausticsDispersion(float2 uv, float2 dispersionOffset, float lod)
	{
		float center = SampleCaustics(uv, lod);
		float3 dispersed = float3(
			SampleCaustics(uv - dispersionOffset * 0.75, lod),
			center,
			SampleCaustics(uv + dispersionOffset, lod));
		return lerp(center.xxx, dispersed, 0.5);
	}

	/**
	 * Caustics on whatever lies under the water, with the light projection and focal-depth blur of Crest's ApplyCaustics
	 * (wave-harmonic/crest, OceanEmission.hlsl, MIT): the pattern is read where the sunlight crossed the surface,
	 * depth * toSun.xy / (projection * toSun.z) away, and blurred by |depth - focal depth| / depth of field mip levels on
	 * top of the mip the hardware would pick. The settings are Water Effects' (SharedData::waterEffectsSettings).
	 */
	float3 ComputeCaustics(float4 waterData, float3 worldPosition)
	{
		static const float MetresPerUnit = 1.0 / 70.0;
		float3 result = 1.0.xxx;
		float causticsDistToWater = waterData.w - worldPosition.z;
		float shoreFactorCaustics = saturate(causticsDistToWater / 64.0);

		if (shoreFactorCaustics > 0.0) {
			float4 caustics0 = SharedData::waterEffectsSettings.Caustics0;
			float causticsFade = 1.0 - saturate(causticsDistToWater / 1024.0);
			causticsFade *= causticsFade;

			float3 toSun = normalize(SharedData::DirLightDirection.xyz);
			float2 projection = toSun.xy * causticsDistToWater / (max(caustics0.x, 0.1) * max(toSun.z, max(caustics0.y, 0.01)));
			float2 causticsUV = (worldPosition.xy + projection + FrameBuffer::CameraPosAdjust.xy) * 0.005;
			float2 dispersionOffset = float2(0.6, 0.8) * (0.025 * shoreFactorCaustics * saturate(causticsDistToWater / 256.0));

			float2 causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.2, 1.0);
			float2 causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.2, -0.5);

			// The mip the hardware would pick, plus the defocus away from the focal depth.
			float defocus = min(abs(causticsDistToWater * MetresPerUnit - caustics0.z) / max(caustics0.w, 0.01), SharedData::waterEffectsSettings.Caustics1.x);
			float lodHigh = WaterCaustics.CalculateLevelOfDetail(SampColorSampler, causticsUV1) + defocus;
			float lodLow = WaterCaustics.CalculateLevelOfDetail(SampColorSampler, causticsUV * 0.5) + defocus;

			const float3 causticsHigh =
				(causticsFade > 0.0) ? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset, lodHigh), SampleCausticsDispersion(causticsUV2, dispersionOffset, lodHigh)) * 4.0) : 1.0.xxx;

			causticsUV *= 0.5;
			dispersionOffset *= 0.5;

			causticsUV1 = PanCausticsUV(causticsUV, 0.5 * 0.1, 1.0);
			causticsUV2 = PanCausticsUV(causticsUV, 1.0 * 0.1, -0.5);

			const float3 causticsLow =
				(causticsFade < 1.0) ? (min(SampleCausticsDispersion(causticsUV1, dispersionOffset, lodLow), SampleCausticsDispersion(causticsUV2, dispersionOffset, lodLow)) * 4.0) : 1.0.xxx;

			const float3 caustics = lerp(causticsLow, causticsHigh, causticsFade);
			result = lerp(1.0.xxx, caustics, shoreFactorCaustics);
		}

		return result;
	}
}
