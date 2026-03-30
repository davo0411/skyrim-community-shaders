#include "PBRWater.h"

#include "Globals.h"
#include "State.h"
#include "ShaderCache.h"
#include "Util.h"
#include "RE/C/Calendar.h"
#include "Features/TerrainShadows.h"

#include <cmath>
#include <d3d11.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::GeneralSettings,
	ShowWireframe,
	WireframeRawMode)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::TessellationSettings,
	EnableTessellation,
	TessellationMaxDistance,
	TessellationMinFactor,
	TessellationMaxFactor)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::WaveSettings,
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
	ShoreBlendStart,
	ShoreBlendEnd,
	ShoreWaveStrength,
	UseFFTWaves,
	FFTSimulationTimeScale,
	FFTMasterIntensity,
	FFTPhysicalHeightScale,
	FFTChoppiness,
	FFTWindSpeedMps,
	FFTWindDirectionRad,
	FFTFetchKm,
	FFTCascadeLength0M,
	FFTCascadeLength1M,
	FFTCascadeLength2M,
	FFTFadeStart,
	FFTFadeEnd,
	FFTTessActivity,
	FFTSwell,
	FFTSpread,
	FFTDetail,
	FFTWaterDepth,
	FFTWhitecap,
	FFTFoamAmount,
	FFTDispFarStart,
	FFTDispFarFalloff,
	FFTBicubicNormals)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::LightingSettings,
	EnableLightingOverrides,
	FresnelBias,
	FresnelPower,
	ReflectionStrength,
	RefractionStrength,
	WaterTransparency,
	AbsorptionDensity,
	ScatteringCoeff,
	SpecularIntensity,
	SunSpecularPower,
	SunSpecularMagnitude,
	SunSparklePower,
	SunSparkleMagnitude,
	SpecularRadius,
	SpecularBrightness)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::FogSettings,
	AboveWaterFogDistNear,
	AboveWaterFogDistFar,
	AboveWaterFogAmount,
	UnderwaterFogDistNear,
	UnderwaterFogDistFar,
	UnderwaterFogAmount)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::DepthSettings,
	DepthReflections,
	DepthRefractions,
	DepthNormals,
	DepthSpecularLighting)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::RippleSettings,
	EnableActorRipples,
	RippleStrength,
	RippleRadius,
	RippleWaveSpeed,
	RippleWaveFreq1,
	RippleWaveFreq2,
	RippleWaveFreq3,
	RippleNormalStrength)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::FoamSettings,
	EnableFoam,
	FoamIntensity,
	FoamIntensityFlowmap,
	FoamThreshold,
	FoamSharpness,
	FoamIntersectionRange,
	FoamIntersectionIntensity)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::BRDFSettings,
	EnableBRDFSpecular,
	EnableWaterScattering)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::Settings,
	general,
	tessellation,
	waves,
	lighting,
	fog,
	depth,
	ripples,
	foam,
	brdf)

