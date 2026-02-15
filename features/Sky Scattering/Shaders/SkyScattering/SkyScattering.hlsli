namespace SkyScattering
{
	TextureCubeArray<float> SkyScatteringTexture : register(t26);

	struct SkyScatteringSettings
	{
		uint Enabled;
		float Opacity;
		uint NumLayers;
		float CloudShadowStrength;

		float3 ScatterTint;
		float ScatterAmount;

		float SilverIntensity;
		float SilverSpread;
		float AmbientDarkening;
		float pad0;
	};

	const static float CloudHeight = (2e3f / 1.428e-2) * 0.25;
	const static float PlanetRadius = (6371e3f / 1.428e-2);
	const static float RcpHPlusR = (1.0 / (CloudHeight + PlanetRadius));

	float3 GetCloudSampleDir(float3 rel_pos, float3 eye_to_sun)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, eye_to_sun);
		float lengthsqr = dot(p, p);
		float t = -dotprod + sqrt(dotprod * dotprod - lengthsqr + 1);
		float3 v = (p + eye_to_sun * t) * (r + CloudHeight) - float3(0, 0, r);
		return v;
	}

	/// Sample cloud scattering from the final layer of the cubemap array.
	/// Returns a shadow multiplier in [0, 1] where 1 = fully lit.
	float GetScatteringShadow(float3 worldPosition, SamplerState textureSampler, float opacity, uint numLayers)
	{
		float3 sampleDir = GetCloudSampleDir(worldPosition, SharedData::DirLightDirection.xyz);

		// Sample the last layer — it accumulates all cloud occlusion from above
		uint lastLayer = max(numLayers, 1u) - 1;
		float cloudSample = SkyScatteringTexture.SampleLevel(textureSampler, float4(sampleDir, (float)lastLayer), 0).x;

		return lerp(1.0, 1.0 - cloudSample, opacity);
	}

	/// Apply volumetric scattering effects to cloud color during sky rendering.
	/// Samples the layer above the current cloud to produce inter-cloud shadowing,
	/// forward scattering tint, silver lining, and ambient darkening.
	///
	/// @param cloudColor    The cloud color to modify (rgb + alpha)
	/// @param viewDir       Normalized view direction from camera
	/// @param textureSampler Sampler state for cubemap sampling
	/// @param settings      Sky scattering settings from constant buffer
	/// @return Modified cloud color with volumetric effects applied
	float4 ApplyCloudScattering(float4 cloudColor, float3 viewDir, SamplerState textureSampler, SkyScatteringSettings settings)
	{
		if (!settings.Enabled || cloudColor.a < 0.001)
			return cloudColor;

		float3 sunDir = SharedData::DirLightDirection.xyz;
		float3 sunColor = SharedData::DirLightColor.xyz;

		// Sample occlusion from layers above this cloud
		// The cubemap array copy (bound at t26) contains accumulated cloud density from upper layers
		float occFromAbove = SkyScatteringTexture.SampleLevel(textureSampler, float4(viewDir, 0.0), 0).x;

		// --- Inter-cloud shadowing ---
		// Upper clouds darken lower clouds, making the sky look layered/volumetric
		float shadowFactor = lerp(1.0, 1.0 - occFromAbove, settings.CloudShadowStrength);
		float3 result = cloudColor.rgb * shadowFactor;

		// --- Forward scattering tint ---
		// Light passing through upper clouds picks up a warm tint
		// Strongest when looking toward the sun (forward scattering)
		float sunAlignment = saturate(dot(viewDir, sunDir));
		float forwardScatter = pow(sunAlignment, 4.0) * occFromAbove;
		result += settings.ScatterTint * sunColor * forwardScatter * settings.ScatterAmount;

		// --- Silver lining ---
		// Thin cloud edges glow when backlit by the sun
		float edgeFactor = cloudColor.a * (1.0 - cloudColor.a) * 4.0;  // peaks at alpha=0.5
		float silverLining = edgeFactor * pow(sunAlignment, 1.0 / max(settings.SilverSpread, 0.01));
		result += sunColor * silverLining * settings.SilverIntensity;

		// --- Ambient darkening ---
		// Thick cloud regions receive less ambient light from above
		float thickness = saturate(cloudColor.a + occFromAbove);
		float ambientReduction = lerp(1.0, 1.0 - thickness * 0.5, settings.AmbientDarkening);
		result *= ambientReduction;

		return float4(result, cloudColor.a);
	}
}
