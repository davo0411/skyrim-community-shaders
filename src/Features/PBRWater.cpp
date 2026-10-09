#include "PBRWater.h"

#include "Features/TerrainShadows.h"
#include "Features/Upscaling.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/UI.h"
#include "Utils/VersionedRelocation.h"
#include "WeatherVariableRegistry.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>

#define I18N_KEY_PREFIX "feature.pbr_water."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::Settings,
	EnableTessellation,
	TessellationTriangleSize,
	TessellationMaxFactor,
	DisplacementFadeStart,
	DisplacementFadeEnd,
	WindSpeedCalm,
	WindSpeedStorm,
	WaveHeight,
	Choppiness,
	DirectionalSpread,
	UseFetch,
	UseBathymetry,
	RiverWaveDamping,
	ShoreWaveHeight,
	ShoreSlope,
	ShoreSteepness,
	ShoreOnsetDepth,
	Roughness,
	SunSpecular,
	PointLightSpecular,
	Subsurface,
	VanillaFresnel,
	Visibility,
	RefractionDistortion,
	ScatteringAnisotropy,
	DownwellingAttenuation,
	Turbidity,
	TurbidityPatchiness,
	TurbidityPatchSize,
	SedimentDensity,
	SedimentColor,
	ShoreResuspension,
	RiverTurbidity,
	StormTurbidity,
	WadingSilt,
	SedimentLayerHeight,
	EnableUnderwater,
	UnderwaterVisibility,
	LightShafts,
	LightShaftDepth,
	SunGlow,
	Meniscus,
	UnderwaterSamples,
	FoamAmount,
	ShoreFoamWidth,
	FoamPersistence,
	CrestFoamThreshold,
	FoamScale,
	BreakingFoam,
	WakeFoam,
	FoamAlbedo,
	EnableRipples,
	PhysicsObjectRipples,
	RippleExtent,
	RippleHeight,
	RippleNormalStrength,
	RippleSpeed,
	RippleHalfLife,
	GameplayWaves,
	BuoyancyStrength,
	WireframeMode,
	DebugView)

namespace
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float CellSize = 4096.0f;
	constexpr float InteriorFetchMetres = 50.0f;
	constexpr float TwoPi = static_cast<float>(PBRWaterModel::TwoPi);

	using SC = SIE::ShaderCache;

	uint32_t WaterTechnique(uint32_t descriptor)
	{
		return (descriptor >> 11) & 0xF;
	}

	bool IsTessellatedTechnique(uint32_t technique)
	{
		return technique != static_cast<uint32_t>(SC::WaterShaderTechniques::Lod);
	}

	bool IsStencilTechnique(uint32_t technique)
	{
		return technique == static_cast<uint32_t>(SC::WaterShaderTechniques::Stencil);
	}

	float SmoothFactor(float dt, float timeConstant)
	{
		return timeConstant > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / timeConstant) : 1.0f;
	}

	bool IsDynamicMotion(const RE::hkpRigidBody* body)
	{
		using Type = RE::hkpMotion::MotionType;
		const auto type = body->motion.type.get();
		return type == Type::kDynamic || type == Type::kSphereInertia || type == Type::kBoxInertia || type == Type::kThinBoxInertia;
	}
}

// ============================================================================
// Settings
// ============================================================================

void PBRWater::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings();
}

void PBRWater::SanitizeSettings()
{
	auto& s = settings;
	auto clamp = [](float& value, float lo, float hi, float fallback) {
		value = std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
	};
	const Settings d{};
	clamp(s.TessellationTriangleSize, 4.0f, 40.0f, d.TessellationTriangleSize);
	clamp(s.TessellationMaxFactor, 1.0f, 64.0f, d.TessellationMaxFactor);
	clamp(s.DisplacementFadeStart, 2048.0f, 65536.0f, d.DisplacementFadeStart);
	clamp(s.DisplacementFadeEnd, 4096.0f, 131072.0f, d.DisplacementFadeEnd);
	s.DisplacementFadeEnd = std::max(s.DisplacementFadeEnd, s.DisplacementFadeStart + 1.0f);
	clamp(s.WindSpeedCalm, 0.0f, 10.0f, d.WindSpeedCalm);
	clamp(s.WindSpeedStorm, 1.0f, 40.0f, d.WindSpeedStorm);
	clamp(s.WaveHeight, 0.0f, 3.0f, d.WaveHeight);
	clamp(s.Choppiness, 0.0f, 0.95f, d.Choppiness);
	clamp(s.DirectionalSpread, 0.0f, 1.0f, d.DirectionalSpread);
	clamp(s.RiverWaveDamping, 0.0f, 1.0f, d.RiverWaveDamping);
	clamp(s.ShoreWaveHeight, 0.0f, 2.0f, d.ShoreWaveHeight);
	clamp(s.ShoreSlope, 0.01f, 0.2f, d.ShoreSlope);
	clamp(s.ShoreSteepness, 0.0f, 1.0f, d.ShoreSteepness);
	clamp(s.ShoreOnsetDepth, 1.0f, 20.0f, d.ShoreOnsetDepth);
	clamp(s.Roughness, 0.02f, 0.5f, d.Roughness);
	clamp(s.SunSpecular, 0.0f, 4.0f, d.SunSpecular);
	clamp(s.PointLightSpecular, 0.0f, 4.0f, d.PointLightSpecular);
	clamp(s.Subsurface, 0.0f, 4.0f, d.Subsurface);
	clamp(s.VanillaFresnel, 0.0f, 1.0f, d.VanillaFresnel);
	clamp(s.Visibility, 0.05f, 5.0f, d.Visibility);
	clamp(s.RefractionDistortion, 0.0f, 2.0f, d.RefractionDistortion);
	clamp(s.ScatteringAnisotropy, 0.0f, 0.9f, d.ScatteringAnisotropy);
	clamp(s.DownwellingAttenuation, 0.0f, 3.0f, d.DownwellingAttenuation);
	clamp(s.Turbidity, 0.0f, 5.0f, d.Turbidity);
	clamp(s.TurbidityPatchiness, 0.0f, 1.0f, d.TurbidityPatchiness);
	clamp(s.TurbidityPatchSize, 10.0f, 1000.0f, d.TurbidityPatchSize);
	clamp(s.SedimentDensity, 0.0f, 3.0f, d.SedimentDensity);
	clamp(s.SedimentColor.x, 0.0f, 1.0f, d.SedimentColor.x);
	clamp(s.SedimentColor.y, 0.0f, 1.0f, d.SedimentColor.y);
	clamp(s.SedimentColor.z, 0.0f, 1.0f, d.SedimentColor.z);
	clamp(s.ShoreResuspension, 0.0f, 3.0f, d.ShoreResuspension);
	clamp(s.RiverTurbidity, 0.0f, 3.0f, d.RiverTurbidity);
	clamp(s.StormTurbidity, 0.0f, 3.0f, d.StormTurbidity);
	clamp(s.WadingSilt, 0.0f, 3.0f, d.WadingSilt);
	clamp(s.SedimentLayerHeight, 0.2f, 10.0f, d.SedimentLayerHeight);
	clamp(s.UnderwaterVisibility, 0.1f, 10.0f, d.UnderwaterVisibility);
	clamp(s.LightShafts, 0.0f, 3.0f, d.LightShafts);
	clamp(s.LightShaftDepth, 1.0f, 60.0f, d.LightShaftDepth);
	clamp(s.SunGlow, 0.0f, 1.0f, d.SunGlow);
	clamp(s.Meniscus, 0.0f, 1.0f, d.Meniscus);
	s.UnderwaterSamples = std::clamp(s.UnderwaterSamples, 4, 32);
	clamp(s.FoamAmount, 0.0f, 3.0f, d.FoamAmount);
	clamp(s.ShoreFoamWidth, 0.0f, 5.0f, d.ShoreFoamWidth);
	clamp(s.FoamPersistence, 0.0f, 5.0f, d.FoamPersistence);
	clamp(s.CrestFoamThreshold, 0.0f, 1.0f, d.CrestFoamThreshold);
	clamp(s.FoamScale, 0.2f, 5.0f, d.FoamScale);
	clamp(s.BreakingFoam, 0.0f, 3.0f, d.BreakingFoam);
	clamp(s.WakeFoam, 0.0f, 3.0f, d.WakeFoam);
	clamp(s.FoamAlbedo, 0.1f, 1.0f, d.FoamAlbedo);
	clamp(s.RippleExtent, 16.0f, 160.0f, d.RippleExtent);
	clamp(s.RippleHeight, 0.0f, 0.5f, d.RippleHeight);
	clamp(s.RippleNormalStrength, 0.0f, 4.0f, d.RippleNormalStrength);
	clamp(s.RippleSpeed, 0.2f, 4.0f, d.RippleSpeed);
	clamp(s.RippleHalfLife, 0.2f, 6.0f, d.RippleHalfLife);
	clamp(s.BuoyancyStrength, 0.0f, 3.0f, d.BuoyancyStrength);
	s.WireframeMode = std::clamp(s.WireframeMode, 0, 2);
	s.DebugView = std::clamp(s.DebugView, 0, 8);
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::RegisterWeatherVariables()
{
	auto* registry = WeatherVariables::GlobalWeatherRegistry::GetSingleton()->GetOrCreateFeatureRegistry(GetShortName());
	const Settings defaults{};

	auto addFloat = [&](const char* name, const char* label, const char* tooltip, float* value, float def, float min, float max) {
		registry->RegisterVariable(std::make_shared<WeatherVariables::FloatVariable>(name, label, tooltip, value, def, min, max));
	};

	addFloat("WaveHeight", "Wave Height", "Scale of the wind-driven wave spectrum (sea state) for this weather", &settings.WaveHeight, defaults.WaveHeight, 0.0f, 3.0f);
	addFloat("Choppiness", "Choppiness", "How sharp and pinched the wave crests are", &settings.Choppiness, defaults.Choppiness, 0.0f, 0.95f);
	addFloat("DirectionalSpread", "Directional Spread", "0 = waves all follow the wind (swell), 1 = confused sea", &settings.DirectionalSpread, defaults.DirectionalSpread, 0.0f, 1.0f);
	addFloat("WindSpeedStorm", "Storm Wind Speed", "Wind speed (m/s) reached at the weather's maximum wind", &settings.WindSpeedStorm, defaults.WindSpeedStorm, 1.0f, 40.0f);
	addFloat("ShoreWaveHeight", "Shore Wave Height", "Height of the breaking shoreline waves relative to the open sea", &settings.ShoreWaveHeight, defaults.ShoreWaveHeight, 0.0f, 2.0f);
	addFloat("FoamAmount", "Foam Amount", "Overall foam coverage", &settings.FoamAmount, defaults.FoamAmount, 0.0f, 3.0f);
	addFloat("Visibility", "Water Visibility", "Scales how far light travels through the water before it is absorbed", &settings.Visibility, defaults.Visibility, 0.05f, 5.0f);
	addFloat("Subsurface", "Subsurface Scattering", "Light glowing through backlit wave crests", &settings.Subsurface, defaults.Subsurface, 0.0f, 4.0f);
	addFloat("Roughness", "Surface Roughness", "Micro-roughness of the water surface below the wave detail", &settings.Roughness, defaults.Roughness, 0.02f, 0.5f);
	addFloat("Turbidity", "Turbidity", "Suspended sediment in the water: higher is murkier", &settings.Turbidity, defaults.Turbidity, 0.0f, 5.0f);
	addFloat("StormTurbidity", "Storm Turbidity", "Extra sediment stirred up by strong wind and rain", &settings.StormTurbidity, defaults.StormTurbidity, 0.0f, 3.0f);
	addFloat("UnderwaterVisibility", "Underwater Visibility", "Scales how far you can see under water", &settings.UnderwaterVisibility, defaults.UnderwaterVisibility, 0.1f, 10.0f);
	addFloat("LightShafts", "Underwater Light Shafts", "Strength of the sun shafts under water", &settings.LightShafts, defaults.LightShafts, 0.0f, 3.0f);
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>("SedimentColor", "Sediment Colour", "Colour of murky water", &settings.SedimentColor, defaults.SedimentColor));
}

