#pragma once

#include "WaterTessellation.h"
#include "WaterWaves.h"
#include "WaterRipples.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace UnifiedWaterSettings
{
	struct GeneralSettings
	{
		bool UseOptimisedMeshes = false;
		bool ShowWireframe = false;
		bool WireframeRawMode = false;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		GeneralSettings,
		UseOptimisedMeshes,
		ShowWireframe,
		WireframeRawMode)

	struct LightingSettings
	{
		bool EnableLightingOverrides = false;
		float FresnelBias = 0.02f;
		float FresnelPower = 5.0f;
		float ReflectionStrength = 1.0f;
		float RefractionStrength = 1.0f;
		float WaterTransparency = 1.0f;
		float AbsorptionDensity = 0.15f;
		float ScatteringCoeff = 0.05f;
		float SpecularIntensity = 1.0f;
		float SunSpecularPower = 250.0f;
		float SunSpecularMagnitude = 1.0f;
		float SunSparklePower = 50.0f;
		float SunSparkleMagnitude = 1.0f;
		float SpecularRadius = 128.0f;
		float SpecularBrightness = 1.0f;
	};

	struct FogSettings
	{
		float AboveWaterFogDistNear = 0.0f;
		float AboveWaterFogDistFar = 163840.0f;
		float AboveWaterFogAmount = 1.0f;
		float UnderwaterFogDistNear = 0.0f;
		float UnderwaterFogDistFar = 4096.0f;
		float UnderwaterFogAmount = 1.0f;
	};

	struct DepthSettings
	{
		float DepthReflections = 1.0f;
		float DepthRefractions = 1.0f;
		float DepthNormals = 1.0f;
		float DepthSpecularLighting = 1.0f;
	};

	struct FoamSettings
	{
		bool EnableFoam = true;
		float FoamIntensity = 1.5f;
		float FoamIntensityFlowmap = 0.3f;
		float FoamThreshold = 0.6f;
		float FoamSharpness = 2.0f;
		float LargeWaveSlopeRequirement = 0.3f;
		float SmallWaveSlopeMultiplier = 3.0f;
		float SmallWaveBaseOffset = 0.2f;
		float SmallWaveHeightRange = 0.7f;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		FoamSettings,
		EnableFoam,
		FoamIntensity,
		FoamIntensityFlowmap,
		FoamThreshold,
		FoamSharpness,
		LargeWaveSlopeRequirement,
		SmallWaveSlopeMultiplier,
		SmallWaveBaseOffset,
		SmallWaveHeightRange)

	// All settings combined
	struct Settings
	{
		GeneralSettings general;
		UnifiedWaterTessellation::TessellationSettings tessellation;
		UnifiedWaterWaves::WaveSettings waves;
		LightingSettings lighting;
		FogSettings fog;
		DepthSettings depth;
		UnifiedWaterRipples::RippleSettings ripples;
		FoamSettings foam;
	};

	// Draw ImGui settings for lighting
	void DrawLightingSettings(LightingSettings& settings, DepthSettings& depth);

	// Draw ImGui settings for fog
	void DrawFogSettings(FogSettings& settings, bool lightingOverridesEnabled);

	// Draw ImGui settings for foam
	void DrawFoamSettings(FoamSettings& settings);
}
