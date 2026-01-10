#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace UnifiedWaterWaves
{
	struct WaveSettings
	{
		float WaveIntensity = 0.3f;
		float WaveAmplitude = 0.7f;
		float WaveSpeed = 0.025f;
		float WaveSteepness = 5.0f;
		float WaveFadeStart = 4096.0f;
		float WaveFadeEnd = 8192.0f;

		float Wave1Amplitude = 0.8f;
		float Wave1Wavelength = 60.0f;
		float Wave1Steepness = 0.4f;
		float Wave1AngleOffset = 0.0f;

		float Wave2Amplitude = 0.5f;
		float Wave2Wavelength = 35.0f;
		float Wave2Steepness = 0.35f;
		float Wave2AngleOffset = 0.6f;

		float Wave3Amplitude = 0.25f;
		float Wave3Wavelength = 18.0f;
		float Wave3Steepness = 0.3f;
		float Wave3AngleOffset = -0.7f;

		float Wave4Amplitude = 0.12f;
		float Wave4Wavelength = 8.0f;
		float Wave4Steepness = 0.25f;
		float Wave4AngleOffset = 0.44f;

		float Wave5Amplitude = 0.06f;
		float Wave5Wavelength = 4.0f;
		float Wave5Steepness = 0.2f;
		float Wave5AngleOffset = -0.44f;

		float Wave6Amplitude = 0.03f;
		float Wave6Wavelength = 2.0f;
		float Wave6Steepness = 0.15f;
		float Wave6AngleOffset = 1.22f;

		// Depth-based wave control
		float ShallowWaveDepthMin = 50.0f;      // Depth where waves start reducing (game units, ~0.7m)
		float ShallowWaveDepthMax = 500.0f;     // Depth where waves reach full strength (game units, ~7m)
		float ShoreWaveDepthThreshold = 300.0f; // Depth range for shore-directed waves (game units, ~4.3m)
		float ShoreWaveStrength = 1.0f;         // Strength of shore-directed wave influence (0-1)
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		WaveSettings,
		WaveIntensity,
		WaveAmplitude,
		WaveSpeed,
		WaveSteepness,
		WaveFadeStart,
		WaveFadeEnd,
		Wave1Amplitude,
		Wave1Wavelength,
		Wave1Steepness,
		Wave1AngleOffset,
		Wave2Amplitude,
		Wave2Wavelength,
		Wave2Steepness,
		Wave2AngleOffset,
		Wave3Amplitude,
		Wave3Wavelength,
		Wave3Steepness,
		Wave3AngleOffset,
		Wave4Amplitude,
		Wave4Wavelength,
		Wave4Steepness,
		Wave4AngleOffset,
		Wave5Amplitude,
		Wave5Wavelength,
		Wave5Steepness,
		Wave5AngleOffset,
		Wave6Amplitude,
		Wave6Wavelength,
		Wave6Steepness,
		Wave6AngleOffset,
		ShallowWaveDepthMin,
		ShallowWaveDepthMax,
		ShoreWaveDepthThreshold,
		ShoreWaveStrength)

	// Draw ImGui settings UI for waves
	void DrawWaveSettings(WaveSettings& settings);
}