void PBRWater::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("waves"), "Waves"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("wave_height"), "Wave Height"), this, "WaveHeight", &settings.WaveHeight, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wave_height_tooltip"), "Scales the wind-driven wave spectrum. The waves themselves come from the weather's wind, the open-water distance upwind (fetch) and the water depth."));
		Util::WeatherUI::SliderFloat(T(TKEY("choppiness"), "Choppiness"), this, "Choppiness", &settings.Choppiness, 0.0f, 0.95f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("choppiness_tooltip"), "How sharp the crests are. Higher values pinch the crests and produce more crest foam."));
		Util::WeatherUI::SliderFloat(T(TKEY("directional_spread"), "Directional Spread"), this, "DirectionalSpread", &settings.DirectionalSpread, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("wind_speed_calm"), "Calm Wind Speed"), &settings.WindSpeedCalm, 0.0f, 10.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_calm_tooltip"), "Wind speed used when the weather reports no wind."));
		Util::WeatherUI::SliderFloat(T(TKEY("wind_speed_storm"), "Storm Wind Speed"), this, "WindSpeedStorm", &settings.WindSpeedStorm, 1.0f, 40.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_storm_tooltip"), "Wind speed reached at the weather's maximum wind. Wave height grows with the square of the wind speed."));
		ImGui::Checkbox(T(TKEY("use_fetch"), "Fetch-Limited Waves"), &settings.UseFetch);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_fetch_tooltip"), "Limits wave size by how much open water lies upwind, so ponds and rivers stay calm while the open sea builds swell."));
		ImGui::Checkbox(T(TKEY("use_bathymetry"), "Depth-Aware Waves"), &settings.UseBathymetry);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_bathymetry_tooltip"), "Uses the Terrain Shadows heightmap to calm waves in shallow water and to drive shoreline waves."));
		ImGui::SliderFloat(T(TKEY("river_wave_damping"), "River Wave Damping"), &settings.RiverWaveDamping, 0.0f, 1.0f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("shore"), "Shoreline"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("shore_wave_height"), "Shore Wave Height"), this, "ShoreWaveHeight", &settings.ShoreWaveHeight, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("shore_slope"), "Beach Slope"), &settings.ShoreSlope, 0.01f, 0.2f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("shore_slope_tooltip"), "Nominal beach slope used to time the shoreline waves. Steeper beaches give shorter, faster breaking waves."));
		ImGui::SliderFloat(T(TKEY("shore_steepness"), "Shore Wave Steepness"), &settings.ShoreSteepness, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("shore_onset_depth"), "Shore Wave Onset Depth"), &settings.ShoreOnsetDepth, 1.0f, 20.0f, "%.1f m");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("lighting"), "Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("roughness"), "Surface Roughness"), this, "Roughness", &settings.Roughness, 0.02f, 0.5f, "%.3f");
		ImGui::SliderFloat(T(TKEY("sun_specular"), "Sun Specular"), &settings.SunSpecular, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("point_light_specular"), "Light Specular"), &settings.PointLightSpecular, 0.0f, 4.0f, "%.2f");
		Util::WeatherUI::SliderFloat(T(TKEY("subsurface"), "Subsurface Scattering"), this, "Subsurface", &settings.Subsurface, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("subsurface_tooltip"), "Sunlight shining through thin, backlit wave crests. Its colour comes from the water: clear water glows green-blue, murky water dimly."));
		Util::WeatherUI::SliderFloat(T(TKEY("visibility"), "Water Visibility"), this, "Visibility", &settings.Visibility, 0.05f, 5.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("visibility_tooltip"), "Scales the water form's visibility distance. Absorption colour comes from the water form's shallow colour."));
		ImGui::SliderFloat(T(TKEY("refraction_distortion"), "Refraction Distortion"), &settings.RefractionDistortion, 0.0f, 2.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("scattering_anisotropy"), "Forward Scattering"), &settings.ScatteringAnisotropy, 0.0f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("scattering_anisotropy_tooltip"), "How strongly particles in the water scatter light forwards. Higher values make the water glow more when looking towards the sun."));
		ImGui::SliderFloat(T(TKEY("downwelling_attenuation"), "Depth Light Falloff"), &settings.DownwellingAttenuation, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("downwelling_attenuation_tooltip"), "How quickly sunlight fades on its way down, darkening deep bottoms and everything under water."));
		ImGui::SliderFloat(T(TKEY("vanilla_fresnel"), "Vanilla Fresnel Blend"), &settings.VanillaFresnel, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("vanilla_fresnel_tooltip"), "0 uses the physical air/water Fresnel; 1 uses the water form's own Fresnel amount."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("clarity"), "Water Clarity"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("turbidity"), "Turbidity"), this, "Turbidity", &settings.Turbidity, 0.0f, 5.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("turbidity_tooltip"), "Sediment suspended everywhere in the water. 0 keeps the water form's own clarity."));
		ImGui::SliderFloat(T(TKEY("turbidity_patchiness"), "Patchiness"), &settings.TurbidityPatchiness, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("turbidity_patchiness_tooltip"), "How unevenly the sediment is spread. Water clears and clouds in slowly drifting patches."));
		ImGui::SliderFloat(T(TKEY("turbidity_patch_size"), "Patch Size"), &settings.TurbidityPatchSize, 10.0f, 1000.0f, "%.0f m");
		ImGui::SliderFloat(T(TKEY("sediment_density"), "Sediment Density"), &settings.SedimentDensity, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("sediment_density_tooltip"), "How much each unit of turbidity clouds the water."));
		Util::WeatherUI::ColorEdit3(T(TKEY("sediment_color"), "Sediment Colour"), this, "SedimentColor", reinterpret_cast<float*>(&settings.SedimentColor));
		ImGui::SliderFloat(T(TKEY("shore_resuspension"), "Wave-Stirred Shallows"), &settings.ShoreResuspension, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("shore_resuspension_tooltip"), "Sediment lifted off the bottom by the waves' motion, strongest in the surf zone and in rough weather."));
		ImGui::SliderFloat(T(TKEY("river_turbidity"), "River Silt"), &settings.RiverTurbidity, 0.0f, 3.0f, "%.2f");
		Util::WeatherUI::SliderFloat(T(TKEY("storm_turbidity"), "Storm Turbidity"), this, "StormTurbidity", &settings.StormTurbidity, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("storm_turbidity_tooltip"), "Extra sediment in strong wind and rain. Builds up and clears over a few minutes."));
		ImGui::SliderFloat(T(TKEY("wading_silt"), "Wading Silt"), &settings.WadingSilt, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wading_silt_tooltip"), "Silt clouds kicked up by anyone walking through shallow water. Needs Interactive Ripples."));
		ImGui::SliderFloat(T(TKEY("sediment_layer_height"), "Sediment Layer Height"), &settings.SedimentLayerHeight, 0.2f, 10.0f, "%.1f m");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("underwater"), "Underwater"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_underwater"), "Volumetric Underwater"), &settings.EnableUnderwater);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_underwater_tooltip"), "Replaces the vanilla underwater fog with light travelling through the water: depth darkening, sun glow, light shafts, murky patches and the waterline across the lens."));
		if (settings.EnableUnderwater) {
			Util::WeatherUI::SliderFloat(T(TKEY("underwater_visibility"), "Underwater Visibility"), this, "UnderwaterVisibility", &settings.UnderwaterVisibility, 0.1f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("underwater_visibility_tooltip"), "Scales the water form's underwater fog distance."));
			Util::WeatherUI::SliderFloat(T(TKEY("light_shafts"), "Light Shafts"), this, "LightShafts", &settings.LightShafts, 0.0f, 3.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("light_shaft_depth"), "Light Shaft Depth"), &settings.LightShaftDepth, 1.0f, 60.0f, "%.0f m");
			ImGui::SliderFloat(T(TKEY("sun_glow"), "Sun Glow"), &settings.SunGlow, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("sun_glow_tooltip"), "Brightening of the water towards the sun from forward scattering."));
			ImGui::SliderFloat(T(TKEY("meniscus"), "Waterline Meniscus"), &settings.Meniscus, 0.0f, 1.0f, "%.2f");
			ImGui::SliderInt(T(TKEY("underwater_samples"), "Underwater Quality"), &settings.UnderwaterSamples, 4, 32);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("underwater_samples_tooltip"), "Samples along each view ray under water. Higher values resolve light shafts and silt clouds better at a higher GPU cost."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("foam"), "Foam"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("foam_amount"), "Foam Amount"), this, "FoamAmount", &settings.FoamAmount, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("shore_foam_width"), "Shore Foam Width"), &settings.ShoreFoamWidth, 0.0f, 5.0f, "%.2f m");
		ImGui::SliderFloat(T(TKEY("crest_foam_threshold"), "Crest Foam Threshold"), &settings.CrestFoamThreshold, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("crest_foam_threshold_tooltip"), "Surface compression at which crests start to foam. Higher values give more crest foam."));
		ImGui::SliderFloat(T(TKEY("foam_persistence"), "Crest Foam Trail"), &settings.FoamPersistence, 0.0f, 5.0f, "%.1f s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("foam_persistence_tooltip"), "How long foam from a breaking crest lingers and thins out behind the wave."));
		ImGui::SliderFloat(T(TKEY("breaking_foam"), "Breaking Wave Foam"), &settings.BreakingFoam, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("wake_foam"), "Wake Foam"), &settings.WakeFoam, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("foam_scale"), "Foam Pattern Size"), &settings.FoamScale, 0.2f, 5.0f, "%.2f m");
		ImGui::SliderFloat(T(TKEY("foam_albedo"), "Foam Brightness"), &settings.FoamAlbedo, 0.1f, 1.0f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("interaction"), "Interaction"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_ripples"), "Interactive Ripples"), &settings.EnableRipples);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_ripples_tooltip"), "Simulates ripples and wakes from every actor and object in the water. Replaces the vanilla wading mesh."));
		if (settings.EnableRipples) {
			ImGui::Checkbox(T(TKEY("physics_object_ripples"), "Physics Object Ripples"), &settings.PhysicsObjectRipples);
			ImGui::SliderFloat(T(TKEY("ripple_extent"), "Ripple Area"), &settings.RippleExtent, 16.0f, 160.0f, "%.0f m");
			ImGui::SliderFloat(T(TKEY("ripple_height"), "Ripple Height"), &settings.RippleHeight, 0.0f, 0.5f, "%.2f m");
			ImGui::SliderFloat(T(TKEY("ripple_normal_strength"), "Ripple Normal Strength"), &settings.RippleNormalStrength, 0.0f, 4.0f, "%.2f");
			ImGui::SliderFloat(T(TKEY("ripple_speed"), "Ripple Speed"), &settings.RippleSpeed, 0.2f, 4.0f, "%.2f m/s");
			ImGui::SliderFloat(T(TKEY("ripple_half_life"), "Ripple Lifetime"), &settings.RippleHalfLife, 0.2f, 6.0f, "%.1f s");
		}
		ImGui::Checkbox(T(TKEY("gameplay_waves"), "Swimming Follows Waves"), &settings.GameplayWaves);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("gameplay_waves_tooltip"), "The game's water height includes the rendered waves, so swimming actors ride them."));
		ImGui::SliderFloat(T(TKEY("buoyancy_strength"), "Floating Object Response"), &settings.BuoyancyStrength, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("buoyancy_strength_tooltip"), "How strongly floating physics objects follow the waves. 0 leaves them on the flat vanilla plane."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("geometry"), "Geometry"), ImGuiTreeNodeFlags_None)) {
		ImGui::Checkbox(T(TKEY("enable_tessellation"), "Tessellation"), &settings.EnableTessellation);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_tessellation_tooltip"), "Subdivides the water mesh on the GPU so waves are real geometry. Without it, waves are limited by the mesh resolution."));
		ImGui::SliderFloat(T(TKEY("tessellation_triangle_size"), "Target Triangle Size"), &settings.TessellationTriangleSize, 4.0f, 40.0f, "%.0f px");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("tessellation_triangle_size_tooltip"), "Smaller values give smoother waves at a higher GPU cost. Scales automatically with the render resolution."));
		ImGui::SliderFloat(T(TKEY("tessellation_max_factor"), "Max Subdivision"), &settings.TessellationMaxFactor, 1.0f, 64.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("displacement_fade_start"), "Wave Geometry Fade Start"), &settings.DisplacementFadeStart, 2048.0f, 65536.0f, "%.0f");
		ImGui::SliderFloat(T(TKEY("displacement_fade_end"), "Wave Geometry Fade End"), &settings.DisplacementFadeEnd, 4096.0f, 131072.0f, "%.0f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"), ImGuiTreeNodeFlags_None)) {
		const char* wireModes[] = { T(TKEY("wireframe_off"), "Off"), T(TKEY("wireframe_overlay"), "Overlay"), T(TKEY("wireframe_only"), "Wireframe Only") };
		ImGui::Combo(T(TKEY("wireframe"), "Wireframe"), &settings.WireframeMode, wireModes, IM_ARRAYSIZE(wireModes));
		const char* views[] = {
			T(TKEY("debug_off"), "Off"),
			T(TKEY("debug_normal"), "Normal"),
			T(TKEY("debug_foam"), "Foam"),
			T(TKEY("debug_depth"), "Depth / Shore"),
			T(TKEY("debug_jacobian"), "Crest Compression"),
			T(TKEY("debug_roughness"), "Roughness"),
			T(TKEY("debug_height"), "Wave Height"),
			T(TKEY("debug_fetch"), "Fetch"),
			T(TKEY("debug_clarity"), "Water Clarity"),
		};
		ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &settings.DebugView, views, IM_ARRAYSIZE(views));

		if (const auto snap = snapshot.load(std::memory_order_acquire)) {
			const auto& s = snap->spectrum;
			ImGui::Text("Wind %.1f m/s, peak period %.1f s, Hs %.2f m", s.windSpeed, TwoPi / s.peakOmega, s.significantHeight / UnitsPerMetre);
			ImGui::Text("Whitecaps %.1f%%, shortest displaced wave %.1f m", s.whitecapCoverage * 100.0f, s.shortestDisplacedWavelength / UnitsPerMetre);
		}
		ImGui::Text("Fetch: %s, bathymetry: %s%s", environment.GetFetch() ? "ready" : "none", environment.GetBathymetry() ? "ready" : "none", environment.IsBuilding() ? " (building)" : "");
		ImGui::TreePop();
	}
}

