#include "PBRWater.h"
#include "PBRWater/EngineWater.h"
#include "PBRWater/PBRWaterInternal.h"
#include "PBRWater/Profiling.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/TerrainShadows.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricShadows.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/DebugSection.h"
#include "Utils/UI.h"
#include "Utils/VersionedRelocation.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#define I18N_KEY_PREFIX "feature.pbr_water."

using namespace PBRWaterDetail;

// Every setting saved as itself, in the struct's order; the spray emitters are merged over their defaults (from_json).
// clang-format off
#define PBRW_SETTINGS_FIELDS(X) \
	X(WindSpeedCalm) X(WindSpeedStorm) X(EnableOceanWaves) X(WaveHeight) X(SwellHeight) X(SwellPeriod) \
	X(SwellDirection) X(SwellPeakedness) X(SwellSpread) X(Choppiness) X(DirectionalSpread) X(FetchScale) \
	X(EnableShoreWaves) X(WetShores) X(SurfQuality) X(ShoreShortestPeriod) X(ShoreLongestPeriod) X(ShoreTrainCycles) \
	X(ShoreWaveVariation) X(ShoreCrestWobble) X(ShoreBoreWhelps) X(ShoreCurl) X(SwashLinger) X(SwashChurnHeight) \
	X(SwashChurnDepth) X(SwashChurnSize) X(SwashChurnSpeed) \
	X(EnableWaveShadows) X(EnablePointLightSpecular) X(EnableSkyOcclusion) X(EnableEnvReflections) X(EnableSSR) X(EnableSunShadows) \
	X(EnableSkylighting) X(EnableFog) X(BatchedStencil) X(EnableWaterline) X(Roughness) X(SunSpecular) \
	X(PointLightSpecular) X(PointLightEmitterRadius) X(EnableSubsurface) X(Subsurface) X(RefractionDistortion) X(WindRoughness) \
	X(VanillaNormalMaps) X(DetailNormalCalm) X(DetailNormalGale) \
	X(EnableClarity) X(Visibility) X(Turbidity) X(SedimentColor) X(SedimentDensity) X(TurbidityPatchiness) \
	X(TurbidityPatchSize) X(ShoreResuspension) X(StormTurbidity) X(WadingSilt) X(SedimentLayerHeight) X(SiltHalfLife) \
	X(ScatteringAnisotropy) X(DownwellingAttenuation) \
	X(EnableFoam) X(FoamAmount) X(FoamBias) X(FoamDecayRate) X(FoamFadeDistance) X(FoamBubbleSize) \
	X(EnableSpray) X(SprayAmount) X(SprayRange) X(SprayQuality) \
	X(EnableRipples) X(EnableWakes) X(UnderwaterVolume) X(LightShaftSamples) X(PhysicalBodyColour) X(LightShaftFocus) \
	X(WakeStrength) X(RippleExtent) X(RippleHeight) X(RippleNormalStrength) X(RippleHalfLife) X(RippleHorizDisplace) \
	X(RippleDisplaceClamp) \
	X(GameplayWaves) X(EnableFloatingObjects) X(CarryActors) X(HullMasks) X(BuoyancyStrength) X(FloatingRange) \
	X(MaxFloatingSize) X(FloatingResponse) \
	X(EnableTessellation) X(DeferredSurface) X(DistantHalfRate) X(DistantHalfRateDistance) X(GridCellSize) X(DisplacementFadeStart) \
	X(DisplacementFadeEnd) X(WaveResolution) X(CheapOffscreenWater) X(EngineReflections) X(LongWaveHalfRate) \
	X(WireframeMode) X(DebugView)
// clang-format on

void to_json(nlohmann::json& j, const PBRWater::Settings& s)
{
#define PBRW_TO_JSON(field) j[#field] = s.field;
	PBRW_SETTINGS_FIELDS(PBRW_TO_JSON)
#undef PBRW_TO_JSON
	j["SprayEmitters"] = s.SprayEmitters;
}