void PBRWater::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::DrawSettings()
{
	if (ImGui::BeginTabBar("PBRWaterTabs")) {
		if (ImGui::BeginTabItem("General")) {
			ImGui::Checkbox("Enable Tessellation", &settings.tessellation.EnableTessellation);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Hardware tessellation for dynamic mesh density based on distance.");
			}

			if (settings.tessellation.EnableTessellation) {
				ImGui::Indent();
				ImGui::SliderFloat("Max Distance", &settings.tessellation.TessellationMaxDistance, 1024.0f, 16384.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Distance (game units) where minimum tessellation is applied.\nBeyond this, water triangles are minimized (~2 tris).");
				ImGui::SliderFloat("Max Factor", &settings.tessellation.TessellationMaxFactor, 4.0f, 64.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Tessellation factor for nearby water.\nHigher = more polygons = better wave detail but slower.\n8-16 is usually sufficient.");
				ImGui::Unindent();
			}

			ImGui::Spacing();

			ImGui::Checkbox("Enable BRDF Specular", &settings.brdf.EnableBRDFSpecular);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Enables physically based BRDF specular reflections on water surfaces");

			ImGui::Checkbox("Enable Water Scattering", &settings.brdf.EnableWaterScattering);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Enables scattering effects in water");

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Waves")) {
			ImGui::Checkbox("Use FFT Waves", &settings.waves.UseFFTWaves);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"FFT ocean simulation: compute-shader pipeline generates displacement/normal textures once per frame.\n"
					"Cost is fixed regardless of tessellation. Disable to fall back to legacy Gerstner wave synthesis.");

			if (settings.waves.UseFFTWaves) {
				ImGui::Spacing();
				ImGui::TextWrapped(
					"With FFT enabled, tune the ocean using \"FFT Ocean\" below. Wave 1–6, Wave Enhancement / Height / "
					"Speed / Steepness, and Gerstner distance fade apply only when FFT is off.");
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextUnformatted("Legacy Gerstner (used when FFT is off)");
			ImGui::SliderFloat("Wave Enhancement", &settings.waves.WaveIntensity, 0.0f, 1.0f, "%.2f");
			ImGui::SliderFloat("Wave Height", &settings.waves.WaveAmplitude, 0.1f, 10.0f, "%.2f");
			ImGui::SliderFloat("Wave Speed", &settings.waves.WaveSpeed, 0.01f, 1.0f, "%.3f");
			ImGui::SliderFloat("Wave Steepness", &settings.waves.WaveSteepness, 0.1f, 10.0f, "%.2f");

			ImGui::Spacing();
			ImGui::Text("Gerstner distance fade (FFT off only)");
			ImGui::SliderFloat("Fade Start", &settings.waves.WaveFadeStart, 1024.0f, 16384.0f, "%.0f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Distance (game units) where waves start fading.\n4096 = ~58 meters.");
			ImGui::SliderFloat("Fade End", &settings.waves.WaveFadeEnd, 2048.0f, 32768.0f, "%.0f");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Distance (game units) where waves fully fade out.\n8192 = ~117 meters.\nDistant water becomes flat beyond this.");

			ImGui::Spacing();
			if (ImGui::TreeNodeEx("Wave 1 (Primary)", ImGuiTreeNodeFlags_None)) {
				ImGui::SliderFloat("W1 Amplitude (m)", &settings.waves.Wave1Amplitude, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("W1 Wavelength (m)", &settings.waves.Wave1Wavelength, 10.0f, 150.0f, "%.1f");
				ImGui::SliderFloat("W1 Steepness", &settings.waves.Wave1Steepness, 0.0f, 0.6f, "%.3f");
				ImGui::SliderFloat("W1 Angle (rad)", &settings.waves.Wave1AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx("Wave 2 (Secondary)", ImGuiTreeNodeFlags_None)) {
				ImGui::SliderFloat("W2 Amplitude (m)", &settings.waves.Wave2Amplitude, 0.0f, 1.5f, "%.2f");
				ImGui::SliderFloat("W2 Wavelength (m)", &settings.waves.Wave2Wavelength, 5.0f, 100.0f, "%.1f");
				ImGui::SliderFloat("W2 Steepness", &settings.waves.Wave2Steepness, 0.0f, 0.5f, "%.3f");
				ImGui::SliderFloat("W2 Angle (rad)", &settings.waves.Wave2AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx("Wave 3 (Detail)", ImGuiTreeNodeFlags_None)) {
				ImGui::SliderFloat("W3 Amplitude (m)", &settings.waves.Wave3Amplitude, 0.0f, 0.5f, "%.3f");
				ImGui::SliderFloat("W3 Wavelength (m)", &settings.waves.Wave3Wavelength, 2.0f, 50.0f, "%.1f");
				ImGui::SliderFloat("W3 Steepness", &settings.waves.Wave3Steepness, 0.0f, 0.4f, "%.3f");
				ImGui::SliderFloat("W3 Angle (rad)", &settings.waves.Wave3AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx("Fine Ripples (4-6)", ImGuiTreeNodeFlags_None)) {
				ImGui::SliderFloat("W4 Amplitude (m)", &settings.waves.Wave4Amplitude, 0.0f, 0.25f, "%.3f");
				ImGui::SliderFloat("W4 Wavelength (m)", &settings.waves.Wave4Wavelength, 1.0f, 20.0f, "%.1f");
				ImGui::SliderFloat("W4 Steepness", &settings.waves.Wave4Steepness, 0.0f, 0.35f, "%.3f");
				ImGui::SliderFloat("W4 Angle (rad)", &settings.waves.Wave4AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::Spacing();
				ImGui::SliderFloat("W5 Amplitude (m)", &settings.waves.Wave5Amplitude, 0.0f, 0.15f, "%.3f");
				ImGui::SliderFloat("W5 Wavelength (m)", &settings.waves.Wave5Wavelength, 0.5f, 10.0f, "%.1f");
				ImGui::SliderFloat("W5 Steepness", &settings.waves.Wave5Steepness, 0.0f, 0.3f, "%.3f");
				ImGui::SliderFloat("W5 Angle (rad)", &settings.waves.Wave5AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::Spacing();
				ImGui::SliderFloat("W6 Amplitude (m)", &settings.waves.Wave6Amplitude, 0.0f, 0.08f, "%.3f");
				ImGui::SliderFloat("W6 Wavelength (m)", &settings.waves.Wave6Wavelength, 0.25f, 6.0f, "%.2f");
				ImGui::SliderFloat("W6 Steepness", &settings.waves.Wave6Steepness, 0.0f, 0.25f, "%.3f");
				ImGui::SliderFloat("W6 Angle (rad)", &settings.waves.Wave6AngleOffset, -3.14f, 3.14f, "%.2f");
				ImGui::TreePop();
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (ImGui::TreeNodeEx("Shore Waves", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::TextWrapped(
					"Shallow-water attenuation in the shader: large-scale energy fades toward shore (~5 m cutoff, ~30 m blend); "
					"fine detail stays stronger in shallow water. With FFT, cascade 0 is treated as the large-scale band. "
					"Shore-directed swell blend uses the same depth bands (shader constants).");
				ImGui::Spacing();
				ImGui::SliderFloat("Shore Wave Strength", &settings.waves.ShoreWaveStrength, 0.0f, 2.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"Intensity of shore-directed swells in the 5-30 m depth band.\n"
						"0 = disabled.");
				}

				ImGui::TreePop();
			}

			if (settings.waves.UseFFTWaves && ImGui::TreeNodeEx("FFT Ocean (Advanced)", ImGuiTreeNodeFlags_None)) {
				ImGui::TextWrapped(
					"FFT simulation and spectrum. These settings do not use the Gerstner Wave 1–6 sliders above.");

				if (ImGui::TreeNodeEx("Simulation & sampling", ImGuiTreeNodeFlags_DefaultOpen)) {
					ImGui::SliderFloat("Master intensity", &settings.waves.FFTMasterIntensity, 0.0f, 2.0f, "%.2f");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Scales FFT displacement on the GPU. Set to 0 to disable ocean updates cheaply.");
					ImGui::SliderFloat("Time scale", &settings.waves.FFTSimulationTimeScale, 0.0f, 3.0f, "%.2f");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Multiplies simulation time advance (not Gerstner Wave Speed).");
					ImGui::SliderFloat("Physical height scale", &settings.waves.FFTPhysicalHeightScale, 0.0f, 5.0f, "%.2f");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Base displacement scale for the spectrum (replaces Gerstner Wave Height for FFT).");
					ImGui::SliderFloat("Horizontal chop", &settings.waves.FFTChoppiness, 0.1f, 5.0f, "%.2f");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Horizontal displacement multiplier (replaces Gerstner Wave Steepness for FFT).");

					ImGui::SliderFloat("Wind speed (m/s)", &settings.waves.FFTWindSpeedMps, 1.0f, 40.0f, "%.1f");
					ImGui::SliderAngle("Wind direction", &settings.waves.FFTWindDirectionRad, -180.0f, 180.0f);
					ImGui::SliderFloat("Fetch (km)", &settings.waves.FFTFetchKm, 10.0f, 2000.0f, "%.0f");

					ImGui::TextUnformatted("Cascade length scales (meters, sets tile size ~ 4x this)");
					ImGui::SliderFloat("Cascade 0 (swell)", &settings.waves.FFTCascadeLength0M, 5.0f, 200.0f, "%.1f");
					ImGui::SliderFloat("Cascade 1 (wind)", &settings.waves.FFTCascadeLength1M, 2.0f, 80.0f, "%.1f");
					ImGui::SliderFloat("Cascade 2 (chop)", &settings.waves.FFTCascadeLength2M, 0.5f, 30.0f, "%.1f");

					ImGui::TextUnformatted("FFT distance fade (optional override)");
					ImGui::TextWrapped(
						"Leave both at 0 to use the Gerstner \"Wave Distance Fade\" sliders above for FFT as well. "
						"Set end > start > 0 only if you want a different fade range for FFT than for Gerstner.");
					ImGui::SliderFloat("FFT fade start", &settings.waves.FFTFadeStart, 0.0f, 16384.0f, "%.0f");
					ImGui::SliderFloat("FFT fade end", &settings.waves.FFTFadeEnd, 0.0f, 327680.0f, "%.0f");

					ImGui::SliderFloat("Hull tess activity", &settings.waves.FFTTessActivity, 0.0f, 1.0f, "%.2f");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(
							"When FFT is on, hull dynamic tess does not use Gerstner phases. "
							"This blends edge subdivision between min and max (0 = flatter mesh, 1 = more aggressive).");

					ImGui::TreePop();
				}

				ImGui::Spacing();

				ImGui::SliderFloat("Swell", &settings.waves.FFTSwell, 0.0f, 2.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Wave elongation / swell factor.\n"
						"Higher = more directionally aligned, parallel wave crests.\n"
						"0 = isotropic, 1-2 = strong ocean swell.");

				ImGui::SliderFloat("Spread", &settings.waves.FFTSpread, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Directional spreading of wave energy.\n"
						"0 = uniform in all directions (fully spread),\n"
						"1 = tightly focused along wind direction.");

				ImGui::SliderFloat("Detail", &settings.waves.FFTDetail, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Small-wave suppression factor.\n"
						"1 = full spectral detail,\n"
						"0 = suppress high-frequency waves (smoother).");

				ImGui::SliderFloat("Water Depth (m)", &settings.waves.FFTWaterDepth, 1.0f, 100.0f, "%.1f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Ocean depth for TMA spectrum attenuation.\n"
						"Shallow water (1-5m) suppresses long waves.\n"
						"Deep water (>50m) has no attenuation.");

				ImGui::Spacing();
				ImGui::Text("Foam Generation");

				ImGui::SliderFloat("Whitecap Threshold", &settings.waves.FFTWhitecap, 0.0f, 2.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Jacobian threshold for whitecap generation.\n"
						"Lower = more foam, Higher = foam only on extreme crests.");

				ImGui::SliderFloat("Foam Amount", &settings.waves.FFTFoamAmount, 0.0f, 10.0f, "%.1f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Overall foam intensity from wave crest folding.\n"
						"Controls both grow and decay rates of Jacobian-based foam.");

				ImGui::Spacing();
				ImGui::Text("Surface sampling (GodotOceanWaves-style)");
				ImGui::SliderFloat("Far disp. falloff start", &settings.waves.FFTDispFarStart, 0.0f, 20000.0f, "%.0f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"0 = disabled. Beyond this camera distance (game units), FFT displacement decays as exp(-(dist-start)*rate), "
						"like GodotOceanWaves vertex falloff (~150 m, rate ~0.007).");
				ImGui::SliderFloat("Far disp. falloff rate", &settings.waves.FFTDispFarFalloff, 0.0f, 0.02f, "%.4f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Exponent per game unit past the start distance. Try ~0.007 to match Godot defaults.");
				ImGui::SliderFloat("Bicubic FFT normals (PS)", &settings.waves.FFTBicubicNormals, 0.0f, 1.0f, "%.2f");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(
						"Pixel shader only: blend FFT normal/foam texture from bilinear toward bicubic (reduces aliasing when zoomed).");

				ImGui::TreePop();
			}

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Lighting")) {
			ImGui::Checkbox("Enable Lighting Overrides", &settings.lighting.EnableLightingOverrides);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Override ESP water lighting values with custom settings.");
			}

			if (settings.lighting.EnableLightingOverrides) {
				ImGui::Spacing();
				ImGui::Text("Fresnel / Reflection");
				ImGui::SliderFloat("Fresnel Bias (F0)", &settings.lighting.FresnelBias, 0.0f, 0.2f, "%.3f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Base reflectivity. Water IOR 1.33 = ~0.02.");
				}
				ImGui::SliderFloat("Fresnel Power", &settings.lighting.FresnelPower, 1.0f, 10.0f, "%.1f");
				ImGui::SliderFloat("Reflection Strength", &settings.lighting.ReflectionStrength, 0.0f, 2.0f, "%.2f");

				ImGui::Spacing();
				ImGui::Text("Refraction / Transparency");
				ImGui::SliderFloat("Refraction Strength", &settings.lighting.RefractionStrength, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Water Transparency", &settings.lighting.WaterTransparency, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Absorption Density", &settings.lighting.AbsorptionDensity, 0.0f, 1.0f, "%.3f");
				ImGui::SliderFloat("Scattering", &settings.lighting.ScatteringCoeff, 0.0f, 0.5f, "%.3f");
				ImGui::SliderFloat("Specular Intensity", &settings.lighting.SpecularIntensity, 0.0f, 5.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Overall multiplier for all specular effects.");
				}

				ImGui::Spacing();
				ImGui::Text("Sun Specular");
				ImGui::SliderFloat("Sun Specular Power", &settings.lighting.SunSpecularPower, 10.0f, 1000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Sharpness of sun reflection. Higher = tighter highlight.");
				}
				ImGui::SliderFloat("Sun Specular Magnitude", &settings.lighting.SunSpecularMagnitude, 0.0f, 5.0f, "%.2f");
				ImGui::SliderFloat("Sun Sparkle Power", &settings.lighting.SunSparklePower, 1.0f, 200.0f, "%.0f");
				ImGui::SliderFloat("Sun Sparkle Magnitude", &settings.lighting.SunSparkleMagnitude, 0.0f, 5.0f, "%.2f");
				ImGui::SliderFloat("Specular Radius", &settings.lighting.SpecularRadius, 1.0f, 512.0f, "%.0f");
				ImGui::SliderFloat("Specular Brightness", &settings.lighting.SpecularBrightness, 0.0f, 5.0f, "%.2f");

				ImGui::Spacing();
				ImGui::Text("Depth Control");
				ImGui::SliderFloat("Depth Reflections", &settings.depth.DepthReflections, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Depth Refractions", &settings.depth.DepthRefractions, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Depth Normals", &settings.depth.DepthNormals, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Depth Specular", &settings.depth.DepthSpecularLighting, 0.0f, 2.0f, "%.2f");
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Fog")) {
			if (!settings.lighting.EnableLightingOverrides) {
				ImGui::TextDisabled("Enable Lighting Overrides to use fog settings.");
			} else {
				ImGui::Text("Above Water Fog");
				ImGui::SliderFloat("Near Distance", &settings.fog.AboveWaterFogDistNear, 0.0f, 10000.0f, "%.0f");
				ImGui::SliderFloat("Far Distance", &settings.fog.AboveWaterFogDistFar, 1000.0f, 500000.0f, "%.0f");
				ImGui::SliderFloat("Fog Amount", &settings.fog.AboveWaterFogAmount, 0.0f, 2.0f, "%.2f");

				ImGui::Spacing();
				ImGui::Text("Underwater Fog");
				ImGui::SliderFloat("UW Near Distance", &settings.fog.UnderwaterFogDistNear, 0.0f, 1000.0f, "%.0f");
				ImGui::SliderFloat("UW Far Distance", &settings.fog.UnderwaterFogDistFar, 100.0f, 20000.0f, "%.0f");
				ImGui::SliderFloat("UW Fog Amount", &settings.fog.UnderwaterFogAmount, 0.0f, 2.0f, "%.2f");
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Wading Ripples")) {
			ImGui::Checkbox("Enable Actor Ripples", &settings.ripples.EnableActorRipples);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Creates ripple effects when actors (player and NPCs) wade through water.");
			}

			if (settings.ripples.EnableActorRipples) {
				ImGui::Spacing();
				ImGui::Text("Ripple Appearance");
				ImGui::SliderFloat("Ripple Strength", &settings.ripples.RippleStrength, 0.0f, 3.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Overall intensity of ripple effects.");
				}
				ImGui::SliderFloat("Ripple Radius", &settings.ripples.RippleRadius, 128.0f, 1024.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Maximum distance ripples spread from actor.");
				}
				ImGui::SliderFloat("Normal Strength", &settings.ripples.RippleNormalStrength, 0.0f, 5.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("How much ripples affect water surface normals.");
				}

				ImGui::Spacing();
				ImGui::Text("Wave Animation");
				ImGui::SliderFloat("Wave Speed", &settings.ripples.RippleWaveSpeed, 1.0f, 10.0f, "%.1f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Speed of ripple wave animation.");
				}

				ImGui::Spacing();
				ImGui::Text("Wave Frequencies");
				ImGui::SliderFloat("Primary Freq", &settings.ripples.RippleWaveFreq1, 0.02f, 0.2f, "%.3f");
				ImGui::SliderFloat("Secondary Freq", &settings.ripples.RippleWaveFreq2, 0.04f, 0.3f, "%.3f");
				ImGui::SliderFloat("Tertiary Freq", &settings.ripples.RippleWaveFreq3, 0.06f, 0.4f, "%.3f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Higher values = more ripple rings per unit distance.");
				}
			}

			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Foam")) {
			ImGui::Checkbox("Enable Foam", &settings.foam.EnableFoam);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Generates foam on wave crests based on wave height and slope.\nFoam appears on steep, sharp waves.");
			}

			if (settings.foam.EnableFoam) {
				ImGui::Spacing();

				ImGui::SliderFloat("Lake / Ocean Intensity", &settings.foam.FoamIntensity, 0.0f, 2.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Foam density on standing water (lakes, ocean).\n0 = no foam, 1 = natural, 2 = heavy.");
				}

				ImGui::SliderFloat("River Intensity", &settings.foam.FoamIntensityFlowmap, 0.0f, 2.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Foam density on flowing water (rivers, streams).\n0 = no foam, 1 = natural, 2 = heavy.");
				}

				ImGui::SliderFloat("Concentration", &settings.foam.FoamThreshold, 0.0f, 0.8f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Controls background foam density.\n0 = heavy foam across the whole surface,\n0.8 = foam mostly on crests and active areas.");
				}

				ImGui::SliderFloat("Edge Sharpness", &settings.foam.FoamSharpness, 0.5f, 4.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Contrast of foam bubble edges.\nLow = soft / diffuse, High = crisp / defined.");
				}

				ImGui::Spacing();
				ImGui::Text("Intersection Foam");
				ImGui::Separator();

				ImGui::SliderFloat("Contact Range", &settings.foam.FoamIntersectionRange, 10.0f, 300.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Distance (game units) from submerged geometry where foam appears.\nControls how wide the foam band is around rocks, shores, etc.");
				}

				ImGui::SliderFloat("Contact Intensity", &settings.foam.FoamIntersectionIntensity, 0.0f, 2.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Strength of foam at geometry intersections.\n0 = disabled, 1 = natural, 2 = heavy.");
				}
			}
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Debug")) {
			ImGui::Checkbox("Show Tri Visualizer", &settings.general.ShowWireframe);
			if (settings.general.ShowWireframe) {
				ImGui::SameLine();
				ImGui::Checkbox("Raw Barycentrics", &settings.general.WireframeRawMode);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Shows raw barycentric coordinates as RGB.\nRed/Green/Blue at vertices = GS working.\nGray/Purple everywhere = GS not running.");
				}
			}

			if (ImGui::Button("Quick Test - Guardian Stones")) {
				if (auto ui = RE::UI::GetSingleton(); ui && !ui->menuStack.empty() && RE::PlayerCharacter::GetSingleton()) {
					RE::Console::ExecuteCommand("player.setav speedmult 1000");
					RE::Console::ExecuteCommand("tgm");
					RE::Console::ExecuteCommand("tcl");
					RE::Console::ExecuteCommand("set timescale to 0");
					RE::Console::ExecuteCommand("set gamehour to 12");
					RE::Console::ExecuteCommand("coc guardianstones");
					RE::Console::ExecuteCommand("fw 81a");
				}
			}

			if (ImGui::Button("Quick Test - Solitude Exterior")) {
				if (auto ui = RE::UI::GetSingleton(); ui && !ui->menuStack.empty() && RE::PlayerCharacter::GetSingleton()) {
					RE::Console::ExecuteCommand("player.setav speedmult 1000");
					RE::Console::ExecuteCommand("tgm");
					RE::Console::ExecuteCommand("tcl");
					RE::Console::ExecuteCommand("set timescale to 0");
					RE::Console::ExecuteCommand("set gamehour to 12");
					RE::Console::ExecuteCommand("coc solitudeexterior01");
					RE::Console::ExecuteCommand("fw 81a");
				}
			}
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void PBRWater::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>());
	perTile = new ConstantBuffer(ConstantBufferDesc<PerTile>());
	tessellationParams = new ConstantBuffer(ConstantBufferDesc<TessellationParams>());
	actorRippleBuffer = new ConstantBuffer(ConstantBufferDesc<ActorRippleBuffer>());
	fftCB = new ConstantBuffer(ConstantBufferDesc<FFTConstantData>());

	CreateFFTResources();
	CompileFFTShaders();
}

void PBRWater::Reset()
{
	hasLastTimingSample = false;
	lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	lastGameTimeHours = 0.0f;
	lastRealTimeSeconds = 0.0f;
	lastTimeScale = 1.0f;
	currentGameTimeHours = 0.0f;
	currentRealTimeSeconds = 0.0f;
	currentTimeScale = 1.0f;
	prevTileData.clear();

	tessellationDiagLoggedHull = false;
	tessellationDiagLoggedDomain = false;
	tessellationDiagLoggedGeometry = false;

	fftTime = 0.0f;
	fftPrevTime = 0.0f;
	fftLastDispatchFrame = std::numeric_limits<std::uint32_t>::max();
	fftLastRealTimeForDt = 0.0f;
	for (auto& cascade : fftCascades)
		cascade.spectrumDirty = true;
}

void PBRWater::ClearShaderCache()
{
	waterHullShader = nullptr;
	waterDomainShader = nullptr;
	waterGeometryShader = nullptr;

	tessellationDiagLoggedHull = false;
	tessellationDiagLoggedDomain = false;
	tessellationDiagLoggedGeometry = false;

	fftButterflyCS = nullptr;
	spectrumComputeCS = nullptr;
	spectrumModulateCS = nullptr;
	fftComputeCS = nullptr;
	transposeCS = nullptr;
	fftUnpackCS = nullptr;
	fftButterflyReady = false;

	for (auto& cascade : fftCascades)
		cascade.spectrumDirty = true;

	CompileFFTShaders();
}

void PBRWater::PostPostLoad()
{
	// Hook InitializeWater to suppress displacement meshes (PBR handles wave displacement via tessellation)
	stl::detour_thunk<TESWaterSystem_InitializeWater>(REL::RelocationID(31388, 32179));

	// Hook SetupGeometry and RestoreGeometry for tessellation and PBR constant buffers
	// These chain with UnifiedWater's hooks: UW::thunk -> PBR::thunk -> original
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);

	// Hook world space changes and sky cell destruction for PBR state cleanup
	// These chain with UnifiedWater's hooks on the same addresses
	stl::detour_thunk<TES_SetWorldSpace>(REL::RelocationID(13170, 13315));
	stl::detour_thunk<TES_DestroySkyCell>(REL::RelocationID(20029, 20463));

	logger::info("[PBR Water] Installed hooks");
}

// ---- FFT Ocean Implementations ----

float PBRWater::JONSWAPAlpha(float windSpeed, float fetchLength)
{
	return 0.076f * std::pow(windSpeed * windSpeed / (fetchLength * 9.81f), 0.22f);
}

float PBRWater::JONSWAPPeakFrequency(float windSpeed, float fetchLength)
{
	return 22.0f * std::pow(9.81f * 9.81f / (windSpeed * fetchLength), 1.0f / 3.0f);
}

void PBRWater::CompileFFTShaders()
{
	auto compile = [](const wchar_t* path) -> ID3D11ComputeShader* {
		return static_cast<ID3D11ComputeShader*>(
			Util::CompileShader(path, {}, "cs_5_0"));
	};

	if (!fftButterflyCS)
		fftButterflyCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\FFTButterflyCS.hlsl"));
	if (!spectrumComputeCS)
		spectrumComputeCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\SpectrumComputeCS.hlsl"));
	if (!spectrumModulateCS)
		spectrumModulateCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\SpectrumModulateCS.hlsl"));
	if (!fftComputeCS)
		fftComputeCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\FFTComputeCS.hlsl"));
	if (!transposeCS)
		transposeCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\TransposeCS.hlsl"));
	if (!fftUnpackCS)
		fftUnpackCS.attach(compile(L"Data\\Shaders\\PBRWater\\FFT\\FFTUnpackCS.hlsl"));

	fftInitialized = fftButterflyCS && spectrumComputeCS && spectrumModulateCS &&
		fftComputeCS && transposeCS && fftUnpackCS;
	if (fftInitialized) {
		logger::info("[PBR Water] FFT compute shaders compiled successfully");
	} else {
		logger::warn("[PBR Water] Some FFT compute shaders failed to compile; FFT waves disabled until compile succeeds");
	}
}

void PBRWater::CreateFFTResources()
{
	auto device = globals::d3d::device;
	const uint32_t mapSize = FFT_MAP_SIZE;
	const uint32_t numCascades = FFT_NUM_CASCADES;
	const uint32_t numStages = static_cast<uint32_t>(std::log2(mapSize));

	// Butterfly factors: numStages * mapSize float4 elements
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = numStages * mapSize * sizeof(float) * 4;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(float) * 4;
		DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, butterflyBuffer.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = numStages * mapSize;
		DX::ThrowIfFailed(device->CreateShaderResourceView(butterflyBuffer.get(), &srvDesc, butterflySRV.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = numStages * mapSize;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(butterflyBuffer.get(), &uavDesc, butterflyUAV.put()));
	}

	// FFT working buffer: numCascades * mapSize^2 * NUM_SPECTRA * 2 (ping-pong) float2 elements
	{
		uint32_t numElements = numCascades * mapSize * mapSize * FFT_NUM_SPECTRA * 2;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = numElements * sizeof(float) * 2;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(float) * 2;
		DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, fftDataBuffer.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = numElements;
		DX::ThrowIfFailed(device->CreateShaderResourceView(fftDataBuffer.get(), &srvDesc, fftDataSRV.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = numElements;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(fftDataBuffer.get(), &uavDesc, fftDataUAV.put()));
	}

	// Spectrum texture: RGBA32F Texture2DArray (numCascades layers)
	{
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = mapSize;
		texDesc.Height = mapSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = numCascades;
		texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, spectrumTexture.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = numCascades;
		DX::ThrowIfFailed(device->CreateShaderResourceView(spectrumTexture.get(), &srvDesc, spectrumSRV.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = numCascades;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(spectrumTexture.get(), &uavDesc, spectrumUAV.put()));
	}

	// Displacement map: RGBA16F Texture2DArray
	auto createTexArray = [&](winrt::com_ptr<ID3D11Texture2D>& tex,
	                          winrt::com_ptr<ID3D11ShaderResourceView>& srv,
	                          winrt::com_ptr<ID3D11UnorderedAccessView>& uav) {
		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = mapSize;
		texDesc.Height = mapSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = numCascades;
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, tex.put()));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = numCascades;
		DX::ThrowIfFailed(device->CreateShaderResourceView(tex.get(), &srvDesc, srv.put()));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray.ArraySize = numCascades;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(tex.get(), &uavDesc, uav.put()));
	};

	createTexArray(displacementTexture, displacementSRV, displacementUAV);
	createTexArray(normalFoamTexture, normalFoamSRV, normalFoamUAV);
	createTexArray(prevDisplacementTexture, prevDisplacementSRV, prevDisplacementUAV);

	// Avoid sampling garbage before the first successful unpack (also gives a stable flat ocean).
	if (auto context = globals::d3d::context) {
		const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearUnorderedAccessViewFloat(displacementUAV.get(), clearZero);
		context->ClearUnorderedAccessViewFloat(normalFoamUAV.get(), clearZero);
		context->ClearUnorderedAccessViewFloat(prevDisplacementUAV.get(), clearZero);
	}

	// Linear wrap sampler for FFT texture sampling in VS/DS/PS
	{
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.MaxAnisotropy = 1;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, fftLinearWrapSampler.put()));
	}

	logger::info("[PBR Water] FFT ocean resources created ({}x{}, {} cascades)", mapSize, mapSize, numCascades);
}

void PBRWater::UpdateCascadeParams()
{
	const float metersToGameUnits = 100.0f / 1.428f;

	const float userSwell = settings.waves.FFTSwell;
	const float userSpread = settings.waves.FFTSpread;
	const float userDetail = settings.waves.FFTDetail;
	const float userWhitecap = settings.waves.FFTWhitecap;
	const float userFoamAmount = settings.waves.FFTFoamAmount;

	const float windSpeedUser = std::max(settings.waves.FFTWindSpeedMps, 0.5f);
	const float windDir = settings.waves.FFTWindDirectionRad;
	const float fetchBaseKm = std::max(settings.waves.FFTFetchKm, 1.0f);
	const float physScale = std::max(settings.waves.FFTPhysicalHeightScale, 0.0f);
	const float fftChop = std::max(settings.waves.FFTChoppiness, 0.01f);

	auto updateCascade = [&](FFTCascadeParams& c, float dominantLenMeters, float tileMinM,
	                         float fetchScale, float fetchMinKm, float windSpeedMin, float dispMul) {
		const float tileM = std::max(dominantLenMeters * 4.0f, tileMinM);
		const float newTileLenX = tileM * metersToGameUnits;
		const float newTileLenY = tileM * metersToGameUnits;

		const float windSpeed = std::max(windSpeedUser, windSpeedMin);
		const float fetchKm = std::max(fetchBaseKm * fetchScale, fetchMinKm);

		if (c.tileLengthX != newTileLenX || c.tileLengthY != newTileLenY ||
		    c.windSpeed != windSpeed || c.windDirection != windDir ||
		    c.fetchLength != fetchKm ||
		    c.swell != userSwell || c.spread != userSpread || c.detail != userDetail)
			c.spectrumDirty = true;

		c.tileLengthX = newTileLenX;
		c.tileLengthY = newTileLenY;
		c.windSpeed = windSpeed;
		c.windDirection = windDir;
		c.fetchLength = fetchKm;
		c.swell = userSwell;
		c.detail = userDetail;
		c.spread = userSpread;
		c.whitecap = userWhitecap;
		c.foamAmount = userFoamAmount;
		c.displacementScale = physScale * metersToGameUnits * dispMul;
		c.normalScale = 1.0f;
		c.choppiness = fftChop;
	};

	const float l0 = std::max(settings.waves.FFTCascadeLength0M, 0.5f);
	const float l1 = std::max(settings.waves.FFTCascadeLength1M, 0.5f);
	const float l2 = std::max(settings.waves.FFTCascadeLength2M, 0.5f);

	updateCascade(fftCascades[0], l0, 50.0f, 1.0f, 100.0f, 3.0f, 1.0f);
	updateCascade(fftCascades[1], l1, 15.0f, 0.5f, 50.0f, 2.0f, 0.5f);
	updateCascade(fftCascades[2], l2, 4.0f, 0.25f, 20.0f, 1.5f, 0.25f);
}

void PBRWater::DispatchFFT(float deltaTime)
{
	if (!fftInitialized || !fftButterflyCS || !spectrumComputeCS ||
	    !spectrumModulateCS || !fftComputeCS || !transposeCS || !fftUnpackCS)
		return;

	auto context = globals::d3d::context;
	const uint32_t mapSize = FFT_MAP_SIZE;
	const uint32_t numStages = static_cast<uint32_t>(std::log2(mapSize));

	// Copy current displacement to previous before updating
	context->CopyResource(prevDisplacementTexture.get(), displacementTexture.get());

	fftPrevTime = fftTime;
	fftTime += deltaTime * settings.waves.FFTSimulationTimeScale;

	UpdateCascadeParams();

	// Butterfly factors: one-time init
	if (!fftButterflyReady) {
		FFTConstantData cbData{};
		cbData.MapSize = mapSize;
		fftCB->Update(cbData);

		ID3D11Buffer* cbs[] = { fftCB->CB() };
		context->CSSetConstantBuffers(6, 1, cbs);

		ID3D11UnorderedAccessView* uavs[] = { butterflyUAV.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(fftButterflyCS.get(), nullptr, 0);
		context->Dispatch(mapSize / 2 / 64, numStages, 1);

		ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		fftButterflyReady = true;
	}

	// Update every cascade each frame. Round-robin (one layer per frame) left two layers
	// stale at different fftTime values; summing all three in the shader produced
	// incoherent beat patterns (spiky “rocks”) and broken motion.
	for (uint32_t cascadeIdx = 0; cascadeIdx < FFT_NUM_CASCADES; ++cascadeIdx) {
		auto& cascade = fftCascades[cascadeIdx];
		float fetchM = cascade.fetchLength * 1000.0f;
		float alpha = JONSWAPAlpha(cascade.windSpeed, fetchM);
		float peakFreq = JONSWAPPeakFrequency(cascade.windSpeed, fetchM);

		FFTConstantData cbData{};
		cbData.MapSize = mapSize;
		cbData.CascadeIndex = cascadeIdx;
		cbData.Time = fftTime;
		cbData.DeltaTime = deltaTime;
		cbData.TileLengthX = cascade.tileLengthX;
		cbData.TileLengthY = cascade.tileLengthY;
		cbData.Depth = settings.waves.FFTWaterDepth;
		cbData.Alpha = alpha;
		cbData.PeakFrequency = peakFreq;
		cbData.WindSpeed = cascade.windSpeed;
		cbData.WindDirection = cascade.windDirection;
		cbData.Swell = cascade.swell;
		cbData.Detail = cascade.detail;
		cbData.Spread = cascade.spread;
		cbData.Whitecap = cascade.whitecap;
		cbData.FoamGrowRate = deltaTime * cascade.foamAmount * 7.5f;
		cbData.FoamDecayRate = deltaTime * std::max(0.5f, 10.0f - cascade.foamAmount) * 1.15f;
		cbData.Choppiness = cascade.choppiness;
		cbData.DisplacementScale = cascade.displacementScale;
		cbData.NormalScale = cascade.normalScale;
		cbData.SpectrumSeedX = cascade.seedX;
		cbData.SpectrumSeedY = cascade.seedY;

		fftCB->Update(cbData);
		ID3D11Buffer* cbs[] = { fftCB->CB() };
		context->CSSetConstantBuffers(6, 1, cbs);

		// Step 1: Regenerate spectrum if params changed
		if (cascade.spectrumDirty) {
			cascade.seedX = static_cast<int32_t>(cascadeIdx * 31337 + 12345);
			cascade.seedY = static_cast<int32_t>(cascadeIdx * 7919 + 54321);
			cbData.SpectrumSeedX = cascade.seedX;
			cbData.SpectrumSeedY = cascade.seedY;
			fftCB->Update(cbData);

			ID3D11UnorderedAccessView* specUavs[] = { spectrumUAV.get() };
			context->CSSetUnorderedAccessViews(0, 1, specUavs, nullptr);
			context->CSSetShader(spectrumComputeCS.get(), nullptr, 0);
			context->Dispatch(mapSize / 16, mapSize / 16, 1);

			ID3D11UnorderedAccessView* nullSpecUav[] = { nullptr };
			context->CSSetUnorderedAccessViews(0, 1, nullSpecUav, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);

			cascade.spectrumDirty = false;
		}

		// Step 2: Modulate spectrum in time
		{
			ID3D11ShaderResourceView* srvs[] = { spectrumSRV.get() };
			context->CSSetShaderResources(0, 1, srvs);

			ID3D11UnorderedAccessView* uavs[] = { fftDataUAV.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

			context->CSSetShader(spectrumModulateCS.get(), nullptr, 0);
			context->Dispatch(mapSize / 16, mapSize / 16, 1);
		}

		// Step 3: FFT row-wise pass
		{
			ID3D11ShaderResourceView* srvs[] = { butterflySRV.get() };
			context->CSSetShaderResources(0, 1, srvs);

			ID3D11UnorderedAccessView* uavs[] = { fftDataUAV.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

			context->CSSetShader(fftComputeCS.get(), nullptr, 0);
			context->Dispatch(1, mapSize, FFT_NUM_SPECTRA);
		}

		// Step 4: Transpose
		{
			context->CSSetShader(transposeCS.get(), nullptr, 0);
			context->Dispatch(mapSize / 32, mapSize / 32, FFT_NUM_SPECTRA);
		}

		// Step 5: FFT row-wise again (effectively column-wise)
		{
			context->CSSetShader(fftComputeCS.get(), nullptr, 0);
			context->Dispatch(1, mapSize, FFT_NUM_SPECTRA);
		}

		// Step 6: Unpack to displacement + normal/foam maps (one array slice per cascade)
		{
			ID3D11UnorderedAccessView* unpackUavs[] = { displacementUAV.get(), normalFoamUAV.get(), fftDataUAV.get() };
			context->CSSetUnorderedAccessViews(0, 3, unpackUavs, nullptr);

			context->CSSetShader(fftUnpackCS.get(), nullptr, 0);
			context->Dispatch(mapSize / 16, mapSize / 16, 1);
		}
	}

	// Clean up CS state
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 1, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}

// ---- Hook Implementations ----

void PBRWater::TESWaterSystem_InitializeWater::thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool noDisplacement, bool isProcedural)
{
	(void)noDisplacement;  // Intentionally unused - we force true below
	// Force noDisplacement=true to prevent the engine from creating a separate
	// displacement mesh (WADING geometry) near the player. This eliminates the
	// double-rendering issue where two water meshes would show Gerstner waves
	// slightly out of sync. The wading ripple effects are still applied by
	// sampling DisplacementTex on the regular water geometry using gDisplacementMeshPos.
	func(waterSystem, waterTri, form, waterHeight, unk4, true, isProcedural);
}

void PBRWater::TES_SetWorldSpace::thunk(RE::TES* tes, RE::TESWorldSpace* worldSpace, bool isExterior)
{
	func(tes, worldSpace, isExterior);

	// Clear PBR Water state on world space change
	auto& singleton = globals::features::pbrWater;
	singleton.prevTileData.clear();
	singleton.hasLastTimingSample = false;
	singleton.lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	singleton.lastGameTimeHours = 0.0f;
	singleton.lastRealTimeSeconds = 0.0f;
	singleton.lastTimeScale = 1.0f;
	singleton.currentGameTimeHours = 0.0f;
	singleton.currentRealTimeSeconds = 0.0f;
	singleton.currentTimeScale = 1.0f;
	singleton.fftLastDispatchFrame = std::numeric_limits<std::uint32_t>::max();
	singleton.fftLastRealTimeForDt = 0.0f;
}

void PBRWater::TES_DestroySkyCell::thunk(RE::TES* tes)
{
	func(tes);

	// Clear PBR Water state on sky cell destruction
	auto& singleton = globals::features::pbrWater;
	singleton.prevTileData.clear();
	singleton.hasLastTimingSample = false;
	singleton.lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	singleton.lastGameTimeHours = 0.0f;
	singleton.lastRealTimeSeconds = 0.0f;
	singleton.lastTimeScale = 1.0f;
	singleton.currentGameTimeHours = 0.0f;
	singleton.currentRealTimeSeconds = 0.0f;
	singleton.currentTimeScale = 1.0f;
	singleton.fftLastDispatchFrame = std::numeric_limits<std::uint32_t>::max();
	singleton.fftLastRealTimeForDt = 0.0f;
}

void PBRWater::BSWaterShader_SetupGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	auto& singleton = globals::features::pbrWater;
	auto context = globals::d3d::context;

	// Lazy compilation of tessellation shaders on first water render
	if (!singleton.waterHullShader || !singleton.waterDomainShader || !singleton.waterGeometryShader) {
		uint32_t descriptor = static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::NormalTexCoord) |
		                      static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::Flowmap) |
		                      static_cast<uint32_t>(SIE::ShaderCache::WaterShaderFlags::BlendNormals);

		// Temporarily force synchronous compilation to ensure shaders are available immediately
		// (GetHullShaderBlob returns nullptr in async mode and queues compilation for later)
		bool wasAsync = globals::shaderCache->IsAsync();
		if (wasAsync) {
			globals::shaderCache->SetAsync(false);
		}

		if (!singleton.waterHullShader) {
			if (auto* hullBlob = globals::shaderCache->GetHullShaderBlob(*waterShader, descriptor)) {
				auto device = globals::d3d::device;
				ID3D11HullShader* hullShader = nullptr;
				const HRESULT hr = device->CreateHullShader(hullBlob->GetBufferPointer(), hullBlob->GetBufferSize(), nullptr, &hullShader);
				if (SUCCEEDED(hr)) {
					singleton.waterHullShader.attach(hullShader);
				} else if (!singleton.tessellationDiagLoggedHull) {
					singleton.tessellationDiagLoggedHull = true;
					logger::error("Failed to create {} shader {}::{:X}",
						magic_enum::enum_name(SIE::ShaderClass::Hull),
						magic_enum::enum_name(waterShader->shaderType.get()),
						descriptor);
				}
			} else if (!singleton.tessellationDiagLoggedHull) {
				singleton.tessellationDiagLoggedHull = true;
				globals::shaderCache->LogHullDomainGeometryBlobUnavailable(*waterShader, descriptor, SIE::ShaderClass::Hull);
			}
		}

		if (!singleton.waterDomainShader) {
			if (auto* domainBlob = globals::shaderCache->GetDomainShaderBlob(*waterShader, descriptor)) {
				auto device = globals::d3d::device;
				ID3D11DomainShader* domainShader = nullptr;
				const HRESULT hr = device->CreateDomainShader(domainBlob->GetBufferPointer(), domainBlob->GetBufferSize(), nullptr, &domainShader);
				if (SUCCEEDED(hr)) {
					singleton.waterDomainShader.attach(domainShader);
				} else if (!singleton.tessellationDiagLoggedDomain) {
					singleton.tessellationDiagLoggedDomain = true;
					logger::error("Failed to create {} shader {}::{:X}",
						magic_enum::enum_name(SIE::ShaderClass::Domain),
						magic_enum::enum_name(waterShader->shaderType.get()),
						descriptor);
				}
			} else if (!singleton.tessellationDiagLoggedDomain) {
				singleton.tessellationDiagLoggedDomain = true;
				globals::shaderCache->LogHullDomainGeometryBlobUnavailable(*waterShader, descriptor, SIE::ShaderClass::Domain);
			}
		}

		if (!singleton.waterGeometryShader) {
			if (auto* geometryBlob = globals::shaderCache->GetGeometryShaderBlob(*waterShader, descriptor)) {
				auto device = globals::d3d::device;
				ID3D11GeometryShader* geometryShader = nullptr;
				const HRESULT hr = device->CreateGeometryShader(geometryBlob->GetBufferPointer(), geometryBlob->GetBufferSize(), nullptr, &geometryShader);
				if (SUCCEEDED(hr)) {
					singleton.waterGeometryShader.attach(geometryShader);
				} else if (!singleton.tessellationDiagLoggedGeometry) {
					singleton.tessellationDiagLoggedGeometry = true;
					logger::error("Failed to create {} shader {}::{:X}",
						magic_enum::enum_name(SIE::ShaderClass::Geometry),
						magic_enum::enum_name(waterShader->shaderType.get()),
						descriptor);
				}
			} else if (!singleton.tessellationDiagLoggedGeometry) {
				singleton.tessellationDiagLoggedGeometry = true;
				globals::shaderCache->LogHullDomainGeometryBlobUnavailable(*waterShader, descriptor, SIE::ShaderClass::Geometry);
			}
		}

		// Restore async mode if it was enabled
		if (wasAsync) {
			globals::shaderCache->SetAsync(true);
		}
	}

	// Clean up any lingering tessellation state from previous passes BEFORE calling func()
	// This ensures the original SetupGeometry sees a clean non-tessellated pipeline state
	if (tessellationActiveForPass) {
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		if (originalTopology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			context->IASetPrimitiveTopology(originalTopology);
		}
	}

	tessellationActiveForPass = false;
	originalTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

	// Tessellation HS/DS is only bound for water techniques < 8; for LOD/simple/etc. VS must displace waves.
	const uint32_t waterTechnique = (pass->passEnum >> 11) & 0xF;
	const bool techniqueSupportsTessel = (waterTechnique < 8);
	const bool tessellationActiveThisPass = singleton.settings.tessellation.EnableTessellation &&
	                                        singleton.waterHullShader && singleton.waterDomainShader && singleton.waterGeometryShader &&
	                                        techniqueSupportsTessel;

	// ---- FFT Ocean: dispatch compute shaders (once per frame; stable dt for animation) ----
	if (singleton.settings.waves.UseFFTWaves && singleton.settings.waves.FFTMasterIntensity > 0.001f) {
		const auto* st = globals::state;
		const std::uint32_t frameIdx = st ? st->frameCount : 0u;
		if (st && frameIdx != singleton.fftLastDispatchFrame) {
			singleton.fftLastDispatchFrame = frameIdx;
			const float realTime = st->timer;
			float dt = realTime - singleton.fftLastRealTimeForDt;
			if (dt < 0.0f || dt > 1.0f)
				dt = 1.0f / 60.0f;
			singleton.fftLastRealTimeForDt = realTime;
			singleton.DispatchFFT(dt);
		}
	}

	// ---- Fill and bind per-frame constant buffer ----
	if (singleton.perFrame) {
		PerFrame perFrameData{};
		perFrameData.WaveIntensity = singleton.settings.waves.WaveIntensity;
		perFrameData.WaveAmplitude = singleton.settings.waves.WaveAmplitude;
		perFrameData.WaveSpeed = singleton.settings.waves.WaveSpeed;
		perFrameData.WaveSteepness = singleton.settings.waves.WaveSteepness;

		// Water lighting override parameters
		perFrameData.EnableLightingOverrides = singleton.settings.lighting.EnableLightingOverrides ? 1.0f : 0.0f;
		perFrameData.FresnelBias = singleton.settings.lighting.FresnelBias;
		perFrameData.FresnelPower = singleton.settings.lighting.FresnelPower;
		perFrameData.ReflectionStrength = singleton.settings.lighting.ReflectionStrength;
		perFrameData.RefractionStrength = singleton.settings.lighting.RefractionStrength;
		perFrameData.WaterTransparency = singleton.settings.lighting.WaterTransparency;
		perFrameData.AbsorptionDensity = singleton.settings.lighting.AbsorptionDensity;
		perFrameData.ScatteringCoeff = singleton.settings.lighting.ScatteringCoeff;
		perFrameData.SpecularIntensity = singleton.settings.lighting.SpecularIntensity;

		// Sun specular overrides
		perFrameData.SunSpecularPower = singleton.settings.lighting.SunSpecularPower;
		perFrameData.SunSpecularMagnitude = singleton.settings.lighting.SunSpecularMagnitude;
		perFrameData.SunSparklePower = singleton.settings.lighting.SunSparklePower;
		perFrameData.SunSparkleMagnitude = singleton.settings.lighting.SunSparkleMagnitude;
		perFrameData.SpecularRadius = singleton.settings.lighting.SpecularRadius;
		perFrameData.SpecularBrightness = singleton.settings.lighting.SpecularBrightness;

		// Fog overrides
		perFrameData.AboveWaterFogDistNear = singleton.settings.fog.AboveWaterFogDistNear;
		perFrameData.AboveWaterFogDistFar = singleton.settings.fog.AboveWaterFogDistFar;
		perFrameData.AboveWaterFogAmount = singleton.settings.fog.AboveWaterFogAmount;
		perFrameData.UnderwaterFogDistNear = singleton.settings.fog.UnderwaterFogDistNear;
		perFrameData.UnderwaterFogDistFar = singleton.settings.fog.UnderwaterFogDistFar;
		perFrameData.UnderwaterFogAmount = singleton.settings.fog.UnderwaterFogAmount;

		// Depth properties
		perFrameData.DepthReflections = singleton.settings.depth.DepthReflections;
		perFrameData.DepthRefractions = singleton.settings.depth.DepthRefractions;
		perFrameData.DepthNormals = singleton.settings.depth.DepthNormals;
		perFrameData.DepthSpecularLighting = singleton.settings.depth.DepthSpecularLighting;

		// WireframeEnabled: 0=off, 1=wireframe, 2=raw barycentrics debug
		perFrameData.WireframeEnabled = singleton.settings.general.ShowWireframe ?
			(singleton.settings.general.WireframeRawMode ? 2.0f : 1.0f) : 0.0f;
		perFrameData.PerFramePad0 = 0.0f;
		perFrameData.PerFramePad1 = 0.0f;
		perFrameData.PerFramePad2 = 0.0f;

		// Wave parameters (Period removed - speed now calculated from wavelength via physics)
		perFrameData.Wave1Amplitude = singleton.settings.waves.Wave1Amplitude;
		perFrameData.Wave1Wavelength = singleton.settings.waves.Wave1Wavelength;
		perFrameData.Wave1Steepness = singleton.settings.waves.Wave1Steepness;

		perFrameData.Wave2Amplitude = singleton.settings.waves.Wave2Amplitude;
		perFrameData.Wave2Wavelength = singleton.settings.waves.Wave2Wavelength;
		perFrameData.Wave2Steepness = singleton.settings.waves.Wave2Steepness;

		perFrameData.Wave3Amplitude = singleton.settings.waves.Wave3Amplitude;
		perFrameData.Wave3Wavelength = singleton.settings.waves.Wave3Wavelength;
		perFrameData.Wave3Steepness = singleton.settings.waves.Wave3Steepness;

		perFrameData.Wave4Amplitude = singleton.settings.waves.Wave4Amplitude;
		perFrameData.Wave4Wavelength = singleton.settings.waves.Wave4Wavelength;
		perFrameData.Wave4Steepness = singleton.settings.waves.Wave4Steepness;

		perFrameData.Wave5Amplitude = singleton.settings.waves.Wave5Amplitude;
		perFrameData.Wave5Wavelength = singleton.settings.waves.Wave5Wavelength;
		perFrameData.Wave5Steepness = singleton.settings.waves.Wave5Steepness;

		perFrameData.Wave6Amplitude = singleton.settings.waves.Wave6Amplitude;
		perFrameData.Wave6Wavelength = singleton.settings.waves.Wave6Wavelength;
		perFrameData.Wave6Steepness = singleton.settings.waves.Wave6Steepness;

		// Wave angles are already in radians from the UI
		perFrameData.Wave1AngleOffset = singleton.settings.waves.Wave1AngleOffset;
		perFrameData.Wave2AngleOffset = singleton.settings.waves.Wave2AngleOffset;
		perFrameData.Wave3AngleOffset = singleton.settings.waves.Wave3AngleOffset;
		perFrameData.Wave4AngleOffset = singleton.settings.waves.Wave4AngleOffset;
		perFrameData.Wave5AngleOffset = singleton.settings.waves.Wave5AngleOffset;
		perFrameData.Wave6AngleOffset = singleton.settings.waves.Wave6AngleOffset;

		// Per-pass: only skip VS displacement when HS/DS will actually run for this draw
		perFrameData.TessellationEnabled = tessellationActiveThisPass ? 1.0f : 0.0f;
		perFrameData.WaveFadeStart = singleton.settings.waves.WaveFadeStart;
		perFrameData.WaveFadeEnd = singleton.settings.waves.WaveFadeEnd;

		// Player ripple data
		perFrameData.PlayerPosX = 0.0f;
		perFrameData.PlayerPosY = 0.0f;
		perFrameData.PlayerPosZ = 0.0f;
		perFrameData.PlayerSpeed = 0.0f;
		perFrameData.PlayerInWater = 0.0f;
		perFrameData.PlayerVelocityX = 0.0f;
		perFrameData.PlayerVelocityY = 0.0f;
		perFrameData.PlayerWaterDepth = 0.0f;

		// Ripple settings from UI
		perFrameData.RippleStrength = singleton.settings.ripples.EnableActorRipples ? singleton.settings.ripples.RippleStrength : 0.0f;
		perFrameData.RippleRadius = singleton.settings.ripples.RippleRadius;
		perFrameData.RippleWaveSpeed = singleton.settings.ripples.RippleWaveSpeed;
		perFrameData.RippleWaveFreq1 = singleton.settings.ripples.RippleWaveFreq1;
		perFrameData.RippleWaveFreq2 = singleton.settings.ripples.RippleWaveFreq2;
		perFrameData.RippleWaveFreq3 = singleton.settings.ripples.RippleWaveFreq3;
		perFrameData.RippleNormalStrength = singleton.settings.ripples.RippleNormalStrength;

		// Foam system
		perFrameData.FoamEnabled = singleton.settings.foam.EnableFoam ? 1.0f : 0.0f;
		perFrameData.FoamIntensity = singleton.settings.foam.FoamIntensity;
		perFrameData.FoamIntensityFlowmap = singleton.settings.foam.FoamIntensityFlowmap;
		perFrameData.FoamThreshold = singleton.settings.foam.FoamThreshold;
		perFrameData.FoamSharpness = singleton.settings.foam.FoamSharpness;
		perFrameData.FoamIntersectionRange = singleton.settings.foam.FoamIntersectionRange;
		perFrameData.FoamIntersectionIntensity = singleton.settings.foam.FoamIntersectionIntensity;
		perFrameData.FoamPad_c22z = 0.0f;
		perFrameData.FoamPad_c22w = 0.0f;
		perFrameData.FoamPad_c23x = 0.0f;
		perFrameData.FoamPad_c23y = 0.0f;
		perFrameData.FoamPad_c23z = 0.0f;

		// Legacy shader slots (unused — shallow large-wave range is fixed in GerstnerWaves.hlsli)
		perFrameData.ShallowWaveDepthMin = 0.0f;
		perFrameData.ShallowWaveDepthMax = 0.0f;
		perFrameData.ShoreWavePad0 = 0.0f;
		perFrameData.ShoreWaveStrength = singleton.settings.waves.ShoreWaveStrength;

		// Get terrain heightmap parameters from Terrain Shadows feature
		auto& terrainShadows = globals::features::terrainShadows;
		auto terrainData = terrainShadows.GetCommonBufferData();
		perFrameData.TerrainHeightmapEnabled = terrainData.EnableTerrainShadow ? 1.0f : 0.0f;
		perFrameData.TerrainScaleX = terrainData.Scale.x;
		perFrameData.TerrainScaleY = terrainData.Scale.y;
		perFrameData.TerrainOffsetX = terrainData.Offset.x;
		perFrameData.TerrainOffsetY = terrainData.Offset.y;
		perFrameData.TerrainZRangeMin = terrainData.ZRange.x;
		perFrameData.TerrainZRangeMax = terrainData.ZRange.y;
		perFrameData.FFTWavesEnabled = (singleton.fftInitialized && singleton.settings.waves.UseFFTWaves) ? 1.0f : 0.0f;

		// FFT cascade parameters for surface shader sampling
		perFrameData.FFTCascade0TileLenX = singleton.fftCascades[0].tileLengthX;
		perFrameData.FFTCascade0TileLenY = singleton.fftCascades[0].tileLengthY;
		perFrameData.FFTCascade0DispScale = singleton.fftCascades[0].displacementScale * singleton.settings.waves.FFTMasterIntensity;
		perFrameData.FFTCascade0NormScale = singleton.fftCascades[0].normalScale;
		perFrameData.FFTCascade1TileLenX = singleton.fftCascades[1].tileLengthX;
		perFrameData.FFTCascade1TileLenY = singleton.fftCascades[1].tileLengthY;
		perFrameData.FFTCascade1DispScale = singleton.fftCascades[1].displacementScale * singleton.settings.waves.FFTMasterIntensity;
		perFrameData.FFTCascade1NormScale = singleton.fftCascades[1].normalScale;
		perFrameData.FFTCascade2TileLenX = singleton.fftCascades[2].tileLengthX;
		perFrameData.FFTCascade2TileLenY = singleton.fftCascades[2].tileLengthY;
		perFrameData.FFTCascade2DispScale = singleton.fftCascades[2].displacementScale * singleton.settings.waves.FFTMasterIntensity;
		perFrameData.FFTCascade2NormScale = singleton.fftCascades[2].normalScale;
		perFrameData.FFTChoppiness = singleton.settings.waves.FFTChoppiness;
		perFrameData.FFTNumCascades = static_cast<float>(FFT_NUM_CASCADES);
		perFrameData.FFTDispFarStart = singleton.settings.waves.FFTDispFarStart;
		perFrameData.FFTDispFarFalloff = singleton.settings.waves.FFTDispFarFalloff;
		perFrameData.FFTBicubicNormals = singleton.settings.waves.FFTBicubicNormals;
		perFrameData.FFTMasterIntensity = singleton.settings.waves.FFTMasterIntensity;
		perFrameData.FFTFadeStart = singleton.settings.waves.FFTFadeStart;
		perFrameData.FFTFadeEnd = singleton.settings.waves.FFTFadeEnd;
		perFrameData.FFTTessActivity = singleton.settings.waves.FFTTessActivity;

		// Get timing data for player velocity calculation
		float currentRealTime = globals::state ? globals::state->timer : 0.0f;

		float waterSurfaceHeight = 0.0f;
		bool hasWaterHeight = false;

		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			auto pos = player->GetPosition();
			perFrameData.PlayerPosX = pos.x;
			perFrameData.PlayerPosY = pos.y;
			perFrameData.PlayerPosZ = pos.z;

			// Get player movement speed from ActorState
			perFrameData.PlayerSpeed = player->AsActorState()->DoGetMovementSpeed();

			// Calculate actual velocity from position change
			if (singleton.hasPlayerMovementData && currentRealTime > singleton.lastPlayerUpdateTime) {
				float deltaTime = currentRealTime - singleton.lastPlayerUpdateTime;
				if (deltaTime > 0.001f && deltaTime < 1.0f) {
					singleton.playerVelocity.x = (pos.x - singleton.lastPlayerPos.x) / deltaTime;
					singleton.playerVelocity.y = (pos.y - singleton.lastPlayerPos.y) / deltaTime;
				}
			}
			singleton.lastPlayerPos = pos;
			singleton.lastPlayerUpdateTime = currentRealTime;
			singleton.hasPlayerMovementData = true;

			perFrameData.PlayerVelocityX = singleton.playerVelocity.x;
			perFrameData.PlayerVelocityY = singleton.playerVelocity.y;

			// Get the relevant water height for the player
			float playerWaterHeight = player->GetWaterHeight();
			if (playerWaterHeight > -1000000.0f) {
				hasWaterHeight = true;
				waterSurfaceHeight = playerWaterHeight;

				// Calculate depth below water surface (positive = underwater)
				perFrameData.PlayerWaterDepth = playerWaterHeight - pos.z;

				// Player head height estimate: ~120 units above feet (player Z position)
				float estimatedHeadHeight = pos.z + 120.0f;

				// Only create ripples if wading (feet wet but head above water)
				if (pos.z < playerWaterHeight + 64.0f && estimatedHeadHeight > playerWaterHeight) {
					perFrameData.PlayerInWater = 1.0f;
				}
			}
		}

		// Update actor ripple buffer with all actors near water
		ActorRippleBuffer actorRippleData{};
		actorRippleData.numActors = 0;

		if (hasWaterHeight) {
			RE::NiPoint3 cameraPos;
			if (auto player = RE::PlayerCharacter::GetSingleton()) {
				cameraPos = player->GetPosition();
			}

			if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
				for (auto& actorHandle : processLists->highActorHandles) {
					if (actorRippleData.numActors >= MAX_ACTOR_RIPPLES)
						break;

					auto actorPtr = actorHandle.get();
					if (!actorPtr || !actorPtr.get() || !actorPtr.get()->Is3DLoaded())
						continue;

					auto actor = actorPtr.get();
					auto pos = actor->GetPosition();

					// Skip actors too far from camera
					float distFromCamera = cameraPos.GetDistance(pos);
					if (distFromCamera > 4096.0f)
						continue;

					// Get the actor's relevant water height
					float actorWaterHeight = actor->GetWaterHeight();
					if (actorWaterHeight <= -1000000.0f)
						continue;

					// Check if actor is near water surface
					float heightAboveWater = pos.z - actorWaterHeight;
					bool nearWater = (heightAboveWater > -256.0f && heightAboveWater < 128.0f);

					if (!nearWater)
						continue;

					ActorRippleData& ripple = actorRippleData.actors[actorRippleData.numActors];
					ripple.PosX = pos.x;
					ripple.PosY = pos.y;

					float estimatedHeadHeight = pos.z + 100.0f;

					ripple.InWater = (heightAboveWater < 64.0f && estimatedHeadHeight > actorWaterHeight) ? 1.0f : 0.0f;
					ripple.WaterDepth = actorWaterHeight - pos.z;

					ripple.Speed = actor->AsActorState()->DoGetMovementSpeed();

					float angleZ = actor->GetAngleZ();
					float speed = ripple.Speed;
					ripple.VelocityX = sin(angleZ) * speed;
					ripple.VelocityY = cos(angleZ) * speed;

					actorRippleData.numActors++;
				}
			}
		}

		singleton.actorRippleBuffer->Update(actorRippleData);

		const auto* state = globals::state;
		const std::uint32_t frameIndex = state ? state->frameCount : singleton.lastTimingFrameIndex;
		if (singleton.lastTimingFrameIndex != frameIndex) {
			if (singleton.hasLastTimingSample) {
				singleton.lastGameTimeHours = singleton.currentGameTimeHours;
				singleton.lastRealTimeSeconds = singleton.currentRealTimeSeconds;
				singleton.lastTimeScale = singleton.currentTimeScale;
			}
			singleton.lastTimingFrameIndex = frameIndex;
		}

		float gameTimeHours = 0.0f;
		float realTimeSeconds = 0.0f;
		float timeScale = 1.0f;

		if (const auto calendar = RE::Calendar::GetSingleton()) {
			gameTimeHours = calendar->GetHoursPassed();
			timeScale = calendar->GetTimescale();
		}

		if (globals::state) {
			realTimeSeconds = globals::state->timer;
		}

		perFrameData.GameTimeHours = gameTimeHours;
		perFrameData.RealTimeSeconds = realTimeSeconds;
		perFrameData.TimeScale = timeScale;
		perFrameData.CellWorldSize = 4096.0f;
		perFrameData.PrevGameTimeHours = singleton.hasLastTimingSample ? singleton.lastGameTimeHours : gameTimeHours;
		perFrameData.PrevRealTimeSeconds = singleton.hasLastTimingSample ? singleton.lastRealTimeSeconds : realTimeSeconds;
		perFrameData.PrevTimeScale = singleton.hasLastTimingSample ? singleton.lastTimeScale : timeScale;

		singleton.perFrame->Update(perFrameData);

		ID3D11Buffer* buffers[1] = { singleton.perFrame->CB() };
		context->VSSetConstantBuffers(7, 1, buffers);
		context->PSSetConstantBuffers(7, 1, buffers);

		// Bind actor ripple buffer to slot 10
		ID3D11Buffer* actorBuffers[1] = { singleton.actorRippleBuffer->CB() };
		context->PSSetConstantBuffers(10, 1, actorBuffers);

		singleton.currentGameTimeHours = gameTimeHours;
		singleton.currentRealTimeSeconds = realTimeSeconds;
		singleton.currentTimeScale = timeScale;
		singleton.hasLastTimingSample = true;
	}

	// ---- Fill and bind per-tile constant buffer ----
	{
		int32_t x, y;
		Util::WorldToCell(pass->geometry->world.translate, x, y);

		// Determine LOD level from the shape name if available
		int32_t lodLevel = 1;

		if (pass->geometry->name.c_str()) {
			const char* name = pass->geometry->name.c_str();
			if (strncmp(name, "WaterLOD_", 9) == 0) {
				lodLevel = atoi(name + 9);
				if (lodLevel != 1 && lodLevel != 4 && lodLevel != 8 && lodLevel != 16 && lodLevel != 32) {
					lodLevel = 1;
				}
			}
		}

		if (singleton.perTile) {
			PerTile perTileData{};

			RE::TESWorldSpace* activeWorldSpace = nullptr;
			std::uint32_t worldSpaceId = 0;
			if (const auto tes = RE::TES::GetSingleton()) {
				activeWorldSpace = tes->GetRuntimeData2().worldSpace;
				if (activeWorldSpace) {
					worldSpaceId = activeWorldSpace->GetFormID();
				}
			}

			auto mixKey = [](std::uint64_t seed, std::uint64_t value) noexcept {
				seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
				return seed;
			};

			std::uint64_t tileKeySeed = 0;
			tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(worldSpaceId));
			tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(lodLevel)));
			tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
			tileKeySeed = mixKey(tileKeySeed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)));
			const std::uint64_t tileKey = tileKeySeed;

			float prevNormalX = 0.0f;
			float prevNormalY = 0.0f;
			float prevDistance = 10000.0f;
			float prevSegments = 32.0f;
			const auto prevTileIt = singleton.prevTileData.find(tileKey);
			if (prevTileIt != singleton.prevTileData.end()) {
				prevNormalX = prevTileIt->second.normalX;
				prevNormalY = prevTileIt->second.normalY;
				prevDistance = prevTileIt->second.distance;
				prevSegments = prevTileIt->second.segmentsPerAxis;
			}

			perTileData.PrevData[0] = prevNormalX;
			perTileData.PrevData[1] = prevNormalY;
			perTileData.PrevData[2] = prevDistance;
			perTileData.PrevData[3] = prevSegments;

			perTileData.TileData[0] = static_cast<float>(x);
			perTileData.TileData[1] = static_cast<float>(y);
			perTileData.TileData[2] = static_cast<float>(lodLevel);
			perTileData.TileData[3] = 1.0f;

			float currentSegmentsPerAxis = prevSegments;
			if (const auto triShape = pass->geometry->AsTriShape()) {
				auto& runtimeData = triShape->GetTrishapeRuntimeData();
				const float triangleCount = static_cast<float>(runtimeData.triangleCount);
				if (triangleCount > 0.0f) {
					currentSegmentsPerAxis = std::max(1.0f, std::sqrt(triangleCount * 0.5f));
				}
			}
			perTileData.PrevData[3] = currentSegmentsPerAxis;

			float storedNormalX = prevNormalX;
			float storedNormalY = prevNormalY;
			float storedDistance = prevDistance;

			singleton.prevTileData[tileKey] = PBRWater::PrevTileData{ storedNormalX, storedNormalY, storedDistance, currentSegmentsPerAxis };

			singleton.perTile->Update(perTileData);

			ID3D11Buffer* tileBuffers[1] = { singleton.perTile->CB() };
			context->VSSetConstantBuffers(8, 1, tileBuffers);
			context->PSSetConstantBuffers(8, 1, tileBuffers);
		}
	}

	static bool loggedTessSetup = false;
	static int tessFrameCount = 0;
	tessFrameCount++;
	bool shouldLog = !loggedTessSetup;

	if (shouldLog) {
		logger::info("[PBR Water] SetupGeometry - passEnum:0x{:X} technique:{} numLights:{} tessCompat:{}",
			pass->passEnum, waterTechnique, pass->numLights, techniqueSupportsTessel);
		loggedTessSetup = true;
	}

	// CRITICAL: Call original SetupGeometry FIRST to set up VS, PS, textures, etc.
	// THEN apply tessellation state after, so it's not overwritten by the original
	func(waterShader, pass);

	// ---- Post-original: Bind terrain heightmap texture to VS/DS ----
	ID3D11ShaderResourceView* terrainHeightSRV[1] = { nullptr };
	context->PSGetShaderResources(60, 1, terrainHeightSRV);
	if (terrainHeightSRV[0]) {
		context->VSSetShaderResources(60, 1, terrainHeightSRV);
		context->DSSetShaderResources(60, 1, terrainHeightSRV);
		terrainHeightSRV[0]->Release();
	}

	// Bind a linear sampler to VS/DS for terrain heightmap sampling (slot 12)
	ID3D11SamplerState* terrainSampler[1] = { nullptr };
	context->PSGetSamplers(4, 1, terrainSampler);
	if (terrainSampler[0]) {
		context->VSSetSamplers(12, 1, terrainSampler);
		context->DSSetSamplers(12, 1, terrainSampler);
		terrainSampler[0]->Release();
	}

	// ---- Bind FFT displacement/normal textures to VS/HS/DS/PS ----
	if (singleton.fftInitialized && singleton.displacementSRV && singleton.normalFoamSRV) {
		ID3D11ShaderResourceView* fftSRVs[3] = {
			singleton.displacementSRV.get(),
			singleton.normalFoamSRV.get(),
			singleton.prevDisplacementSRV.get()
		};
		context->VSSetShaderResources(61, 3, fftSRVs);
		context->HSSetShaderResources(61, 3, fftSRVs);
		context->DSSetShaderResources(61, 3, fftSRVs);
		context->PSSetShaderResources(61, 3, fftSRVs);

		ID3D11SamplerState* fftSamplers[1] = { singleton.fftLinearWrapSampler.get() };
		context->VSSetSamplers(13, 1, fftSamplers);
		context->HSSetSamplers(13, 1, fftSamplers);
		context->DSSetSamplers(13, 1, fftSamplers);
		context->PSSetSamplers(13, 1, fftSamplers);
	}

	// ---- Tessellation Setup ----

	// Track if we need to bind just the geometry shader (for tri visualizer without tessellation)
	bool geometryShaderOnlyForVisualizer = !tessellationActiveThisPass &&
	                                        singleton.settings.general.ShowWireframe &&
	                                        singleton.waterHullShader && singleton.waterDomainShader && singleton.waterGeometryShader &&
	                                        techniqueSupportsTessel;

	if (tessellationActiveThisPass) {
		// Update tessellation constant buffer
		if (singleton.tessellationParams) {
			TessellationParams tessParams{};
			tessParams.TessellationMinDistance = 0.0f;
			tessParams.TessellationMaxDistance = singleton.settings.tessellation.TessellationMaxDistance;
			tessParams.TessellationMinFactor = singleton.settings.tessellation.TessellationMinFactor;
			tessParams.TessellationMaxFactor = singleton.settings.tessellation.TessellationMaxFactor;

			auto cameraPos = Util::GetEyePosition(0);
			tessParams.CameraWorldPosX = cameraPos.x;
			tessParams.CameraWorldPosY = cameraPos.y;
			tessParams.CameraWorldPosZ = cameraPos.z;
			tessParams.DetailHeightScale = 0.0f;

			singleton.tessellationParams->Update(tessParams);

			ID3D11Buffer* tessCB = singleton.tessellationParams->CB();
			context->HSSetConstantBuffers(9, 1, &tessCB);
			context->DSSetConstantBuffers(9, 1, &tessCB);
		}

		context->IAGetPrimitiveTopology(&originalTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		context->HSSetShader(singleton.waterHullShader.get(), nullptr, 0);
		context->DSSetShader(singleton.waterDomainShader.get(), nullptr, 0);
		// Geometry shader exists only to emit barycentrics for wireframe debug — binding it on every
		// tessellated draw destroys throughput (historically ~2–3× GS expansion cost on top of tess).
		if (singleton.settings.general.ShowWireframe && singleton.waterGeometryShader) {
			context->GSSetShader(singleton.waterGeometryShader.get(), nullptr, 0);
		} else {
			context->GSSetShader(nullptr, nullptr, 0);
		}

		// Forward VS constant buffers (b0-b2) to DS for transforms
		ID3D11Buffer* vsBuffers[3] = { nullptr, nullptr, nullptr };
		context->VSGetConstantBuffers(0, 3, vsBuffers);
		context->DSSetConstantBuffers(0, 3, vsBuffers);

		// Forward FrameBuffer (b12) to HS and DS for CameraPosAdjust
		ID3D11Buffer* frameBuffer = nullptr;
		context->PSGetConstantBuffers(12, 1, &frameBuffer);
		if (frameBuffer) {
			context->HSSetConstantBuffers(12, 1, &frameBuffer);
			context->DSSetConstantBuffers(12, 1, &frameBuffer);
		}

		// Forward PBR Water per-frame (b7) to HS/DS
		if (singleton.perFrame) {
			ID3D11Buffer* perFrameCB = singleton.perFrame->CB();
			context->HSSetConstantBuffers(7, 1, &perFrameCB);
			context->DSSetConstantBuffers(7, 1, &perFrameCB);
		}

		// Forward normal SRVs/samplers (slots 4-6) and flowmap (slots 8-9) to DS
		ID3D11ShaderResourceView* srvs[5] = {};
		ID3D11SamplerState* samplers[5] = {};
		context->PSGetShaderResources(4, 3, srvs);
		context->PSGetSamplers(4, 3, samplers);
		context->PSGetShaderResources(8, 2, srvs + 3);
		context->PSGetSamplers(8, 2, samplers + 3);

		context->DSSetShaderResources(4, 3, srvs);
		context->DSSetSamplers(4, 3, samplers);
		context->DSSetShaderResources(8, 2, srvs + 3);
		context->DSSetSamplers(8, 2, samplers + 3);

		for (int i = 0; i < 5; i++) {
			if (srvs[i]) srvs[i]->Release();
			if (samplers[i]) samplers[i]->Release();
		}

		tessellationActiveForPass = true;
	} else if (geometryShaderOnlyForVisualizer) {
		context->GSSetShader(singleton.waterGeometryShader.get(), nullptr, 0);
		tessellationActiveForPass = true;
	} else if (!loggedTessSetup && singleton.settings.tessellation.EnableTessellation && techniqueSupportsTessel &&
	           !(singleton.waterHullShader && singleton.waterDomainShader && singleton.waterGeometryShader)) {
		logger::warn("Water pass: tessellation enabled but hull, domain, or geometry shader is unavailable; check earlier shader log lines.");
		loggedTessSetup = true;
	}
}

void PBRWater::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	// Restore tessellation state after the draw call
	if (tessellationActiveForPass) {
		auto context = globals::d3d::context;

		// Unbind hull, domain, and geometry shaders
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);

		// Restore original topology
		if (originalTopology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			context->IASetPrimitiveTopology(originalTopology);
		}

		tessellationActiveForPass = false;
	}

	func(waterShader, pass, renderFlags);
}