// ============================================================================
// Resources
// ============================================================================

void PBRWater::SetupResources()
{
	gpuBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::Constants");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
	Util::SetResourceName(linearClampSampler.get(), "PBRWater::LinearClampSampler");

	ripples.SetupResources();
}

void PBRWater::ClearShaderCache()
{
	ripples.ClearShaderCache();
	tessellationFailureLogged = false;
}

void PBRWater::Reset()
{
	frameDataFrame = UINT32_MAX;
}

void PBRWater::UpdateFetchTexture()
{
	const uint32_t generation = environment.GetFetchGeneration();
	if (generation == uploadedFetchGeneration)
		return;
	uploadedFetchGeneration = generation;

	auto field = environment.GetFetch();
	uploadedFetch = field;
	fetchTexture.reset();
	if (!field || !field->Valid())
		return;

	using PBRWaterModel::FetchField;
	std::vector<uint16_t> halfData(field->kilometres.size());
	for (size_t i = 0; i < halfData.size(); ++i)
		halfData[i] = DirectX::PackedVector::XMConvertFloatToHalf(field->kilometres[i]);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = field->width;
	desc.Height = field->height;
	desc.MipLevels = 1;
	desc.ArraySize = FetchField::Directions;
	desc.Format = DXGI_FORMAT_R16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	std::array<D3D11_SUBRESOURCE_DATA, FetchField::Directions> init{};
	for (uint32_t d = 0; d < FetchField::Directions; ++d) {
		init[d].pSysMem = &halfData[static_cast<size_t>(d) * field->width * field->height];
		init[d].SysMemPitch = static_cast<UINT>(field->width * sizeof(uint16_t));
	}

	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, init.data(), texture.put()))) {
		logger::warn("[PBR Water] Failed to create the fetch texture");
		return;
	}
	fetchTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::Fetch");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.ArraySize = FetchField::Directions;
	fetchTexture->CreateSRV(srvDesc);
}