void from_json(const nlohmann::json& j, PBRWater::Settings& s)
{
	const PBRWater::Settings defaults{};
#define PBRW_FROM_JSON(field) s.field = j.value(#field, defaults.field);
	PBRW_SETTINGS_FIELDS(PBRW_FROM_JSON)
#undef PBRW_FROM_JSON
	// Emitter by emitter over the defaults: a file with fewer, or with missing values, keeps the rest. An emitter saved for
	// another layout or emission (SprayParticles::EmitterVersion, stored in each) starts over from its default.
	s.SprayEmitters = SprayParticles::DefaultEmitters();
	if (const auto it = j.find("SprayEmitters"); it != j.end() && it->is_array()) {
		for (size_t i = 0; i < std::min<size_t>(it->size(), SprayParticles::EmitterCount); ++i) {
			const auto& saved = (*it)[i];
			const auto version = saved.is_object() ? saved.find("Version") : saved.end();
			if (version != saved.end() && version->is_number_integer() && version->get<int>() == SprayParticles::EmitterVersion)
				from_json(saved, s.SprayEmitters[i]);
		}
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
	// Safety bounds only, not the sliders' ranges: Ctrl+click may type any value past a slider's end and it must stick
	// (this runs every frame). Only what would break the maths is held back: non-finite values, divisors and sizes that
	// must stay positive, factors that only mean something in 0..1, and the grid's limits.
	auto& s = settings;
	constexpr float Huge = 1.0e6f;
	auto clamp = [](float& value, float lo, float hi, float fallback) {
		value = std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
	};
	static const Settings d{};  // the defaults, built once (this runs every frame)
	clamp(s.GridCellSize, 2.0f, 64.0f, d.GridCellSize);
	clamp(s.WakeStrength, 0.0f, 10.0f, d.WakeStrength);
	s.LightShaftSamples = std::clamp(s.LightShaftSamples, 0, 64);
	clamp(s.LightShaftFocus, 0.0f, 4.0f, d.LightShaftFocus);
	clamp(s.DistantHalfRateDistance, 1.0f, Huge, d.DistantHalfRateDistance);
	clamp(s.WindSpeedCalm, 0.0f, Huge, d.WindSpeedCalm);
	clamp(s.WindSpeedStorm, 0.1f, Huge, d.WindSpeedStorm);
	clamp(s.WaveHeight, 0.0f, Huge, d.WaveHeight);
	clamp(s.SwellHeight, 0.0f, Huge, d.SwellHeight);
	clamp(s.SwellPeriod, 0.5f, Huge, d.SwellPeriod);
	clamp(s.SwellDirection, -Huge, Huge, d.SwellDirection);
	clamp(s.SwellPeakedness, 1.0f, 100.0f, d.SwellPeakedness);
	clamp(s.SwellSpread, 1.0f, 120.0f, d.SwellSpread);
	clamp(s.Choppiness, 0.0f, 10.0f, d.Choppiness);
	clamp(s.DirectionalSpread, 0.0f, 1.0f, d.DirectionalSpread);
	clamp(s.FetchScale, 1.0f, 1000.0f, d.FetchScale);
	clamp(s.ShoreShortestPeriod, 0.5f, 60.0f, d.ShoreShortestPeriod);
	clamp(s.ShoreLongestPeriod, 0.5f, 120.0f, d.ShoreLongestPeriod);
	clamp(s.ShoreTrainCycles, 1.0f, 64.0f, d.ShoreTrainCycles);
	clamp(s.ShoreWaveVariation, 0.0f, 1.5f, d.ShoreWaveVariation);
	clamp(s.ShoreCrestWobble, 0.0f, 2.0f, d.ShoreCrestWobble);
	clamp(s.ShoreBoreWhelps, 0.0f, 2.0f, d.ShoreBoreWhelps);
	clamp(s.ShoreCurl, 0.0f, 2.0f, d.ShoreCurl);
	clamp(s.SwashLinger, 1.0f, 8.0f, d.SwashLinger);
	clamp(s.SwashChurnHeight, 0.0f, 0.5f, d.SwashChurnHeight);
	clamp(s.SwashChurnDepth, 0.0f, 1.0f, d.SwashChurnDepth);
	clamp(s.SwashChurnSize, 0.05f, 3.0f, d.SwashChurnSize);
	clamp(s.SwashChurnSpeed, 0.0f, 10.0f, d.SwashChurnSpeed);
	clamp(s.Roughness, 0.001f, 1.0f, d.Roughness);
	clamp(s.SunSpecular, 0.0f, Huge, d.SunSpecular);
	clamp(s.PointLightSpecular, 0.0f, Huge, d.PointLightSpecular);
	clamp(s.PointLightEmitterRadius, 0.1f, 1000.0f, d.PointLightEmitterRadius);
	clamp(s.Subsurface, 0.0f, Huge, d.Subsurface);
	clamp(s.RefractionDistortion, 0.0f, Huge, d.RefractionDistortion);
	clamp(s.WindRoughness, 0.0f, Huge, d.WindRoughness);
	clamp(s.DetailNormalCalm, 0.0f, 10.0f, d.DetailNormalCalm);
	clamp(s.DetailNormalGale, 0.0f, 10.0f, d.DetailNormalGale);
	clamp(s.Visibility, 0.001f, Huge, d.Visibility);
	clamp(s.Turbidity, 0.0f, Huge, d.Turbidity);
	clamp(s.SedimentDensity, 0.0f, Huge, d.SedimentDensity);
	clamp(s.TurbidityPatchiness, 0.0f, 1.0f, d.TurbidityPatchiness);
	clamp(s.TurbidityPatchSize, 1.0f, 100000.0f, d.TurbidityPatchSize);
	clamp(s.ShoreResuspension, 0.0f, Huge, d.ShoreResuspension);
	clamp(s.StormTurbidity, 0.0f, Huge, d.StormTurbidity);
	clamp(s.WadingSilt, 0.0f, Huge, d.WadingSilt);
	clamp(s.SedimentLayerHeight, 0.01f, 1000.0f, d.SedimentLayerHeight);
	clamp(s.SiltHalfLife, 0.5f, Huge, d.SiltHalfLife);
	clamp(s.ScatteringAnisotropy, -0.99f, 0.99f, d.ScatteringAnisotropy);
	clamp(s.DownwellingAttenuation, 0.0f, Huge, d.DownwellingAttenuation);
	clamp(s.FoamAmount, 0.0f, Huge, d.FoamAmount);
	clamp(s.FoamBias, -100.0f, 100.0f, d.FoamBias);
	clamp(s.FoamDecayRate, 0.0f, Huge, d.FoamDecayRate);
	clamp(s.FoamFadeDistance, 1.0f, 1000000.0f, d.FoamFadeDistance);
	clamp(s.FoamBubbleSize, 0.01f, 100.0f, d.FoamBubbleSize);
	clamp(s.SprayAmount, 0.0f, 100.0f, d.SprayAmount);
	clamp(s.SprayRange, 16.0f, 2000.0f, d.SprayRange);
	for (auto& emitter : s.SprayEmitters)
		SprayParticles::Sanitize(emitter);
	clamp(s.RippleExtent, 4.0f, 2000.0f, d.RippleExtent);
	clamp(s.RippleHeight, 0.0f, 5.0f, d.RippleHeight);
	clamp(s.RippleNormalStrength, 0.0f, 100.0f, d.RippleNormalStrength);
	clamp(s.RippleHalfLife, 0.05f, Huge, d.RippleHalfLife);
	clamp(s.RippleHorizDisplace, 0.0f, 100.0f, d.RippleHorizDisplace);
	clamp(s.RippleDisplaceClamp, 0.0f, 1.0f, d.RippleDisplaceClamp);
	clamp(s.BuoyancyStrength, 0.0f, Huge, d.BuoyancyStrength);
	clamp(s.FloatingRange, 1.0f, 10000.0f, d.FloatingRange);
	clamp(s.MaxFloatingSize, 1.0f, 10000.0f, d.MaxFloatingSize);
	clamp(s.FloatingResponse, 0.0f, 100.0f, d.FloatingResponse);
	clamp(s.DisplacementFadeStart, 0.0f, 10000000.0f, d.DisplacementFadeStart);
	clamp(s.DisplacementFadeEnd, 1.0f, 10000000.0f, d.DisplacementFadeEnd);
	clamp(s.SedimentColor.x, 0.0f, Huge, d.SedimentColor.x);
	clamp(s.SedimentColor.y, 0.0f, Huge, d.SedimentColor.y);
	clamp(s.SedimentColor.z, 0.0f, Huge, d.SedimentColor.z);
	// Ranges whose ends depend on each other.
	s.ShoreLongestPeriod = std::max(s.ShoreLongestPeriod, s.ShoreShortestPeriod);
	s.DisplacementFadeEnd = std::max(s.DisplacementFadeEnd, s.DisplacementFadeStart + 1.0f);
	s.WaveResolution = std::clamp(s.WaveResolution, 0, 2);
	s.SprayQuality = std::clamp(s.SprayQuality, 0, 2);
	s.SurfQuality = std::clamp(s.SurfQuality, 0, 2);
	s.WireframeMode = std::clamp(s.WireframeMode, 0, 2);
	s.DebugView = std::clamp(s.DebugView, 0, 14);
	s.EngineReflections = std::clamp(s.EngineReflections, 0, 2);
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::DrawSpraySettings()
{
	auto& s = settings;
	Util::CheckboxTip(T(TKEY("enable_spray"), "Sea Spray"), &s.EnableSpray,
		T(TKEY("enable_spray_tooltip"), "Breakers throw splashes, droplets and mist into the air, and the open sea's whitecaps throw spray that a strong wind tears off the crests. Drawn with the game's own effect textures."));
	if (!s.EnableSpray)
		return;
	Util::SliderFloatTip(T(TKEY("spray_amount"), "Spray Amount"), &s.SprayAmount, 0.0f, 3.0f, "%.2f",
		T(TKEY("spray_amount_tooltip"), "Scales how many particles every emitter makes. Default 1."));
	Util::SliderFloatTip(T(TKEY("spray_range"), "Spray Range"), &s.SprayRange, 32.0f, 512.0f, "%.0f m",
		T(TKEY("spray_range_tooltip"), "Distance from the camera within which the waves throw spray. A larger range samples the waves more coarsely. Default 128 m."), ImGuiSliderFlags_Logarithmic);
	{
		const char* qualities[] = { T(TKEY("spray_quality_low"), "Low (16k particles)"), T(TKEY("spray_quality_medium"), "Medium (64k particles)"), T(TKEY("spray_quality_high"), "High (128k particles)") };
		ImGui::Combo(T(TKEY("spray_quality"), "Spray Particles"), &s.SprayQuality, qualities, IM_ARRAYSIZE(qualities));
		Util::HoverTip(T(TKEY("spray_quality_tooltip"), "The most spray particles alive at once. When a storm asks for more, the oldest go first."));
	}
	if (Util::Button(T(TKEY("spray_reset_all"), "Reset All Emitters")))
		s.SprayEmitters = SprayParticles::DefaultEmitters();

	ImGui::SeparatorText(T(TKEY("spray_emitters"), "Emitters"));
	const char* sources[] = {
		T(TKEY("spray_source_plunge"), "Plunging breaker"),
		T(TKEY("spray_source_roller"), "Broken wave's roller"),
		T(TKEY("spray_source_surf"), "Surf on the beach"),
		T(TKEY("spray_source_whitecap"), "Open sea whitecaps"),
		T(TKEY("spray_source_spindrift"), "Spindrift (strong wind)"),
	};
	const char* shapes[] = {
		T(TKEY("spray_shape_droplets"), "Droplets (motion streaks)"),
		T(TKEY("spray_shape_puffs"), "Puffs (spray and mist)"),
	};
	const char* names[SprayParticles::EmitterCount] = {
		T(TKEY("spray_emitter_breaker_sheet"), "Breaker Sheet"),
		T(TKEY("spray_emitter_breaker_droplets"), "Breaker Droplets"),
		T(TKEY("spray_emitter_breaker_mist"), "Breaker Mist"),
		T(TKEY("spray_emitter_roller_spray"), "Roller Spray"),
		T(TKEY("spray_emitter_roller_mist"), "Roller Mist"),
		T(TKEY("spray_emitter_surf_droplets"), "Surf Droplets"),
		T(TKEY("spray_emitter_whitecap_spray"), "Whitecap Spray"),
		T(TKEY("spray_emitter_spindrift"), "Spindrift"),
	};
	const auto defaults = SprayParticles::DefaultEmitters();
	for (uint32_t i = 0; i < SprayParticles::EmitterCount; ++i) {
		auto& e = s.SprayEmitters[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::Checkbox("##enabled", &e.Enabled);
		ImGui::SameLine();
		if (ImGui::CollapsingHeader(names[i])) {
			ImGui::Combo(T(TKEY("spray_source"), "Source"), &e.SourceKind, sources, IM_ARRAYSIZE(sources));
			Util::HoverTip(T(TKEY("spray_source_tooltip"), "What makes this emitter's particles: the splash-up where a plunging breaker's lip lands, the churning front of a broken wave, the white water running up the beach, the open sea's breaking crests, or the spray a gale tears off them (from 9 m/s of wind)."));
			ImGui::Combo(T(TKEY("spray_shape"), "Shape"), &e.ShapeKind, shapes, IM_ARRAYSIZE(shapes));
			Util::HoverTip(T(TKEY("spray_shape_tooltip"), "Droplets are drawn as the streak each one makes while it flies, at least a pixel wide. Puffs are soft blobs broken up by noise that tears them apart as they age: the spray sheet, fine spray and mist."));
			if (e.ShapeKind == SprayParticles::Puffs) {
				Util::SliderFloatTip(T(TKEY("spray_softness"), "Softness"), &e.Softness, 0.02f, 1.0f, "%.2f",
					T(TKEY("spray_softness_tooltip"), "How soft a puff's edges are: low for torn sheets of spray, high for mist."));
				Util::SliderFloatTip(T(TKEY("spray_detail"), "Break-up"), &e.Detail, 0.0f, 1.0f, "%.2f",
					T(TKEY("spray_detail_tooltip"), "How much noise breaks a puff up, and how far it tears apart as it ages."));
			}

			ImGui::SeparatorText(T(TKEY("spray_emission"), "Emission"));
			Util::SliderFloatTip(T(TKEY("spray_rate"), "Rate"), &e.Rate, 0.0f, 200.0f, "%.2f /m2/s",
				T(TKEY("spray_rate_tooltip"), "Particles per square metre per second where the source is at full strength."), ImGuiSliderFlags_Logarithmic);
			Util::SliderFloatTip(T(TKEY("spray_strength_min"), "Threshold"), &e.StrengthMin, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_strength_min_tooltip"), "Source strength below which the emitter makes nothing."));
			Util::SliderFloatTip(T(TKEY("spray_strength_full"), "Full Strength"), &e.StrengthFull, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_strength_full_tooltip"), "Source strength at which the emitter makes its full rate."));
			Util::SliderFloatTip(T(TKEY("spray_max_distance"), "Max Distance"), &e.MaxDistance, 10.0f, 1000.0f, "%.0f m",
				T(TKEY("spray_max_distance_tooltip"), "The farthest from the camera these particles are made and drawn."), ImGuiSliderFlags_Logarithmic);

			ImGui::SeparatorText(T(TKEY("spray_launch"), "Launch"));
			Util::SliderFloatTip(T(TKEY("spray_speed"), "Speed"), &e.Speed, 0.0f, 15.0f, "%.2f m/s",
				T(TKEY("spray_speed_tooltip"), "Launch speed. For shore sources this is for a 1 m wave; it grows with the square root of the wave's height."));
			Util::SliderFloatTip(T(TKEY("spray_speed_variation"), "Speed Variation"), &e.SpeedVariation, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_speed_variation_tooltip"), "Random share of the speed, either way."));
			Util::SliderFloatTip(T(TKEY("spray_declination"), "Declination"), &e.Declination, 0.0f, 120.0f, "%.0f deg",
				T(TKEY("spray_declination_tooltip"), "Angle from straight up, tilted towards the waves' travel (out at sea, the wind)."));
			Util::SliderFloatTip(T(TKEY("spray_declination_variation"), "Declination Variation"), &e.DeclinationVariation, 0.0f, 90.0f, "%.0f deg",
				T(TKEY("spray_declination_variation_tooltip"), "Random spread of the declination, either way."));
			Util::SliderFloatTip(T(TKEY("spray_planar_spread"), "Sideways Spread"), &e.PlanarSpread, 0.0f, 180.0f, "%.0f deg",
				T(TKEY("spray_planar_spread_tooltip"), "How far either side of the waves' travel the particles may fly."));
			Util::SliderFloatTip(T(TKEY("spray_inherit_wave"), "Wave Speed Carried"), &e.InheritWave, 0.0f, 1.5f, "%.2f",
				T(TKEY("spray_inherit_wave_tooltip"), "Share of the wave's own speed the particles keep along its travel."));
			Util::SliderFloatTip(T(TKEY("spray_inherit_wind"), "Wind Carried"), &e.InheritWind, 0.0f, 1.5f, "%.2f",
				T(TKEY("spray_inherit_wind_tooltip"), "Share of the wind's speed the particles start with."));
			Util::SliderFloatTip(T(TKEY("spray_start_height"), "Start Height"), &e.StartHeight, -0.5f, 2.0f, "%.2f m",
				T(TKEY("spray_start_height_tooltip"), "Height above the water the particles start at."));

			ImGui::SeparatorText(T(TKEY("spray_life_size"), "Life and Size"));
			Util::SliderFloatTip(T(TKEY("spray_life"), "Lifespan"), &e.LifeSpan, 0.1f, 10.0f, "%.2f s",
				T(TKEY("spray_life_tooltip"), "How long a particle lives, unless it falls back into the water first."), ImGuiSliderFlags_Logarithmic);
			Util::SliderFloatTip(T(TKEY("spray_life_variation"), "Lifespan Variation"), &e.LifeVariation, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_life_variation_tooltip"), "Random share of the lifespan, either way."));
			Util::SliderFloatTip(T(TKEY("spray_radius"), "Radius"), &e.Radius, 0.002f, 5.0f, "%.3f m",
				T(TKEY("spray_radius_tooltip"), "Half the width of a particle at birth."), ImGuiSliderFlags_Logarithmic);
			Util::SliderFloatTip(T(TKEY("spray_radius_variation"), "Radius Variation"), &e.RadiusVariation, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_radius_variation_tooltip"), "Random share of the radius, either way."));
			Util::SliderFloatTip(T(TKEY("spray_end_size"), "End Size"), &e.EndSize, 0.0f, 6.0f, "%.2f x",
				T(TKEY("spray_end_size_tooltip"), "Size at the end of life, relative to the start: spray spreads as it flies."));
			Util::SliderFloatTip(T(TKEY("spray_grow_time"), "Grow Time"), &e.GrowTime, 0.0f, 2.0f, "%.2f s",
				T(TKEY("spray_grow_time_tooltip"), "Time a particle takes to grow from nothing."));
			Util::SliderFloatTip(T(TKEY("spray_fade_time"), "Fade Time"), &e.FadeTime, 0.0f, 5.0f, "%.2f s",
				T(TKEY("spray_fade_time_tooltip"), "Time a particle takes to fade out at the end of its life."));
			Util::SliderFloatTip(T(TKEY("spray_fade_in"), "Fade In"), &e.FadeIn, 0.0f, 0.5f, "%.2f",
				T(TKEY("spray_fade_in_tooltip"), "Share of its life a particle takes to fade in."));

			ImGui::SeparatorText(T(TKEY("spray_motion"), "Motion"));
			Util::SliderFloatTip(T(TKEY("spray_gravity"), "Gravity"), &e.Gravity, 0.0f, 2.0f, "%.2f g",
				T(TKEY("spray_gravity_tooltip"), "Gravity on the particles. Mist hangs in the air (near 0)."));
			Util::SliderFloatTip(T(TKEY("spray_drag"), "Air Drag"), &e.Drag, 0.0f, 5.0f, "%.3f /m",
				T(TKEY("spray_drag_tooltip"), "Quadratic air drag towards the wind. Droplets fly almost freely (about 0.0002 / radius); mist is held in the air and drifts with the wind (1 and more)."), ImGuiSliderFlags_Logarithmic);
			Util::SliderFloatTip(T(TKEY("spray_rotation_speed"), "Spin"), &e.RotationSpeed, 0.0f, 180.0f, "%.0f deg/s",
				T(TKEY("spray_rotation_speed_tooltip"), "How fast the particles turn, either way."));
			Util::SliderFloatTip(T(TKEY("spray_rotation_variation"), "Spin Variation"), &e.RotationVariation, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_rotation_variation_tooltip"), "Random share of the spin, either way."));

			ImGui::SeparatorText(T(TKEY("spray_look"), "Look"));
			ImGui::ColorEdit3(T(TKEY("spray_color"), "Colour"), reinterpret_cast<float*>(&e.Color));
			Util::SliderFloatTip(T(TKEY("spray_alpha"), "Opacity"), &e.Alpha, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_alpha_tooltip"), "Opacity of a particle at full strength."));
			Util::SliderFloatTip(T(TKEY("spray_lighting"), "Lighting Influence"), &e.LightingInfluence, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_lighting_tooltip"), "1 lights the particles by the sun and sky like the vanilla particles; 0 draws them unlit."));
			Util::SliderFloatTip(T(TKEY("spray_forward_scattering"), "Sun Glow"), &e.ForwardScattering, 0.0f, 0.9f, "%.2f",
				T(TKEY("spray_forward_scattering_tooltip"), "How much brighter spray seen against the sun glows (forward scattering of the droplets)."));
			Util::SliderFloatTip(T(TKEY("spray_additive"), "Additive"), &e.Additive, 0.0f, 1.0f, "%.2f",
				T(TKEY("spray_additive_tooltip"), "0 blends the particles over the scene, 1 adds their light to it."));
			Util::SliderFloatTip(T(TKEY("spray_soft_depth"), "Soft Edge Depth"), &e.SoftDepth, 0.0f, 5.0f, "%.2f m",
				T(TKEY("spray_soft_depth_tooltip"), "Depth over which a particle fades where it meets the water or the ground."));
			Util::SliderFloatTip(T(TKEY("spray_motion_stretch"), "Motion Stretch"), &e.MotionStretch, 0.0f, 0.1f, "%.3f s",
				T(TKEY("spray_motion_stretch_tooltip"), "Draws each particle out along its motion over this much time, like a streak of falling water."));
			if (Util::Button(T(TKEY("spray_reset_emitter"), "Reset Emitter")))
				e = defaults[i];
		}
		ImGui::PopID();
	}
}

void PBRWater::DrawSettings()
{
	// Each tab keeps its widgets under its own ID scope; every slider carries its default in the tooltip.
	struct IdScope
	{
		explicit IdScope(const char* id) { ImGui::PushID(id); }
		~IdScope() { ImGui::PopID(); }
		IdScope(const IdScope&) = delete;
		IdScope& operator=(const IdScope&) = delete;
	};
	auto& s = settings;

	if (!ImGui::BeginTabBar("##PBRWaterTabs", ImGuiTabBarFlags_FittingPolicyScroll))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_waves"), "Waves"))) {
		IdScope id("waves");
		Util::CheckboxTip(T(TKEY("enable_ocean_waves"), "Ocean Waves"), &s.EnableOceanWaves,
			T(TKEY("enable_ocean_waves_tooltip"), "The wind-driven open-sea waves (FFT simulation). Off: the sea is flat apart from the shore waves, ripples and the vanilla normal maps, and the simulation costs nothing."));
		Util::SliderFloatTip(T(TKEY("wave_height"), "Wave Height"), &s.WaveHeight, 0.0f, 3.0f, "%.2f",
			T(TKEY("wave_height_tooltip"), "Scales the wind-driven wave spectrum. The waves themselves come from the weather's wind, the open-water distance upwind (fetch) and the water depth. Default 1."));
		Util::SliderFloatTip(T(TKEY("wind_speed_calm"), "Calm Wind Speed"), &s.WindSpeedCalm, 0.0f, 10.0f, "%.1f m/s",
			T(TKEY("wind_speed_calm_tooltip"), "Wind speed used when the weather reports no wind. Default 3."));
		Util::SliderFloatTip(T(TKEY("wind_speed_storm"), "Storm Wind Speed"), &s.WindSpeedStorm, 1.0f, 40.0f, "%.1f m/s",
			T(TKEY("wind_speed_storm_tooltip"), "Wind speed reached at the weather's maximum wind. Wave height grows with the square of the wind speed. Default 22."));
		Util::SliderFloatTip(T(TKEY("choppiness"), "Choppiness"), &s.Choppiness, 0.0f, 2.5f, "%.2f",
			T(TKEY("choppiness_tooltip"), "Horizontal displacement of the waves: sharper crests and flatter troughs. 1 is linear wave theory, 0 gives rounded sine-like waves. It is held back where the surface would fold over itself. Default 1."));
		Util::SliderFloatTip(T(TKEY("directional_spread"), "Wind Turbulence"), &s.DirectionalSpread, 0.0f, 1.0f, "%.2f",
			T(TKEY("directional_spread_tooltip"), "How far the waves stray from the wind's direction. 0 lines every wave up with the wind, 1 gives a confused, short-crested sea. Default 0.6."));
		Util::SliderFloatTip(T(TKEY("fetch_scale"), "Fetch Scale"), &s.FetchScale, 1.0f, 50.0f, "%.1f",
			T(TKEY("fetch_scale_tooltip"), "Skyrim's distances are compressed, so the open water upwind of a spot counts this many times over when working out how far the wind has grown the sea. Higher gives bigger waves in bays and lakes. Default 15."));

		ImGui::SeparatorText(T(TKEY("swell"), "Swell"));
		Util::SliderFloatTip(T(TKEY("swell_height"), "Swell Height"), &s.SwellHeight, 0.0f, 4.0f, "%.2f m",
			T(TKEY("swell_height_tooltip"), "Long, regular waves arriving from distant storms, on top of the local wind sea. Only on the open sea: sheltered water never sees them. Default 0.5 m."));
		Util::SliderFloatTip(T(TKEY("swell_period"), "Swell Period"), &s.SwellPeriod, 4.0f, 20.0f, "%.1f s",
			T(TKEY("swell_period_tooltip"), "Time between swell crests. Longer periods mean longer waves. Default 10 s."));
		Util::SliderFloatTip(T(TKEY("swell_direction"), "Swell Direction"), &s.SwellDirection, -180.0f, 180.0f, "%.0f deg",
			T(TKEY("swell_direction_tooltip"), "Direction the swell travels, relative to the wind. Swell crossing the wind sea gives the most natural, irregular water. Default 35 degrees."));

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("swell_peakedness"), "Swell Peakedness"), &s.SwellPeakedness, 1.0f, 20.0f, "%.1f",
				T(TKEY("swell_peakedness_tooltip"), "JONSWAP peak enhancement of the swell. Higher is narrower banded: waves of nearly one period. Default 7."));
			Util::SliderFloatTip(T(TKEY("swell_spread"), "Swell Directional Spread"), &s.SwellSpread, 4.0f, 80.0f, "%.0f",
				T(TKEY("swell_spread_tooltip"), "Directional spreading exponent of the swell. Higher gives longer, more parallel crests. Default 30."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_shore"), "Shore"))) {
		IdScope id("shore");
		Util::CheckboxTip(T(TKEY("enable_shore_waves"), "Shore Waves"), &s.EnableShoreWaves,
			T(TKEY("enable_shore_waves_tooltip"), "Waves roll in towards the shore, slow down and close up over the shoaling bed, steepen, pitch forward and break into foaming bores, and each one runs up the beach and drains back, leaving wet sand. Only on beaches where waves break: they surge against rock faces and cliffs instead."));
		if (s.EnableShoreWaves) {
			Util::CheckboxTip(T(TKEY("wet_shores"), "Wet Shores"), &s.WetShores,
				T(TKEY("wet_shores_tooltip"), "Sand, rocks and anything else the swash reaches turn wet and slowly dry out (Wetness Effects). Off: the wetness grid is not updated, and the caustics on the shore follow the still water instead of the waves."));
			const char* surfQualities[] = { T(TKEY("surf_quality_off"), "Off"), T(TKEY("surf_quality_low"), "Low (34 cm)"), T(TKEY("surf_quality_high"), "High (17 cm)") };
			ImGui::Combo(T(TKEY("surf_quality"), "Surf Simulation"), &s.SurfQuality, surfQualities, IM_ARRAYSIZE(surfQualities));
			Util::HoverTip(T(TKEY("surf_quality_tooltip"), "Simulates the broken waves' water through the inner surf and up the beach: bores, uprush, backwash and the foam they carry. Off: the shore waves end at the waterline, with no swash and no foam left on the water, and the whole simulation costs nothing. Low simulates a quarter of the cells at half the steps of High."));
		}
		Util::SliderFloatTip(T(TKEY("shore_train_cycles"), "Shore Wave Reach"), &s.ShoreTrainCycles, 2.0f, 16.0f, "%.1f waves",
			T(TKEY("shore_train_cycles_tooltip"), "How many waves out from the shore the breaking wave train reaches. Beyond it the open sea's waves carry on. Default 7."));
		Util::SliderFloatTip(T(TKEY("shore_crest_wobble"), "Crest Irregularity"), &s.ShoreCrestWobble, 0.0f, 1.0f, "%.2f",
			T(TKEY("shore_crest_wobble_tooltip"), "How far the crests bend and drift along the coast, in waves, so stretches of beach break at different times instead of in one straight line. Default 0.35."));
		Util::SliderFloatTip(T(TKEY("shore_bore_whelps"), "Bore Whelps"), &s.ShoreBoreWhelps, 0.0f, 1.5f, "%.2f",
			T(TKEY("shore_bore_whelps_tooltip"), "A weak bore, or a wave that has passed over a sandbar into deeper water, trails a short string of smaller crests that follow its front and die away behind it. This scales their height. 0 leaves only the main waves. Default 0.6."));
		Util::SliderFloatTip(T(TKEY("shore_curl"), "Breaker Curl"), &s.ShoreCurl, 0.0f, 1.5f, "%.2f",
			T(TKEY("shore_curl_tooltip"), "How far plunging breakers curl over: the crest is thrown ahead of the wave and falls into the trough in front. 0 keeps the lip without the curl. Needs tessellation. Default 1."));
		Util::SliderFloatTip(T(TKEY("swash_linger"), "Swash Linger"), &s.SwashLinger, 1.0f, 5.0f, "%.1fx",
			T(TKEY("swash_linger_tooltip"), "How strongly the sand drags on each sheet of swash: higher, the water runs up less far and drains back down the beach more slowly, and its foam lingers on the sand. Default 2."));
		Util::SliderFloatTip(T(TKEY("swash_churn_height"), "Swash Churn Height"), &s.SwashChurnHeight, 0.0f, 0.2f, "%.3f m",
			T(TKEY("swash_churn_height_tooltip"), "The tallest the swash's churning lumps stand: the small waves that run through it, meet and heap up. 0 leaves the swash a smooth sheet. Default 0.05 m."));
		Util::SliderFloatTip(T(TKEY("swash_churn_depth"), "Swash Churn Depth Share"), &s.SwashChurnDepth, 0.0f, 1.0f, "%.2f",
			T(TKEY("swash_churn_depth_tooltip"), "How high the lumps stand as a share of the water's depth, up to Swash Churn Height: a deep, foaming uprush churns, a thin film barely moves. Default 0.35."));
		Util::SliderFloatTip(T(TKEY("swash_churn_size"), "Swash Churn Size"), &s.SwashChurnSize, 0.1f, 1.5f, "%.2f m",
			T(TKEY("swash_churn_size_tooltip"), "How large the churning lumps are. Smaller is choppier; lumps finer than the mesh show only in the shading. Default 0.35 m."));
		Util::SliderFloatTip(T(TKEY("swash_churn_speed"), "Swash Churn Speed"), &s.SwashChurnSpeed, 0.0f, 5.0f, "%.1fx",
			T(TKEY("swash_churn_speed_tooltip"), "How fast the lumps boil and drift, relative to the waves. Default 1.7."));

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("shore_shortest_period"), "Shortest Shore Period"), &s.ShoreShortestPeriod, 1.0f, 6.0f, "%.1f s",
				T(TKEY("shore_shortest_period_tooltip"), "The shore waves follow the sea's period, held between this and the longest period: the open sea's own periods would be as long as a beach is deep. Default 2.5 s."));
			Util::SliderFloatTip(T(TKEY("shore_longest_period"), "Longest Shore Period"), &s.ShoreLongestPeriod, 2.0f, 8.0f, "%.1f s",
				T(TKEY("shore_longest_period_tooltip"), "The longest period the shore waves take. Default 3.5 s."));
			Util::SliderFloatTip(T(TKEY("shore_wave_variation"), "Wave Height Variation"), &s.ShoreWaveVariation, 0.0f, 1.5f, "%.2f",
				T(TKEY("shore_wave_variation_tooltip"), "How much one wave's height differs from the next. Real waves are random and come in groups of big and small ones, and the big ones break further out: at 1 each wave breaks at its own distance from the shore, at 0 every wave is alike and they all break at the same place. How short the crests are (so how much one wave varies along its length) follows Wind Turbulence. Default 1."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_surface"), "Surface & Light"))) {
		IdScope id("surface");
		Util::CheckboxTip(T(TKEY("enable_wave_shadows"), "Wave Shadows"), &s.EnableWaveShadows,
			T(TKEY("enable_wave_shadows_tooltip"), "Waves shade the water behind them from the sun, and the glint is hidden by the crests in front. Off: skips the wave heightfield pass and its per-pixel march."));
		Util::CheckboxTip(T(TKEY("enable_point_light_specular"), "Point Light Reflections"), &s.EnablePointLightSpecular,
			T(TKEY("enable_point_light_specular_tooltip"), "Torches and other lights glint in the water. Off: the per-pixel light loop is skipped."));
		{
			Util::DisableGuard disableLights(!s.EnablePointLightSpecular);
			Util::SliderFloatTip(T(TKEY("point_light_specular"), "Point Light Specular"), &s.PointLightSpecular, 0.0f, 3.0f, "%.2f",
				T(TKEY("point_light_specular_tooltip"), "Brightness of torches and other lights reflected in the water. Default 1."));
		}
		Util::CheckboxTip(T(TKEY("enable_sky_occlusion"), "Wave Sky Occlusion"), &s.EnableSkyOcclusion,
			T(TKEY("enable_sky_occlusion_tooltip"), "The waves occlude the sky in Skylighting's probes, so troughs see less of it than crests. Off: the displaced water grid is not drawn into the probes' occlusion map. Needs Skylighting."));
		Util::CheckboxTip(T(TKEY("enable_waterline"), "Per-Pixel Waterline"), &s.EnableWaterline,
			T(TKEY("enable_waterline_tooltip"), "With waves, the water's mesh is held on the sand near the shore and where the water ends is found per pixel against the ground, so the edge follows the terrain instead of the triangles. Off: skips its terrain reads in the vertex, stencil and surface passes; the waves' triangles cut through the shore and the swash can show gaps. Without any waves it is skipped anyway."));
		Util::CheckboxTip(T(TKEY("enable_env_reflections"), "Environment Reflections"), &s.EnableEnvReflections,
			T(TKEY("enable_env_reflections_tooltip"), "The water reflects the sky and surroundings from the cubemap. Off: it reflects the water form's flat reflection colour and no cubemap is read."));
		Util::CheckboxTip(T(TKEY("enable_ssr"), "Screen-Space Reflections"), &s.EnableSSR,
			T(TKEY("enable_ssr_tooltip"), "What is on screen is reflected in the water. Off: the screen-space reflection targets are not read."));
		Util::CheckboxTip(T(TKEY("enable_sun_shadows"), "Sun Shadows"), &s.EnableSunShadows,
			T(TKEY("enable_sun_shadows_tooltip"), "Shadows of terrain, objects and clouds fall on the water. Off: no shadow lookups; the water is lit as if nothing blocked the sun."));
		Util::CheckboxTip(T(TKEY("enable_skylighting"), "Skylighting"), &s.EnableSkylighting,
			T(TKEY("enable_skylighting_tooltip"), "Skylighting darkens the sky the water sees under cliffs, hulls and overhangs. Off: its probes are not read and the water sees the open sky."));
		Util::CheckboxTip(T(TKEY("enable_fog"), "Fog"), &s.EnableFog,
			T(TKEY("enable_fog_tooltip"), "Distance fog, height fog and the atmosphere fade distant water. Off: none of them are applied to the water."));
		Util::CheckboxTip(T(TKEY("batched_stencil"), "Batched Stencil Pass"), &s.BatchedStencil,
			T(TKEY("batched_stencil_tooltip"), "Unified Water's distant, flat tiles are drawn in one draw in the water's depth and mask pass, instead of the game setting up and drawing each tile. Saves CPU time with many water tiles in view. Turn off if distant water shows holes or flickers."));
		Util::SliderFloatTip(T(TKEY("roughness"), "Surface Roughness"), &s.Roughness, 0.02f, 0.5f, "%.3f",
			T(TKEY("roughness_tooltip"), "Base roughness of the calm water surface. Default 0.06."));
		Util::SliderFloatTip(T(TKEY("wind_roughness"), "Wind Roughness"), &s.WindRoughness, 0.0f, 3.0f, "%.2f",
			T(TKEY("wind_roughness_tooltip"), "Scales the roughness the wind adds with its tiny ripples (Cox and Munk). Default 1."));
		Util::CheckboxTip(T(TKEY("vanilla_normal_maps"), "Vanilla Normal Maps"), &s.VanillaNormalMaps,
			T(TKEY("vanilla_normal_maps_tooltip"), "Reads the water form's own normal maps on top of the simulated waves. Off: the three normal map reads are skipped; the waves, ripples and rain carry the detail, and water without any of them lies flat. Rivers always keep theirs."));
		Util::SliderFloatTip(T(TKEY("detail_normal_calm"), "Normal Map Strength (Calm)"), &s.DetailNormalCalm, 0.0f, 2.0f, "%.2f",
			T(TKEY("detail_normal_calm_tooltip"), "Strength of the water form's normal maps on top of the simulated waves in calm air. They carry the fine ripple texture. Default 0.75."));
		Util::SliderFloatTip(T(TKEY("detail_normal_gale"), "Normal Map Strength (Windy)"), &s.DetailNormalGale, 0.0f, 2.0f, "%.2f",
			T(TKEY("detail_normal_gale_tooltip"), "The same at 8 m/s of wind and over; in between it blends. Default 1.3."));
		Util::SliderFloatTip(T(TKEY("refraction_distortion"), "Refraction Strength"), &s.RefractionDistortion, 0.0f, 3.0f, "%.2f",
			T(TKEY("refraction_distortion_tooltip"), "How far the surface bends the view of what lies below, along the refracted ray to the bed (Snell's law): the deeper the water, the larger the shift. 1 is physically correct. Default 1."));
		Util::SliderFloatTip(T(TKEY("sun_specular"), "Sun Specular"), &s.SunSpecular, 0.0f, 3.0f, "%.2f",
			T(TKEY("sun_specular_tooltip"), "Brightness of the sun's glint on the surface. Default 1."));
		Util::CheckboxTip(T(TKEY("enable_subsurface"), "Subsurface Scattering"), &s.EnableSubsurface,
			T(TKEY("enable_subsurface_tooltip"), "Sunlight scatters through all the water and back to the eye. Off: it is not evaluated at all, which saves a scattering evaluation per pixel."));
		{
			Util::DisableGuard disableSubsurface(!s.EnableSubsurface);
			Util::SliderFloatTip(T(TKEY("subsurface"), "Subsurface Strength"), &s.Subsurface, 0.0f, 3.0f, "%.2f",
				T(TKEY("subsurface_tooltip"), "Strength of the sunlight that shines through the water and scatters back to the eye, over all the water. Default 1."));
		}

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("point_light_emitter_radius"), "Point Light Size"), &s.PointLightEmitterRadius, 1.0f, 40.0f, "%.1f units",
				T(TKEY("point_light_emitter_radius_tooltip"), "Skyrim's lights have no size; this is the radius given to a point light when working out its reflection (about a torch flame). Bigger gives softer, wider glints. Default 8 units."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_clarity"), "Water Clarity"))) {
		IdScope id("clarity");
		Util::CheckboxTip(T(TKEY("enable_clarity"), "Water Clarity"), &s.EnableClarity,
			T(TKEY("enable_clarity_tooltip"), "Suspended sediment: drifting plumes, stirred-up shallows, storm silt, wading silt. Off: the water is as clear as its form's colours make it, and the sediment is not evaluated."));
		Util::SliderFloatTip(T(TKEY("visibility"), "Water Visibility"), &s.Visibility, 0.05f, 5.0f, "%.2f",
			T(TKEY("visibility_tooltip"), "Scales the water form's visibility distance. Absorption colour comes from the water form's shallow colour. Default 1."));
		Util::CheckboxTip(T(TKEY("physical_body_colour"), "Physical Water Colour"), &s.PhysicalBodyColour,
			T(TKEY("physical_body_colour_tooltip"), "The colour of deep water comes from how it absorbs and scatters light (blue in clear water, green and brown as particles and sediment take over) instead of the water type's colour."));
		Util::SliderFloatTip(T(TKEY("turbidity"), "Turbidity"), &s.Turbidity, 0.0f, 5.0f, "%.2f",
			T(TKEY("turbidity_tooltip"), "Sediment suspended everywhere in the water. 0 keeps the water form's own clarity. Default 0.15."));
		ImGui::ColorEdit3(T(TKEY("sediment_color"), "Sediment Colour"), reinterpret_cast<float*>(&s.SedimentColor));
		Util::SliderFloatTip(T(TKEY("sediment_density"), "Sediment Density"), &s.SedimentDensity, 0.05f, 2.0f, "%.2f",
			T(TKEY("sediment_density_tooltip"), "How murky each unit of turbidity makes the water (extinction per metre). Default 0.35."));
		Util::SliderFloatTip(T(TKEY("turbidity_patchiness"), "Patchiness"), &s.TurbidityPatchiness, 0.0f, 1.0f, "%.2f",
			T(TKEY("turbidity_patchiness_tooltip"), "How unevenly the sediment is spread: 0 is uniform, 1 has drifting clear and murky plumes. Default 0.7."));
		Util::SliderFloatTip(T(TKEY("turbidity_patch_size"), "Patch Size"), &s.TurbidityPatchSize, 30.0f, 1000.0f, "%.0f m",
			T(TKEY("turbidity_patch_size_tooltip"), "Size of the sediment plumes. Default 150 m."));
		Util::SliderFloatTip(T(TKEY("shore_resuspension"), "Shallows Stirring"), &s.ShoreResuspension, 0.0f, 3.0f, "%.2f",
			T(TKEY("shore_resuspension_tooltip"), "Sediment stirred up by the waves in the shallows and the surf. Default 1."));
		Util::SliderFloatTip(T(TKEY("storm_turbidity"), "Storm Silt"), &s.StormTurbidity, 0.0f, 3.0f, "%.2f",
			T(TKEY("storm_turbidity_tooltip"), "Extra sediment in strong wind and rain. Default 1."));
		Util::SliderFloatTip(T(TKEY("wading_silt"), "Wading Silt"), &s.WadingSilt, 0.0f, 3.0f, "%.2f",
			T(TKEY("wading_silt_tooltip"), "Silt kicked up by feet on the bed (with Interactive Ripples). Default 1."));

		ImGui::SeparatorText(T(TKEY("underwater"), "Underwater"));
		Util::CheckboxTip(T(TKEY("underwater_volume"), "Physical Underwater Water"), &s.UnderwaterVolume,
			T(TKEY("underwater_volume_tooltip"), "Under the surface, the water absorbs and scatters light as it does from above (red first, then green, blue last), in place of the game's underwater fog."));
		{
			Util::DisableGuard disableShafts(!s.UnderwaterVolume);
			ImGui::SliderInt(T(TKEY("light_shaft_samples"), "Underwater Light Shafts"), &s.LightShaftSamples, 0, 32);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("light_shaft_samples_tooltip"), "Steps of the light shafts under water: sunlight through the waves, shadowed by what stands in it. More steps are smoother and cost more. 0: no shafts. Default 12."));
			Util::SliderFloatTip(T(TKEY("light_shaft_focus"), "Light Shaft Focusing"), &s.LightShaftFocus, 0.0f, 3.0f, "%.2f",
				T(TKEY("light_shaft_focus_tooltip"), "How strongly the waves focus the sunlight into streaks under water. 1 is physical. Default 1."));
		}

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("sediment_layer_height"), "Sediment Layer Height"), &s.SedimentLayerHeight, 0.2f, 10.0f, "%.1f m",
				T(TKEY("sediment_layer_height_tooltip"), "How far above the bed the stirred-up sediment reaches. Default 1.5 m."));
			Util::SliderFloatTip(T(TKEY("silt_half_life"), "Silt Settling Time"), &s.SiltHalfLife, 2.0f, 120.0f, "%.0f s",
				T(TKEY("silt_half_life_tooltip"), "Half-life of silt kicked up from the bed: how long it takes to settle. Default 20 s."));
			Util::SliderFloatTip(T(TKEY("scattering_anisotropy"), "Scattering Anisotropy"), &s.ScatteringAnisotropy, 0.0f, 0.95f, "%.2f",
				T(TKEY("scattering_anisotropy_tooltip"), "Henyey-Greenstein g of the particles in the water: how much they scatter light forwards. Default 0.6."));
			Util::SliderFloatTip(T(TKEY("downwelling_attenuation"), "Light Attenuation"), &s.DownwellingAttenuation, 0.0f, 3.0f, "%.2f",
				T(TKEY("downwelling_attenuation_tooltip"), "Scales how fast sunlight fades on its way down through the water. Default 1."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_foam"), "Foam"))) {
		IdScope id("foam");
		Util::CheckboxTip(T(TKEY("enable_foam"), "Foam"), &s.EnableFoam,
			T(TKEY("enable_foam_tooltip"), "White water on the open sea, the breakers and the surf, all drawn as one foam. Off: no foam is read or lit."));
		Util::SliderFloatTip(T(TKEY("foam_amount"), "Foam Amount"), &s.FoamAmount, 0.0f, 3.0f, "%.2f",
			T(TKEY("foam_amount_tooltip"), "Foam is made only where waves break, in the open sea and in the surf. Scales how much foam the open sea's crests add each moment, and the breakers' and the surf's white water with it. Default 1."));
		Util::SliderFloatTip(T(TKEY("foam_bias"), "Foam Coverage"), &s.FoamBias, -1.0f, 2.0f, "%.2f",
			T(TKEY("foam_bias_tooltip"), "The wave compression (Jacobian) below which the open sea starts to foam. Higher foams on gentler waves and thickens the breakers' and the surf's white water. Default 0.85."));
		Util::SliderFloatTip(T(TKEY("foam_decay_rate"), "Foam Fade Rate"), &s.FoamDecayRate, 0.0f, 0.2f, "%.4f",
			T(TKEY("foam_decay_rate_tooltip"), "How quickly the open sea's foam fades once a crest has passed: by exp(-rate) every 1/60 s. Default 0.0175."));
		Util::SliderFloatTip(T(TKEY("foam_fade_distance"), "Foam Fade Distance"), &s.FoamFadeDistance, 100.0f, 5000.0f, "%.0f m",
			T(TKEY("foam_fade_distance_tooltip"), "Distance at which all foam has faded out. Default 1000 m."), ImGuiSliderFlags_Logarithmic);
		Util::SliderFloatTip(T(TKEY("foam_bubble_size"), "Bubble Size"), &s.FoamBubbleSize, 0.01f, 1.0f, "%.3f m",
			T(TKEY("foam_bubble_size_tooltip"), "Size of the bubbles in the foam pattern. Surf foam is a microfoam of bubbles a few centimetres across; the pattern also draws lace at three and nine times this. Default 0.03 m."), ImGuiSliderFlags_Logarithmic);
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_spray"), "Spray"))) {
		IdScope id("spray");
		DrawSpraySettings();
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_ripples"), "Ripples"))) {
		IdScope id("ripples");
		Util::CheckboxTip(T(TKEY("enable_ripples"), "Interactive Ripples"), &s.EnableRipples,
			T(TKEY("enable_ripples_tooltip"), "Simulates ripples and wakes from every actor and object in the water. Replaces the vanilla wading mesh."));
		Util::CheckboxTip(T(TKEY("enable_wakes"), "Kelvin Wakes"), &s.EnableWakes,
			T(TKEY("enable_wakes_tooltip"), "Swimmers, waders and horses moving through the water leave the V-shaped wake of real deep-water waves behind them, its waves longer the faster they go."));
		{
			Util::DisableGuard disableWakes(!s.EnableWakes);
			Util::SliderFloatTip(T(TKEY("wake_strength"), "Wake Strength"), &s.WakeStrength, 0.0f, 3.0f, "%.2f",
				T(TKEY("wake_strength_tooltip"), "Scales the height of the wakes. Default 1."));
		}
		Util::SliderFloatTip(T(TKEY("ripple_extent"), "Ripple Range"), &s.RippleExtent, 16.0f, 128.0f, "%.0f m",
			T(TKEY("ripple_extent_tooltip"), "Width of the area around the camera in which ripples are simulated. A wider area spreads the same simulation thinner, so ripples look coarser. Default 64 m."));
		Util::SliderFloatTip(T(TKEY("ripple_height"), "Ripple Height"), &s.RippleHeight, 0.0f, 0.5f, "%.3f m",
			T(TKEY("ripple_height_tooltip"), "Height of a ripple at full strength. Default 0.12 m."));
		Util::SliderFloatTip(T(TKEY("ripple_normal_strength"), "Ripple Normal Strength"), &s.RippleNormalStrength, 0.0f, 3.0f, "%.2f",
			T(TKEY("ripple_normal_strength_tooltip"), "Scales how strongly ripples tilt the surface for lighting. Default 1."));
		Util::SliderFloatTip(T(TKEY("ripple_half_life"), "Ripple Lifetime"), &s.RippleHalfLife, 0.2f, 6.0f, "%.2f s",
			T(TKEY("ripple_half_life_tooltip"), "Half-life of a ripple's amplitude: how long it takes to die away. Default 1.5 s."));

		if (ImGui::CollapsingHeader(T(TKEY("ripples_advanced"), "Ripples Advanced"))) {
			Util::SliderFloatTip(T(TKEY("ripple_horiz_displace"), "Horizontal Displacement"), &s.RippleHorizDisplace, 0.0f, 6.0f, "%.2f",
				T(TKEY("ripple_horiz_displace_tooltip"), "Ripples push the surface sideways as well as up, which sharpens their crests. 0 turns it off. Default 3."));
			Util::SliderFloatTip(T(TKEY("ripple_displace_clamp"), "Displacement Limit"), &s.RippleDisplaceClamp, 0.1f, 0.6f, "%.2f",
				T(TKEY("ripple_displace_clamp_tooltip"), "Largest sideways displacement, in texels of the ripple simulation. Default 0.3."));
		}

		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_floating"), "Floating Objects"))) {
		IdScope id("floating");
		Util::CheckboxTip(T(TKEY("gameplay_waves"), "Swimming Follows Waves"), &s.GameplayWaves,
			T(TKEY("gameplay_waves_tooltip"), "The game's water height includes the rendered waves, so swimming actors ride them."));
		Util::CheckboxTip(T(TKEY("enable_floating_objects"), "Boats and Ice Ride the Waves"), &s.EnableFloatingObjects,
			T(TKEY("enable_floating_objects_tooltip"), "Boats, ships, rafts and ice floes heave, pitch and roll on the rendered waves, with everything on board. Found automatically: anything placed afloat that does not rest on the bottom. Nothing is saved; objects are back in place whenever the game saves."));
		if (s.EnableFloatingObjects) {
			Util::CheckboxTip(T(TKEY("carry_actors"), "Carry Actors on Board"), &s.CarryActors,
				T(TKEY("carry_actors_tooltip"), "The player and NPCs standing on a floating hull move with its deck, and their weight sinks and tilts small boats."));
			Util::SliderFloatTip(T(TKEY("floating_response"), "Hull Motion"), &s.FloatingResponse, 0.0f, 2.0f, "%.2f",
				T(TKEY("floating_response_tooltip"), "Scales how much the hulls heave, pitch and roll. Default 1."));
			Util::SliderFloatTip(T(TKEY("floating_range"), "Floating Range"), &s.FloatingRange, 50.0f, 500.0f, "%.0f m",
				T(TKEY("floating_range_tooltip"), "Objects within this distance of the player float. Default 150 m."));
			Util::SliderFloatTip(T(TKEY("max_floating_size"), "Largest Floating Hull"), &s.MaxFloatingSize, 20.0f, 200.0f, "%.0f m",
				T(TKEY("max_floating_size_tooltip"), "The longest hull that floats (the large Skyrim ships are about 65 m). Default 90 m."));
		}
		Util::CheckboxTip(T(TKEY("hull_masks"), "Keep Water Out of Hulls"), &s.HullMasks,
			T(TKEY("hull_masks_tooltip"), "The water is not drawn inside boats, ships and rafts, so the waves passing through a hull no longer show on its deck or in its bilge. Each hull's outline at the waterline is measured once against its collision, floating or not. Works with the boats that stay put too."));
		Util::SliderFloatTip(T(TKEY("buoyancy_strength"), "Small Object Buoyancy"), &s.BuoyancyStrength, 0.0f, 3.0f, "%.2f",
			T(TKEY("buoyancy_strength_tooltip"), "How strongly the waves push loose props and bodies that float on the water. Default 1."));
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_performance"), "Performance"))) {
		IdScope id("performance");
		{
			const char* sizes[] = { T(TKEY("wave_resolution_low"), "Low (128)"), T(TKEY("wave_resolution_medium"), "Medium (256)"), T(TKEY("wave_resolution_high"), "High (512)") };
			ImGui::Combo(T(TKEY("wave_resolution"), "Wave Detail"), &s.WaveResolution, sizes, IM_ARRAYSIZE(sizes));
			Util::HoverTip(T(TKEY("wave_resolution_tooltip"), "Resolution of each FFT wave cascade. Higher values add finer ripples close up at a small GPU cost; gameplay is unaffected."));
		}
		Util::CheckboxTip(T(TKEY("long_wave_half_rate"), "Half-Rate Long Waves"), &s.LongWaveHalfRate,
			T(TKEY("long_wave_half_rate_tooltip"), "The two longest wave cascades update on alternate frames, saving about half their FFT. Their waves move in steps of two frames. Cascades no water near the camera can show are skipped either way."));
		Util::CheckboxTip(T(TKEY("cheap_offscreen_water"), "Cheap Reflected Water"), &s.CheapOffscreenWater,
			T(TKEY("cheap_offscreen_water_tooltip"), "Water seen in reflections, cubemaps and the map is drawn without shore waves, foam, sediment, subsurface light or point lights: it is small or blurred there."));
		{
			const char* modes[] = { T(TKEY("engine_reflections_full"), "Full Scene"), T(TKEY("engine_reflections_sky"), "Sky Only"), T(TKEY("engine_reflections_off"), "Off") };
			ImGui::Combo(T(TKEY("engine_reflections"), "Engine Reflections"), &s.EngineReflections, modes, IM_ARRAYSIZE(modes));
			Util::HoverTip(T(TKEY("engine_reflections_tooltip"), "What the game renders into its own water reflection cubemap each frame. PBR water reflects Dynamic Cubemaps and screen-space reflections, so the game's mirrored scene is mostly unseen. Sky Only keeps the sky (Cloud Shadows and Dynamic Cubemaps fill from it); Off skips the render and needs Dynamic Cubemaps without Cloud Shadows, else it acts as Sky Only."));
		}
		Util::CheckboxTip(T(TKEY("enable_tessellation"), "Tessellation"), &s.EnableTessellation,
			T(TKEY("enable_tessellation_tooltip"), "Draws the sea and the lakes as one grid each, spread evenly over the screen and cast onto the water, with its waves evaluated once a frame for every pass: the waves are real geometry out to the horizon, the triangles the same size on screen near and far, and breakers get finer cells where they curl. Rivers keep the game's meshes, cut finer near the camera; interior water keeps the game's meshes. Off: all water is the game's meshes, and the waves are limited by their resolution."));
		{
			Util::DisableGuard disable(!s.EnableTessellation);
			Util::SliderFloatTip(T(TKEY("grid_cell_size"), "Grid Cell Size"), &s.GridCellSize, 4.0f, 24.0f, "%.0f px",
				T(TKEY("grid_cell_size_tooltip"), "Size of the grid's cells on screen. Smaller values carry shorter waves in the geometry at a higher GPU cost. Scales with the render resolution. Default 8 px."));
			Util::CheckboxTip(T(TKEY("deferred_surface"), "Deferred Surface"), &s.DeferredSurface,
				T(TKEY("deferred_surface_tooltip"), "Shades each pixel of the grid's water once, from what the depth pass recorded there, instead of once per triangle touching it. Small triangles then cost no extra shading. Turn off only to compare."));
			Util::DisableGuard disableDistant(!s.DeferredSurface);
			Util::CheckboxTip(T(TKEY("distant_half_rate"), "Half-Rate Distant Water"), &s.DistantHalfRate,
				T(TKEY("distant_half_rate_tooltip"), "Shades distant water at a quarter of its pixels and fills in the rest from them; temporal anti-aliasing recovers the detail. Distant water is often half the cost of the surface."));
			Util::DisableGuard disableDistance(!s.DistantHalfRate);
			Util::SliderFloatTip(T(TKEY("distant_half_rate_distance"), "Distant Water From"), &s.DistantHalfRateDistance, 50.0f, 1000.0f, "%.0f m",
				T(TKEY("distant_half_rate_distance_tooltip"), "Distance beyond which water is shaded at half rate, decided per 8x8 pixel tile by its nearest water. Lower is faster and softer. Default 60 m."));
		}
		Util::SliderFloatTip(T(TKEY("displacement_fade_start"), "Wave Fade Start"), &s.DisplacementFadeStart, 2000.0f, 60000.0f, "%.0f units",
			T(TKEY("displacement_fade_start_tooltip"), "On the game's own water meshes (rivers, interiors, and all water with Tessellation off), the distance at which the waves start to flatten towards the flat distant water (70 units are a metre). The grids carry the waves to the horizon. Default 16384 units."));
		Util::SliderFloatTip(T(TKEY("displacement_fade_end"), "Wave Fade End"), &s.DisplacementFadeEnd, 4000.0f, 120000.0f, "%.0f units",
			T(TKEY("displacement_fade_end_tooltip"), "Distance at which the waves have flattened completely, and where floating objects stop riding them. Default 32768 units."));
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}

void PBRWater::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;
	auto& s = settings;
	const char* wireModes[] = { T(TKEY("wireframe_off"), "Off"), T(TKEY("wireframe_overlay"), "Overlay"), T(TKEY("wireframe_only"), "Wireframe Only") };
	ImGui::Combo(T(TKEY("wireframe"), "Wireframe"), &s.WireframeMode, wireModes, IM_ARRAYSIZE(wireModes));
	const char* views[] = {
		T(TKEY("debug_off"), "Off"),
		T(TKEY("debug_normal"), "Normal"),
		T(TKEY("debug_foam"), "Foam"),
		T(TKEY("debug_depth"), "Depth / Shore"),
		T(TKEY("debug_cascade_foam"), "Cascade Foam"),
		T(TKEY("debug_roughness"), "Roughness"),
		T(TKEY("debug_height"), "Wave Height"),
		T(TKEY("debug_fetch"), "Fetch"),
		T(TKEY("debug_clarity"), "Water Clarity"),
		T(TKEY("debug_wind"), "Local Wind"),
		T(TKEY("debug_shore_field"), "Shore Field"),
		T(TKEY("debug_wave_direction"), "Wave Direction"),
		T(TKEY("debug_shore_level"), "Shore Field Level"),
		T(TKEY("debug_shore_wave"), "Shore Wave"),
		T(TKEY("debug_wave_sun"), "Wave Sun Shadows"),
	};
	ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &s.DebugView, views, IM_ARRAYSIZE(views));
	ImGui::Text(T(TKEY("stat_engine_reflections"), "Effective engine reflections: %d"), EffectiveEngineReflections());
	if (projectedGrid.HasCells()) {
		const uint32_t gridFrame = globals::state->frameCount;
		ImGui::Text(T(TKEY("stat_projected_grids"), "Projected grids: %u (sea at %.0f)"), projectedGrid.BuiltCount(gridFrame), projectedGrid.SeaLevel());
		for (uint32_t slot = 0; slot < projectedGrid.BuiltCount(gridFrame); ++slot) {
			const auto& grid = projectedGrid.Built(slot);
			ImGui::Text(T(TKEY("stat_projected_grid_level"), "Level %.0f: %u x %u cells"), grid.z, grid.columns, grid.rows);
		}
	}

	const char* ready = T(TKEY("stat_ready"), "ready");
	const char* none = T(TKEY("stat_none"), "none");
	const char* running = T(TKEY("stat_running"), "running");
	const char* idle = T(TKEY("stat_idle"), "idle");
	const char* building = T(TKEY("stat_building"), " (building)");
	if (const auto snap = snapshot.load(std::memory_order_acquire); snap && snap->ocean) {
		const auto& o = *snap->ocean;
		ImGui::Text(T(TKEY("stat_ocean"), "Wind %.1f m/s, peak period %.1f s, Hs %.2f m, choppiness %.2f"), o.windSpeed, TwoPi / o.peakOmega, o.significantHeight / UnitsPerMetre, o.choppiness);
		ImGui::Text(T(TKEY("stat_slope"), "Mean square slope %.3f"), o.meanSquareSlope);
		ImGui::Text(T(TKEY("stat_fft"), "FFT %u x %u cascades: %s, gameplay mirror: %s"), o.resolution, PBRWaterModel::NumCascades, oceanReady.load() ? running : T(TKEY("stat_unavailable"), "unavailable"), snap->mirror0 ? running : idle);
	}
	ImGui::Text(T(TKEY("stat_draws"), "Water draws last frame: %u, %u of them projected grids"), lastDrawStats.draws, lastDrawStats.computeDraws);
	{
		const auto& ds = lastDrawStats;
		ImGui::Text(T(TKEY("stat_deferred"), "Deferred surface: %u meshes recorded, %u draws shaded from the record, %u drawn forward"), ds.deferredRecorded, ds.deferredShaded, ds.deferredForward);
		if (ds.deferredReason) {
			ImGui::SameLine();
			ImGui::TextUnformatted(ds.deferredReason);  // i18n-ignore: internal diagnostic tag
		}
	}
	{
		const auto& n = lastDrawStats.byTechnique;
		ImGui::Text(T(TKEY("stat_techniques"), "  surface %u, point lights %u, underwater %u, LOD %u, stencil %u, simple %u"), n[0], n[1], n[2], n[3], n[4], n[5]);
	}
	ImGui::Text(T(TKEY("stat_main_thread"), "Main thread update: %.3f ms (peak %.3f ms)"), mainUpdateMs.load(std::memory_order_relaxed), mainUpdatePeakMs.load(std::memory_order_relaxed));
	ImGui::Text(T(TKEY("stat_builds"), "Last builds: fetch and terrain %.0f ms, shore field %.0f ms (worker threads)"), environment.GetLastBuildMs(), environment.GetLastShoreBuildMs());
	ImGui::Text(T(TKEY("stat_steps"), "Steps last frame: ripples %u, swash %u"), settings.EnableRipples ? ripples.GetLastSteps() : 0u, frameData.Shore0.x > 0.5f ? swash.GetLastSwashSteps() : 0u);
	ImGui::TextUnformatted(T(TKEY("stat_profiling_hint"), "GPU and CPU timings per pass: Profiling menu, PBRWater::"));
	ImGui::Text(T(TKEY("stat_fetch"), "Fetch: %s, bathymetry: %s%s"), environment.GetFetch() ? ready : none, environment.GetBathymetry() ? ready : none, environment.IsBuilding() ? building : "");
	ImGui::Text(T(TKEY("stat_shore"), "Shore field: %s%s, swash: %s%s"), environment.GetShore() ? ready : none, environment.IsBuildingShore() ? building : "",
		frameData.Swash0.w > 0.5f ? running : idle, surfAsleep ? T(TKEY("stat_surf_asleep"), ", surf asleep") : "");
	const auto floatingStats = floating.GetStats();
	ImGui::Text(T(TKEY("stat_floating"), "Floating: %u hulls, %u attached, %u actors on board, %u small objects riding the water"), floatingStats.floaters, floatingStats.parts, floatingStats.carried, floatingStats.riders);
	ImGui::Text(T(TKEY("stat_hull_masks"), "Hull masks: %u hulls keep the water out (%u outlined from collision), %u drawn"), floatingStats.masks, floatingStats.measured,
		static_cast<uint32_t>(frameData.Hull0.x));
	ImGui::TextUnformatted(floating.GetSelectionReport().c_str());  // i18n-ignore: diagnostic report built from form names
}

// ============================================================================
// Resources
// ============================================================================

void PBRWater::SetupResources()
{
	gpuBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::Constants");
	{
		// Half floats: the profile is smooth, and the bilinear filter does the rest.
		const auto& profile = PBRWaterModel::GetShoreProfile();
		using Table = PBRWaterModel::ShoreProfileTable;
		std::vector<uint16_t> texels(profile.slice0.size() + profile.slice1.size());
		for (size_t i = 0; i < profile.slice0.size(); ++i) {
			texels[i] = DirectX::PackedVector::XMConvertFloatToHalf(profile.slice0[i]);
			texels[profile.slice0.size() + i] = DirectX::PackedVector::XMConvertFloatToHalf(profile.slice1[i]);
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = Table::Phases;
		desc.Height = Table::Stages;
		desc.MipLevels = 1;
		desc.ArraySize = 2;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const UINT pitch = Table::Phases * 4 * sizeof(uint16_t);
		D3D11_SUBRESOURCE_DATA init[2] = { { texels.data(), pitch, 0 }, { texels.data() + profile.slice0.size(), pitch, 0 } };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (SUCCEEDED(globals::d3d::device->CreateTexture2D(&desc, init, texture.put()))) {
			shoreProfileTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::ShoreProfile");
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.ArraySize = 2;
			shoreProfileTexture->CreateSRV(srvDesc);
		}
	}
	flatDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::FlatDrawConstants");
	riverLattice.Setup();
	offscreenDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::OffscreenDrawConstants");
	spacingDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::SpacingDrawConstants");
	swallowDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::SwallowDrawConstants");
	for (auto& buffer : gridDrawBuffers)
		buffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::GridDrawConstants");
	for (auto& buffer : patchDrawBuffers)
		buffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::GridPatchDrawConstants");
	projectedGrid.Setup();
	kelvinPattern = PBRWaterWake::CreatePattern();
	CompileUnderwater();
	underwaterCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<float4[5]>(), "PBRWater::UnderwaterCB");
	{
		D3D11_BLEND_DESC desc{};
		auto& target = desc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_SRC1_COLOR;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		if (SUCCEEDED(globals::d3d::device->CreateBlendState(&desc, underwaterBlend.put())))
			Util::SetResourceName(underwaterBlend.get(), "PBRWater::UnderwaterBlend");
	}
	if (!underwaterVS || !underwaterPS || !underwaterBlend)
		logger::warn("[PBR Water] The underwater volume's shaders failed; the game's underwater fog stays");
	deferredDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::DeferredDrawConstants");
	instancedDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::InstancedStencilConstants");
	stencilInstanceBuffer = std::make_unique<StructuredBuffer>(StructuredBufferDesc<float4>(MaxStencilInstances), MaxStencilInstances, "PBRWater::StencilInstances");
	stencilInstanceBuffer->CreateSRV();
	stencilUpload.assign(MaxStencilInstances, float4{});
	stencilInstances.reserve(MaxStencilInstances);
	deferredCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DeferredDrawCB>(), "PBRWater::DeferredDrawCB");
	deferredZeroCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DeferredDrawCB>(), "PBRWater::DeferredZeroCB");
	deferredZeroCB->Update(DeferredDrawCB{});

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
	spray.SetupResources();

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

	// The waves' heightfield for their sun shadows: the same grid, finer, drawn from above.
	{
		constexpr uint32_t verts = SunMapQuads + 1;
		std::vector<uint32_t> indices;
		indices.reserve(static_cast<size_t>(SunMapQuads) * SunMapQuads * 6);
		for (uint32_t y = 0; y < SunMapQuads; ++y) {
			for (uint32_t x = 0; x < SunMapQuads; ++x) {
				const uint32_t a = y * verts + x;
				const uint32_t b = a + 1;
				const uint32_t c = a + verts;
				const uint32_t e = c + 1;
				indices.insert(indices.end(), { a, c, b, b, c, e });
			}
		}
		D3D11_BUFFER_DESC indexDesc{};
		indexDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
		indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
		indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		const D3D11_SUBRESOURCE_DATA indexData{ indices.data(), 0, 0 };
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&indexDesc, &indexData, sunMapIndices.put()));
		Util::SetResourceName(sunMapIndices.get(), "PBRWater::WaveHeightfieldIndices");

		D3D11_TEXTURE2D_DESC texDesc{};
		texDesc.Width = SunMapSize;
		texDesc.Height = SunMapSize;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		sunMapTexture = std::make_unique<Texture2D>(texDesc, "PBRWater::WaveHeightfield");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		sunMapTexture->CreateSRV(srvDesc);
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		sunMapTexture->CreateDSV(dsvDesc);
	}
	ocean.SetupResources();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = SurfaceNoiseSize;
		desc.Height = SurfaceNoiseSize;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		noiseTexture = std::make_unique<Texture2D>(desc, "PBRWater::SurfaceNoise");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		noiseTexture->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		noiseTexture->CreateUAV(uavDesc);
	}
	CompileSurfaceNoise();

	// The foam texture takes a few hundred milliseconds to bake: off the render thread, uploaded when done.
	if (!foamTexture && !foamBake.valid())
		foamBake = std::async(std::launch::async, [] { return PBRWaterFoam::Bake(); });
}

void PBRWater::ClearShaderCache()
{
	CompileUnderwater();
	projectedGrid.CompileShaders();
	ripples.ClearShaderCache();
	swash.ClearShaderCache();
	spray.ClearShaderCache();
	CompileOccluder();
	CompileSurfaceNoise();
	ocean.ClearShaderCache();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);
}

void PBRWater::Reset()
{
	frameDataFrame = UINT32_MAX;
}

void PBRWater::UpdateFoamTexture()
{
	if (foamTexture || !foamBake.valid() || foamBake.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	std::vector<PBRWaterFoam::Level> levels;
	try {
		levels = foamBake.get();
	} catch (...) {
		logger::warn("[PBR Water] Failed to bake the foam texture; the water draws no foam pattern");
		return;
	}
	if (levels.empty())
		return;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = levels[0].size;
	desc.Height = levels[0].size;
	desc.MipLevels = static_cast<UINT>(levels.size());
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
	for (size_t i = 0; i < levels.size(); ++i) {
		init[i].pSysMem = levels[i].texels.data();
		init[i].SysMemPitch = levels[i].size;
	}
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, init.data(), texture.put()))) {
		logger::warn("[PBR Water] Failed to create the foam texture");
		return;
	}
	foamTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::FoamPattern");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	foamTexture->CreateSRV(srvDesc);
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

