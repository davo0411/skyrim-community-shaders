#include "PBRWater.h"

#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
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

// Two NLOHMANN_JSON_PASTE runs: the field count exceeds the single-macro limit of 63.
void to_json(nlohmann::json& nlohmann_json_j, const PBRWater::Settings& nlohmann_json_t)
{
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
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
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		WaveResolution,
		UseFetch,
		FetchScale,
		UseBathymetry,
		RiverWaveDamping,
		EnableShoreWaves,
		SwashFoam,
		Roughness,
		SunSpecular,
		PointLightSpecular,
		Subsurface,
		VanillaFresnel,
		Visibility,
		RefractionDistortion,
		ScatteringAnisotropy,
		DownwellingAttenuation,
		WindRoughness,
		Gusts,
		GustSize,
		WindStreaks,
		Turbidity,
		TurbidityPatchiness))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		TurbidityPatchSize,
		SedimentDensity,
		SedimentColor,
		ShoreResuspension,
		RiverTurbidity,
		StormTurbidity,
		WadingSilt,
		SedimentLayerHeight,
		FoamAmount,
		ShoreFoamWidth,
		FoamPersistence,
		FoamScale,
		BreakingFoam,
		WakeFoam,
		WakeFoamLifetime,
		SplashFoam,
		FoamAlbedo,
		FoamDrift,
		WhitecapAmount,
		WhitecapScale,
		WhitecapStreak,
		BubbleAmount,
		EnableRipples,
		PhysicsObjectRipples,
		RippleExtent,
		RippleHeight,
		RippleNormalStrength,
		RippleSpeed,
		RippleHalfLife,
		GameplayWaves,
		BuoyancyStrength,
		EnableFloatingObjects,
		FloatingRange,
		MaxFloatingSize,
		FloatingResponse,
		CarryActors,
		WetShores,
		UnderwaterFollowsWaves,
		WireframeMode,
		DebugView))
}

void from_json(const nlohmann::json& nlohmann_json_j, PBRWater::Settings& nlohmann_json_t)
{
	const PBRWater::Settings nlohmann_json_default_obj{};
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
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
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		WaveResolution,
		UseFetch,
		FetchScale,
		UseBathymetry,
		RiverWaveDamping,
		EnableShoreWaves,
		SwashFoam,
		Roughness,
		SunSpecular,
		PointLightSpecular,
		Subsurface,
		VanillaFresnel,
		Visibility,
		RefractionDistortion,
		ScatteringAnisotropy,
		DownwellingAttenuation,
		WindRoughness,
		Gusts,
		GustSize,
		WindStreaks,
		Turbidity,
		TurbidityPatchiness))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		TurbidityPatchSize,
		SedimentDensity,
		SedimentColor,
		ShoreResuspension,
		RiverTurbidity,
		StormTurbidity,
		WadingSilt,
		SedimentLayerHeight,
		FoamAmount,
		ShoreFoamWidth,
		FoamPersistence,
		FoamScale,
		BreakingFoam,
		WakeFoam,
		WakeFoamLifetime,
		SplashFoam,
		FoamAlbedo,
		FoamDrift,
		WhitecapAmount,
		WhitecapScale,
		WhitecapStreak,
		BubbleAmount,
		EnableRipples,
		PhysicsObjectRipples,
		RippleExtent,
		RippleHeight,
		RippleNormalStrength,
		RippleSpeed,
		RippleHalfLife,
		GameplayWaves,
		BuoyancyStrength,
		EnableFloatingObjects,
		FloatingRange,
		MaxFloatingSize,
		FloatingResponse,
		CarryActors,
		WetShores,
		UnderwaterFollowsWaves,
		WireframeMode,
		DebugView))
}

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
	clamp(s.Choppiness, 0.0f, 1.5f, d.Choppiness);
	clamp(s.DirectionalSpread, 0.0f, 1.0f, d.DirectionalSpread);
	clamp(s.SwellHeight, 0.0f, 4.0f, d.SwellHeight);
	clamp(s.SwellPeriod, 4.0f, 20.0f, d.SwellPeriod);
	clamp(s.SwellDirection, -180.0f, 180.0f, d.SwellDirection);
	s.WaveResolution = std::clamp(s.WaveResolution, 0, 2);
	clamp(s.RiverWaveDamping, 0.0f, 1.0f, d.RiverWaveDamping);
	clamp(s.SwashFoam, 0.0f, 3.0f, d.SwashFoam);
	clamp(s.FetchScale, 1.0f, 50.0f, d.FetchScale);
	clamp(s.Roughness, 0.02f, 0.5f, d.Roughness);
	clamp(s.SunSpecular, 0.0f, 4.0f, d.SunSpecular);
	clamp(s.PointLightSpecular, 0.0f, 4.0f, d.PointLightSpecular);
	clamp(s.Subsurface, 0.0f, 4.0f, d.Subsurface);
	clamp(s.VanillaFresnel, 0.0f, 1.0f, d.VanillaFresnel);
	clamp(s.Visibility, 0.05f, 5.0f, d.Visibility);
	clamp(s.RefractionDistortion, 0.0f, 2.0f, d.RefractionDistortion);
	clamp(s.ScatteringAnisotropy, 0.0f, 0.9f, d.ScatteringAnisotropy);
	clamp(s.DownwellingAttenuation, 0.0f, 3.0f, d.DownwellingAttenuation);
	clamp(s.WindRoughness, 0.0f, 3.0f, d.WindRoughness);
	clamp(s.Gusts, 0.0f, 1.0f, d.Gusts);
	clamp(s.GustSize, 10.0f, 300.0f, d.GustSize);
	clamp(s.WindStreaks, 0.0f, 3.0f, d.WindStreaks);
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
	clamp(s.FoamAmount, 0.0f, 3.0f, d.FoamAmount);
	clamp(s.ShoreFoamWidth, 0.0f, 5.0f, d.ShoreFoamWidth);
	clamp(s.FoamPersistence, 0.0f, 5.0f, d.FoamPersistence);
	clamp(s.FoamScale, 0.2f, 5.0f, d.FoamScale);
	clamp(s.BreakingFoam, 0.0f, 3.0f, d.BreakingFoam);
	clamp(s.WakeFoam, 0.0f, 3.0f, d.WakeFoam);
	clamp(s.WakeFoamLifetime, 1.0f, 30.0f, d.WakeFoamLifetime);
	clamp(s.SplashFoam, 0.0f, 3.0f, d.SplashFoam);
	clamp(s.FoamAlbedo, 0.1f, 1.0f, d.FoamAlbedo);
	clamp(s.FoamDrift, 0.0f, 4.0f, d.FoamDrift);
	clamp(s.WhitecapAmount, 0.0f, 3.0f, d.WhitecapAmount);
	clamp(s.WhitecapScale, 0.5f, 10.0f, d.WhitecapScale);
	clamp(s.WhitecapStreak, 1.0f, 8.0f, d.WhitecapStreak);
	clamp(s.BubbleAmount, 0.0f, 3.0f, d.BubbleAmount);
	clamp(s.RippleExtent, 16.0f, 160.0f, d.RippleExtent);
	clamp(s.RippleHeight, 0.0f, 0.5f, d.RippleHeight);
	clamp(s.RippleNormalStrength, 0.0f, 4.0f, d.RippleNormalStrength);
	clamp(s.RippleSpeed, 0.2f, 4.0f, d.RippleSpeed);
	clamp(s.RippleHalfLife, 0.2f, 6.0f, d.RippleHalfLife);
	clamp(s.BuoyancyStrength, 0.0f, 3.0f, d.BuoyancyStrength);
	clamp(s.FloatingRange, 30.0f, 300.0f, d.FloatingRange);
	clamp(s.MaxFloatingSize, 2.0f, 150.0f, d.MaxFloatingSize);
	clamp(s.FloatingResponse, 0.0f, 2.0f, d.FloatingResponse);
	s.WireframeMode = std::clamp(s.WireframeMode, 0, 2);
	s.DebugView = std::clamp(s.DebugView, 0, 9);
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
	addFloat("Choppiness", "Choppiness", "How sharp and pinched the wave crests are", &settings.Choppiness, defaults.Choppiness, 0.0f, 1.5f);
	addFloat("DirectionalSpread", "Directional Spread", "0 = waves all follow the wind (swell), 1 = confused sea", &settings.DirectionalSpread, defaults.DirectionalSpread, 0.0f, 1.0f);
	addFloat("WindSpeedStorm", "Storm Wind Speed", "Wind speed (m/s) reached at the weather's maximum wind", &settings.WindSpeedStorm, defaults.WindSpeedStorm, 1.0f, 40.0f);
	addFloat("SwellHeight", "Swell Height", "Significant height (m) of the swell arriving from distant storms", &settings.SwellHeight, defaults.SwellHeight, 0.0f, 4.0f);
	addFloat("SwellPeriod", "Swell Period", "Period (s) of the swell: longer is faster, longer crests", &settings.SwellPeriod, defaults.SwellPeriod, 4.0f, 20.0f);
	addFloat("SwellDirection", "Swell Direction", "Direction the swell travels, in degrees from the wind", &settings.SwellDirection, defaults.SwellDirection, -180.0f, 180.0f);
	addFloat("FoamAmount", "Foam Amount", "Overall foam coverage", &settings.FoamAmount, defaults.FoamAmount, 0.0f, 3.0f);
	addFloat("Visibility", "Water Visibility", "Scales how far light travels through the water before it is absorbed", &settings.Visibility, defaults.Visibility, 0.05f, 5.0f);
	addFloat("Subsurface", "Subsurface Scattering", "Light glowing through backlit wave crests", &settings.Subsurface, defaults.Subsurface, 0.0f, 4.0f);
	addFloat("Roughness", "Surface Roughness", "Micro-roughness of the water surface below the wave detail", &settings.Roughness, defaults.Roughness, 0.02f, 0.5f);
	addFloat("Turbidity", "Turbidity", "Suspended sediment in the water: higher is murkier", &settings.Turbidity, defaults.Turbidity, 0.0f, 5.0f);
	addFloat("StormTurbidity", "Storm Turbidity", "Extra sediment stirred up by strong wind and rain", &settings.StormTurbidity, defaults.StormTurbidity, 0.0f, 3.0f);
	addFloat("Gusts", "Gusts", "How strongly gusts vary the wind over the water", &settings.Gusts, defaults.Gusts, 0.0f, 1.0f);
	registry->RegisterVariable(std::make_shared<WeatherVariables::Float3Variable>("SedimentColor", "Sediment Colour", "Colour of murky water", &settings.SedimentColor, defaults.SedimentColor));
}