// ============================================================================
// Per-frame (render thread)
// ============================================================================

void PBRWater::Prepass()
{
	UpdateFetchTexture();

	if (settings.EnableRipples) {
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		RippleSimulation::Settings rippleSettings;
		rippleSettings.extent = settings.RippleExtent * UnitsPerMetre;
		rippleSettings.waveSpeed = settings.RippleSpeed;
		rippleSettings.halfLife = settings.RippleHalfLife;
		rippleSettings.foamHalfLife = settings.RippleHalfLife * 0.5f;
		rippleSettings.siltHalfLife = 20.0f;
		ripples.Update(cameraPos.x, cameraPos.y, renderDelta.load(std::memory_order_acquire), rippleSettings);
	}

	UpdateFrameConstants();
}

void PBRWater::UpdateFrameConstants()
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == frameDataFrame)
		return;
	frameDataFrame = frame;

	GpuData d{};
	const auto snap = snapshot.load(std::memory_order_acquire);

	if (snap) {
		const auto& s = snap->spectrum;
		for (uint32_t i = 0; i < s.count; ++i) {
			const auto& w = s.waves[i];
			d.WaveDirK[i] = { w.dirX, w.dirY, w.k, w.omega };
			d.WaveAmp[i] = { w.amplitude, w.steepness, static_cast<float>(snap->phase[i]), static_cast<float>(snap->phasePrev[i]) };
			d.WaveExtra[i] = { w.pmWeightOpenSea, w.wavelength, 0.0f, 0.0f };
		}
		d.Params0 = { static_cast<float>(s.count), s.windSpeed, s.peakOmega, static_cast<float>(PBRWaterModel::Gravity) * UnitsPerMetre };
		d.Params1 = { std::cos(snap->windDirection), std::sin(snap->windDirection), settings.DisplacementFadeStart, settings.DisplacementFadeEnd };
		d.Params2 = { settings.Choppiness, s.whitecapCoverage, s.amplitudeSum + snap->shore.amplitude, s.shortestDisplacedWavelength };
		d.RefCamPos = { static_cast<float>(snap->refX), static_cast<float>(snap->refY), static_cast<float>(snap->refZ), 0.0f };
		d.Shore0 = { snap->shore.amplitude, snap->shore.omega, snap->shore.slope, snap->shore.onsetDepth };
		d.Shore1 = { snap->shore.steepness, settings.BreakingFoam, static_cast<float>(snap->shorePhase), static_cast<float>(snap->shorePhasePrev) };

		// Fetch
		if (uploadedFetch && fetchTexture && snap->exterior && settings.UseFetch) {
			const float originX = uploadedFetch->minCellX * CellSize;
			const float originY = uploadedFetch->minCellY * CellSize;
			d.Fetch0 = { originX, originY, 1.0f / (uploadedFetch->width * CellSize), 1.0f / (uploadedFetch->height * CellSize) };
			float slice = snap->windDirection / TwoPi;
			slice = (slice - std::floor(slice)) * PBRWaterModel::FetchField::Directions;
			d.Fetch1 = { 1.0f, static_cast<float>(PBRWaterModel::FetchField::Directions), slice, uploadedFetch->openSeaKm * 1000.0f };
		} else {
			d.Fetch1 = { 0.0f, 0.0f, 0.0f, snap->exterior ? 500000.0f : InteriorFetchMetres };
		}
	}

	// Bathymetry from the Terrain Shadows heightmap (same mapping it uses for shadows).
	auto& terrainShadows = globals::features::terrainShadows;
	if (settings.UseBathymetry && terrainShadows.loaded && terrainShadows.IsHeightMapReady() && terrainShadows.texHeightMap) {
		const auto terrain = terrainShadows.GetCommonBufferData();
		d.Terrain0 = { terrain.Scale.x, terrain.Scale.y, terrain.Offset.x, terrain.Offset.y };
		const float texel = std::abs(1.0f / terrain.Scale.x) / static_cast<float>(terrainShadows.texHeightMap->desc.Width);
		// The heightmap texels decode with the heightmap's own z range (pos0.z..pos1.z, Terrain Shadows'
		// PosRange); ZRange only normalises its shadow-height texture.
		const auto* heightmap = terrainShadows.cachedHeightmap;
		d.Terrain1 = { heightmap->pos0.z, heightmap->pos1.z, 1.0f, std::max(texel, 1.0f) };
	}

	// Tessellation: on-screen size uses the internal render resolution, so it scales with upscalers.
	float renderHeight = static_cast<float>(globals::game::graphicsState->screenHeight);
	if (globals::features::upscaling.loaded)
		renderHeight *= globals::features::upscaling.resolutionScale.y;
	const float projY = std::abs(globals::game::frameBufferCached.GetCameraProj()._22);
	d.Tess0 = { 0.0f, settings.TessellationTriangleSize, settings.TessellationMaxFactor, 0.5f * renderHeight * projY };
	d.Draw0 = { 0.0f, static_cast<float>(settings.WireframeMode), static_cast<float>(settings.DebugView), 0.0f };

	if (settings.EnableRipples && ripples.IsReady()) {
		d.Ripple0 = { ripples.GetOriginX(), ripples.GetOriginY(), 1.0f / ripples.GetExtent(), 1.0f };
		d.Ripple1 = { settings.RippleHeight * UnitsPerMetre, settings.RippleNormalStrength, 0.0f, 1.0f / RippleSimulation::GridSize };
		d.Ripple2 = { ripples.GetPreviousOriginX(), ripples.GetPreviousOriginY(), ripples.GetStepAlpha(), ripples.GetPreviousStepAlpha() };
	}

	d.Light0 = { settings.Roughness, settings.Subsurface, settings.VanillaFresnel, settings.SunSpecular };
	d.Light1 = { settings.Visibility, settings.FoamAlbedo, settings.RefractionDistortion, settings.PointLightSpecular };
	d.Foam0 = { settings.ShoreFoamWidth * UnitsPerMetre, settings.CrestFoamThreshold, settings.FoamAmount, settings.FoamScale * UnitsPerMetre };
	// The foam animation only drifts noise slowly; wrapping once an hour keeps float precision.
	d.Foam1 = { settings.FoamPersistence, std::fmod(globals::state->timer, 3600.0f), settings.WakeFoam, settings.RiverWaveDamping };

	// Water clarity and the light in the water.
	d.Clarity0 = { settings.SedimentDensity / UnitsPerMetre, settings.Turbidity, settings.TurbidityPatchiness, settings.TurbidityPatchSize * UnitsPerMetre };
	d.Clarity1 = { settings.SedimentColor.x, settings.SedimentColor.y, settings.SedimentColor.z, settings.ShoreResuspension };
	d.Clarity2 = { settings.RiverTurbidity, weatherTurbidity.load(std::memory_order_acquire), settings.EnableRipples ? settings.WadingSilt : 0.0f, settings.SedimentLayerHeight * UnitsPerMetre };
	// Sediment plumes drift and reshape over hours; a daily wrap keeps their coordinates precise.
	d.Optics0 = { settings.ScatteringAnisotropy, settings.DownwellingAttenuation, std::fmod(globals::state->timer, 86400.0f), snap ? snap->spectrum.significantHeight : 0.0f };

	// Underwater view.
	const auto camera = cameraWater.load(std::memory_order_acquire);
	if (settings.EnableUnderwater && camera && camera->underwater) {
		d.Underwater0 = { 1.0f, camera->flatZ, camera->nearSurface ? camera->band : 0.0f, camera->visibility * settings.UnderwaterVisibility * settings.Visibility };
		d.Underwater1 = { camera->shallow.x, camera->shallow.y, camera->shallow.z, settings.LightShafts };
		d.Underwater2 = { camera->deep.x, camera->deep.y, camera->deep.z, settings.LightShaftDepth * UnitsPerMetre };
	}
	d.Underwater3 = { settings.Meniscus, settings.SunGlow, 0.0f, static_cast<float>(settings.UnderwaterSamples) };

	frameData = d;
}