void PBRWater::UpdateTerrainTexture()
{
	const uint32_t generation = environment.GetBathymetryGeneration();
	if (generation == uploadedTerrainGeneration)
		return;
	uploadedTerrainGeneration = generation;

	auto grid = environment.GetBathymetry();
	uploadedTerrain = grid;
	terrainTexture.reset();
	if (!grid || !grid->Valid())
		return;

	// 16 bits over the land's range: about 2 cm, and half the memory of floats for a worldspace of 12 million vertices.
	std::vector<uint16_t> texels(grid->heights.size());
	const float scale = 65535.0f / std::max(grid->maxZ - grid->minZ, 1.0f);
	std::transform(grid->heights.begin(), grid->heights.end(), texels.begin(), [&](float z) { return static_cast<uint16_t>((z - grid->minZ) * scale + 0.5f); });

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = grid->width;
	desc.Height = grid->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init{ texels.data(), static_cast<UINT>(grid->width * sizeof(uint16_t)), 0 };
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
		logger::warn("[PBR Water] Failed to create the terrain texture");
		uploadedTerrain.reset();
		return;
	}
	terrainTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::Terrain");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	terrainTexture->CreateSRV(srvDesc);
}

// ============================================================================
// Per-frame (render thread)
// ============================================================================

uint32_t PBRWater::WaveResolutionSize() const
{
	return 128u << static_cast<uint32_t>(std::clamp(settings.WaveResolution, 0, 2));
}

