#include "WaterSettings.h"

#include "Util.h"
#include <imgui.h>

namespace UnifiedWaterSettings
{
	void DrawLightingSettings(LightingSettings& settings, DepthSettings& depth)
	{
		ImGui::Checkbox("Enable Lighting Overrides", &settings.EnableLightingOverrides);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Override ESP water lighting values with custom settings.");
		}

		if (settings.EnableLightingOverrides) {
			ImGui::Spacing();
			ImGui::Text("Fresnel / Reflection");
			ImGui::SliderFloat("Fresnel Bias (F0)", &settings.FresnelBias, 0.0f, 0.2f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Base reflectivity. Water IOR 1.33 = ~0.02.");
			}
			ImGui::SliderFloat("Fresnel Power", &settings.FresnelPower, 1.0f, 10.0f, "%.1f");
			ImGui::SliderFloat("Reflection Strength", &settings.ReflectionStrength, 0.0f, 2.0f, "%.2f");

			ImGui::Spacing();
			ImGui::Text("Refraction / Transparency");
			ImGui::SliderFloat("Refraction Strength", &settings.RefractionStrength, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Water Transparency", &settings.WaterTransparency, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Absorption Density", &settings.AbsorptionDensity, 0.0f, 1.0f, "%.3f");
			ImGui::SliderFloat("Scattering", &settings.ScatteringCoeff, 0.0f, 0.5f, "%.3f");
			ImGui::SliderFloat("Specular Intensity", &settings.SpecularIntensity, 0.0f, 5.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Overall multiplier for all specular effects.");
			}

			ImGui::Spacing();
			ImGui::Text("Sun Specular");
			ImGui::SliderFloat("Sun Specular Power", &settings.SunSpecularPower, 10.0f, 1000.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Sharpness of sun reflection. Higher = tighter highlight.");
			}
			ImGui::SliderFloat("Sun Specular Magnitude", &settings.SunSpecularMagnitude, 0.0f, 5.0f, "%.2f");
			ImGui::SliderFloat("Sun Sparkle Power", &settings.SunSparklePower, 1.0f, 200.0f, "%.0f");
			ImGui::SliderFloat("Sun Sparkle Magnitude", &settings.SunSparkleMagnitude, 0.0f, 5.0f, "%.2f");
			ImGui::SliderFloat("Specular Radius", &settings.SpecularRadius, 1.0f, 512.0f, "%.0f");
			ImGui::SliderFloat("Specular Brightness", &settings.SpecularBrightness, 0.0f, 5.0f, "%.2f");

			ImGui::Spacing();
			ImGui::Text("Depth Control");
			ImGui::SliderFloat("Depth Reflections", &depth.DepthReflections, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Depth Refractions", &depth.DepthRefractions, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Depth Normals", &depth.DepthNormals, 0.0f, 2.0f, "%.2f");
			ImGui::SliderFloat("Depth Specular", &depth.DepthSpecularLighting, 0.0f, 2.0f, "%.2f");
		}
	}

	void DrawFogSettings(FogSettings& settings, bool lightingOverridesEnabled)
	{
		if (!lightingOverridesEnabled) {
			ImGui::TextDisabled("Enable Lighting Overrides to use fog settings.");
		} else {
			ImGui::Text("Above Water Fog");
			ImGui::SliderFloat("Near Distance", &settings.AboveWaterFogDistNear, 0.0f, 10000.0f, "%.0f");
			ImGui::SliderFloat("Far Distance", &settings.AboveWaterFogDistFar, 1000.0f, 500000.0f, "%.0f");
			ImGui::SliderFloat("Fog Amount", &settings.AboveWaterFogAmount, 0.0f, 2.0f, "%.2f");

			ImGui::Spacing();
			ImGui::Text("Underwater Fog");
			ImGui::SliderFloat("UW Near Distance", &settings.UnderwaterFogDistNear, 0.0f, 1000.0f, "%.0f");
			ImGui::SliderFloat("UW Far Distance", &settings.UnderwaterFogDistFar, 100.0f, 20000.0f, "%.0f");
			ImGui::SliderFloat("UW Fog Amount", &settings.UnderwaterFogAmount, 0.0f, 2.0f, "%.2f");
		}
	}

	void DrawFoamSettings(FoamSettings& settings)
	{
		ImGui::Checkbox("Enable Foam", &settings.EnableFoam);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Generates foam on wave crests based on wave height and slope.\nFoam appears on steep, sharp waves.");
		}

		if (settings.EnableFoam) {
			ImGui::Spacing();
			ImGui::Text("General Foam Settings");
			ImGui::Separator();

			ImGui::SliderFloat("Foam Intensity", &settings.FoamIntensity, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Overall foam strength for non-flowmap water (lakes, ocean).\nAlso shifts height threshold upward (higher = tighter to peak).");
			}

			ImGui::SliderFloat("Foam Intensity (Flowmap)", &settings.FoamIntensityFlowmap, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Foam strength for flowmap water (rivers).");
			}

			ImGui::SliderFloat("Height Threshold", &settings.FoamThreshold, 0.0f, 0.9f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Minimum wave height for foam appearance.\nLower values = foam appears lower on waves.");
			}

			ImGui::SliderFloat("Edge Sharpness", &settings.FoamSharpness, 0.5f, 8.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How sharply foam edges are defined.\nHigher = more crisp edges.");
			}

			ImGui::Spacing();
			ImGui::Text("Large Wave Foam (Waves 1-3)");
			ImGui::Separator();

			ImGui::SliderFloat("Slope Requirement", &settings.LargeWaveSlopeRequirement, 0.0f, 0.8f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Minimum slope ratio for large waves to generate foam.\nHigher = only steep waves foam (prevents foam on gentle rolling tops).\nLower = more foam on larger waves.");
			}

			ImGui::Spacing();
			ImGui::Text("Small Wave Foam (Waves 4-6)");
			ImGui::Separator();

			ImGui::SliderFloat("Slope Multiplier", &settings.SmallWaveSlopeMultiplier, 1.0f, 5.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How easily small waves generate foam based on slope.\nHigher = more foam on small waves.");
			}

			ImGui::SliderFloat("Base Foam Offset", &settings.SmallWaveBaseOffset, 0.0f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Base foam amount for small waves regardless of slope.\nAdds minimum foam to all small wave crests.");
			}

			ImGui::SliderFloat("Height Range", &settings.SmallWaveHeightRange, 0.5f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Height range requirement for small wave foam.\nLower = foam appears on smaller height variations.");
			}
		}
	}
}