void PBRWater::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("waves"), "Waves"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("wave_height"), "Wave Height"), this, "WaveHeight", &settings.WaveHeight, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wave_height_tooltip"), "Scales the wind-driven wave spectrum. The waves themselves come from the weather's wind, the open-water distance upwind (fetch) and the water depth."));
		Util::WeatherUI::SliderFloat(T(TKEY("choppiness"), "Choppiness"), this, "Choppiness", &settings.Choppiness, 0.0f, 1.5f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("choppiness_tooltip"), "How sharp the crests are: how far the water moves sideways towards each crest. 1 follows linear wave theory; higher values pinch the crests and produce more crest foam. Automatically held back where the sea would fold over itself."));
		Util::WeatherUI::SliderFloat(T(TKEY("directional_spread"), "Directional Spread"), this, "DirectionalSpread", &settings.DirectionalSpread, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("directional_spread_tooltip"), "0 lines the waves up with the wind in long crests; 1 gives a confused, short-crested sea."));
		Util::WeatherUI::SliderFloat(T(TKEY("swell_height"), "Swell Height"), this, "SwellHeight", &settings.SwellHeight, 0.0f, 4.0f, "%.2f m");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("swell_height_tooltip"), "Long, regular waves arriving from distant storms, on top of the local wind sea. Only on the open sea: sheltered water never sees them."));
		Util::WeatherUI::SliderFloat(T(TKEY("swell_period"), "Swell Period"), this, "SwellPeriod", &settings.SwellPeriod, 4.0f, 20.0f, "%.1f s");
		Util::WeatherUI::SliderFloat(T(TKEY("swell_direction"), "Swell Direction"), this, "SwellDirection", &settings.SwellDirection, -180.0f, 180.0f, "%.0f deg");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("swell_direction_tooltip"), "Direction the swell travels, relative to the wind. Swell crossing the wind sea gives the most natural, irregular water."));
		{
			const char* sizes[] = { T(TKEY("wave_resolution_low"), "Low (128)"), T(TKEY("wave_resolution_medium"), "Medium (256)"), T(TKEY("wave_resolution_high"), "High (512)") };
			ImGui::Combo(T(TKEY("wave_resolution"), "Wave Detail"), &settings.WaveResolution, sizes, IM_ARRAYSIZE(sizes));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wave_resolution_tooltip"), "Resolution of each FFT wave cascade. Higher values add finer ripples close up at a small GPU cost; gameplay is unaffected."));
		}
		ImGui::SliderFloat(T(TKEY("wind_speed_calm"), "Calm Wind Speed"), &settings.WindSpeedCalm, 0.0f, 10.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_calm_tooltip"), "Wind speed used when the weather reports no wind."));
		Util::WeatherUI::SliderFloat(T(TKEY("wind_speed_storm"), "Storm Wind Speed"), this, "WindSpeedStorm", &settings.WindSpeedStorm, 1.0f, 40.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_storm_tooltip"), "Wind speed reached at the weather's maximum wind. Wave height grows with the square of the wind speed."));
		ImGui::Checkbox(T(TKEY("use_fetch"), "Fetch-Limited Waves"), &settings.UseFetch);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_fetch_tooltip"), "Limits wave size by how much open water lies upwind, so ponds and rivers stay calm while the open sea builds swell."));
		if (settings.UseFetch) {
			ImGui::SliderFloat(T(TKEY("fetch_scale"), "Fetch Scale"), &settings.FetchScale, 1.0f, 50.0f, "%.0fx");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("fetch_scale_tooltip"), "Skyrim's distances are compressed, so real fetch only builds a full sea far beyond where you can sail. Higher values let waves grow over shorter stretches of open water: river mouths and bays get rougher, while rivers and ponds stay calm."));
		}
		ImGui::Checkbox(T(TKEY("use_bathymetry"), "Depth-Aware Waves"), &settings.UseBathymetry);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("use_bathymetry_tooltip"), "Uses the Terrain Shadows heightmap to calm waves in shallow water and to drive shoreline waves."));
		ImGui::SliderFloat(T(TKEY("river_wave_damping"), "River Wave Damping"), &settings.RiverWaveDamping, 0.0f, 1.0f, "%.2f");
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("shore"), "Shoreline"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_shore_waves"), "Shore Waves"), &settings.EnableShoreWaves);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_shore_waves_tooltip"), "The sea's own waves slow down over the shoaling bed, turn towards the shore, steepen and break, and each one runs up the beach and drains back, leaving foam and wet sand. Only on beaches where waves break: they surge against rock faces and cliffs instead. Needs Depth-Aware Waves."));
		if (settings.EnableShoreWaves) {
			ImGui::SliderFloat(T(TKEY("swash_foam"), "Swash Foam"), &settings.SwashFoam, 0.0f, 3.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("swash_foam_tooltip"), "Foam riding each wave up the beach and left on the sand when it drains back."));
			ImGui::Checkbox(T(TKEY("wet_shores"), "Wet Shores"), &settings.WetShores);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wet_shores_tooltip"), "Sand, rocks and anything else the swash reaches turn wet and slowly dry out (Wetness Effects)."));
		}
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
		ImGui::SliderFloat(T(TKEY("wind_roughness"), "Wind Roughness"), &settings.WindRoughness, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_roughness_tooltip"), "Tiny ripples raised by the local wind blur the reflections and spread the sun's glitter. Calm water is glassy, rain roughens it."));
		Util::WeatherUI::SliderFloat(T(TKEY("gusts"), "Gusts"), this, "Gusts", &settings.Gusts, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("gusts_tooltip"), "Gusts sweep across the water as darker, rougher patches (cat's paws). Water in the lee of the upwind shore stays calm."));
		ImGui::SliderFloat(T(TKEY("gust_size"), "Gust Size"), &settings.GustSize, 10.0f, 300.0f, "%.0f m");
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

	if (ImGui::TreeNodeEx(T(TKEY("foam"), "Surface Foam"), ImGuiTreeNodeFlags_DefaultOpen)) {
		Util::WeatherUI::SliderFloat(T(TKEY("foam_amount"), "Foam Amount"), this, "FoamAmount", &settings.FoamAmount, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("foam_amount_tooltip"), "Settled foam along shores, around objects, in wakes and where shore waves break."));
		ImGui::SliderFloat(T(TKEY("shore_foam_width"), "Shore Foam Width"), &settings.ShoreFoamWidth, 0.0f, 5.0f, "%.2f m");
		ImGui::SliderFloat(T(TKEY("breaking_foam"), "Breaking Wave Foam"), &settings.BreakingFoam, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("wake_foam"), "Wake Bubbles"), &settings.WakeFoam, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wake_foam_tooltip"), "Bubbles churned into the water around anything moving through it and by breaking ripples. They drift with the surface current and leave almost no foam."));
		ImGui::SliderFloat(T(TKEY("wake_foam_lifetime"), "Wake Bubble Lifetime"), &settings.WakeFoamLifetime, 1.0f, 30.0f, "%.1f s");
		ImGui::SliderFloat(T(TKEY("splash_foam"), "Splash Bubbles"), &settings.SplashFoam, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("splash_foam_tooltip"), "Air churned in by anything moving through the water, and a burst when a body hits it fast: jumping or falling in, a thrown object."));
		ImGui::SliderFloat(T(TKEY("wind_streaks"), "Wind Streaks"), &settings.WindStreaks, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_streaks_tooltip"), "Lines along the wind (windrows): old foam gathers into streaks in a strong wind, and smooth, glassy slicks form in a light breeze."));
		ImGui::SliderFloat(T(TKEY("foam_scale"), "Foam Pattern Size"), &settings.FoamScale, 0.2f, 5.0f, "%.2f m");
		ImGui::SliderFloat(T(TKEY("foam_albedo"), "Foam Brightness"), &settings.FoamAlbedo, 0.1f, 1.0f, "%.2f");
		ImGui::SliderFloat(T(TKEY("foam_drift"), "Foam Drift"), &settings.FoamDrift, 0.0f, 4.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("foam_drift_tooltip"), "How fast foam and bubbles are carried downwind by the surface drift."));
		ImGui::SliderFloat(T(TKEY("bubble_amount"), "Bubbles"), &settings.BubbleAmount, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("bubble_amount_tooltip"), "Drifting bubble specks under foam, whitecaps and wakes."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("whitecaps"), "Whitecaps"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("whitecap_amount"), "Whitecap Amount"), &settings.WhitecapAmount, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("whitecap_amount_tooltip"), "Scales the measured share of breaking waves. Which crests break is physical: the steepness of the dominant waves sets how many of them break (Banner, Babanin and Young 2000; nothing breaks below a threshold steepness), and only their most compressed fronts turn white. 1 is as measured."));
		ImGui::SliderFloat(T(TKEY("foam_persistence"), "Crest Foam Trail"), &settings.FoamPersistence, 0.0f, 5.0f, "%.1f s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("foam_persistence_tooltip"), "How long foam from a breaking crest lingers and thins out behind the wave."));
		ImGui::SliderFloat(T(TKEY("whitecap_scale"), "Whitecap Pattern Size"), &settings.WhitecapScale, 0.5f, 10.0f, "%.1f m");
		ImGui::SliderFloat(T(TKEY("whitecap_streak"), "Whitecap Streaks"), &settings.WhitecapStreak, 1.0f, 8.0f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("whitecap_streak_tooltip"), "How strongly whitecap foam is torn into streaks along the wind."));
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
		ImGui::Checkbox(T(TKEY("underwater_follows_waves"), "Underwater Follows Waves"), &settings.UnderwaterFollowsWaves);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("underwater_follows_waves_tooltip"), "The underwater view starts where the camera dips under the rendered waves, not under the flat water level. Turn off if the underwater view flickers near the surface."));
		ImGui::SliderFloat(T(TKEY("buoyancy_strength"), "Floating Object Response"), &settings.BuoyancyStrength, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("buoyancy_strength_tooltip"), "How strongly floating physics objects follow the waves. 0 leaves them on the flat vanilla plane."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("floating_objects"), "Floating Objects"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_floating_objects"), "Boats and Ice Ride the Waves"), &settings.EnableFloatingObjects);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_floating_objects_tooltip"), "Boats, ships, rafts and ice floes heave, pitch and roll on the rendered waves, with everything on board. Found automatically: anything placed afloat that does not rest on the bottom. Nothing is saved; objects are back in place whenever the game saves."));
		if (settings.EnableFloatingObjects) {
			ImGui::SliderFloat(T(TKEY("floating_response"), "Hull Motion"), &settings.FloatingResponse, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("floating_response_tooltip"), "Scales how much floating hulls move with the waves."));
			ImGui::Checkbox(T(TKEY("carry_actors"), "Carry Actors on Board"), &settings.CarryActors);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("carry_actors_tooltip"), "The player and NPCs standing on a floating hull move with its deck, and their weight sinks and tilts small boats."));
			ImGui::SliderFloat(T(TKEY("floating_range"), "Range"), &settings.FloatingRange, 30.0f, 300.0f, "%.0f m");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("floating_range_tooltip"), "Objects within this distance of the player float. Further away they rest in place, where the waves have flattened."));
			ImGui::SliderFloat(T(TKEY("max_floating_size"), "Largest Hull"), &settings.MaxFloatingSize, 2.0f, 150.0f, "%.0f m");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("max_floating_size_tooltip"), "Longer objects placed in the water stay fixed. Lower it if a large structure moves that should not."));
		}
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
			T(TKEY("debug_wind"), "Local Wind"),
		};
		ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &settings.DebugView, views, IM_ARRAYSIZE(views));

		if (const auto snap = snapshot.load(std::memory_order_acquire); snap && snap->ocean) {
			const auto& o = *snap->ocean;
			ImGui::Text("Wind %.1f m/s, peak period %.1f s, Hs %.2f m, choppiness %.2f", o.windSpeed, TwoPi / o.peakOmega, o.significantHeight / UnitsPerMetre, o.choppiness);
			ImGui::Text("Whitecaps %.1f%%, mean square slope %.3f", o.whitecapCoverage * 100.0f, o.meanSquareSlope);
			ImGui::Text("Breaking waves per band: %.1f%% %.1f%% %.1f%%", PBRWaterModel::CascadeBreaking(o, 0, 1.0f).probability * 100.0f,
				PBRWaterModel::CascadeBreaking(o, 1, 1.0f).probability * 100.0f, PBRWaterModel::CascadeBreaking(o, 2, 1.0f).probability * 100.0f);
			ImGui::Text("FFT %u x %u cascades: %s, gameplay mirror: %s", o.resolution, PBRWaterModel::NumCascades, oceanReady.load() ? "running" : "unavailable", snap->mirror0 ? "running" : "idle");
		}
		ImGui::Text("Fetch: %s, bathymetry: %s%s", environment.GetFetch() ? "ready" : "none", environment.GetBathymetry() ? "ready" : "none", environment.IsBuilding() ? " (building)" : "");
		ImGui::Text("Shore field: %s%s, swash: %s", environment.GetShore() ? "ready" : "none", environment.IsBuildingShore() ? " (building)" : "", frameData.Swash0.w > 0.5f ? "running" : "idle");
		const auto floatingStats = floating.GetStats();
		ImGui::Text("Floating: %u hulls, %u attached, %u actors on board, %u small objects riding the water", floatingStats.floaters, floatingStats.parts, floatingStats.carried, floatingStats.riders);
		ImGui::TextUnformatted(floating.GetSelectionReport().c_str());
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

	// The ocean tiles repeat (wrap) and are seen at grazing angles (anisotropic filtering).
	D3D11_SAMPLER_DESC oceanDesc = samplerDesc;
	oceanDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	oceanDesc.MaxAnisotropy = 8;
	oceanDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	oceanDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&oceanDesc, oceanSampler.put()));
	Util::SetResourceName(oceanSampler.get(), "PBRWater::OceanSampler");

	ripples.SetupResources();
	swash.SetupResources();

	// The Skylighting occluder: a camera-centred grid of (OccluderQuads + 1)^2 vertices made by the vertex shader.
	{
		constexpr uint32_t verts = OccluderQuads + 1;
		static_assert(verts * verts <= 65536, "16-bit indices");
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<size_t>(OccluderQuads) * OccluderQuads * 6);
		for (uint32_t y = 0; y < OccluderQuads; ++y) {
			for (uint32_t x = 0; x < OccluderQuads; ++x) {
				const auto a = static_cast<uint16_t>(y * verts + x);
				const auto b = static_cast<uint16_t>(a + 1);
				const auto c = static_cast<uint16_t>(a + verts);
				const auto e = static_cast<uint16_t>(c + 1);
				indices.insert(indices.end(), { a, c, b, b, c, e });
			}
		}
		D3D11_BUFFER_DESC indexDesc{};
		indexDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
		indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
		indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		const D3D11_SUBRESOURCE_DATA indexData{ indices.data(), 0, 0 };
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&indexDesc, &indexData, occluderIndices.put()));
		Util::SetResourceName(occluderIndices.get(), "PBRWater::SkylightingOccluderIndices");

		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthDesc, occluderDepthState.put()));
		Util::SetResourceName(occluderDepthState.get(), "PBRWater::SkylightingOccluderDepth");

		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, occluderRasterizer.put()));
		Util::SetResourceName(occluderRasterizer.get(), "PBRWater::SkylightingOccluderRasterizer");

		occluderCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<OccluderCB>(), "PBRWater::SkylightingOccluderCB");
		CompileOccluder();
	}
	ocean.SetupResources();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);
	hullAtlas.SetupResources();
}