std::vector<std::pair<std::string_view, std::string_view>> PBRWater::GetShaderDefineOptions()
{
	std::vector<std::pair<std::string_view, std::string_view>> result;
	if (settings.DebugView != 0 || settings.WireframeMode != 0)
		result.push_back({ "PBRW_DEBUG", "" });
	return result;
}

void PBRWater::Prepass()
{
	// Toggling a debug view (menu, settings load, reset, devbench) recompiles the water shaders with or without the debug code.
	const bool debugWanted = settings.DebugView != 0 || settings.WireframeMode != 0;
	if (debugWanted != debugCompiled) {
		debugCompiled = debugWanted;
		globals::shaderCache->Clear(RE::BSShader::Type::Water);
	}

	{
		PBRWaterProfiling::Scope pass("PBRWater::Uploads");
		UpdateFoamTexture();
		UpdateFetchTexture();
		UpdateTerrainTexture();
	}

	// Last frame's water draws leave our textures bound; the simulations below write them.
	{
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
		context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
		context->PSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
		context->PSSetShaderResources(WaveHeightfieldSlot, 1, nullSrvs);
	}
	drawResourcesFrame = UINT32_MAX;
	{
		PBRWaterProfiling::Scope pass("PBRWater::Uploads");
		UpdateShoreTextures();
	}

	const float dt = renderDelta.load(std::memory_order_acquire);
	if (const auto snap = snapshot.load(std::memory_order_acquire)) {
		OceanSimulation::Settings oceanSettings;
		oceanSettings.resolution = WaveResolutionSize();
		oceanSettings.deltaTime = dt;
		oceanSettings.foamMips = settings.EnableFoam;
		oceanSettings.foamBias = settings.FoamBias;
		oceanSettings.foamThreshold = 0.0f;
		oceanSettings.foamAdd = OceanFoamGrowth * settings.FoamAmount;
		oceanSettings.foamDecayRate = settings.FoamDecayRate;
		oceanSettings.cascadeMask = neededCascades.load(std::memory_order_relaxed);
		// Half-rate long waves: the two longest cascades alternate frames (even frames skip cascade 1, odd frames cascade 0).
		if (settings.LongWaveHalfRate)
			oceanSettings.cascadeMask &= ~(1u << ((globals::state->frameCount & 1u) == 0 ? 1u : 0u));
		ocean.Update(*snap, oceanSettings);
	}
	oceanReady.store(ocean.IsReady(), std::memory_order_release);

	if (settings.EnableRipples) {
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		RippleSimulation::Settings rippleSettings;
		rippleSettings.extent = settings.RippleExtent * UnitsPerMetre;
		rippleSettings.halfLife = settings.RippleHalfLife;
		rippleSettings.siltHalfLife = settings.SiltHalfLife;
		// The bed as uploaded for the last frame (this runs before UpdateFrameConstants).
		RippleSimulation::Environment rippleEnvironment;
		if (frameData.Terrain1.z > 0.5f && terrainTexture) {
			rippleEnvironment.terrain = terrainTexture->srv.get();
			rippleEnvironment.terrain0 = frameData.Terrain0;
			rippleEnvironment.terrain1 = frameData.Terrain1;
		}
		ripples.Update(cameraPos.x, cameraPos.y, dt, rippleSettings, rippleEnvironment);
	} else {
		PBRWaterProfiling::Skipped("PBRWater::RippleStep");
	}

	UpdateFrameConstants();
	UpdateSurfaceNoise();
	UpdateSwash(dt);
	UpdateSpray(dt);
	UpdateTessellation();
	ApplyCameraWater();
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
			// w: the band's slope variance, which a pixel covering most of a tile sees as roughness (EvaluateWavesFiltered).
			d.Cascade1[c] = { band.meanOmega, 1.0f / std::max(band.pmWeightOpenSea, 1e-4f), band.meanK / UnitsPerMetre, band.meanSquareSlope };
			// Height spread of the band (depth-limited breaking).
			d.Cascade2[c] = { std::sqrt(std::max(band.variance, 0.0f)) * UnitsPerMetre, 0.0f, 0.0f, 0.0f };
		}
		const bool active = ocean.IsReady() && !o.calm;
		d.Params0 = { active ? 1.0f : 0.0f, o.windSpeed, o.peakOmega, static_cast<float>(PBRWaterModel::Gravity) * UnitsPerMetre };
		d.Params1 = { std::cos(snap->windDirection), std::sin(snap->windDirection), settings.DisplacementFadeStart, settings.DisplacementFadeEnd };
		d.Params2 = { o.choppiness, 0.0f, active ? o.displacementBound : 0.0f, static_cast<float>(std::max(ocean.GetMipCount(), 1u) - 1) };
		d.RefCamPos = { static_cast<float>(snap->refX), static_cast<float>(snap->refY), static_cast<float>(snap->refZ), 0.0f };
		// Shore waves need the coastline maps the snapshot blends between on the GPU. The clock runs at this frame's
		// (eased) frequency, but each map's lag omega tau uses the frequency it was traced for, so easing the sea never
		// sweeps the crests by tau d(omega)/dt. A map is rebuilt when the two differ by more than a few percent.
		const auto& shore = snap->shore;
		const bool shoreReady = shore.enabled && snap->shoreField && shoreTextures.field == snap->shoreField;
		const bool blending = shoreReady && snap->shoreBlend < 1.0f && snap->shorePrevious && previousShoreTextures.field == snap->shorePrevious;
		d.Shore0 = { shoreReady ? 1.0f : 0.0f, blending ? snap->shoreBlend : 1.0f, 1.0f, shore.enabled ? shore.maxRunup : 0.0f };
		d.Shore1 = { shore.crestWobble, 0.0f, shore.swellHeight, shore.windSeaHeight };
		// The clock now and as last frame drew it (motion vectors); the first frame has no motion.
		const float shoreClock = static_cast<float>(shore.phase);
		const bool consecutive = lastFrame != UINT32_MAX && frame == lastFrame + 1 && frameData.Shore2.w > 0.0f;
		d.Shore2 = { shoreClock, consecutive ? frameData.Shore2.x : shoreClock, 0.0f, shore.period };
		d.Shore3 = { settings.SwashLinger, shore.whelps, settings.ShoreCurl, shore.deepLength };
		d.Shore4 = { shoreReady ? snap->shoreField->omega : shore.omega, blending ? snap->shorePrevious->omega : shore.omega, 0.0f, 0.0f };
		auto fieldMap = [](const PBRWaterModel::ShoreField& f) {
			return float4{ f.minX - 0.5f * f.step, f.minY - 0.5f * f.step, 1.0f / (static_cast<float>(f.width) * f.step), 1.0f / (static_cast<float>(f.height) * f.step) };
		};
		if (shoreReady) {
			d.ShoreMap = fieldMap(*snap->shoreField);
			if (blending)
				d.Shore6 = fieldMap(*snap->shorePrevious);
			// Last frame's wet and foam grids, until this frame's step (UpdateSwash) replaces them.
			d.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), settings.WetShores && swash.IsReady() ? 1.0f : 0.0f };
			d.Foam2 = { swash.GetFoamOriginX(), swash.GetFoamOriginY(), 1.0f / swash.GetFoamExtent(), settings.SurfQuality > 0 && swash.IsFoamReady() ? 1.0f : 0.0f };
		}

		// Fetch
		if (uploadedFetch && fetchTexture && snap->exterior) {
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

	// The worldspace's terrain: a texel per vertex, so texel (0, 0)'s centre lies on the grid's first vertex.
	if (snap && snap->exterior && uploadedTerrain && terrainTexture) {
		const auto& t = *uploadedTerrain;
		const float scaleX = 1.0f / (static_cast<float>(t.width) * t.stepX);
		const float scaleY = 1.0f / (static_cast<float>(t.height) * t.stepY);
		d.Terrain0 = { scaleX, scaleY, 0.5f / static_cast<float>(t.width) - t.minX * scaleX, 0.5f / static_cast<float>(t.height) - t.minY * scaleY };
		d.Terrain1 = { t.minZ, t.maxZ, 1.0f, t.stepX };
	}

	// The projected grid's spacing (PBRWater.hlsli GridSpacing): its cell on screen, at the internal render resolution so it
	// scales with upscalers, and a breaker patch's cells across as ProjectedGrid::Build sizes them.
	float renderHeight = static_cast<float>(globals::game::graphicsState->screenHeight);
	if (globals::features::upscaling.loaded)
		renderHeight *= globals::features::upscaling.resolutionScale.y;
	const float projY = std::abs(globals::game::frameBufferCached.GetCameraProj()._22);
	const float cellPixels = std::max(settings.GridCellSize, 2.0f);
	const float patchCells = std::clamp(std::floor(cellPixels / 2.0f), 1.0f, static_cast<float>(ProjectedGrid::Subdivision));
	d.Tess0 = { 0.0f, cellPixels, patchCells, 0.5f * renderHeight * projY };
	d.Draw0 = { 0.0f, static_cast<float>(settings.WireframeMode), static_cast<float>(settings.DebugView), 0.0f };
	// The loaded cells: their water meets the flat LOD water at the grid's border, so the waves fade out before it.
	// Read from the cells the grid holds, not TES::currentGridX/Y, which lag the grid.
	d.Grid0 = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (const auto tes = RE::TES::GetSingleton(); tes && tes->gridCells && snap && snap->exterior) {
		const auto* grid = tes->gridCells;
		int32_t lo[2] = { INT32_MAX, INT32_MAX };
		int32_t hi[2] = { INT32_MIN, INT32_MIN };
		auto scan = [&]() {
			for (uint32_t x = 0; x < grid->length; ++x) {
				for (uint32_t y = 0; y < grid->length; ++y) {
					auto* cell = grid->GetCell(x, y);
					const auto* coords = cell && cell->IsExteriorCell() ? cell->GetCoordinates() : nullptr;
					if (!coords)
						continue;
					lo[0] = std::min(lo[0], coords->cellX);
					lo[1] = std::min(lo[1], coords->cellY);
					hi[0] = std::max(hi[0], coords->cellX);
					hi[1] = std::max(hi[1], coords->cellY);
				}
			}
		};
		if (REL::Module::IsAE() && REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
			// Skyrim 1.7 guards the grid's cell lookups with this lock (as UnifiedWater does).
			static REL::Relocation<RE::BSReadWriteLock*> terrainCellMapLock{ REL::ID(564236) };
			const RE::BSReadLockGuard lock(*terrainCellMapLock);
			scan();
		} else {
			scan();
		}
		if (lo[0] <= hi[0])
			d.Grid0 = { static_cast<float>(lo[0]) * CellSize, static_cast<float>(lo[1]) * CellSize,
				static_cast<float>(hi[0] + 1) * CellSize, static_cast<float>(hi[1] + 1) * CellSize };
	}

	if (settings.EnableRipples && ripples.IsReady()) {
		d.Ripple0 = { ripples.GetOriginX(), ripples.GetOriginY(), 1.0f / ripples.GetExtent(), 1.0f };
		d.Ripple1 = { settings.RippleHeight * UnitsPerMetre, settings.RippleNormalStrength, 0.0f, 1.0f / RippleSimulation::GridSize };
		d.Ripple2 = { ripples.GetPreviousOriginX(), ripples.GetPreviousOriginY(), ripples.GetStepAlpha(), ripples.GetPreviousStepAlpha() };
	}

	d.Light0 = { settings.Roughness, settings.EnableSubsurface ? settings.Subsurface : 0.0f, settings.EnableEnvReflections ? 1.0f : 0.0f, settings.SunSpecular };
	// The surface's lighting inputs (Water.hlsl): set here, after the waves' constants, which share these vectors.
	d.Params2.y = settings.EnableSSR ? 1.0f : 0.0f;
	d.Shore4.z = settings.EnableSunShadows ? 1.0f : 0.0f;
	d.Shore4.w = settings.EnableSkylighting ? 1.0f : 0.0f;
	d.RefCamPos.w = settings.EnableFog ? 1.0f : 0.0f;
	d.Light1 = { settings.Visibility, settings.EnableFoam ? 1.0f : 0.0f, settings.RefractionDistortion, settings.EnablePointLightSpecular ? settings.PointLightSpecular : 0.0f };
	// The open sea's foam test, also applied to the shore waves' crests and folds (PBRWater::EvaluateWavesFiltered); z the
	// same Amount and Coverage as a gain on the shore's foam coverages, which have no Jacobian (1 at the defaults).
	d.Foam1 = { settings.FoamBias, settings.FoamFadeDistance, settings.FoamAmount * std::max(settings.FoamBias, 0.0f) / 0.85f, settings.PhysicalBodyColour ? 1.0f : 0.0f };

	// Water clarity and the light in the water.
	d.Clarity0 = { settings.SedimentDensity / UnitsPerMetre, settings.Turbidity, settings.TurbidityPatchiness, settings.TurbidityPatchSize * UnitsPerMetre };
	d.Clarity1 = { settings.SedimentColor.x, settings.SedimentColor.y, settings.SedimentColor.z, settings.ShoreResuspension };
	d.Clarity2 = { 0.0f, weatherTurbidity.load(std::memory_order_acquire), settings.EnableRipples ? settings.WadingSilt : 0.0f, settings.SedimentLayerHeight * UnitsPerMetre };
	// Sediment plumes drift and reshape over hours; a daily wrap keeps their coordinates precise.
	d.Optics0 = { settings.ScatteringAnisotropy, settings.DownwellingAttenuation, std::fmod(globals::state->timer, 86400.0f), (snap && snap->ocean) ? snap->ocean->significantHeight : 0.0f };

	// Wind and rain on the surface.
	d.Surface0 = { settings.WindRoughness, 0.0f, 0.0f, rainIntensity.load(std::memory_order_acquire) };
	{
		// The surface noise bake around the camera (UpdateSurfaceNoise): four texels across a cell of the sediment
		// patches' finest octave (their third, a quarter of their size), between 0.5 m and 16 m (640 m across at the
		// defaults). In whole texels, so the samples stay put as the camera moves. Only the sediment is in it: without
		// Water Clarity nothing is baked (Noise0.z = 0).
		const float finest = settings.TurbidityPatchSize / 4.12f * UnitsPerMetre;
		const float texel = std::clamp(finest / 4.0f, 0.5f * UnitsPerMetre, 16.0f * UnitsPerMetre);
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const float half = 0.5f * static_cast<float>(SurfaceNoiseSize);
		const float originX = (std::floor(cameraPos.x / texel) - half) * texel;
		const float originY = (std::floor(cameraPos.y / texel) - half) * texel;
		// w is set once this frame's bake has run.
		d.Noise0 = { originX, originY, settings.EnableClarity ? 1.0f / (texel * static_cast<float>(SurfaceNoiseSize)) : 0.0f, 0.0f };
	}
	// x: 0 no waves of any kind (the mesh stays flat and the pixels skip the wave program), 1 waves, 2 waves and the per-pixel waterline.
	const bool anyWaves = d.Params0.x > 0.5f || d.Shore0.x > 0.5f || d.Ripple0.w > 0.5f;
	d.Surface1 = { anyWaves ? (settings.EnableWaterline ? 2.0f : 1.0f) : 0.0f, settings.EnableClarity ? 1.0f : 0.0f, settings.FetchScale, settings.VanillaNormalMaps ? 0.0f : 1.0f };

	// The artistic scales the shaders read (PBRWater.hlsli Tune3..Tune5).
	// The actors' Kelvin wakes (GatherInteractions, main thread).
	d.WakeInfo = {};
	if (settings.EnableWakes && kelvinPattern) {
		if (const auto wakes = wakeSources.load(std::memory_order_acquire)) {
			const float gravity = static_cast<float>(9.81 * UnitsPerMetre);
			uint32_t count = 0;
			for (const auto& w : *wakes) {
				if (count >= PBRWaterWake::MaxSources || w.speed <= 0.0f)
					break;
				d.Wake0[count] = { w.x, w.y, gravity / (w.speed * w.speed), w.amplitude * settings.WakeStrength };
				d.Wake1[count] = { w.dirX, w.dirY, 0.0f, 0.0f };
				++count;
			}
			d.WakeInfo.x = static_cast<float>(count);
		}
	}
	d.Tune3 = { settings.RippleHorizDisplace, settings.RippleDisplaceClamp, settings.ShoreTrainCycles, 0.0f };
	d.Tune4 = { settings.ShoreWaveVariation, PBRWaterModel::ShoreCrestLength(settings.DirectionalSpread), settings.DetailNormalCalm, settings.DetailNormalGale };
	d.Tune5 = { settings.PointLightEmitterRadius, 0.0f, settings.FoamBubbleSize, 0.0f };
	d.Swash1 = { settings.SwashChurnHeight * UnitsPerMetre, settings.SwashChurnDepth, settings.SwashChurnSize * UnitsPerMetre, settings.SwashChurnSpeed };

	// The hulls the water is kept out of (PBRWater.hlsli InsideHull), nearest the camera first. Their centres relative to
	// the reference camera, like the cascades' tile coordinates, so every view reads them at full precision.
	if (settings.HullMasks) {
		const auto hulls = floating.GetHullMasks();
		const uint32_t count = std::min(static_cast<uint32_t>(hulls.size()), MaxHullMasks);
		for (uint32_t i = 0; i < count; ++i) {
			const auto& h = hulls[i];
			const float3 centre = { static_cast<float>(static_cast<double>(h.centre.x) - static_cast<double>(d.RefCamPos.x)),
				static_cast<float>(static_cast<double>(h.centre.y) - static_cast<double>(d.RefCamPos.y)),
				static_cast<float>(static_cast<double>(h.centre.z) - static_cast<double>(d.RefCamPos.z)) };
			float4* out = d.HullData + i * HullMaskStride;
			const RE::NiPoint3* axes[3] = { &h.axisX, &h.axisY, &h.axisZ };
			for (uint32_t row = 0; row < 3; ++row) {
				const auto& a = *axes[row];
				out[row] = { a.x, a.y, a.z, -(a.x * centre.x + a.y * centre.y + a.z * centre.z) };
			}
			out[3] = { h.bottom, h.top, h.front, h.back };
			for (uint32_t j = 0; j < FloatingObjects::HullStations; j += 4)
				out[4 + j / 4] = { h.width[j], h.width[j + 1], h.width[j + 2], h.width[j + 3] };
		}
		d.Hull0.x = static_cast<float>(count);
	}

	frameData = d;
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
	params.swellGamma = settings.SwellPeakedness;
	params.swellSpread = settings.SwellSpread;
	// Swell comes in from the open sea; there is none indoors.
	params.swellHeight = exterior ? settings.SwellHeight : 0.0f;
	params.swellPeriod = settings.SwellPeriod;
	params.swellDirection = params.windDirection + settings.SwellDirection * (TwoPi / 360.0f);
	params.resolution = WaveResolutionSize();
	return params;
}