// ============================================================================
// Per-draw (render thread)
// ============================================================================

void PBRWater::SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	if (!gpuBuffer || !waterShader || !pass || !pass->geometry)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;

	UpdateFrameConstants();

	const uint32_t vertexDescriptor = state->modifiedVertexDescriptor;
	const uint32_t pixelDescriptor = state->modifiedPixelDescriptor;
	const uint32_t technique = WaterTechnique(vertexDescriptor);

	// Our hull/domain shaders only match our own vertex and pixel shaders of the same descriptor. While
	// those are still compiling the vanilla shaders are bound, and tessellating them would break the
	// stage signatures, so only tessellate once both are live.
	const bool customShadersLive = shaderCache->IsEnabled() &&
	                               shaderCache->GetVertexShader(*waterShader, vertexDescriptor) &&
	                               shaderCache->GetPixelShader(*waterShader, pixelDescriptor);

	const auto snap = snapshot.load(std::memory_order_acquire);
	const bool hasWaves = snap && (snap->spectrum.amplitudeSum > 1.0f || snap->shore.amplitude > 1.0f || settings.EnableRipples);

	ID3D11HullShader* hullShader = nullptr;
	ID3D11DomainShader* domainShader = nullptr;
	if (settings.EnableTessellation && customShadersLive && hasWaves && IsTessellatedTechnique(technique)) {
		hullShader = shaderCache->GetHullShader(*waterShader, vertexDescriptor);
		domainShader = shaderCache->GetDomainShader(*waterShader, vertexDescriptor);
		if ((!hullShader || !domainShader) && !shaderCache->IsCompiling() && !tessellationFailureLogged) {
			tessellationFailureLogged = true;
			logger::warn("[PBR Water] Hull/domain shader unavailable for water descriptor {:X}; drawing without tessellation", vertexDescriptor);
		}
	}
	const bool tessellate = hullShader && domainShader;

	ID3D11GeometryShader* geometryShader = nullptr;
	if (settings.WireframeMode > 0 && customShadersLive && !IsStencilTechnique(technique))
		geometryShader = shaderCache->GetGeometryShader(*waterShader, vertexDescriptor);

	// Per-draw constants
	GpuData& d = frameData;
	d.Tess0.x = tessellate ? 1.0f : 0.0f;
	// The surface seen from below only adds the water in front of Snell's window if the composite has not.
	d.Underwater3.z = underwaterCompositeFrame == globals::state->frameCount ? 1.0f : 0.0f;
	{
		// Vertex spacing of this mesh, for filtering waves the untessellated grid cannot carry.
		float spacing = 0.0f;
		if (const auto triShape = pass->geometry->AsTriShape()) {
			const float triangles = static_cast<float>(triShape->GetTrishapeRuntimeData().triangleCount);
			if (triangles > 0.0f)
				spacing = pass->geometry->worldBound.radius * 1.41421356f / std::max(std::sqrt(triangles * 0.5f), 1.0f);
		}
		d.Draw0.x = spacing;
	}
	gpuBuffer->Update(d);

	// Constants and resources for every stage that evaluates the wave model.
	ID3D11Buffer* cb = gpuBuffer->CB();
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);

	ID3D11ShaderResourceView* srvs[4] = {
		settings.EnableRipples ? ripples.GetSRV() : nullptr,
		fetchTexture ? fetchTexture->srv.get() : nullptr,
		(d.Terrain1.z > 0.5f && globals::features::terrainShadows.texHeightMap) ? globals::features::terrainShadows.texHeightMap->srv.get() : nullptr,
		settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr
	};
	ID3D11SamplerState* sampler = linearClampSampler.get();
	context->VSSetShaderResources(110, 4, srvs);
	context->PSSetShaderResources(110, 4, srvs);
	context->VSSetSamplers(12, 1, &sampler);
	context->PSSetSamplers(12, 1, &sampler);

	// The vertex program reads the flowmap to calm waves on rivers.
	ID3D11ShaderResourceView* flowmap = nullptr;
	ID3D11SamplerState* flowmapSampler = nullptr;
	context->PSGetShaderResources(8, 1, &flowmap);
	context->PSGetSamplers(8, 1, &flowmapSampler);
	context->VSSetShaderResources(8, 1, &flowmap);
	context->VSSetSamplers(8, 1, &flowmapSampler);

	if (tessellate) {
		// The domain shader runs the full vertex program: give it everything the vertex shader has.
		ID3D11Buffer* vsBuffers[3] = {};
		context->VSGetConstantBuffers(0, 3, vsBuffers);
		ID3D11Buffer* frameBuffer = nullptr;
		context->VSGetConstantBuffers(12, 1, &frameBuffer);
		if (!frameBuffer)
			context->PSGetConstantBuffers(12, 1, &frameBuffer);

		context->HSSetConstantBuffers(0, 3, vsBuffers);
		context->HSSetConstantBuffers(7, 1, &cb);
		context->HSSetConstantBuffers(12, 1, &frameBuffer);
		context->DSSetConstantBuffers(0, 3, vsBuffers);
		context->DSSetConstantBuffers(7, 1, &cb);
		context->DSSetConstantBuffers(12, 1, &frameBuffer);
		context->DSSetShaderResources(110, 4, srvs);
		context->DSSetSamplers(12, 1, &sampler);
		context->DSSetShaderResources(8, 1, &flowmap);
		context->DSSetSamplers(8, 1, &flowmapSampler);

		for (auto* buffer : vsBuffers) {
			if (buffer)
				buffer->Release();
		}
		if (frameBuffer)
			frameBuffer->Release();

		// The renderer only re-applies its cached topology when it changes, so make sure its cache
		// says "triangle list" with nothing pending, then switch the device to patches behind it.
		// RestoreDraw() puts the device back in sync before anything else is drawn.
		auto& shadow = globals::game::shadowState->GetRuntimeData();
		shadow.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadow.stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		context->HSSetShader(hullShader, nullptr, 0);
		context->DSSetShader(domainShader, nullptr, 0);
		tessellationBound = true;
	}

	if (geometryShader) {
		context->GSSetConstantBuffers(7, 1, &cb);
		context->GSSetShader(geometryShader, nullptr, 0);
		geometryShaderBound = true;
	}

	if (flowmap)
		flowmap->Release();
	if (flowmapSampler)
		flowmapSampler->Release();
}