void PBRWater::ClearShaderCache()
{
	ripples.ClearShaderCache();
	swash.ClearShaderCache();
	CompileOccluder();
	ocean.ClearShaderCache();
	hullAtlas.ClearShaderCache();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);
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

void PBRWater::UpdateLandTexture()
{
	const uint32_t generation = environment.GetLandGeneration();
	if (generation == uploadedLandGeneration)
		return;
	uploadedLandGeneration = generation;

	auto grid = environment.GetLand();
	uploadedLand = grid;
	landTexture.reset();
	if (!grid || !grid->Valid())
		return;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = grid->width;
	desc.Height = grid->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init{ grid->heights.data(), static_cast<UINT>(grid->width * sizeof(float)), 0 };
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
		logger::warn("[PBR Water] Failed to create the terrain grid texture");
		uploadedLand.reset();
		return;
	}
	landTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::LoadedTerrain");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	landTexture->CreateSRV(srvDesc);
}

// ============================================================================
// Per-frame (render thread)
// ============================================================================

uint32_t PBRWater::WaveResolutionSize() const
{
	return 128u << static_cast<uint32_t>(std::clamp(settings.WaveResolution, 0, 2));
}

void PBRWater::Prepass()
{
	UpdateFetchTexture();
	UpdateLandTexture();

	// Last frame's water draws leave our textures bound; the simulations below write them.
	{
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
		context->VSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->HSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->DSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->PSSetShaderResources(110, WaterSRVCount, nullSrvs);
	}
	UpdateShoreTextures();

	const float dt = renderDelta.load(std::memory_order_acquire);
	if (const auto snap = snapshot.load(std::memory_order_acquire)) {
		OceanSimulation::Settings oceanSettings;
		oceanSettings.resolution = WaveResolutionSize();
		if (snap->ocean) {
			for (uint32_t c = 0; c < PBRWaterModel::NumCascades; ++c)
				oceanSettings.breaking[c] = PBRWaterModel::CascadeBreaking(*snap->ocean, c, settings.WhitecapAmount);
		}
		oceanSettings.foamTrail = settings.FoamPersistence;
		ocean.Update(*snap, dt, oceanSettings);
	}
	oceanReady.store(ocean.IsReady(), std::memory_order_release);

	if (settings.EnableRipples) {
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		RippleSimulation::Settings rippleSettings;
		rippleSettings.extent = settings.RippleExtent * UnitsPerMetre;
		rippleSettings.waveSpeed = settings.RippleSpeed;
		rippleSettings.halfLife = settings.RippleHalfLife;
		rippleSettings.foamHalfLife = settings.WakeFoamLifetime * 0.5f;
		rippleSettings.siltHalfLife = 20.0f;
		rippleSettings.heightScale = settings.RippleHeight * UnitsPerMetre;
		if (const auto snap = snapshot.load(std::memory_order_acquire)) {
			rippleSettings.driftX = snap->foamDriftVelocity[0];
			rippleSettings.driftY = snap->foamDriftVelocity[1];
		}
		ripples.Update(cameraPos.x, cameraPos.y, dt, rippleSettings);
	}

	// Hulls that came near get the maps that keep the water out of them; the maps of hulls gone are let go.
	if (const auto masks = settings.EnableFloatingObjects ? floating.GetHullMasks() : nullptr)
		hullAtlas.Update(*masks);
	else
		hullAtlas.Update({});

	UpdateFrameConstants();
	UpdateSwash(dt);
	ApplyCameraWater();
}

