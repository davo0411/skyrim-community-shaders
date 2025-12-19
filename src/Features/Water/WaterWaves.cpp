#include "WaterWaves.h"

#include "Util.h"
#include <imgui.h>

namespace UnifiedWaterWaves
{
	void DrawWaveSettings(WaveSettings& settings)
	{
		ImGui::Text("Wave System");
		ImGui::SliderFloat("Wave Enhancement", &settings.WaveIntensity, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Wave Height", &settings.WaveAmplitude, 0.1f, 10.0f, "%.2f");
		ImGui::SliderFloat("Wave Speed", &settings.WaveSpeed, 0.01f, 1.0f, "%.3f");
		ImGui::SliderFloat("Wave Steepness", &settings.WaveSteepness, 0.1f, 10.0f, "%.2f");

		ImGui::Spacing();
		ImGui::Text("Wave Distance Fade");
		ImGui::SliderFloat("Fade Start", &settings.WaveFadeStart, 1024.0f, 16384.0f, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Distance (game units) where waves start fading.\n4096 = ~58 meters.");
		ImGui::SliderFloat("Fade End", &settings.WaveFadeEnd, 2048.0f, 32768.0f, "%.0f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Distance (game units) where waves fully fade out.\n8192 = ~117 meters.\nDistant water becomes flat beyond this.");

		ImGui::Spacing();
		if (ImGui::TreeNodeEx("Wave 1 (Primary)", ImGuiTreeNodeFlags_None)) {
			ImGui::SliderFloat("W1 Amplitude (m)", &settings.Wave1Amplitude, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("W1 Wavelength (m)", &settings.Wave1Wavelength, 10.0f, 150.0f, "%.1f");
			ImGui::SliderFloat("W1 Steepness", &settings.Wave1Steepness, 0.0f, 0.6f, "%.3f");
			ImGui::SliderFloat("W1 Angle (rad)", &settings.Wave1AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("Wave 2 (Secondary)", ImGuiTreeNodeFlags_None)) {
			ImGui::SliderFloat("W2 Amplitude (m)", &settings.Wave2Amplitude, 0.0f, 1.5f, "%.2f");
			ImGui::SliderFloat("W2 Wavelength (m)", &settings.Wave2Wavelength, 5.0f, 100.0f, "%.1f");
			ImGui::SliderFloat("W2 Steepness", &settings.Wave2Steepness, 0.0f, 0.5f, "%.3f");
			ImGui::SliderFloat("W2 Angle (rad)", &settings.Wave2AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("Wave 3 (Detail)", ImGuiTreeNodeFlags_None)) {
			ImGui::SliderFloat("W3 Amplitude (m)", &settings.Wave3Amplitude, 0.0f, 0.5f, "%.3f");
			ImGui::SliderFloat("W3 Wavelength (m)", &settings.Wave3Wavelength, 2.0f, 50.0f, "%.1f");
			ImGui::SliderFloat("W3 Steepness", &settings.Wave3Steepness, 0.0f, 0.4f, "%.3f");
			ImGui::SliderFloat("W3 Angle (rad)", &settings.Wave3AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::TreePop();
		}
		if (ImGui::TreeNodeEx("Fine Ripples (4-6)", ImGuiTreeNodeFlags_None)) {
			ImGui::SliderFloat("W4 Amplitude (m)", &settings.Wave4Amplitude, 0.0f, 0.25f, "%.3f");
			ImGui::SliderFloat("W4 Wavelength (m)", &settings.Wave4Wavelength, 1.0f, 20.0f, "%.1f");
			ImGui::SliderFloat("W4 Steepness", &settings.Wave4Steepness, 0.0f, 0.35f, "%.3f");
			ImGui::SliderFloat("W4 Angle (rad)", &settings.Wave4AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::Spacing();
			ImGui::SliderFloat("W5 Amplitude (m)", &settings.Wave5Amplitude, 0.0f, 0.15f, "%.3f");
			ImGui::SliderFloat("W5 Wavelength (m)", &settings.Wave5Wavelength, 0.5f, 10.0f, "%.1f");
			ImGui::SliderFloat("W5 Steepness", &settings.Wave5Steepness, 0.0f, 0.3f, "%.3f");
			ImGui::SliderFloat("W5 Angle (rad)", &settings.Wave5AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::Spacing();
			ImGui::SliderFloat("W6 Amplitude (m)", &settings.Wave6Amplitude, 0.0f, 0.08f, "%.3f");
			ImGui::SliderFloat("W6 Wavelength (m)", &settings.Wave6Wavelength, 0.25f, 6.0f, "%.2f");
			ImGui::SliderFloat("W6 Steepness", &settings.Wave6Steepness, 0.0f, 0.25f, "%.3f");
			ImGui::SliderFloat("W6 Angle (rad)", &settings.Wave6AngleOffset, -3.14f, 3.14f, "%.2f");
			ImGui::TreePop();
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::TreeNodeEx("Depth-Based Wave Modulation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Shallow Water Effects");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Reduces wave amplitude in shallow water for more realistic shoreline behavior.");
			}

			ImGui::SliderFloat("Shallow Depth Min", &settings.ShallowWaveDepthMin, 0.0f, 500.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Depth (game units) where waves fully disappear.\n~70 units = 1 meter.\nDefault: 50 (~0.7m)");
			}

			ImGui::SliderFloat("Shallow Depth Max", &settings.ShallowWaveDepthMax, 50.0f, 2000.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Depth (game units) where waves reach full amplitude.\nDefault: 500 (~7m)");
			}

			ImGui::Spacing();
			ImGui::Text("Shore-Directed Waves");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Creates waves that flow toward the shore in shallow water.");
			}

			ImGui::SliderFloat("Shore Depth Threshold", &settings.ShoreWaveDepthThreshold, 50.0f, 1000.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Depth (game units) below which shore waves activate.\nDefault: 300 (~4.3m)");
			}

			ImGui::SliderFloat("Shore Wave Strength", &settings.ShoreWaveStrength, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Intensity of shore-directed wave bias.\n0 = disabled, 1 = default, 2 = very strong");
			}

			ImGui::TreePop();
		}
	}
}