void PBRWater::BindUnderwaterComposite()
{
	if (!gpuBuffer)
		return;

	UpdateFrameConstants();
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);
	if (d.Underwater0.x > 0.5f)
		underwaterCompositeFrame = globals::state->frameCount;

	auto context = globals::d3d::context;
	ID3D11Buffer* cb = gpuBuffer->CB();
	context->PSSetConstantBuffers(7, 1, &cb);

	ID3D11ShaderResourceView* srvs[4] = {
		settings.EnableRipples ? ripples.GetSRV() : nullptr,
		fetchTexture ? fetchTexture->srv.get() : nullptr,
		(d.Terrain1.z > 0.5f && globals::features::terrainShadows.texHeightMap) ? globals::features::terrainShadows.texHeightMap->srv.get() : nullptr,
		settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr
	};
	context->PSSetShaderResources(110, 4, srvs);
	ID3D11SamplerState* sampler = linearClampSampler.get();
	context->PSSetSamplers(12, 1, &sampler);
}

void PBRWater::RestoreDraw()
{
	auto context = globals::d3d::context;
	if (tessellationBound) {
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		tessellationBound = false;
	}
	if (geometryShaderBound) {
		context->GSSetShader(nullptr, nullptr, 0);
		geometryShaderBound = false;
	}
}

// ============================================================================
// Main thread
// ============================================================================

PBRWaterModel::SpectrumParams PBRWater::CurrentSpectrumParams(float windSpeed) const
{
	PBRWaterModel::SpectrumParams params;
	params.windSpeed = windSpeed;
	params.windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	params.heightScale = settings.WaveHeight;
	params.choppiness = settings.Choppiness;
	params.spread = settings.DirectionalSpread;
	return params;
}

void PBRWater::MainThreadUpdate()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto tes = globals::game::tes;
	if (!player || !tes)
		return;

	// Weather blends and typed-in values write straight into the settings; keep them in range.
	SanitizeSettings();

	const auto ui = RE::UI::GetSingleton();
	const float dt = (ui && ui->GameIsPaused()) ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);

	const auto cell = player->GetParentCell();
	const bool exterior = cell && cell->IsExteriorCell();

	// Wind from the weather system (already blended across weather transitions); still air indoors.
	float windNormalised = 0.0f;
	float windAngle = std::atan2(smoothedWindDirY, smoothedWindDirX);
	if (exterior) {
		if (const auto sky = globals::game::sky) {
			windNormalised = std::clamp(sky->windSpeed, 0.0f, 1.0f);
			windAngle = sky->windAngle;
		}
	}
	const float targetWind = exterior ? settings.WindSpeedCalm + (settings.WindSpeedStorm - settings.WindSpeedCalm) * windNormalised : 0.5f;

	// A sea takes time to respond to the wind; this also hides abrupt weather data changes.
	const float response = SmoothFactor(dt, 8.0f);
	if (smoothedWindSpeed < 0.0f) {
		smoothedWindSpeed = targetWind;
		smoothedWindDirX = std::cos(windAngle);
		smoothedWindDirY = std::sin(windAngle);
	} else {
		smoothedWindSpeed += (targetWind - smoothedWindSpeed) * response;
		smoothedWindDirX += (std::cos(windAngle) - smoothedWindDirX) * response;
		smoothedWindDirY += (std::sin(windAngle) - smoothedWindDirY) * response;
		const float len = std::max(std::sqrt(smoothedWindDirX * smoothedWindDirX + smoothedWindDirY * smoothedWindDirY), 1e-4f);
		smoothedWindDirX /= len;
		smoothedWindDirY /= len;
	}

	auto next = std::make_shared<PBRWaterModel::WaveSnapshot>();
	next->spectrum = PBRWaterModel::GenerateSpectrum(CurrentSpectrumParams(smoothedWindSpeed));
	next->windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	next->exterior = exterior;

	// Shoreline waves: the dominant period of the sea, arriving from deep water.
	auto& shore = next->shore;
	shore.omega = std::clamp(next->spectrum.peakOmega, static_cast<float>(PBRWaterModel::TwoPi / 14.0), static_cast<float>(PBRWaterModel::TwoPi / 4.0));
	shore.amplitude = exterior ? settings.ShoreWaveHeight * next->spectrum.significantHeight * 0.5f : 0.0f;
	shore.slope = settings.ShoreSlope;
	shore.steepness = settings.ShoreSteepness;
	const float deepWavelengthMetres = static_cast<float>(PBRWaterModel::Gravity * PBRWaterModel::TwoPi / (shore.omega * shore.omega));
	shore.onsetDepth = std::min(deepWavelengthMetres * 0.5f, settings.ShoreOnsetDepth) * UnitsPerMetre;

	const auto eye = Util::GetEyePosition();
	phases.Advance(next->spectrum, shore, dt, eye.x, eye.y, eye.z);
	phases.Fill(*next);

	environment.Update(exterior ? tes->GetRuntimeData2().worldSpace : nullptr);
	if (settings.UseFetch)
		next->fetch = environment.GetFetch();
	if (settings.UseBathymetry)
		next->bathymetry = environment.GetBathymetry();

	std::shared_ptr<const PBRWaterModel::WaveSnapshot> published = next;
	snapshot.store(published, std::memory_order_release);
	renderDelta.store(dt, std::memory_order_release);

	// Storms and rain cloud the water over minutes, and it takes as long to clear.
	smoothedWeatherTurbidity += (WeatherTurbidityTarget() - smoothedWeatherTurbidity) * SmoothFactor(dt, 90.0f);
	weatherTurbidity.store(exterior ? smoothedWeatherTurbidity : 0.0f, std::memory_order_release);

	cameraWater.store(settings.EnableUnderwater ? FindCameraWater(*published, exterior) : nullptr, std::memory_order_release);

	GatherInteractions(*published, dt);
}