// Cascades with any fetch-limited weight in the water within `range` of the eye (WaveSnapshot::BuildContext before depth and
// shoaling, so it keeps a cascade that shoaling would later thin out: conservative).
static uint32_t NeededCascadeMask(const PBRWaterModel::WaveSnapshot& snap, float eyeX, float eyeY, float range)
{
	constexpr int Grid = 5;
	const float windSpeed = snap.ocean->windSpeed;
	const float openPeak = snap.ocean->peakOmega;
	uint32_t mask = 0;
	for (int j = 0; j < Grid; ++j) {
		for (int i = 0; i < Grid; ++i) {
			const float x = eyeX + range * (2.0f * static_cast<float>(i) / (Grid - 1) - 1.0f);
			const float y = eyeY + range * (2.0f * static_cast<float>(j) / (Grid - 1) - 1.0f);
			const float fetchMetres = snap.fetch->SampleMetres(x, y, snap.windDirection) * snap.fetchScale;
			const float peakOmega = PBRWaterModel::FetchPeakOmega(fetchMetres, windSpeed, openPeak);
			const float energyGain = PBRWaterModel::FetchEnergyGain(fetchMetres, windSpeed);
			for (uint32_t c = 0; c < PBRWaterModel::NumCascades; ++c) {
				const auto& band = snap.ocean->cascades[c];
				const float r = peakOmega / band.meanOmega;
				const float r2 = r * r;
				const float weight = std::clamp(std::exp(-1.25f * r2 * r2) / std::max(band.pmWeightOpenSea, 1e-4f), 0.0f, 1.0f) * energyGain;
				if (weight > 0.01f)
					mask |= 1u << c;
			}
		}
	}
	return mask;
}