void PBRWater::UpdateShoreTextures()
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	const auto current = snap ? snap->shoreField : nullptr;
	const auto previous = snap ? snap->shorePrevious : nullptr;
	if (current != shoreTextures.field) {
		if (previous && previous == shoreTextures.field)
			previousShoreTextures = std::move(shoreTextures);
		shoreTextures = UploadShoreField(current);
	}
	if (previous != previousShoreTextures.field)
		previousShoreTextures = UploadShoreField(previous);
}

PBRWater::ShoreTextures PBRWater::UploadShoreField(std::shared_ptr<const PBRWaterModel::ShoreField> field)
{
	ShoreTextures result;
	if (!field || !field->Valid())
		return result;

	const size_t count = static_cast<size_t>(field->width) * field->height;
	// Full precision for the arrival times: they become the waves' phase (omega T, tens of radians), where half floats
	// would step it visibly. The slownesses (0.05-1 s/m) and the rest are fine as halves.
	std::vector<float> times(count * 4);
	std::vector<uint16_t> slowness(count * 4), beach(count * 4);
	auto half = [](float v) { return DirectX::PackedVector::XMConvertFloatToHalf(v); };
	for (size_t i = 0; i < count; ++i) {
		times[i * 4 + 0] = field->travelTime[0][i];
		times[i * 4 + 1] = field->travelTime[1][i];
		times[i * 4 + 2] = field->travelTime[2][i];
		// No water in reach: a sentinel the shaders test for (NaN would spread through the bilinear filter).
		times[i * 4 + 3] = std::isfinite(field->level[i]) ? field->level[i] : -1.0e9f;
		slowness[i * 4 + 0] = half(field->slownessX[0][i]);
		slowness[i * 4 + 1] = half(field->slownessY[0][i]);
		slowness[i * 4 + 2] = half(field->slownessX[1][i]);
		slowness[i * 4 + 3] = half(field->slownessY[1][i]);
		beach[i * 4 + 0] = half(field->slownessX[2][i]);
		beach[i * 4 + 1] = half(field->slownessY[2][i]);
		beach[i * 4 + 2] = half(field->slope[i]);
		beach[i * 4 + 3] = half(field->exposure[i]);
	}

	auto create = [&](DXGI_FORMAT format, const void* data, UINT pitch, const char* name) -> std::unique_ptr<Texture2D> {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = field->width;
		desc.Height = field->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{ data, pitch, 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
			logger::warn("[PBR Water] Failed to create the {} texture", name);
			return nullptr;
		}
		auto texture2D = std::make_unique<Texture2D>(texture.detach(), name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		texture2D->CreateSRV(srvDesc);
		return texture2D;
	};
	result.times = create(DXGI_FORMAT_R32G32B32A32_FLOAT, times.data(), static_cast<UINT>(field->width * 4 * sizeof(float)), "PBRWater::ShoreTimes");
	result.slowness = create(DXGI_FORMAT_R16G16B16A16_FLOAT, slowness.data(), static_cast<UINT>(field->width * 4 * sizeof(uint16_t)), "PBRWater::ShoreSlowness");
	result.beach = create(DXGI_FORMAT_R16G16B16A16_FLOAT, beach.data(), static_cast<UINT>(field->width * 4 * sizeof(uint16_t)), "PBRWater::ShoreBeach");
	if (result.times && result.slowness && result.beach)
		result.field = std::move(field);
	else
		result = {};
	return result;
}

void PBRWater::UpdateSwash(float dt)
{
	if (!gpuBuffer || frameData.Shore0.x < 0.5f) {
		frameData.Swash0 = {};
		return;
	}

	// The swash steps with this frame's wave constants and textures.
	auto context = globals::d3d::context;
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	context->CSSetConstantBuffers(7, 1, cbs);
	ID3D11Buffer* frameBuffers[1] = { *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->CSSetShaderResources(110, 13, srvs);  // t110-t122: the swash's own history is bound by the simulation
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(12, 2, samplers);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	ShoreSimulation::Settings swashSettings;
	// The swash runs wherever the waves are still at full height (about a metre per texel at the default fade).
	swashSettings.extent = std::clamp(2.0f * settings.DisplacementFadeStart, 8960.0f, 32768.0f);
	// The backwash's foam lasts about three waves on the sand, unless the next uprush sweeps it away first.
	swashSettings.foamLifetime = 3.0f * std::max(frameData.Shore3.y, 1.0f);
	swash.Update(cameraPos.x, cameraPos.y, dt, swashSettings);

	ID3D11ShaderResourceView* nullSrvs[13] = {};
	context->CSSetShaderResources(110, 13, nullSrvs);
	ID3D11Buffer* nullCbs[1] = { nullptr };
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->CSSetSamplers(12, 2, nullSamplers);

	// The draws read the wet state the simulation just wrote.
	frameData.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), swash.IsReady() ? 1.0f : 0.0f };

	// Lit terrain and objects read the wet sand, the foam on it and the water height from it.
	ID3D11ShaderResourceView* wetSrv = swash.IsReady() ? swash.GetWetSRV() : nullptr;
	context->PSSetShaderResources(123, 1, &wetSrv);
}