float PBRWater::WeatherTurbidityTarget() const
{
	const auto sky = globals::game::sky;
	if (!sky)
		return 0.0f;
	// Strong wind lifts sediment in the shallows and mixes it through; rain washes silt in from the land.
	const float wind = std::clamp((smoothedWindSpeed - 6.0f) / 14.0f, 0.0f, 1.0f);
	auto rainy = [](const RE::TESWeather* weather) {
		return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) ? 1.0f : 0.0f;
	};
	const float blend = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	const float rain = rainy(sky->currentWeather) * blend + rainy(sky->lastWeather) * (1.0f - blend);
	return settings.StormTurbidity * (wind + 0.5f * rain);
}

std::shared_ptr<const PBRWater::CameraWater> PBRWater::FindCameraWater(const PBRWaterModel::WaveSnapshot& snap, bool exterior) const
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto tes = globals::game::tes;
	if (!player || !tes)
		return nullptr;

	const auto eye = Util::GetEyePosition();
	RE::TESObjectCELL* cell = exterior ? tes->GetCell(eye) : nullptr;
	if (!cell)
		cell = player->GetParentCell();
	float flatZ = 0.0f;
	if (!cell || !TESObjectCELL_GetWaterHeight::func(cell, eye, flatZ) || flatZ <= -1e6f)
		return nullptr;

	auto water = std::make_shared<CameraWater>();
	water->flatZ = flatZ;
	// How far waves (shoaling included) and ripples can lift or drop the surface, plus a margin for the lens.
	water->band = 2.0f * (snap.spectrum.amplitudeSum + snap.shore.amplitude) + 4.0f * settings.RippleHeight * UnitsPerMetre + 16.0f;
	water->underwater = eye.z < flatZ + water->band;
	water->nearSurface = eye.z > flatZ - water->band;
	if (!water->underwater)
		return water;

	RE::TESWaterForm* form = nullptr;
	if (const auto extra = cell->extraList.GetByType<RE::ExtraCellWaterType>())
		form = extra->water;
	if (!form && exterior) {
		if (const auto worldSpace = tes->GetRuntimeData2().worldSpace)
			form = worldSpace->worldWater;
	}
	if (!form) {
		if (const auto waterSystem = RE::TESWaterSystem::GetSingleton())
			form = waterSystem->currentWaterType;
	}
	if (!form) {
		water->underwater = false;
		return water;
	}

	// The weather scales the water colours through the day (as the water shader's material colours).
	float3 multiplier{ 1.0f, 1.0f, 1.0f };
	if (const auto sky = globals::game::sky) {
		const auto& color = sky->skyColor[RE::TESWeather::ColorTypes::kWaterMultiplier];
		multiplier = { color.red, color.green, color.blue };
	}
	const auto& data = form->data;
	water->shallow = { data.shallowWaterColor.red / 255.0f * multiplier.x, data.shallowWaterColor.green / 255.0f * multiplier.y, data.shallowWaterColor.blue / 255.0f * multiplier.z };
	water->deep = { data.deepWaterColor.red / 255.0f * multiplier.x, data.deepWaterColor.green / 255.0f * multiplier.y, data.deepWaterColor.blue / 255.0f * multiplier.z };
	float visibility = data.underwaterFogDistFar > 1.0f ? data.underwaterFogDistFar : data.aboveWaterFogDistFar;
	water->visibility = std::clamp(std::isfinite(visibility) ? visibility : 2048.0f, 128.0f, 200000.0f);
	return water;
}

bool PBRWater::GetWaveHeight(const RE::NiPoint3& position, float flatWaterZ, float& height) const
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap)
		return false;
	height = snap->SampleAt(position.x, position.y, flatWaterZ).height;
	return true;
}