void PBRWater::MainThreadUpdate()
{
	ApplyUnderwaterFogOverride();
	auto player = RE::PlayerCharacter::GetSingleton();
	auto tes = globals::game::tes;
	if (!player || !tes)
		return;

	// Weather blends and typed-in values write straight into the settings; keep them safe (not within the sliders).
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
			// Sky::windAngle is the weather's wind direction (radians) from north, counter-clockwise: the engine's own
			// wind (AE 1.6.1170 0x140E13930, from TES's per-frame update) is speed * (-sin a, cos a), so as a maths
			// angle from +X it is a + pi/2.
			windAngle = sky->windAngle + 0.5f * std::numbers::pi_v<float>;
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
	auto generated = PBRWaterModel::GenerateSpectrum(CurrentSpectrumParams(smoothedWindSpeed, exterior));
	// Enable Ocean Waves off: calm skips the spectrum, FFT, mips and the gameplay mirror, and the shaders skip the cascades. The shore
	// train reads the spectrum's own parameters, not this flag.
	if (!settings.EnableOceanWaves)
		generated.calm = true;
	const auto spectrum = std::make_shared<const PBRWaterModel::OceanSpectrum>(std::move(generated));
	next->ocean = spectrum;
	next->windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	next->exterior = exterior;

	// Shoreline waves: the sea's dominant waves as a wave train, its clock carried on from last frame (it keeps running
	// while shore waves are off, so turning them on never starts every beach in step).
	PBRWaterModel::ShoreTuning shoreTuning;
	shoreTuning.shortestPeriod = settings.ShoreShortestPeriod;
	shoreTuning.longestPeriod = settings.ShoreLongestPeriod;
	shoreTuning.trainCycles = settings.ShoreTrainCycles;
	shoreTuning.setVariation = settings.ShoreWaveVariation;
	shoreTuning.crestLength = PBRWaterModel::ShoreCrestLength(settings.DirectionalSpread);
	shoreTuning.crestWobble = settings.ShoreCrestWobble;
	shoreTuning.whelps = settings.ShoreBoreWhelps;
	// After a settings change the train starts over from the new settings (UpdateShoreReset): no easing towards the new
	// period, and its clock carries on at the new rate, so nothing pauses or races while the old state catches up.
	const bool resetShore = UpdateShoreReset(dt);
	const bool hadShoreState = hasShoreState;
	const double previousClock = shoreState.phase;
	shoreState = PBRWaterModel::UpdateShoreParams(*spectrum, hasShoreState && !resetShore ? &shoreState : nullptr, dt, shoreTuning);
	if (resetShore && hadShoreState)
		shoreState.phase = std::fmod(previousClock + static_cast<double>(shoreState.rate) * dt, PBRWaterModel::TwoPi * PBRWaterModel::ShoreClockCycles);
	hasShoreState = true;
	const bool shoreEnabled = exterior && settings.EnableShoreWaves;

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
	next->fetch = environment.GetFetch();
	next->fetchScale = settings.FetchScale;
	if (exterior)
		next->bathymetry = environment.GetBathymetry();
	environment.UpdateShore(eye.x, eye.y, shoreState, environment.GetFetch(), settings.FetchScale, shoreEnabled);
	UpdateShoreField(dt, shoreEnabled);
	next->shore = shoreState;
	next->shore.enabled = shoreEnabled;
	next->shoreField = currentShore;
	next->shorePrevious = previousShore;
	next->shoreBlend = shoreBlend;

	// Cascades nothing near the camera can see are skipped on the GPU. Sampled every 10th call; without a fetch or ocean, all are needed.
	if (cascadeMaskCalls++ % 10 == 0) {
		const bool sampled = next->ocean && next->fetch;
		neededCascades.store(sampled ? NeededCascadeMask(*next, eye.x, eye.y, settings.DisplacementFadeEnd) : 0xF, std::memory_order_relaxed);
	}

	// Surf sleep: every 15th frame, look for a breaking beach within the surf grid around the camera. Frames between
	// still add up the time, so the surf sleeps after 3 s without one.
	static uint32_t surfProbeCalls = 0;
	static float surfProbeElapsed = 0.0f;
	surfProbeElapsed += dt;
	if (!shoreEnabled || !next->shoreField || !next->shoreField->Valid()) {
		surfNearby.store(true, std::memory_order_release);
		surfIdleTime = 0.0f;
		surfProbeElapsed = 0.0f;
	} else if (++surfProbeCalls % 15 == 0) {
		const auto& field = *next->shoreField;
		// The still water at the nearest texel of the field (NaN: no water there).
		auto levelAt = [&field](float x, float y) {
			const float ix = std::round((x - field.minX) / field.step);
			const float iy = std::round((y - field.minY) / field.step);
			if (ix < 0.0f || iy < 0.0f || ix > static_cast<float>(field.width - 1) || iy > static_cast<float>(field.height - 1))
				return std::numeric_limits<float>::quiet_NaN();
			return field.level[static_cast<size_t>(iy) * field.width + static_cast<size_t>(ix)];
		};
		const float half = ShoreSimulation::FoamExtent * 0.5f;
		bool beach = false;
		// 11 m apart: the beach share runs out along the rays over the whole breaking zone, wider than that.
		for (int j = 0; j < 17 && !beach; ++j) {
			for (int i = 0; i < 17 && !beach; ++i) {
				const float x = eye.x - half + 2.0f * half * static_cast<float>(i) / 16.0f;
				const float y = eye.y - half + 2.0f * half * static_cast<float>(j) / 16.0f;
				if (next->ShoreAt(x, y).weight <= 0.0f)
					continue;
				const float level = levelAt(x, y);
				if (!std::isfinite(level) || level < -1.0e6f)  // < -1e6: no water (ShoreField::level)
					continue;
				if (next->BuildContext(x, y, level).train.beach > 0.01f)
					beach = true;
			}
		}
		surfIdleTime = beach ? 0.0f : surfIdleTime + surfProbeElapsed;
		surfProbeElapsed = 0.0f;
		surfNearby.store(surfIdleTime < 3.0f, std::memory_order_release);
	}

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
	// The hulls are also found for their masks alone, when they do not float.
	floatingSettings.enabled = (settings.EnableFloatingObjects || settings.HullMasks) && globals::shaderCache->IsEnabled();
	floatingSettings.move = settings.EnableFloatingObjects;
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
	// Seconds a new coastline map takes to take over: long enough that a map traced for a new period or moving with the
	// player only makes the crests drift a little for a moment, never jump.
	constexpr float BlendTime = 4.0f;
	const auto latest = enabled ? environment.GetShore() : nullptr;
	if (latest != currentShore) {
		// After a reset the new field takes over at once: it was traced for other settings, so blending it in would
		// make the crests stall or race.
		previousShore = latest && currentShore && !shoreHardSwap ? currentShore : nullptr;
		shoreBlend = previousShore ? 0.0f : 1.0f;
		currentShore = latest;
		if (latest)
			shoreHardSwap = false;
	}
	shoreBlend = std::min(shoreBlend + dt / BlendTime, 1.0f);
	if (shoreBlend >= 1.0f)
		previousShore = nullptr;
}