PBRWater::SharedSettings PBRWater::GetCommonBufferData() const
{
	SharedSettings data{};
	if (!loaded)
		return data;
	if (frameData.Swash0.w > 0.5f) {
		data.SwashGrid = frameData.Swash0;
		data.Wetness = { settings.WetShores ? 1.0f : 0.0f, settings.SwashFoam, settings.FoamScale * UnitsPerMetre, 0.0f };
	}
	// Drawn this frame or the last (the reflection passes update the shared data before Skylighting's probes).
	const bool occluding = occluderFrame != UINT32_MAX && globals::state->frameCount - occluderFrame <= 1;
	data.Skylighting = { occluding ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	return data;
}

void PBRWater::CompileOccluder()
{
	occluderVS = nullptr;
	if (auto* vs = reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SkylightingOccluderVS.hlsl", {}, "vs_5_0")))
		occluderVS.attach(vs);
	else
		logger::warn("[PBR Water] Skylighting occluder failed to compile; the waves do not occlude the sky");
}

void PBRWater::DrawSkylightingOccluder(Texture2D& occlusion, const REX::W32::XMFLOAT4X4& occlusionViewProj)
{
	ID3D11DepthStencilView* dsv = occlusion.dsv.get();
	if (!loaded || !dsv || !occluderVS || !occluderIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer || !globals::shaderCache->IsEnabled())
		return;
	// Calm water is the flat plane the probes are lifted above anyway: only waves need drawing.
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	// The still water of the cells around the eye, as SharedData::GetWaterData sees them.
	OccluderCB cb{};
	cb.occlusionViewProj = occlusionViewProj;
	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	cb.eye = { eye.x, eye.y, eye.z, OccluderExtent };
	cb.grid = { static_cast<float>(OccluderQuads), OccluderExtent / OccluderQuads, eyeCellX, eyeCellY };
	bool anyWater = false;
	for (int32_t j = -2; j <= 2; ++j) {
		for (int32_t i = -2; i <= 2; ++i) {
			float level = -1.0e9f;
			const RE::NiPoint3 centre{ (eyeCellX + static_cast<float>(i) + 0.5f) * CellSize, (eyeCellY + static_cast<float>(j) + 0.5f) * CellSize, eye.z };
			if (auto* cell = tes->GetCell(centre)) {
				if (const float height = cell->GetExteriorWaterHeight(); height > -1.0e6f) {
					level = height;
					anyWater = true;
				}
			}
			cb.cellLevels[(i + 2) + 5 * (j + 2)] = { level, 0.0f, 0.0f, 0.0f };
		}
	}
	if (!anyWater)
		return;
	occluderCB->Update(cb);
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);

	auto context = globals::d3d::context;

	// Save what this draw changes.
	winrt::com_ptr<ID3D11InputLayout> savedLayout;
	D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
	winrt::com_ptr<ID3D11Buffer> savedIndices;
	DXGI_FORMAT savedIndexFormat{};
	UINT savedIndexOffset = 0;
	context->IAGetInputLayout(savedLayout.put());
	context->IAGetPrimitiveTopology(&savedTopology);
	context->IAGetIndexBuffer(savedIndices.put(), &savedIndexFormat, &savedIndexOffset);
	winrt::com_ptr<ID3D11VertexShader> savedVS;
	winrt::com_ptr<ID3D11HullShader> savedHS;
	winrt::com_ptr<ID3D11DomainShader> savedDS;
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	winrt::com_ptr<ID3D11PixelShader> savedPS;
	context->VSGetShader(savedVS.put(), nullptr, nullptr);
	context->HSGetShader(savedHS.put(), nullptr, nullptr);
	context->DSGetShader(savedDS.put(), nullptr, nullptr);
	context->GSGetShader(savedGS.put(), nullptr, nullptr);
	context->PSGetShader(savedPS.put(), nullptr, nullptr);
	ID3D11Buffer* savedCB0 = nullptr;
	ID3D11Buffer* savedCB7 = nullptr;
	ID3D11Buffer* savedCB12 = nullptr;
	context->VSGetConstantBuffers(0, 1, &savedCB0);
	context->VSGetConstantBuffers(7, 1, &savedCB7);
	context->VSGetConstantBuffers(12, 1, &savedCB12);
	ID3D11ShaderResourceView* savedFlowmap = nullptr;
	ID3D11ShaderResourceView* savedSRVs[WaterSRVCount] = {};
	context->VSGetShaderResources(8, 1, &savedFlowmap);
	context->VSGetShaderResources(110, WaterSRVCount, savedSRVs);
	ID3D11SamplerState* savedSamplers[2] = {};
	context->VSGetSamplers(12, 2, savedSamplers);
	winrt::com_ptr<ID3D11RasterizerState> savedRasterizer;
	context->RSGetState(savedRasterizer.put());
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	context->RSGetViewports(&viewportCount, savedViewports);
	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepthState;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepthState.put(), &savedStencilRef);

	// Depth only, with Skylighting's own transform.
	context->OMSetRenderTargets(0, nullptr, dsv);
	context->OMSetDepthStencilState(occluderDepthState.get(), 0);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(occlusion.desc.Width), static_cast<float>(occlusion.desc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(occluderRasterizer.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetIndexBuffer(occluderIndices.get(), DXGI_FORMAT_R16_UINT, 0);
	context->VSSetShader(occluderVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* cb0 = occluderCB->CB();
	ID3D11Buffer* cb7 = gpuBuffer->CB();
	ID3D11Buffer* cb12 = *globals::game::perFrame.get();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->VSSetConstantBuffers(7, 1, &cb7);
	context->VSSetConstantBuffers(12, 1, &cb12);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->VSSetShaderResources(110, WaterSRVCount, srvs);
	ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
	context->VSSetShaderResources(8, 1, &flowmap);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->VSSetSamplers(12, 2, samplers);

	context->DrawIndexed(OccluderQuads * OccluderQuads * 6, 0, 0);

	// Restore.
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	context->OMSetDepthStencilState(savedDepthState.get(), savedStencilRef);
	context->RSSetViewports(viewportCount, savedViewports);
	context->RSSetState(savedRasterizer.get());
	context->IASetInputLayout(savedLayout.get());
	context->IASetPrimitiveTopology(savedTopology);
	context->IASetIndexBuffer(savedIndices.get(), savedIndexFormat, savedIndexOffset);
	context->VSSetShader(savedVS.get(), nullptr, 0);
	context->HSSetShader(savedHS.get(), nullptr, 0);
	context->DSSetShader(savedDS.get(), nullptr, 0);
	context->GSSetShader(savedGS.get(), nullptr, 0);
	context->PSSetShader(savedPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &savedCB0);
	context->VSSetConstantBuffers(7, 1, &savedCB7);
	context->VSSetConstantBuffers(12, 1, &savedCB12);
	context->VSSetShaderResources(8, 1, &savedFlowmap);
	context->VSSetShaderResources(110, WaterSRVCount, savedSRVs);
	context->VSSetSamplers(12, 2, savedSamplers);
	auto release = [](auto*& p) {
		if (p)
			p->Release();
		p = nullptr;
	};
	release(savedCB0);
	release(savedCB7);
	release(savedCB12);
	release(savedFlowmap);
	for (auto*& srv : savedSRVs)
		release(srv);
	for (auto*& sampler : savedSamplers)
		release(sampler);
	for (auto*& rtv : savedRTVs)
		release(rtv);
	release(savedDSV);

	occluderFrame = globals::state->frameCount;
}

void PBRWater::UpdateFrameConstants()
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == frameDataFrame)
		return;
	const uint32_t lastFrame = frameDataFrame;  // frameData still holds that frame's constants
	frameDataFrame = frame;

	GpuData d{};
	const auto snap = snapshot.load(std::memory_order_acquire);

	if (snap && snap->ocean) {
		const auto& o = *snap->ocean;
		const uint32_t size = ocean.GetResolution() ? ocean.GetResolution() : WaveResolutionSize();
		for (uint32_t c = 0; c < PBRWaterModel::NumCascades; ++c) {
			// Tile coordinate of the reference camera, in double precision: the GPU only adds camera-relative offsets.
			const double tile = PBRWaterModel::CascadeLength[c] * PBRWaterModel::UnitsPerMetre;
			double u = snap->refX / tile;
			double v = snap->refY / tile;
			u -= std::floor(u);
			v -= std::floor(v);
			// FFT sample j sits at j * tile / size, i.e. at texel centres shifted by half a texel.
			const double halfTexel = 0.5 / size;
			const auto& band = o.cascades[c];
			d.Cascade0[c] = { static_cast<float>(1.0 / tile), static_cast<float>(u + halfTexel), static_cast<float>(v + halfTexel), static_cast<float>(tile / size) };
			d.Cascade1[c] = { band.meanOmega, 1.0f / std::max(band.pmWeightOpenSea, 1e-4f), band.meanK / UnitsPerMetre, o.choppiness * std::sqrt(band.meanSquareSlope) };
			// Height spread (depth-limited breaking) and deep-water phase speed of the band.
			d.Cascade2[c] = { std::sqrt(std::max(band.variance, 0.0f)) * UnitsPerMetre, static_cast<float>(PBRWaterModel::Gravity) / std::max(band.meanOmega, 1e-3f) * UnitsPerMetre, 0.0f, 0.0f };
		}
		const bool active = ocean.IsReady() && !o.calm;
		d.Params0 = { active ? 1.0f : 0.0f, o.windSpeed, o.peakOmega, static_cast<float>(PBRWaterModel::Gravity) * UnitsPerMetre };
		d.Params1 = { std::cos(snap->windDirection), std::sin(snap->windDirection), settings.DisplacementFadeStart, settings.DisplacementFadeEnd };
		d.Params2 = { o.choppiness, o.whitecapCoverage, active ? o.displacementBound : 0.0f, static_cast<float>(std::max(ocean.GetMipCount(), 1u) - 1) };
		d.RefCamPos = { static_cast<float>(snap->refX), static_cast<float>(snap->refY), static_cast<float>(snap->refZ), 0.0f };
		// Shore waves need the shore fields the snapshot blends between on the GPU. The components' frequencies are this
		// frame's (eased) ones, not the ones the field was traced for: it is rebuilt when they differ by more than a few
		// percent, and its arrival times hardly depend on the frequency in the shallows where the train runs.
		const auto& shore = snap->shore;
		const bool shoreReady = shore.enabled && snap->shoreField && shoreTextures.field == snap->shoreField;
		const bool blending = shoreReady && snap->shoreBlend < 1.0f && snap->shorePrevious && previousShoreTextures.field == snap->shorePrevious;
		d.Shore0 = { shoreReady ? 1.0f : 0.0f, blending ? snap->shoreBlend : 1.0f, 0.0f, shore.enabled ? shore.maxRunup : 0.0f };
		d.Shore1 = { settings.BreakingFoam, settings.SwashFoam, shore.swellHeight, shore.windSeaHeight };
		// Clocks now and as last frame drew them (motion vectors), last frame's moved onto this frame's field reference
		// when the field changed (as MainThreadUpdate moved the clocks); the first frame has no motion.
		float clocks[PBRWaterModel::ShoreComponents], lastClocks[PBRWaterModel::ShoreComponents];
		const bool consecutive = lastFrame != UINT32_MAX && frame == lastFrame + 1 && frameData.Shore2.w > 0.0f;
		const float* previousClocks = &frameData.Shore2.x;
		for (uint32_t c = 0; c < PBRWaterModel::ShoreComponents; ++c) {
			clocks[c] = static_cast<float>(shore.phase[c]);
			lastClocks[c] = consecutive ? previousClocks[c] : clocks[c];
			if (consecutive && drawnShore && shoreReady && drawnShore != snap->shoreField)
				lastClocks[c] += static_cast<float>(shore.componentOmega[c] * (drawnShore->referenceTime[c] - snap->shoreField->referenceTime[c]));
		}
		d.Shore2 = { clocks[0], clocks[1], clocks[2], shore.period };
		d.Shore3 = { lastClocks[0], lastClocks[1], lastClocks[2], shore.deepLength };
		d.Shore4 = { shore.componentOmega[0], shore.componentOmega[1], shore.componentOmega[2], 0.0f };
		auto fieldMap = [](const PBRWaterModel::ShoreField& f) {
			return float4{ f.minX - 0.5f * f.step, f.minY - 0.5f * f.step, 1.0f / (static_cast<float>(f.width) * f.step), 1.0f / (static_cast<float>(f.height) * f.step) };
		};
		if (shoreReady) {
			const auto& f = *snap->shoreField;
			d.ShoreMap = fieldMap(f);
			if (blending) {
				const auto& p = *snap->shorePrevious;
				d.Shore5 = { static_cast<float>(p.referenceTime[0] - f.referenceTime[0]), static_cast<float>(p.referenceTime[1] - f.referenceTime[1]), static_cast<float>(p.referenceTime[2] - f.referenceTime[2]), 0.0f };
				d.Shore6 = fieldMap(p);
			}
			// Last frame's wet grid, until this frame's step (UpdateSwash) replaces it.
			d.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), swash.IsReady() ? 1.0f : 0.0f };
		}
		drawnShore = shoreReady ? snap->shoreField : nullptr;

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
	// Exact terrain of the loaded cells (vertex heights); preferred over the heightmap where it covers.
	if (settings.UseBathymetry && uploadedLand && landTexture) {
		const auto& g = *uploadedLand;
		d.Land0 = { g.minX - 0.5f * g.stepX, g.minY - 0.5f * g.stepY, 1.0f / (g.width * g.stepX), 1.0f / (g.height * g.stepY) };
		d.Land1 = { 1.0f, g.stepX, 0.0f, 0.0f };
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
	d.Foam0 = { settings.ShoreFoamWidth * UnitsPerMetre, 0.0f, settings.FoamAmount, settings.FoamScale * UnitsPerMetre };
	// The foam animation only drifts noise slowly; wrapping once an hour keeps float precision.
	d.Foam1 = { settings.FoamPersistence, std::fmod(globals::state->timer, 3600.0f), settings.WakeFoam, settings.RiverWaveDamping };
	d.Foam2 = { settings.WhitecapAmount, settings.WhitecapScale * UnitsPerMetre, settings.WhitecapStreak, settings.BubbleAmount };
	if (snap)
		d.Foam3 = { snap->foamDrift[0], snap->foamDrift[1], snap->foamDrift[2], snap->foamDrift[3] };

	// Water clarity and the light in the water.
	d.Clarity0 = { settings.SedimentDensity / UnitsPerMetre, settings.Turbidity, settings.TurbidityPatchiness, settings.TurbidityPatchSize * UnitsPerMetre };
	d.Clarity1 = { settings.SedimentColor.x, settings.SedimentColor.y, settings.SedimentColor.z, settings.ShoreResuspension };
	d.Clarity2 = { settings.RiverTurbidity, weatherTurbidity.load(std::memory_order_acquire), settings.EnableRipples ? settings.WadingSilt : 0.0f, settings.SedimentLayerHeight * UnitsPerMetre };
	// Sediment plumes drift and reshape over hours; a daily wrap keeps their coordinates precise.
	d.Optics0 = { settings.ScatteringAnisotropy, settings.DownwellingAttenuation, std::fmod(globals::state->timer, 86400.0f), (snap && snap->ocean) ? snap->ocean->significantHeight : 0.0f };

	// Wind and rain on the surface.
	d.Surface0 = { settings.WindRoughness, settings.Gusts, settings.GustSize * UnitsPerMetre, rainIntensity.load(std::memory_order_acquire) };
	d.Surface1 = { settings.WindStreaks, 0.0f, settings.UseFetch ? settings.FetchScale : 1.0f, 0.0f };

	// Hulls near the camera stay dry inside: each one's frame as it moves this frame, and its maps.
	if (const auto masks = floating.GetHullMasks(); masks && settings.EnableFloatingObjects) {
		uint32_t count = 0;
		for (const auto& m : *masks) {
			const int slot = hullAtlas.FindSlot(m.shape.get());
			if (slot < 0 || count == FloatingObjects::MaxHullMasks)
				continue;
			const auto& s = *m.shape;
			const RE::NiPoint3 corner = m.origin + m.axisX * s.minX + m.axisY * s.minY;
			d.HullA[count] = { corner.x, corner.y, corner.z, static_cast<float>(slot) };
			d.HullB[count] = { m.axisX.x, m.axisX.y, m.axisX.z, static_cast<float>(HullMaskAtlas::Size) / (s.maxX - s.minX) };
			d.HullC[count] = { m.axisY.x, m.axisY.y, m.axisY.z, static_cast<float>(HullMaskAtlas::Size) / (s.maxY - s.minY) };
			++count;
		}
		d.Draw0.w = static_cast<float>(count);
	}

	// The worldspace flowmap calms waves on rivers, looked up by position in every water pass.
	worldFlowmap = nullptr;
	if (globals::features::unifiedWater.GetWorldFlowmap(worldFlowmap, d.Flow0) && !worldFlowmap)
		d.Flow0 = {};

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
	const bool hasWaves = snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled || settings.EnableRipples);

	ID3D11HullShader* hullShader = nullptr;
	ID3D11DomainShader* domainShader = nullptr;
	if (settings.EnableTessellation && customShadersLive && hasWaves && IsTessellatedTechnique(technique)) {
		hullShader = shaderCache->GetHullShader(*waterShader, vertexDescriptor);
		domainShader = shaderCache->GetDomainShader(*waterShader, vertexDescriptor);
		// Async compiles are queued on first request, so only a shader still missing well after the
		// compile queue drained is a real failure.
		if ((hullShader && domainShader) || shaderCache->IsCompiling()) {
			tessellationMissingSince = UINT32_MAX;
		} else if (tessellationMissingSince == UINT32_MAX) {
			tessellationMissingSince = state->frameCount;
		} else if (state->frameCount - tessellationMissingSince > 300 && !tessellationFailureLogged) {
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

	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	ID3D11SamplerState* sampler = linearClampSampler.get();
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	RestoreSamplers();
	context->VSGetSamplers(12, 2, savedVSSamplers.data());
	context->PSGetSamplers(12, 2, savedPSSamplers.data());
	samplersSaved = true;
	context->VSSetShaderResources(110, WaterSRVCount, srvs);
	context->PSSetShaderResources(110, WaterSRVCount, srvs);
	context->VSSetSamplers(12, 2, samplers);
	context->PSSetSamplers(12, 2, samplers);

	// The renderer binds textures and constant buffers lazily, right before the draw call, so the
	// device still holds the previous draw's state here: read this draw's from the shadow state.
	auto& shadow = globals::game::shadowState->GetRuntimeData();
	// That flush also re-binds every sampler slot the renderer has marked modified (Lighting uses 12-13),
	// which would replace ours with a point or wrap sampler depending on what was drawn before: drop those
	// pending updates for this draw. RestoreSamplers() marks them again so the next draw gets the game's.
	shadow.PSSamplerModifiedBits &= ~(3u << 12);

	// The vertex program reads the worldspace flowmap to calm waves on rivers.
	ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
	context->VSSetShaderResources(8, 1, &flowmap);
	context->VSSetSamplers(8, 1, &sampler);

	if (tessellate) {
		// The domain shader runs the full vertex program: give it everything the vertex shader has.
		ID3D11Buffer* vsBuffers[3] = {};
		if (const auto* vertexShader = shadow.currentVertexShader) {
			for (uint32_t i = 0; i < 3; ++i)
				vsBuffers[i] = reinterpret_cast<ID3D11Buffer*>(vertexShader->constantBuffers[i].buffer);
		}
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
		context->DSSetShaderResources(110, WaterSRVCount, srvs);
		context->HSSetShaderResources(110, WaterSRVCount, srvs);
		context->HSSetSamplers(12, 2, samplers);
		context->DSSetSamplers(12, 2, samplers);
		context->DSSetShaderResources(8, 1, &flowmap);
		context->DSSetSamplers(8, 1, &sampler);

		if (frameBuffer)
			frameBuffer->Release();

		// The renderer only re-applies its cached topology when it changes, so make sure its cache
		// says "triangle list" with nothing pending, then switch the device to patches behind it.
		// RestoreDraw() puts the device back in sync before anything else is drawn.
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
}

void PBRWater::RestoreSamplers()
{
	if (!samplersSaved)
		return;
	auto context = globals::d3d::context;
	context->VSSetSamplers(12, 2, savedVSSamplers.data());
	context->PSSetSamplers(12, 2, savedPSSamplers.data());
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= (3u << 12);
	for (auto* sampler : savedVSSamplers) {
		if (sampler)
			sampler->Release();
	}
	for (auto* sampler : savedPSSamplers) {
		if (sampler)
			sampler->Release();
	}
	savedVSSamplers = {};
	savedPSSamplers = {};
	samplersSaved = false;
}

void PBRWater::GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const
{
	srvs[0] = settings.EnableRipples ? ripples.GetSRV() : nullptr;
	srvs[1] = fetchTexture ? fetchTexture->srv.get() : nullptr;
	srvs[2] = (d.Terrain1.z > 0.5f && globals::features::terrainShadows.texHeightMap) ? globals::features::terrainShadows.texHeightMap->srv.get() : nullptr;
	srvs[3] = settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr;
	srvs[4] = (d.Land1.x > 0.5f && landTexture) ? landTexture->srv.get() : nullptr;
	srvs[5] = ocean.GetDisplacementSRV();
	srvs[6] = ocean.GetDerivativesSRV();
	srvs[7] = ocean.GetSurfaceSRV();
	srvs[8] = ocean.GetPreviousDisplacementSRV();
	const bool shore = d.Shore0.x > 0.5f && shoreTextures.field;
	srvs[9] = shore ? shoreTextures.times->srv.get() : nullptr;
	srvs[10] = shore ? shoreTextures.slowness->srv.get() : nullptr;
	srvs[11] = shore ? shoreTextures.beach->srv.get() : nullptr;
	srvs[12] = shore && d.Shore0.y < 1.0f && previousShoreTextures.field ? previousShoreTextures.times->srv.get() : nullptr;
	srvs[13] = shore && d.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	srvs[14] = d.Draw0.w > 0.5f ? hullAtlas.GetSRV() : nullptr;
}

void PBRWater::RestoreDraw()
{
	auto context = globals::d3d::context;
	RestoreSamplers();
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

PBRWaterModel::SpectrumParams PBRWater::CurrentSpectrumParams(float windSpeed, bool exterior) const
{
	PBRWaterModel::SpectrumParams params;
	params.windSpeed = windSpeed;
	params.windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	params.heightScale = settings.WaveHeight;
	params.choppiness = settings.Choppiness;
	params.spread = settings.DirectionalSpread;
	// Swell comes in from the open sea; there is none indoors.
	params.swellHeight = exterior ? settings.SwellHeight : 0.0f;
	params.swellPeriod = settings.SwellPeriod;
	params.swellDirection = params.windDirection + settings.SwellDirection * (TwoPi / 360.0f);
	params.resolution = WaveResolutionSize();
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
	const auto spectrum = std::make_shared<const PBRWaterModel::OceanSpectrum>(PBRWaterModel::GenerateSpectrum(CurrentSpectrumParams(smoothedWindSpeed, exterior)));
	next->ocean = spectrum;
	next->windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	next->exterior = exterior;

	// Surface (Stokes) drift carries foam and bubbles downwind at ~1.5% of the wind speed; bubbles
	// below the surface lag and wander a little across it.
	{
		constexpr double Wrap = 1048576.0;
		const double speed = smoothedWindSpeed * 0.015 * settings.FoamDrift * UnitsPerMetre;
		const double drift = speed * dt;
		next->foamDriftVelocity[0] = static_cast<float>(smoothedWindDirX * speed);
		next->foamDriftVelocity[1] = static_cast<float>(smoothedWindDirY * speed);
		foamDriftX = std::fmod(foamDriftX + smoothedWindDirX * drift, Wrap);
		foamDriftY = std::fmod(foamDriftY + smoothedWindDirY * drift, Wrap);
		bubbleDriftX = std::fmod(bubbleDriftX + (-smoothedWindDirY * 0.3 - smoothedWindDirX * 0.3) * drift, Wrap);
		bubbleDriftY = std::fmod(bubbleDriftY + (smoothedWindDirX * 0.3 - smoothedWindDirY * 0.3) * drift, Wrap);
		next->foamDrift[0] = static_cast<float>(foamDriftX);
		next->foamDrift[1] = static_cast<float>(foamDriftY);
		next->foamDrift[2] = static_cast<float>(bubbleDriftX);
		next->foamDrift[3] = static_cast<float>(bubbleDriftY);
	}

	// Shoreline waves: the sea's dominant waves as a wave train, its phases carried on from last frame (they keep
	// running while shore waves are off, so turning them on never starts every beach in step).
	shoreState = PBRWaterModel::UpdateShoreParams(*spectrum, hasShoreState ? &shoreState : nullptr, dt);
	hasShoreState = true;
	const bool shoreEnabled = exterior && settings.EnableShoreWaves && settings.UseBathymetry;

	const auto eye = Util::GetMainThreadEyePosition();
	clock.Advance(dt, eye.x, eye.y, eye.z);
	clock.Fill(*next);

	// Gameplay follows the long-wave cascades through the CPU mirror, but only while the GPU renders them.
	if (oceanReady.load(std::memory_order_acquire)) {
		mirror.Update(spectrum, clock.Time());
		next->mirror0 = mirror.frame0;
		next->mirror1 = mirror.frame1;
		next->mirrorAlpha = mirror.alpha;
	} else {
		mirror.Reset();
	}

	environment.Update(exterior ? tes->GetRuntimeData2().worldSpace : nullptr);
	environment.UpdateLoadedLand(exterior && settings.UseBathymetry);
	if (settings.UseFetch)
		next->fetch = environment.GetFetch();
	next->fetchScale = settings.FetchScale;
	if (settings.UseBathymetry)
		next->bathymetry = environment.GetBathymetry();
	if (settings.UseBathymetry)
		next->land = environment.GetLand();
	environment.UpdateShore(eye.x, eye.y, shoreState, settings.UseFetch ? environment.GetFetch() : nullptr, settings.FetchScale, shoreEnabled);
	UpdateShoreField(dt, shoreEnabled);
	next->shore = shoreState;
	next->shore.enabled = shoreEnabled;
	next->shoreField = currentShore;
	next->shorePrevious = previousShore;
	next->shoreBlend = shoreBlend;

	std::shared_ptr<const PBRWaterModel::WaveSnapshot> published = next;
	snapshot.store(published, std::memory_order_release);
	renderDelta.store(dt, std::memory_order_release);
	UpdateCameraWater(*published, exterior);

	// Storms and rain cloud the water over minutes, and it takes as long to clear.
	smoothedWeatherTurbidity += (WeatherTurbidityTarget() - smoothedWeatherTurbidity) * SmoothFactor(dt, 90.0f);
	weatherTurbidity.store(exterior ? smoothedWeatherTurbidity : 0.0f, std::memory_order_release);
	// Rain on the surface follows the precipitation within seconds.
	smoothedRain += ((exterior ? RainFraction() : 0.0f) - smoothedRain) * SmoothFactor(dt, 4.0f);
	rainIntensity.store(smoothedRain, std::memory_order_release);

	GatherInteractions(*published, dt);

	FloatingObjects::Settings floatingSettings;
	floatingSettings.enabled = settings.EnableFloatingObjects && globals::shaderCache->IsEnabled();
	floatingSettings.range = settings.FloatingRange * UnitsPerMetre;
	floatingSettings.maxSize = settings.MaxFloatingSize * UnitsPerMetre;
	floatingSettings.response = settings.FloatingResponse;
	floatingSettings.carryActors = settings.CarryActors;
	floatingSettings.fadeStart = settings.DisplacementFadeStart;
	floatingSettings.fadeEnd = settings.DisplacementFadeEnd;
	floating.Update(*published, floatingSettings, dt);
}

void PBRWater::UpdateShoreField(float dt, bool enabled)
{
	// Seconds a new shore field takes to take over: long enough that a sea turning or a field moving with the player
	// only makes the crests drift a little faster or slower for a moment, never jump.
	constexpr float BlendTime = 4.0f;
	const auto latest = enabled ? environment.GetShore() : nullptr;
	if (latest != currentShore) {
		if (latest && currentShore) {
			// The clocks follow the new field's reference times, so a crest's phase omega (t - T) runs on unchanged.
			for (uint32_t c = 0; c < PBRWaterModel::ShoreComponents; ++c) {
				const double phase = shoreState.phase[c] + shoreState.componentOmega[c] * (currentShore->referenceTime[c] - latest->referenceTime[c]);
				shoreState.phase[c] = phase - PBRWaterModel::TwoPi * std::floor(phase / PBRWaterModel::TwoPi);
			}
			previousShore = currentShore;
			shoreBlend = 0.0f;
		} else {
			previousShore = nullptr;
			shoreBlend = 1.0f;
		}
		currentShore = latest;
	}
	shoreBlend = std::min(shoreBlend + dt / BlendTime, 1.0f);
	if (shoreBlend >= 1.0f)
		previousShore = nullptr;
}

float PBRWater::WeatherTurbidityTarget() const
{
	const auto sky = globals::game::sky;
	if (!sky)
		return 0.0f;
	// Strong wind lifts sediment in the shallows and mixes it through; rain washes silt in from the land.
	const float wind = std::clamp((smoothedWindSpeed - 6.0f) / 14.0f, 0.0f, 1.0f);
	return settings.StormTurbidity * (wind + 0.5f * RainFraction());
}

float PBRWater::RainFraction() const
{
	const auto sky = globals::game::sky;
	if (!sky)
		return 0.0f;
	auto rainy = [](const RE::TESWeather* weather) {
		return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) ? 1.0f : 0.0f;
	};
	const float blend = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	return rainy(sky->currentWeather) * blend + rainy(sky->lastWeather) * (1.0f - blend);
}

void PBRWater::UpdateCameraWater(const PBRWaterModel::WaveSnapshot& snap, bool exterior)
{
	cameraSurfaceValid.store(false, std::memory_order_release);
	if (!settings.UnderwaterFollowsWaves || !exterior || !globals::shaderCache->IsEnabled())
		return;
	auto* tes = globals::game::tes;
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!tes || !waterSystem || !waterSystem->currentWaterType || waterSystem->waterObjects.empty())
		return;
	const auto eye = Util::GetMainThreadEyePosition();
	auto* cell = tes->GetCell(eye);
	float flat = 0.0f;
	if (!cell || !TESObjectCELL_GetWaterHeight::func(cell, eye, flat) || flat <= -1e6f)
		return;
	// Only within reach of the waves: further away the game's own state is right.
	if (std::abs(eye.z - flat) > snap.VerticalBound() + 32.0f)
		return;
	// The water level the camera is under is the displaced surface above it, not the flat plane: the underwater
	// view and the waterline across the lens follow the waves.
	const float surface = flat + snap.SampleAt(eye.x, eye.y, flat).height;
	cameraSurface.store(surface, std::memory_order_release);
	cameraSurfaceFrame.store(globals::state->frameCount, std::memory_order_release);
	cameraSurfaceValid.store(true, std::memory_order_release);
	waterSystem->underwaterHeight = surface;
	waterSystem->playerUnderwater = eye.z < surface;
}

void PBRWater::ApplyCameraWater()
{
	if (!cameraSurfaceValid.load(std::memory_order_acquire))
		return;
	// Only this frame's state, and only while the game has water to be under: during a load the main thread stops
	// updating while the world still renders, and the engine's underwater effects then expect a water object that a
	// stale "under water" would make them look for.
	if (globals::state->frameCount - cameraSurfaceFrame.load(std::memory_order_acquire) > 1)
		return;
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!waterSystem || !waterSystem->currentWaterType || waterSystem->waterObjects.empty())
		return;
	// Again right before the world is drawn, in case the game refreshed it after the main update.
	const float surface = cameraSurface.load(std::memory_order_acquire);
	const auto eye = Util::GetEyePosition();
	waterSystem->underwaterHeight = surface;
	waterSystem->playerUnderwater = eye.z < surface;
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
	const auto eye = Util::GetMainThreadEyePosition();
	const float range = settings.RippleExtent * UnitsPerMetre * 0.5f;
	const float rangeSq = range * range;

	std::vector<RippleSimulation::Source> sources;
	sources.reserve(RippleSimulation::MaxSources);
	std::unordered_map<const void*, RE::NiPoint3> waveVelocities;

	// The water at a position: flat plane height and the displaced surface above it, or false when there is none.
	auto surfaceAt = [&](RE::TESObjectREFR* ref, const RE::NiPoint3& pos, float& flatZ, PBRWaterModel::WaveSnapshot::Sample& wave) {
		auto parentCell = ref->GetParentCell();
		if (!parentCell)
			return false;
		float h = 0.0f;
		if (!TESObjectCELL_GetWaterHeight::func(parentCell, pos, h) || h <= -1e6f)
			return false;
		flatZ = h;
		wave = snap.SampleAt(pos.x, pos.y, h);
		return true;
	};

	// The ripple simulation lives in the water's rest (Lagrangian) coordinates and the shaders sample it at
	// undisplaced positions, so a body is placed where the water under it rests: its rings and foam then
	// ride the waves with the water instead of sliding across them. Its motion is measured relative to the
	// water too, so a body bobbing with the waves makes no ripples, while one moving through them does.
	std::unordered_map<const void*, SourceTrack> tracks;
	const float splashFoam = settings.SplashFoam;
	auto addSource = [&](const void* key, const RE::NiPoint3& center, float radius, float flatZ, const PBRWaterModel::WaveSnapshot::Sample& wave, const RE::NiPoint3& anchor) {
		if (!wantRipples || sources.size() >= RippleSimulation::MaxSources)
			return;
		const float surfaceZ = flatZ + wave.height;
		// The waves move the water sideways by (anchor - origin); a limb is offset from the anchor by far less
		// than a wavelength, so it shares that displacement.
		const double originX = wave.originX + (center.x - anchor.x);
		const double originY = wave.originY + (center.y - anchor.y);
		float offset = center.z - surfaceZ;
		tracks[key] = { originX, originY, offset };
		const auto previous = lastSources.find(key);
		if (previous == lastSources.end() || dt <= 0.0f)
			return;
		// A fast fall can carry a body through the surface between two frames: that still hits the water.
		const bool crossed = previous->second.offset > 0.0f && offset < 0.0f;
		if (std::abs(offset) >= radius && !crossed)
			return;
		const float moveX = static_cast<float>(originX - previous->second.originX);
		const float moveY = static_cast<float>(originY - previous->second.originY);
		const float sink = previous->second.offset - offset;  // > 0: moving down into the water
		const float speed = std::sqrt(moveX * moveX + moveY * moveY + sink * sink) / dt;
		const float motion = std::clamp(speed / (1.5f * UnitsPerMetre), 0.0f, 1.0f);
		// A splash: hitting the water fast from above digs a crater and throws up whitewater.
		const float entry = std::clamp(sink / dt / (3.0f * UnitsPerMetre), 0.0f, 1.0f);
		if (motion <= 0.01f && entry <= 0.0f)
			return;
		// Footprint of the body where it cuts the surface, and how much of it is under water.
		offset = std::clamp(offset, -radius, radius);
		const float footprint = std::max(std::sqrt(std::max(radius * radius - offset * offset, 0.0f)), crossed ? 0.7f * radius : 0.0f);
		const float submerged = std::clamp(0.5f - offset / (2.0f * radius), 0.0f, 1.0f);
		const float size = std::clamp(footprint / 24.0f, 0.25f, 1.0f);
		RippleSimulation::Source s;
		s.x = static_cast<float>(originX);
		s.y = static_cast<float>(originY);
		s.radius = footprint;
		s.depth = submerged * size * motion + entry * size;
		// Air churned into the water: anything moving through the surface leaves a cloud of bubbles around
		// it, an impact a burst (per simulation step).
		s.foam = splashFoam * size * (0.3f * motion + 0.8f * entry);
		sources.push_back(s);
	};

	// Silt kicked up by feet moving over the bed, released at the start of the next simulation steps.
	const float siltRate = settings.WadingSilt > 0.0f ? 0.06f : 0.0f;
	auto addSilt = [&](double x, double y, float amount) {
		if (sources.size() >= RippleSimulation::MaxSources || amount <= 0.0f)
			return;
		RippleSimulation::Source s;
		s.x = static_cast<float>(x);
		s.y = static_cast<float>(y);
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
			float flatZ;
			PBRWaterModel::WaveSnapshot::Sample wave;
			if (!surfaceAt(actor, pos, flatZ, wave))
				return;
			// Wading (feet on the bed, under water): moving feet stir the bottom up.
			if (siltRate > 0.0f && pos.z < flatZ + wave.height - 2.0f) {
				const auto state = actor->AsActorState();
				if (state && !state->IsSwimming()) {
					RE::NiPoint3 velocity;
					actor->GetLinearVelocity(velocity);
					const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y) / UnitsPerMetre;
					addSilt(wave.originX, wave.originY, siltRate * std::clamp(speed, 0.0f, 1.5f));
				}
			}
			auto root = actor->Get3D(false);
			if (!root)
				return;
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 center;
				float radius;
				if (Util::GetShapeBound(object, center, radius))
					addSource(object, center, radius, flatZ, wave, pos);
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

			float flatZ;
			PBRWaterModel::WaveSnapshot::Sample wave;
			if (!surfaceAt(ref, center, flatZ, wave))
				return RE::BSContainer::ForEachResult::kContinue;

			// Only bodies floating at the surface; sunken or airborne ones are left alone.
			if (std::abs(center.z - (flatZ + wave.height)) > radius * 1.5f)
				return RE::BSContainer::ForEachResult::kContinue;

			float velocity[4];
			_mm_storeu_ps(velocity, body->motion.linearVelocity.quad);

			if (wantRipples && settings.PhysicsObjectRipples)
				addSource(niCollision, center, radius, flatZ, wave, center);

			if (wantBuoyancy && dt > 0.0f) {
				// The engine's own buoyancy floats objects on the flat plane (and lets heavy ones sink).
				// Add only the water's orbital motion, as a change in velocity relative to what was added
				// last frame: floating objects ride the waves, sinking ones keep sinking, and nothing is
				// pinned to the surface.
				const float strength = std::clamp(settings.BuoyancyStrength, 0.0f, 3.0f) / 3.0f;
				const RE::NiPoint3 target{ wave.velocityX * strength, wave.velocityY * strength, wave.verticalVelocity * strength };
				RE::NiPoint3 previous{};
				if (const auto it = lastWaveVelocities.find(bhkBody); it != lastWaveVelocities.end())
					previous = it->second;
				waveVelocities[bhkBody] = target;
				const float worldScale = RE::bhkWorld::GetWorldScale();
				velocity[0] += (target.x - previous.x) * worldScale;
				velocity[1] += (target.y - previous.y) * worldScale;
				velocity[2] += (target.z - previous.z) * worldScale;
				bhkBody->SetLinearVelocity(RE::hkVector4(velocity[0], velocity[1], velocity[2], 0.0f));
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	lastSources = std::move(tracks);
	lastWaveVelocities = std::move(waveVelocities);
	if (wantRipples)
		ripples.SubmitSources(std::move(sources));
}

// ============================================================================
// Hooks
// ============================================================================

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

void PBRWater::GameLoaded()
{
	// The loaded game's references have new 3D; whatever floated before belongs to the old one.
	floating.Reset();
	cameraSurfaceValid.store(false, std::memory_order_release);
}

void PBRWater::SavingGame()
{
	floating.RestorePlacements();
}

void PBRWater::MainUpdate::thunk()
{
	func();
	globals::features::pbrWater.MainThreadUpdate();
}

#undef I18N_KEY_PREFIX