void PBRWater::GatherInteractions(const PBRWaterModel::WaveSnapshot& snap, float dt)
{
	const bool wantRipples = settings.EnableRipples;
	const bool wantBuoyancy = settings.BuoyancyStrength > 0.0f;
	if (!wantRipples && !wantBuoyancy)
		return;

	auto player = RE::PlayerCharacter::GetSingleton();
	const auto eye = Util::GetEyePosition();
	const float range = settings.RippleExtent * UnitsPerMetre * 0.5f;
	const float rangeSq = range * range;

	std::vector<RippleSimulation::Source> sources;
	sources.reserve(RippleSimulation::MaxSources);

	// Surface z (flat plane + waves) at a position, or false when there is no water.
	auto surfaceAt = [&](RE::TESObjectREFR* ref, const RE::NiPoint3& pos, float& flatZ, float& surfaceZ) {
		auto parentCell = ref->GetParentCell();
		if (!parentCell)
			return false;
		float h = 0.0f;
		if (!TESObjectCELL_GetWaterHeight::func(parentCell, pos, h) || h <= -1e6f)
			return false;
		flatZ = h;
		surfaceZ = h + snap.SampleAt(pos.x, pos.y, h).height;
		return true;
	};

	auto addSource = [&](const RE::NiPoint3& center, float radius, float surfaceZ) {
		if (!wantRipples || sources.size() >= RippleSimulation::MaxSources)
			return;
		const float offset = center.z - surfaceZ;
		if (std::abs(offset) >= radius)
			return;
		// Footprint of the body where it cuts the surface, and how much of it is under water.
		const float footprint = std::sqrt(std::max(radius * radius - offset * offset, 0.0f));
		const float submerged = std::clamp(0.5f - offset / (2.0f * radius), 0.0f, 1.0f);
		RippleSimulation::Source s;
		s.x = center.x;
		s.y = center.y;
		s.radius = footprint;
		s.depth = submerged * std::clamp(footprint / 24.0f, 0.25f, 1.0f);
		sources.push_back(s);
	};

	// Silt kicked up by feet moving over the bed, released at the start of the next simulation steps.
	const float siltRate = settings.WadingSilt > 0.0f ? 0.06f : 0.0f;
	auto addSilt = [&](const RE::NiPoint3& feet, float amount) {
		if (sources.size() >= RippleSimulation::MaxSources || amount <= 0.0f)
			return;
		RippleSimulation::Source s;
		s.x = feet.x;
		s.y = feet.y;
		s.radius = 0.5f * UnitsPerMetre;
		s.depth = 0.0f;
		s.silt = amount;
		sources.push_back(s);
	};

	// Actors: every collision shape (legs, torso, ...) that crosses the surface makes its own ripple.
	if (wantRipples) {
		auto handleActor = [&](RE::Actor* actor) {
			if (!actor || !actor->Is3DLoaded())
				return;
			const auto pos = actor->GetPosition();
			if (pos.GetSquaredDistance(eye) > rangeSq)
				return;
			float flatZ, surfaceZ;
			if (!surfaceAt(actor, pos, flatZ, surfaceZ))
				return;
			// Wading (feet on the bed, under water): moving feet stir the bottom up.
			if (siltRate > 0.0f && pos.z < surfaceZ - 2.0f) {
				const auto state = actor->AsActorState();
				if (state && !state->IsSwimming()) {
					RE::NiPoint3 velocity;
					actor->GetLinearVelocity(velocity);
					const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y) / UnitsPerMetre;
					addSilt(pos, siltRate * std::clamp(speed, 0.0f, 1.5f));
				}
			}
			auto root = actor->Get3D(false);
			if (!root)
				return;
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 center;
				float radius;
				if (Util::GetShapeBound(object, center, radius))
					addSource(center, radius, surfaceZ);
				return sources.size() < RippleSimulation::MaxSources ? RE::BSVisit::BSVisitControl::kContinue : RE::BSVisit::BSVisitControl::kStop;
			});
		};

		if (player)
			handleActor(player);
		if (const auto processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles) {
				if (auto actor = handle.get())
					handleActor(actor.get());
			}
		}
	}

	// Loose physics objects: ripples, plus buoyancy that follows the wave surface.
	if (player && (wantBuoyancy || (wantRipples && settings.PhysicsObjectRipples))) {
		globals::game::tes->ForEachReferenceInRange(player, range, [&](RE::TESObjectREFR* ref) {
			if (!ref || ref->IsDisabled() || ref->IsDeleted() || ref->As<RE::Actor>() || !ref->Is3DLoaded())
				return RE::BSContainer::ForEachResult::kContinue;
			auto root = ref->Get3D();
			RE::bhkNiCollisionObject* niCollision = root ? root->GetCollisionObject() : nullptr;
			if (!niCollision)
				return RE::BSContainer::ForEachResult::kContinue;

			auto bhkBody = niCollision->body.get() ? niCollision->body.get()->AsBhkRigidBody() : nullptr;
			auto body = bhkBody ? bhkBody->GetRigidBody() : nullptr;
			if (!body || !IsDynamicMotion(body))
				return RE::BSContainer::ForEachResult::kContinue;

			RE::NiPoint3 center;
			float radius;
			if (!Util::GetShapeBound(niCollision, center, radius))
				return RE::BSContainer::ForEachResult::kContinue;

			float flatZ, surfaceZ;
			if (!surfaceAt(ref, center, flatZ, surfaceZ))
				return RE::BSContainer::ForEachResult::kContinue;

			// Only bodies floating at the surface; sunken or airborne ones are left alone.
			if (std::abs(center.z - surfaceZ) > radius * 1.5f)
				return RE::BSContainer::ForEachResult::kContinue;

			float velocity[4];
			_mm_storeu_ps(velocity, body->motion.linearVelocity.quad);
			const float worldScaleInv = RE::bhkWorld::GetWorldScaleInverse();

			if (wantRipples && settings.PhysicsObjectRipples)
				addSource(center, radius, surfaceZ);

			if (wantBuoyancy && dt > 0.0f) {
				// The engine floats objects on the flat plane; add a spring towards the displaced
				// surface and match its orbital motion so the object rides the waves.
				const auto wave = snap.SampleAt(center.x, center.y, flatZ);
				const float stiffness = 6.0f * settings.BuoyancyStrength;
				const float damping = 3.0f * settings.BuoyancyStrength;
				const float vz = velocity[2] * worldScaleInv;
				const float targetVz = wave.verticalVelocity + (surfaceZ - center.z) * stiffness;
				const float response = std::clamp(damping * dt, 0.0f, 1.0f);
				const float horizontal = std::clamp(settings.BuoyancyStrength * 0.5f * dt, 0.0f, 1.0f);
				const float worldScale = RE::bhkWorld::GetWorldScale();
				velocity[0] += (wave.velocityX * worldScale - velocity[0]) * horizontal;
				velocity[1] += (wave.velocityY * worldScale - velocity[1]) * horizontal;
				velocity[2] = (vz + (targetVz - vz) * response) * worldScale;
				bhkBody->SetLinearVelocity(RE::hkVector4(velocity[0], velocity[1], velocity[2], 0.0f));
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	if (wantRipples)
		ripples.SubmitSources(std::move(sources));
}

// ============================================================================
// Hooks
// ============================================================================

template <int Variant>
void PBRWater::ISSAOComposite_Render<Variant>::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	globals::features::pbrWater.BindUnderwaterComposite();
	func(imageSpaceShader, shape, param);
}

void PBRWater::PostPostLoad()
{
	// Without the vanilla displacement (wading) mesh: it is a second water surface around the player
	// that cannot follow the waves. Its job is done by the ripple simulation on the real surface.
	stl::detour_thunk<TESWaterSystem_InitializeWater>(REL::RelocationID(31388, 32179));

	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);

	stl::detour_thunk<TESObjectCELL_GetWaterHeight>(REL::RelocationID(18543, 19002));

	// Same Main::Update call site Grass Collision uses; write_thunk_call chains with it.
	stl::write_thunk_call<MainUpdate>(REL::RelocationID(35565, 36564).address() + Util::VersionedRelocation::Select(0x748, 0xC26, 0xC38));

	// The SAO composite fogs the opaque scene; under water it renders the water volume instead.
	stl::write_vfunc<0x1, ISSAOComposite_Render<0>>(RE::VTABLE_BSImagespaceShaderISSAOCompositeSAO[3]);
	stl::write_vfunc<0x1, ISSAOComposite_Render<1>>(RE::VTABLE_BSImagespaceShaderISSAOCompositeFog[3]);
	stl::write_vfunc<0x1, ISSAOComposite_Render<2>>(RE::VTABLE_BSImagespaceShaderISSAOCompositeSAOFog[3]);

	logger::info("[PBR Water] Installed hooks");
}

void PBRWater::TESWaterSystem_InitializeWater::thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool, bool isProcedural)
{
	func(waterSystem, waterTri, form, waterHeight, unk4, true, isProcedural);
}

void PBRWater::BSWaterShader_SetupGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	func(waterShader, pass, renderFlags);
	globals::features::pbrWater.SetupDraw(waterShader, pass);
}

void PBRWater::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	globals::features::pbrWater.RestoreDraw();
	func(waterShader, pass, renderFlags);
}

bool PBRWater::TESObjectCELL_GetWaterHeight::thunk(RE::TESObjectCELL* cell, const RE::NiPoint3& position, float& waterHeight)
{
	const bool result = func(cell, position, waterHeight);
	auto& feature = globals::features::pbrWater;
	// Only when the waves are actually rendered, so physics never follows an invisible surface.
	if (result && feature.settings.GameplayWaves && waterHeight > -1e6f && globals::shaderCache->IsEnabled()) {
		float wave;
		if (feature.GetWaveHeight(position, waterHeight, wave))
			waterHeight += wave;
	}
	return result;
}

void PBRWater::MainUpdate::thunk()
{
	func();
	globals::features::pbrWater.MainThreadUpdate();
}

#undef I18N_KEY_PREFIX