std::array<float, 21> PBRWater::ShoreSignature() const
{
	const auto& s = settings;
	return { s.EnableShoreWaves ? 1.0f : 0.0f, s.ShoreShortestPeriod, s.ShoreLongestPeriod, 0.0f, s.ShoreTrainCycles,
		s.ShoreWaveVariation, s.DirectionalSpread, s.ShoreCrestWobble, s.ShoreBoreWhelps, s.ShoreCurl, s.SwashLinger, s.FetchScale,
		s.SwellHeight, s.SwellPeriod, s.SwellDirection, s.SwellSpread, s.SwellPeakedness, s.WaveHeight, s.WindSpeedCalm,
		s.WindSpeedStorm, 0.0f };
}

bool PBRWater::UpdateShoreReset(float dt)
{
	// A slider sends a new value every frame while it is dragged: the reset waits until the settings hold still.
	constexpr float SettleTime = 0.3f;
	const auto signature = ShoreSignature();
	if (!hasShoreSignature) {
		shoreSignature = signature;
		hasShoreSignature = true;
		return false;
	}
	if (signature != shoreSignature) {
		shoreSignature = signature;
		shoreResetDelay = SettleTime;
		return false;
	}
	if (shoreResetDelay < 0.0f)
		return false;
	shoreResetDelay -= dt;
	if (shoreResetDelay >= 0.0f)
		return false;

	// Trace a new coastline map for the new waves now, hand over to it without blending, and forget where the swash
	// reached under the old settings.
	environment.InvalidateShore();
	shoreHardSwap = true;
	previousShore = nullptr;
	shoreBlend = 1.0f;
	swashResetRequested.store(true, std::memory_order_release);
	logger::debug("[PBR Water] Shore settings changed: shore waves reset");
	return true;
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
	if (!exterior || !globals::shaderCache->IsEnabled())
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
	// The water level the camera is under is the displaced surface above it, not the flat plane: the underwater
	// view and the waterline across the lens follow the waves. Out of their reach the flat plane, but still decided
	// here, by the camera (TESWaterSystem_SetUnderwater).
	float surface = flat;
	if (std::abs(eye.z - flat) <= snap.VerticalBound() + 32.0f)
		surface += snap.SampleAt(eye.x, eye.y, flat).height;
	cameraSurface.store(surface, std::memory_order_release);
	cameraSurfaceFrame.store(globals::state->frameCount, std::memory_order_release);
	cameraSurfaceValid.store(true, std::memory_order_release);
	// Through the engine's setter (the original): it also updates the global copy the water shaders' technique follows.
	TESWaterSystem_SetUnderwater::func(waterSystem, eye.z < surface, surface);
}

int PBRWater::EffectiveEngineReflections() const
{
	int mode = settings.EngineReflections;
	// Without Dynamic Cubemaps the water reflects the engine's cubemap itself; Cloud Shadows draws its clouds into it.
	if (!globals::features::dynamicCubemaps.loaded)
		return 0;
	if (mode == 2 && globals::features::cloudShadows.loaded)
		mode = 1;
	return mode;
}

bool PBRWater::TESWaterReflections_UpdateActor::thunk(RE::TESWaterReflections* reflections)
{
	auto& feature = globals::features::pbrWater;
	const int mode = feature.loaded ? feature.EffectiveEngineReflections() : 0;
	if (mode == 0 || !reflections)
		return func(reflections);
	using Flags = RE::TESWaterReflections::Flags;
	if (mode == 2) {
		// The engine skips this frame's render and clears the flag itself (as it does for static cubemaps).
		reflections->flags.set(Flags::kDontUpdate);
		return func(reflections);
	}
	// Sky only, for this call: the scene bits are put back so the setting can be switched off at any time.
	constexpr auto scene = static_cast<uint16_t>(std::to_underlying(Flags::kLODScene) | std::to_underlying(Flags::kFullScene) | std::to_underlying(Flags::kLand) |
												 std::to_underlying(Flags::kExplosions) | std::to_underlying(Flags::kSilhouette) | std::to_underlying(Flags::kSelective));
	const uint16_t original = reflections->flags.underlying();
	reflections->flags = static_cast<Flags>(original & ~scene);
	const bool result = func(reflections);
	reflections->flags = static_cast<Flags>((reflections->flags.underlying() & ~scene) | (original & scene));
	return result;
}

void PBRWater::TESWaterSystem_SetUnderwater::thunk(RE::TESWaterSystem* waterSystem, bool underwater, float height)
{
	// Out of the engine's own calls, the camera decides, against the displaced surface over it.
	auto& feature = globals::features::pbrWater;
	if (feature.cameraSurfaceValid.load(std::memory_order_acquire) &&
		globals::state->frameCount - feature.cameraSurfaceFrame.load(std::memory_order_acquire) <= 1) {
		height = feature.cameraSurface.load(std::memory_order_acquire);
		underwater = Util::GetMainThreadEyePosition().z < height;
	}
	func(waterSystem, underwater, height);
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

RippleSimulation::WaterLevels PBRWater::GatherWaterLevels(const RE::NiPoint3& eye, bool exterior) const
{
	// The still water the ripple grid is for, per exterior cell around the eye (as the Skylighting occluder reads it):
	// the bed's depth in RippleSimCS is this level minus the terrain. The cell under the eye uses the water height at the
	// eye itself, which also follows a river or lake placed at another height than the cell's default.
	RippleSimulation::WaterLevels levels;
	auto* tes = globals::game::tes;
	if (!tes || !exterior)
		return levels;
	constexpr int32_t Half = RippleSimulation::LevelCells / 2;
	const int32_t eyeCellX = static_cast<int32_t>(std::floor(eye.x / RippleSimulation::CellSize));
	const int32_t eyeCellY = static_cast<int32_t>(std::floor(eye.y / RippleSimulation::CellSize));
	levels.baseCellX = eyeCellX - Half;
	levels.baseCellY = eyeCellY - Half;
	for (int32_t j = 0; j < RippleSimulation::LevelCells; ++j) {
		for (int32_t i = 0; i < RippleSimulation::LevelCells; ++i) {
			float level = -1.0e9f;
			const RE::NiPoint3 centre{ (static_cast<float>(levels.baseCellX + i) + 0.5f) * RippleSimulation::CellSize, (static_cast<float>(levels.baseCellY + j) + 0.5f) * RippleSimulation::CellSize, eye.z };
			if (auto* cell = tes->GetCell(centre)) {
				if (i == Half && j == Half) {
					float atEye = 0.0f;
					if (TESObjectCELL_GetWaterHeight::func(cell, eye, atEye) && atEye > -1.0e6f)
						level = atEye;
				}
				if (level < -1.0e8f) {
					if (const float height = cell->GetExteriorWaterHeight(); height > -1.0e6f)
						level = height;
				}
			}
			levels.level[i + RippleSimulation::LevelCells * j] = level;
		}
	}
	levels.valid = true;
	return levels;
}

void PBRWater::GatherInteractions(const PBRWaterModel::WaveSnapshot& snap, float dt)
{
	const bool wantRipples = settings.EnableRipples;

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
	// undisplaced positions, so a body is placed where the water under it rests: its rings then ride the
	// waves with the water instead of sliding across them. Its motion is measured relative to the
	// water too, so a body bobbing with the waves makes no ripples, while one moving through them does.
	std::unordered_map<const void*, SourceTrack> tracks;
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
		// A splash: hitting the water fast from above digs a crater.
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
		sources.push_back(s);
	};

	// Silt kicked up by feet moving over the bed, released at the start of the next simulation steps.
	constexpr float siltRate = 0.06f;
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

	// Kelvin wakes: actors moving through the water, nearest the camera first.
	std::vector<PBRWaterWake::Source> wakes;

	// Actors: every collision shape (legs, torso, ...) that crosses the surface makes its own ripple.
	if (wantRipples || settings.EnableWakes) {
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
			// A Kelvin wake from a body moving through the water at the surface: swimming, wading at least knee deep, a horse
			// fording. Its height is empirical: a few centimetres for a swimmer at a walk, growing with the hull's draft
			// and its speed (Wake Strength scales it).
			if (settings.EnableWakes) {
				RE::NiPoint3 velocity;
				actor->GetLinearVelocity(velocity);
				const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
				const float draft = flatZ + wave.height - pos.z;  // the water above the feet
				const float bodyHeight = std::max(actor->GetHeight(), 1.0f);
				if (speed > 0.4f * UnitsPerMetre && draft > 0.3f * UnitsPerMetre && draft < bodyHeight + 0.5f * UnitsPerMetre) {
					PBRWaterWake::Source w;
					w.x = pos.x;
					w.y = pos.y;
					w.dirX = velocity.x / speed;
					w.dirY = velocity.y / speed;
					w.speed = speed;
					const float immersed = std::min(draft, bodyHeight) / UnitsPerMetre;
					w.amplitude = 0.05f * UnitsPerMetre * std::min(immersed, 1.5f) * std::min(speed / (1.5f * UnitsPerMetre), 2.0f);
					wakes.push_back(w);
				}
			}
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
	if (player) {
		globals::game::tes->ForEachReferenceInRange(player, range, [&](RE::TESObjectREFR* ref) {
			if (!ref || ref->IsDisabled() || ref->IsDeleted() || ref->As<RE::Actor>() || !ref->Is3DLoaded())
				return RE::BSContainer::ForEachResult::kContinue;
			auto root = ref->Get3D();
			RE::bhkNiCollisionObject* niCollision = root ? root->GetCollisionObject() : nullptr;
			if (!niCollision)
				return RE::BSContainer::ForEachResult::kContinue;

			auto bhkBody = niCollision->body.get() ? niCollision->body.get()->AsBhkRigidBody() : nullptr;
			auto body = bhkBody ? bhkBody->GetRigidBody() : nullptr;
			if (!body || !IsDynamicMotion(body) || IsConstrained(body))
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

			if (wantRipples)
				addSource(niCollision, center, radius, flatZ, wave, center);

			if (dt > 0.0f) {
				// The engine's own buoyancy floats objects on the flat plane (and lets heavy ones sink).
				// Add only the water's orbital motion, as a change in velocity relative to what was added
				// last frame: floating objects ride the waves, sinking ones keep sinking, and nothing is
				// pinned to the surface.
				const float strength = std::clamp(settings.BuoyancyStrength, 0.0f, 3.0f) / 3.0f;
				// Props are denser than the water parcels around them, so they lag its rise (0.75), and nothing is
				// lifted once it rides above the surface.
				const float surface = flatZ + wave.height;
				const float lift = center.z > surface ? std::min(wave.verticalVelocity, 0.0f) : wave.verticalVelocity * 0.75f;
				const RE::NiPoint3 target{ wave.velocityX * strength, wave.velocityY * strength, lift * strength };
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
	std::sort(wakes.begin(), wakes.end(), [&](const PBRWaterWake::Source& a, const PBRWaterWake::Source& b) {
		return (a.x - eye.x) * (a.x - eye.x) + (a.y - eye.y) * (a.y - eye.y) < (b.x - eye.x) * (b.x - eye.x) + (b.y - eye.y) * (b.y - eye.y);
	});
	if (wakes.size() > PBRWaterWake::MaxSources)
		wakes.resize(PBRWaterWake::MaxSources);
	wakeSources.store(std::make_shared<const std::vector<PBRWaterWake::Source>>(std::move(wakes)), std::memory_order_release);
	if (wantRipples) {
		ripples.SubmitSources(std::move(sources));
		ripples.SubmitWaterLevels(GatherWaterLevels(eye, snap.exterior));
	}
}

// ============================================================================
// Hooks
// ============================================================================

void PBRWater::PostPostLoad()
{
	// Unified Water places the water (one mesh everywhere, no LOD water): the shading assumes it and skips the vanilla paths it replaces.
	if (!globals::features::unifiedWater.loaded) {
		failedLoadedMessage = T("feature.pbr_water.unified_water_required", "Unified Water is required for PBR Water to function.");
		loaded = false;
		logger::warn("[PBR Water] {}", failedLoadedMessage);
		return;
	}

	// Without the vanilla displacement (wading) mesh: it is a second water surface around the player
	// that cannot follow the waves. Its job is done by the ripple simulation on the real surface.
	stl::detour_thunk<TESWaterSystem_InitializeWater>(REL::RelocationID(31388, 32179));

	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x2, BSWaterShader_SetupTechnique>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x3, BSWaterShader_RestoreTechnique>(RE::VTABLE_BSWaterShader[0]);

	stl::detour_thunk<TESObjectCELL_GetWaterHeight>(REL::RelocationID(18543, 19002));
	stl::detour_thunk<TESWaterReflections_UpdateActor>(REL::RelocationID(31373, 32160));
	// SE 31409 sits with its neighbours AddWater (31406 / 32213) and AddRipple (31410 / 32217).
	stl::detour_thunk<TESWaterSystem_SetUnderwater>(REL::RelocationID(31409, 32216));

	// Same Main::Update call site Grass Collision uses; write_thunk_call chains with it.
	stl::write_thunk_call<MainUpdate>(REL::RelocationID(35565, 36564).address() + Util::VersionedRelocation::Select(0x748, 0xC26, 0xC38));

	// The batch renderer's passes (the call sites Light Limit Fix hooks; each chains the hook already there): Unified Water's
	// far stencil tiles become one instanced draw.
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(REL::RelocationID(100877, 107673).address() + REL::Relocate(0x1E5, 0x1EE));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(REL::RelocationID(100871, 107667).address() + REL::Relocate(0xEE, 0xED));

	// The world's effects pass (Frame Annotations' "Effects"); Detours chains with its detour.
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));

	// The engine's ripples (list and rain meshes): the ripple simulation replaces them.
	PBRWaterEngine::Install();

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
	auto& feature = globals::features::pbrWater;
	const auto start = std::chrono::steady_clock::now();
	feature.MainThreadUpdate();
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	// About a second of smoothing at 60 Hz; the peak decays over a few seconds.
	feature.mainUpdateMs.store(feature.mainUpdateMs.load(std::memory_order_relaxed) * 0.98f + ms * 0.02f, std::memory_order_relaxed);
	feature.mainUpdatePeakMs.store(std::max(feature.mainUpdatePeakMs.load(std::memory_order_relaxed) * 0.995f, ms), std::memory_order_relaxed);
}

bool PBRWater::BSWaterShader_SetupTechnique::thunk(RE::BSShader* waterShader, uint32_t technique)
{
	// The tiles collected under the last technique are drawn with its state, before this one replaces it.
	globals::features::pbrWater.FlushStencilInstances();
	// After the original: its BeginTechnique sets the descriptors the batch is named by. Before any draw of the batch, so
	// the batch's pass encloses every draw's own markers (Frame Annotations) instead of interleaving with them.
	const bool ok = func(waterShader, technique);
	if (!ok)
		return false;
	// The surface seen from below (Underwater) draws too: Water.hlsl's PBR branch, and its batch draws the underwater volume.
	globals::features::pbrWater.SetupBatch();
	return ok;
}

void PBRWater::BSWaterShader_RestoreTechnique::thunk(RE::BSShader* waterShader, uint32_t technique)
{
	globals::features::pbrWater.FlushStencilInstances();
	globals::features::pbrWater.stencilBatch.ready = false;
	func(waterShader, technique);
	globals::features::pbrWater.EndDrawBatch();
}

template <int N>
void PBRWater::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags)
{
	auto& feature = globals::features::pbrWater;
	if (feature.CollectStencilTile(pass, technique) || feature.SkipGridPass(pass))
		return;
	// Anything else the engine draws may change the state the collected tiles are drawn with.
	feature.FlushStencilInstances();
	func(pass, technique, alphaTest, renderFlags);
	feature.AfterEnginePass(pass, technique);
}

void PBRWater::BSShaderAccumulator_RenderEffects::thunk(RE::BSShaderAccumulator* accumulator, uint32_t renderFlags)
{
	// The main view's world pass only: not the reflections, the cubemaps or the first person view.
	const auto* state = globals::state;
	if (state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		globals::features::pbrWater.DrawSpray();
	func(accumulator, renderFlags);
}

#undef I18N_KEY_PREFIX
