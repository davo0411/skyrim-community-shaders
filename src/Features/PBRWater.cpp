#include "PBRWater.h"
#include "PBRWater/Profiling.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
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

// NLOHMANN_JSON_PASTE takes at most 63 arguments: the fields go in runs.
void to_json(nlohmann::json& nlohmann_json_j, const PBRWater::Settings& nlohmann_json_t)
{
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		TessellationTriangleSize,
		WindSpeedCalm,
		WindSpeedStorm,
		WaveHeight,
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		SwellPeakedness,
		SwellSpread,
		Choppiness,
		DirectionalSpread,
		FetchScale,
		ShoreShortestPeriod,
		ShoreLongestPeriod,
		ShoreTrainCycles,
		ShoreWaveVariation,
		ShoreResidualFoam,
		ShoreFoamLifetime,
		ShoreCrestWobble,
		ShoreBoreWhelps,
		ShoreCurl,
		ShoreCurlDetail,
		SwashLinger,
		SwashChurnHeight,
		SwashChurnDepth,
		SwashChurnSize,
		SwashChurnSpeed,
		Roughness,
		SunSpecular,
		PointLightSpecular,
		PointLightEmitterRadius,
		Subsurface,
		VanillaFresnel,
		RefractionDistortion,
		WindRoughness,
		VanillaNormalMaps,
		DetailNormalCalm,
		DetailNormalGale,
		Visibility,
		Turbidity,
		SedimentDensity))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		TurbidityPatchiness,
		TurbidityPatchSize,
		ShoreResuspension,
		RiverTurbidity,
		StormTurbidity,
		WadingSilt,
		SedimentLayerHeight,
		SiltHalfLife,
		ScatteringAnisotropy,
		DownwellingAttenuation,
		FoamAmount,
		FoamBias,
		FoamThreshold,
		FoamAdd,
		FoamDecayRate,
		FoamOffset0,
		FoamOffset1,
		FoamOffset2,
		FoamOffset3,
		FoamCascadeWeightFull,
		FoamDepthFalloff,
		FoamFadeDistance,
		FoamRoughnessModifier,
		FoamFeather,
		FoamBubbleSize,
		RippleExtent,
		RippleHeight,
		RippleNormalStrength,
		RippleHalfLife,
		RippleHorizDisplace,
		RippleDisplaceClamp,
		RiverCurrentSpeed,
		RiverWaveDamping,
		FlowPeriodScale,
		FlowPeriodMin,
		FlowPeriodMax,
		BuoyancyStrength,
		FloatingRange,
		MaxFloatingSize,
		FloatingResponse))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		TessellationMaxFactor,
		AdaptiveTessellation,
		TessellationErrorPixels,
		TessellationMaxCoarsening,
		CheapOffscreenWater,
		EngineReflections,
		TriangleBudget,
		LongWaveHalfRate,
		DisplacementFadeStart,
		DisplacementFadeEnd,
		EnableTessellation,
		DeferredSurface,
		DistantHalfRate,
		DistantHalfRateDistance,
		WaveResolution,
		EnableShoreWaves,
		WetShores,
		SurfQuality,
		EnableOceanWaves,
		EnableWaveShadows,
		EnablePointLightSpecular,
		EnableSkyOcclusion,
		EnableClarity,
		EnableSubsurface,
		EnableFoam,
		SedimentColor,
		EnableRipples,
		GameplayWaves,
		EnableFloatingObjects,
		CarryActors,
		EnableSpray,
		SprayAmount,
		SprayRange,
		SprayQuality,
		WireframeMode,
		DebugView))
	nlohmann_json_j["SprayEmitters"] = nlohmann_json_t.SprayEmitters;
}

void from_json(const nlohmann::json& nlohmann_json_j, PBRWater::Settings& nlohmann_json_t)
{
	const PBRWater::Settings nlohmann_json_default_obj{};
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		TessellationTriangleSize,
		WindSpeedCalm,
		WindSpeedStorm,
		WaveHeight,
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		SwellPeakedness,
		SwellSpread,
		Choppiness,
		DirectionalSpread,
		FetchScale,
		ShoreShortestPeriod,
		ShoreLongestPeriod,
		ShoreTrainCycles,
		ShoreWaveVariation,
		ShoreResidualFoam,
		ShoreFoamLifetime,
		ShoreCrestWobble,
		ShoreBoreWhelps,
		ShoreCurl,
		ShoreCurlDetail,
		SwashLinger,
		SwashChurnHeight,
		SwashChurnDepth,
		SwashChurnSize,
		SwashChurnSpeed,
		Roughness,
		SunSpecular,
		PointLightSpecular,
		PointLightEmitterRadius,
		Subsurface,
		VanillaFresnel,
		RefractionDistortion,
		WindRoughness,
		VanillaNormalMaps,
		DetailNormalCalm,
		DetailNormalGale,
		Visibility,
		Turbidity,
		SedimentDensity))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		TurbidityPatchiness,
		TurbidityPatchSize,
		ShoreResuspension,
		RiverTurbidity,
		StormTurbidity,
		WadingSilt,
		SedimentLayerHeight,
		SiltHalfLife,
		ScatteringAnisotropy,
		DownwellingAttenuation,
		FoamAmount,
		FoamBias,
		FoamThreshold,
		FoamAdd,
		FoamDecayRate,
		FoamOffset0,
		FoamOffset1,
		FoamOffset2,
		FoamOffset3,
		FoamCascadeWeightFull,
		FoamDepthFalloff,
		FoamFadeDistance,
		FoamRoughnessModifier,
		FoamFeather,
		FoamBubbleSize,
		RippleExtent,
		RippleHeight,
		RippleNormalStrength,
		RippleHalfLife,
		RippleHorizDisplace,
		RippleDisplaceClamp,
		RiverCurrentSpeed,
		RiverWaveDamping,
		FlowPeriodScale,
		FlowPeriodMin,
		FlowPeriodMax,
		BuoyancyStrength,
		FloatingRange,
		MaxFloatingSize,
		FloatingResponse))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		TessellationMaxFactor,
		AdaptiveTessellation,
		TessellationErrorPixels,
		TessellationMaxCoarsening,
		CheapOffscreenWater,
		EngineReflections,
		TriangleBudget,
		LongWaveHalfRate,
		DisplacementFadeStart,
		DisplacementFadeEnd,
		EnableTessellation,
		DeferredSurface,
		DistantHalfRate,
		DistantHalfRateDistance,
		WaveResolution,
		EnableShoreWaves,
		WetShores,
		SurfQuality,
		EnableOceanWaves,
		EnableWaveShadows,
		EnablePointLightSpecular,
		EnableSkyOcclusion,
		EnableClarity,
		EnableSubsurface,
		EnableFoam,
		SedimentColor,
		EnableRipples,
		GameplayWaves,
		EnableFloatingObjects,
		CarryActors,
		EnableSpray,
		SprayAmount,
		SprayRange,
		SprayQuality,
		WireframeMode,
		DebugView))
	// Emitter by emitter over the defaults: a file with fewer, or with missing values, keeps the rest. An emitter saved for
	// another layout or emission (SprayParticles::EmitterVersion, stored in each: the saved settings are merged over the
	// defaults, which would lend a top-level version to an old file) starts over from its default.
	nlohmann_json_t.SprayEmitters = SprayParticles::DefaultEmitters();
	if (const auto it = nlohmann_json_j.find("SprayEmitters"); it != nlohmann_json_j.end() && it->is_array()) {
		for (size_t i = 0; i < std::min<size_t>(it->size(), SprayParticles::EmitterCount); ++i) {
			const auto& saved = (*it)[i];
			const auto version = saved.is_object() ? saved.find("Version") : saved.end();
			if (version != saved.end() && version->is_number_integer() && version->get<int>() == SprayParticles::EmitterVersion)
				from_json(saved, nlohmann_json_t.SprayEmitters[i]);
		}
	}
}

namespace
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float CellSize = static_cast<float>(PBRWaterModel::CellSize);
	constexpr float InteriorFetchMetres = 50.0f;
	constexpr float TwoPi = static_cast<float>(PBRWaterModel::TwoPi);

	using SC = SIE::ShaderCache;

	/// Profiler names of the water draw batches by technique (DrawPassIndex): the surface, its point light passes, then
	/// Underwater, LOD, Stencil and Simple; in the world and off it (reflections, cubemaps, the map).
	constexpr const char* DrawPassNames[2][6] = {
		{ "PBRWater::Draw Surface", "PBRWater::Draw Point Lights", "PBRWater::Draw Underwater", "PBRWater::Draw LOD", "PBRWater::Draw Stencil", "PBRWater::Draw Simple" },
		{ "PBRWater::Draw Surface (Offscreen)", "PBRWater::Draw Point Lights (Offscreen)", "PBRWater::Draw Underwater (Offscreen)", "PBRWater::Draw LOD (Offscreen)", "PBRWater::Draw Stencil (Offscreen)", "PBRWater::Draw Simple (Offscreen)" },
	};

	/// Row of DrawPassNames for a water technique (WaterTechnique): 0 the surface, 1-7 its point light passes, 8-11 the rest.
	uint32_t DrawPassIndex(uint32_t technique)
	{
		if (technique == 0)
			return 0;
		if (technique < 8)
			return 1;
		return technique <= 11 ? technique - 6 : 0;
	}

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
	// Safety bounds only, not the sliders' ranges: Ctrl+click may type any value past a slider's end and it must stick
	// (this runs every frame). Only what would break the maths is held back: non-finite values, divisors and sizes that
	// must stay positive, factors that only mean something in 0..1, and the tessellation's limits.
	auto& s = settings;
	constexpr float Huge = 1.0e6f;
	auto clamp = [](float& value, float lo, float hi, float fallback) {
		value = std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
	};
	static const Settings d{};  // the defaults, built once (this runs every frame)
	clamp(s.TessellationTriangleSize, 1.0f, Huge, d.TessellationTriangleSize);
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
	clamp(s.ShoreResidualFoam, 0.0f, 1.0f, d.ShoreResidualFoam);
	clamp(s.ShoreFoamLifetime, 0.5f, 120.0f, d.ShoreFoamLifetime);
	clamp(s.ShoreCrestWobble, 0.0f, 2.0f, d.ShoreCrestWobble);
	clamp(s.ShoreBoreWhelps, 0.0f, 2.0f, d.ShoreBoreWhelps);
	clamp(s.ShoreCurl, 0.0f, 2.0f, d.ShoreCurl);
	clamp(s.ShoreCurlDetail, 1.0f, 4.0f, d.ShoreCurlDetail);
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
	clamp(s.VanillaFresnel, 0.0f, 1.0f, d.VanillaFresnel);
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
	clamp(s.RiverTurbidity, 0.0f, Huge, d.RiverTurbidity);
	clamp(s.StormTurbidity, 0.0f, Huge, d.StormTurbidity);
	clamp(s.WadingSilt, 0.0f, Huge, d.WadingSilt);
	clamp(s.SedimentLayerHeight, 0.01f, 1000.0f, d.SedimentLayerHeight);
	clamp(s.SiltHalfLife, 0.5f, Huge, d.SiltHalfLife);
	clamp(s.ScatteringAnisotropy, -0.99f, 0.99f, d.ScatteringAnisotropy);
	clamp(s.DownwellingAttenuation, 0.0f, Huge, d.DownwellingAttenuation);
	clamp(s.FoamAmount, 0.0f, Huge, d.FoamAmount);
	clamp(s.FoamBias, -100.0f, 100.0f, d.FoamBias);
	clamp(s.FoamThreshold, 0.0f, Huge, d.FoamThreshold);
	clamp(s.FoamAdd, 0.0f, Huge, d.FoamAdd);
	clamp(s.FoamDecayRate, 0.0f, Huge, d.FoamDecayRate);
	clamp(s.FoamOffset0, -100.0f, 100.0f, d.FoamOffset0);
	clamp(s.FoamOffset1, -100.0f, 100.0f, d.FoamOffset1);
	clamp(s.FoamOffset2, -100.0f, 100.0f, d.FoamOffset2);
	clamp(s.FoamOffset3, -100.0f, 100.0f, d.FoamOffset3);
	clamp(s.FoamCascadeWeightFull, 0.01f, 1000.0f, d.FoamCascadeWeightFull);
	clamp(s.FoamDepthFalloff, 0.0f, 20.0f, d.FoamDepthFalloff);
	clamp(s.FoamFadeDistance, 1.0f, 1000000.0f, d.FoamFadeDistance);
	clamp(s.FoamRoughnessModifier, 0.0f, Huge, d.FoamRoughnessModifier);
	clamp(s.FoamFeather, 0.001f, 0.5f, d.FoamFeather);
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
	clamp(s.RiverCurrentSpeed, 0.0f, 100.0f, d.RiverCurrentSpeed);
	clamp(s.RiverWaveDamping, 0.0f, 1.0f, d.RiverWaveDamping);
	clamp(s.FlowPeriodScale, 0.1f, 100.0f, d.FlowPeriodScale);
	clamp(s.FlowPeriodMin, 0.25f, 1024.0f, d.FlowPeriodMin);
	clamp(s.FlowPeriodMax, 0.25f, 4096.0f, d.FlowPeriodMax);
	clamp(s.BuoyancyStrength, 0.0f, Huge, d.BuoyancyStrength);
	clamp(s.FloatingRange, 1.0f, 10000.0f, d.FloatingRange);
	clamp(s.MaxFloatingSize, 1.0f, 10000.0f, d.MaxFloatingSize);
	clamp(s.FloatingResponse, 0.0f, 100.0f, d.FloatingResponse);
	clamp(s.TessellationMaxFactor, 1.0f, 64.0f, d.TessellationMaxFactor);
	clamp(s.TessellationErrorPixels, 0.05f, 100.0f, d.TessellationErrorPixels);
	clamp(s.TessellationMaxCoarsening, 1.0f, 64.0f, d.TessellationMaxCoarsening);
	clamp(s.TriangleBudget, 0.0f, 100000.0f, d.TriangleBudget);
	clamp(s.DisplacementFadeStart, 0.0f, 10000000.0f, d.DisplacementFadeStart);
	clamp(s.DisplacementFadeEnd, 1.0f, 10000000.0f, d.DisplacementFadeEnd);
	clamp(s.SedimentColor.x, 0.0f, Huge, d.SedimentColor.x);
	clamp(s.SedimentColor.y, 0.0f, Huge, d.SedimentColor.y);
	clamp(s.SedimentColor.z, 0.0f, Huge, d.SedimentColor.z);
	// Ranges whose ends depend on each other.
	s.ShoreLongestPeriod = std::max(s.ShoreLongestPeriod, s.ShoreShortestPeriod);
	s.FlowPeriodMax = std::max(s.FlowPeriodMax, s.FlowPeriodMin);
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
			Util::SliderFloatTip(T(TKEY("shore_curl_detail"), "Curl Detail"), &s.ShoreCurlDetail, 1.0f, 4.0f, "%.1fx",
				T(TKEY("shore_curl_detail_tooltip"), "Extra tessellation near the camera where shore waves can curl, so their lips hold their shape further away. Costs triangles. Default 2."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_surface"), "Surface & Light"))) {
		IdScope id("surface");
		Util::CheckboxTip(T(TKEY("enable_wave_shadows"), "Wave Shadows"), &s.EnableWaveShadows,
			T(TKEY("enable_wave_shadows_tooltip"), "Waves shade the water behind them from the sun, and the glint is hidden by the crests in front. Off: skips the wave heightfield pass and its per-pixel march."));
		Util::CheckboxTip(T(TKEY("enable_point_light_specular"), "Point Light Reflections"), &s.EnablePointLightSpecular,
			T(TKEY("enable_point_light_specular_tooltip"), "Torches and other lights glint in the water. Off: the per-pixel light loop is skipped."));
		Util::CheckboxTip(T(TKEY("enable_sky_occlusion"), "Wave Sky Occlusion"), &s.EnableSkyOcclusion,
			T(TKEY("enable_sky_occlusion_tooltip"), "The waves occlude the sky in Skylighting's probes, so troughs see less of it than crests. Off: the displaced water grid is not drawn into the probes' occlusion map. Needs Skylighting."));
		Util::SliderFloatTip(T(TKEY("roughness"), "Surface Roughness"), &s.Roughness, 0.02f, 0.5f, "%.3f",
			T(TKEY("roughness_tooltip"), "Base roughness of the calm water surface. Default 0.06."));
		Util::SliderFloatTip(T(TKEY("wind_roughness"), "Wind Roughness"), &s.WindRoughness, 0.0f, 3.0f, "%.2f",
			T(TKEY("wind_roughness_tooltip"), "Scales the roughness the wind adds with its tiny ripples (Cox and Munk). Default 1."));
		Util::CheckboxTip(T(TKEY("vanilla_normal_maps"), "Vanilla Normal Maps"), &s.VanillaNormalMaps,
			T(TKEY("vanilla_normal_maps_tooltip"), "Reads the water form's own normal maps on top of the simulated waves. Off: where the ocean waves run, their finest cascade carries the small ripples and the three normal map reads are skipped. Rivers and water without waves always keep them."));
		Util::SliderFloatTip(T(TKEY("detail_normal_calm"), "Normal Map Strength (Calm)"), &s.DetailNormalCalm, 0.0f, 2.0f, "%.2f",
			T(TKEY("detail_normal_calm_tooltip"), "Strength of the water form's normal maps on top of the simulated waves in calm air. They carry the fine ripple texture. Default 0.75."));
		Util::SliderFloatTip(T(TKEY("detail_normal_gale"), "Normal Map Strength (Windy)"), &s.DetailNormalGale, 0.0f, 2.0f, "%.2f",
			T(TKEY("detail_normal_gale_tooltip"), "The same at 8 m/s of wind and over; in between it blends. Default 1.3."));
		Util::SliderFloatTip(T(TKEY("refraction_distortion"), "Refraction Strength"), &s.RefractionDistortion, 0.0f, 3.0f, "%.2f",
			T(TKEY("refraction_distortion_tooltip"), "How far the surface bends the view of what lies below. Default 1."));
		Util::SliderFloatTip(T(TKEY("sun_specular"), "Sun Specular"), &s.SunSpecular, 0.0f, 3.0f, "%.2f",
			T(TKEY("sun_specular_tooltip"), "Brightness of the sun's glint on the surface. Default 1."));
		Util::SliderFloatTip(T(TKEY("point_light_specular"), "Point Light Specular"), &s.PointLightSpecular, 0.0f, 3.0f, "%.2f",
			T(TKEY("point_light_specular_tooltip"), "Brightness of torches and other lights reflected in the water. Default 1."));
		Util::CheckboxTip(T(TKEY("enable_subsurface"), "Subsurface Scattering"), &s.EnableSubsurface,
			T(TKEY("enable_subsurface_tooltip"), "Sunlight scatters through all the water and back to the eye. Off: it is not evaluated at all, which saves a scattering evaluation per pixel."));
		Util::SliderFloatTip(T(TKEY("subsurface"), "Subsurface Strength"), &s.Subsurface, 0.0f, 3.0f, "%.2f",
			T(TKEY("subsurface_tooltip"), "Strength of the sunlight that shines through the water and scatters back to the eye, over all the water. Default 1."));
		Util::SliderFloatTip(T(TKEY("vanilla_fresnel"), "Vanilla Fresnel Blend"), &s.VanillaFresnel, 0.0f, 1.0f, "%.2f",
			T(TKEY("vanilla_fresnel_tooltip"), "Blends the game's own reflectivity curve into the physical Fresnel. 0 is fully physical. Default 0."));

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("point_light_emitter_radius"), "Point Light Size"), &s.PointLightEmitterRadius, 1.0f, 40.0f, "%.1f units",
				T(TKEY("point_light_emitter_radius_tooltip"), "Skyrim's lights have no size; this is the radius given to a point light when working out its reflection (about a torch flame). Bigger gives softer, wider glints. Default 8 units."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_clarity"), "Water Clarity"))) {
		IdScope id("clarity");
		Util::CheckboxTip(T(TKEY("enable_clarity"), "Water Clarity"), &s.EnableClarity,
			T(TKEY("enable_clarity_tooltip"), "Suspended sediment: drifting plumes, stirred-up shallows, river and storm silt, wading silt. Off: the water is as clear as its form's colours make it, and the sediment is not evaluated."));
		Util::SliderFloatTip(T(TKEY("visibility"), "Water Visibility"), &s.Visibility, 0.05f, 5.0f, "%.2f",
			T(TKEY("visibility_tooltip"), "Scales the water form's visibility distance. Absorption colour comes from the water form's shallow colour. Default 1."));
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
		Util::SliderFloatTip(T(TKEY("river_turbidity"), "River Silt"), &s.RiverTurbidity, 0.0f, 3.0f, "%.2f",
			T(TKEY("river_turbidity_tooltip"), "Silt carried by river currents. Default 0.8."));
		Util::SliderFloatTip(T(TKEY("storm_turbidity"), "Storm Silt"), &s.StormTurbidity, 0.0f, 3.0f, "%.2f",
			T(TKEY("storm_turbidity_tooltip"), "Extra sediment in strong wind and rain. Default 1."));
		Util::SliderFloatTip(T(TKEY("wading_silt"), "Wading Silt"), &s.WadingSilt, 0.0f, 3.0f, "%.2f",
			T(TKEY("wading_silt_tooltip"), "Silt kicked up by feet on the bed (with Interactive Ripples). Default 1."));

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
			T(TKEY("enable_foam_tooltip"), "White water on the open sea, the breakers and the surf. Off: no foam is read, lit or added to the roughness."));
		Util::SliderFloatTip(T(TKEY("foam_amount"), "Foam Amount"), &s.FoamAmount, 0.0f, 3.0f, "%.2f",
			T(TKEY("foam_amount_tooltip"), "Foam is made only where waves break, in the open sea and in the surf. Scales how much foam the open sea's crests add each moment, and the surf's foam with it. Default 1."));
		Util::SliderFloatTip(T(TKEY("foam_bias"), "Foam Coverage"), &s.FoamBias, -1.0f, 2.0f, "%.2f",
			T(TKEY("foam_bias_tooltip"), "The wave compression (Jacobian) below which the surface starts to foam. Higher foams on gentler waves and thickens the surf's foam. Default 0.85."));
		Util::SliderFloatTip(T(TKEY("foam_fade_distance"), "Foam Fade Distance"), &s.FoamFadeDistance, 100.0f, 5000.0f, "%.0f m",
			T(TKEY("foam_fade_distance_tooltip"), "Distance at which all foam has faded out. Default 1000 m."), ImGuiSliderFlags_Logarithmic);
		Util::SliderFloatTip(T(TKEY("foam_depth_falloff"), "Foam Distance Falloff"), &s.FoamDepthFalloff, 0.1f, 4.0f, "%.2f",
			T(TKEY("foam_depth_falloff_tooltip"), "Exponent of the foam's fade with distance. Higher fades it sooner. Default 1."));
		Util::SliderFloatTip(T(TKEY("foam_roughness_modifier"), "Foam Roughness"), &s.FoamRoughnessModifier, 0.0f, 2.0f, "%.2f",
			T(TKEY("foam_roughness_modifier_tooltip"), "Roughness added to the surface per unit of foam. Default 0."));

		ImGui::SeparatorText(T(TKEY("open_sea_foam"), "Open Sea"));
		Util::SliderFloatTip(T(TKEY("foam_threshold"), "Foam Threshold"), &s.FoamThreshold, 0.0f, 1.0f, "%.2f",
			T(TKEY("foam_threshold_tooltip"), "Foam is added only above this amount of compression past the coverage point. Default 0."));
		Util::SliderFloatTip(T(TKEY("foam_add"), "Foam Growth"), &s.FoamAdd, 0.0f, 1.0f, "%.3f",
			T(TKEY("foam_add_tooltip"), "Foam added per 1/60 s per unit of compression. Default 0.1."));
		Util::SliderFloatTip(T(TKEY("foam_decay_rate"), "Foam Fade Rate"), &s.FoamDecayRate, 0.0f, 0.2f, "%.4f",
			T(TKEY("foam_decay_rate_tooltip"), "How quickly foam decays: it fades by exp(-rate) per 1/60 s. Default 0.0175."));

		ImGui::SeparatorText(T(TKEY("foam_pattern"), "Pattern"));
		Util::SliderFloatTip(T(TKEY("foam_bubble_size"), "Bubble Size"), &s.FoamBubbleSize, 0.01f, 1.0f, "%.3f m",
			T(TKEY("foam_bubble_size_tooltip"), "Size of the bubbles in the foam pattern. Surf foam is a microfoam of bubbles a few centimetres across; the pattern also draws lace at four and sixteen times this. Default 0.03 m."), ImGuiSliderFlags_Logarithmic);
		Util::SliderFloatTip(T(TKEY("foam_feather"), "Foam Feather"), &s.FoamFeather, 0.01f, 0.4f, "%.2f",
			T(TKEY("foam_feather_tooltip"), "How soft the edges of the foam bubbles are. Default 0.12."));

		ImGui::SeparatorText(T(TKEY("surf_foam"), "Surf"));
		Util::SliderFloatTip(T(TKEY("shore_foam_left"), "Foam Left Behind"), &s.ShoreResidualFoam, 0.0f, 1.0f, "%.2f",
			T(TKEY("shore_foam_left_tooltip"), "How much of a breaker's white water stays on the water as a lace once the wave has passed. The water carries it, the uprush takes it up the beach and the backwash pulls it back. Default 0.5."));
		Util::SliderFloatTip(T(TKEY("shore_foam_lifetime"), "Surf Foam Lifetime"), &s.ShoreFoamLifetime, 2.0f, 60.0f, "%.0f s",
			T(TKEY("shore_foam_lifetime_tooltip"), "How long the foam the surf leaves on the water takes to fade. Foam stranded on dry sand drains away within a second. Default 12 s."));

		if (ImGui::CollapsingHeader(T(TKEY("advanced"), "Advanced"))) {
			Util::SliderFloatTip(T(TKEY("foam_offset_0"), "Foam Offset, Longest Waves"), &s.FoamOffset0, -1.0f, 1.0f, "%.2f",
				T(TKEY("foam_offset_tooltip"), "Added to each wave cascade's foam before they are summed. Negative keeps those waves from foaming. Default 0.04."));
			Util::SliderFloatTip(T(TKEY("foam_offset_1"), "Foam Offset, Long Waves"), &s.FoamOffset1, -1.0f, 1.0f, "%.2f",
				T(TKEY("foam_offset_1_tooltip"), "Foam offset of the second cascade. Default -0.04."));
			Util::SliderFloatTip(T(TKEY("foam_offset_2"), "Foam Offset, Short Waves"), &s.FoamOffset2, -1.0f, 1.0f, "%.2f",
				T(TKEY("foam_offset_2_tooltip"), "Foam offset of the third cascade. Default -0.46."));
			Util::SliderFloatTip(T(TKEY("foam_offset_3"), "Foam Offset, Shortest Waves"), &s.FoamOffset3, -1.0f, 1.0f, "%.2f",
				T(TKEY("foam_offset_3_tooltip"), "Foam offset of the shortest cascade. Default -0.38."));
			Util::SliderFloatTip(T(TKEY("foam_cascade_weight"), "Cascade Weight Threshold"), &s.FoamCascadeWeightFull, 0.1f, 1.0f, "%.2f",
				T(TKEY("foam_cascade_weight_tooltip"), "A wave cascade counts fully in the foam from this share of its amplitude; below it, it counts in proportion. Default 1."));
		}
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_spray"), "Spray"))) {
		IdScope id("spray");
		DrawSpraySettings();
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_ripples"), "Ripples & Rivers"))) {
		IdScope id("ripples");
		Util::CheckboxTip(T(TKEY("enable_ripples"), "Interactive Ripples"), &s.EnableRipples,
			T(TKEY("enable_ripples_tooltip"), "Simulates ripples and wakes from every actor and object in the water. Replaces the vanilla wading mesh."));
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

		ImGui::SeparatorText(T(TKEY("rivers"), "Rivers"));
		Util::SliderFloatTip(T(TKEY("river_current_speed"), "River Current Speed"), &s.RiverCurrentSpeed, 0.3f, 4.0f, "%.2f m/s",
			T(TKEY("river_current_speed_tooltip"), "Speed of the current at full flow strength. It carries the ripples and the waves downstream. Default 1.5 m/s."));
		Util::SliderFloatTip(T(TKEY("river_wave_damping"), "River Wave Damping"), &s.RiverWaveDamping, 0.0f, 1.0f, "%.2f",
			T(TKEY("river_wave_damping_tooltip"), "How much running water flattens the waves. Default 0.5."));
		if (ImGui::CollapsingHeader(T(TKEY("rivers_advanced"), "Rivers Advanced"))) {
			Util::SliderFloatTip(T(TKEY("flow_period_scale"), "Flow Period"), &s.FlowPeriodScale, 1.0f, 8.0f, "%.1f",
				T(TKEY("flow_period_scale_tooltip"), "Waves on a river are carried along by cross-fading two copies. This sets how long each copy runs per metre of a cascade's texel: longer means fewer cross-fades but more stretching. Default 3."));
			Util::SliderFloatTip(T(TKEY("flow_period_min"), "Shortest Flow Period"), &s.FlowPeriodMin, 1.0f, 4.0f, "%.1f s",
				T(TKEY("flow_period_min_tooltip"), "The shortest a flow period gets, whatever the texel size. Default 1 s."));
			Util::SliderFloatTip(T(TKEY("flow_period_max"), "Longest Flow Period"), &s.FlowPeriodMax, 4.0f, 64.0f, "%.0f s",
				T(TKEY("flow_period_max_tooltip"), "The longest a flow period gets. Default 16 s."));
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
			T(TKEY("enable_tessellation_tooltip"), "Subdivides the water mesh adaptively once a frame so waves are real geometry, and evaluates each vertex's waves once for every pass that draws the water. Without it, waves are limited by the mesh resolution."));
		{
			Util::DisableGuard disable(!s.EnableTessellation);
			Util::CheckboxTip(T(TKEY("deferred_surface"), "Deferred Surface"), &s.DeferredSurface,
				T(TKEY("deferred_surface_tooltip"), "Shades each pixel of the tessellated water once, from what the depth pass recorded there, instead of once per triangle touching it. Small triangles then cost no extra shading. Turn off only to compare."));
			Util::DisableGuard disableDistant(!s.DeferredSurface);
			Util::CheckboxTip(T(TKEY("distant_half_rate"), "Half-Rate Distant Water"), &s.DistantHalfRate,
				T(TKEY("distant_half_rate_tooltip"), "Shades distant water at a quarter of its pixels and fills in the rest from them; temporal anti-aliasing recovers the detail. Distant water is often half the cost of the surface."));
			Util::DisableGuard disableDistance(!s.DistantHalfRate);
			Util::SliderFloatTip(T(TKEY("distant_half_rate_distance"), "Distant Water From"), &s.DistantHalfRateDistance, 50.0f, 1000.0f, "%.0f m",
				T(TKEY("distant_half_rate_distance_tooltip"), "Distance beyond which water is shaded at half rate, decided per 8x8 pixel tile by its nearest water. Lower is faster and softer. Default 60 m."));
		}
		Util::SliderFloatTip(T(TKEY("tessellation_triangle_size"), "Target Triangle Size"), &s.TessellationTriangleSize, 4.0f, 40.0f, "%.0f px",
			T(TKEY("tessellation_triangle_size_tooltip"), "Smaller values give smoother waves at a higher GPU cost. Scales automatically with the render resolution. Default 10 px."));
		Util::SliderFloatTip(T(TKEY("tessellation_max_factor"), "Maximum Tessellation"), &s.TessellationMaxFactor, 4.0f, 64.0f, "%.0f",
			T(TKEY("tessellation_max_factor_tooltip"), "Caps how finely the water is subdivided near the camera: the smallest triangle is the water mesh's grid (about 1.8 m) divided by this. Higher values make waves smoother up close but cost more. Default 32."));
		Util::SliderFloatTip(T(TKEY("triangle_budget"), "Triangle Budget"), &s.TriangleBudget, 0.0f, 8000.0f, "%.0fk",
			T(TKEY("triangle_budget_tooltip"), "The most tessellated water triangles a frame, in thousands. Above it the target triangle size grows until the count fits, so the worst views cost no more than this. 0: no budget. Default 1500k."));
		Util::CheckboxTip(T(TKEY("adaptive_tessellation"), "Adaptive Tessellation"), &s.AdaptiveTessellation,
			T(TKEY("adaptive_tessellation_tooltip"), "Sizes the open water's triangles by how much the waves there move, not by distance alone: calm seas, lakes and ponds take large triangles, a storm keeps the fine mesh. The shore band and the ripples around the player always keep the target size."));
		{
			Util::DisableGuard disableAdaptive(!s.AdaptiveTessellation);
			Util::SliderFloatTip(T(TKEY("tessellation_error_pixels"), "Allowed Wave Error"), &s.TessellationErrorPixels, 0.1f, 4.0f, "%.2f px",
				T(TKEY("tessellation_error_pixels_tooltip"), "How far, in pixels, the mesh may miss the waves' true surface between its vertices before it subdivides. Higher is coarser and faster. Default 0.5 px."), ImGuiSliderFlags_Logarithmic);
			Util::SliderFloatTip(T(TKEY("tessellation_max_coarsening"), "Largest Coarsening"), &s.TessellationMaxCoarsening, 1.0f, 16.0f, "%.0fx",
				T(TKEY("tessellation_max_coarsening_tooltip"), "The most times larger than the target size the open water's triangles grow on calm water. Default 8x."));
		}
		Util::SliderFloatTip(T(TKEY("displacement_fade_start"), "Wave Fade Start"), &s.DisplacementFadeStart, 2000.0f, 60000.0f, "%.0f units",
			T(TKEY("displacement_fade_start_tooltip"), "Distance at which the waves start to flatten towards the game's flat distant water (70 units are a metre). Default 16384 units."));
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
	ImGui::Text(T(TKEY("stat_draws"), "Water draws last frame: %u, %u of them from the tessellated mesh"), lastDrawStats.draws, lastDrawStats.computeDraws);
	{
		const auto& b = bisection.GetStats();
		ImGui::Text(T(TKEY("stat_bisection"), "Bisection tessellation: %u meshes, %u base triangles, %u shared root edges, %u bisectors (%u in view), %u of %u iterations (CPU %.3f ms), budget coarsening %.2fx"), b.meshes, b.roots, b.twins,
			b.bisectors, b.listed, b.iterations, b.iterationCap, b.cpuMs, budgetCoarsening);
		if (b.reused)
			ImGui::Text("%s", T(TKEY("stat_bisection_reused"), "Still view: subdivision kept, only its vertices evaluated"));
	}
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
	ImGui::TextUnformatted(floating.GetSelectionReport().c_str());  // i18n-ignore: diagnostic report built from form names
}

// ============================================================================
// Resources
// ============================================================================

void PBRWater::SetupResources()
{
	gpuBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::Constants");
	drawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::DrawConstants");
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
	offscreenDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::OffscreenDrawConstants");
	spacingDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::SpacingDrawConstants");
	swallowDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::SwallowDrawConstants");
	deferredDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::DeferredDrawConstants");
	deferredCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DeferredDrawCB>(), "PBRWater::DeferredDrawCB");
	deferredZeroCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<DeferredDrawCB>(), "PBRWater::DeferredZeroCB");
	deferredZeroCB->Update(DeferredDrawCB{});
	bisection.SetupResources();

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
	ripples.ClearShaderCache();
	swash.ClearShaderCache();
	spray.ClearShaderCache();
	CompileOccluder();
	CompileSurfaceNoise();
	ocean.ClearShaderCache();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);
	bisection.CompileShaders();
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

void PBRWater::Prepass()
{
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
		oceanSettings.foamThreshold = settings.FoamThreshold;
		oceanSettings.foamAdd = settings.FoamAdd * settings.FoamAmount;
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
		rippleSettings.flowSpeed = settings.RiverCurrentSpeed * UnitsPerMetre;
		// The bed and the flowmap as uploaded for the last frame (this runs before UpdateFrameConstants).
		RippleSimulation::Environment rippleEnvironment;
		if (frameData.Terrain1.z > 0.5f && terrainTexture) {
			rippleEnvironment.terrain = terrainTexture->srv.get();
			rippleEnvironment.terrain0 = frameData.Terrain0;
			rippleEnvironment.terrain1 = frameData.Terrain1;
		}
		if (frameData.Flow0.x != 0.0f && worldFlowmap) {
			rippleEnvironment.flowmap = worldFlowmap;
			rippleEnvironment.flow0 = frameData.Flow0;
		}
		rippleEnvironment.sampler = linearClampSampler.get();
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

void PBRWater::UpdateTessellation()
{
	if (!gpuBuffer || !settings.EnableTessellation || !bisection.IsReady() || !globals::shaderCache->IsEnabled()) {
		waterMeshes.Reset();
		bisection.Clear();
		budgetCoarsening = 1.0f;
		PBRWaterProfiling::Skipped("PBRWater::Tessellate");
		return;
	}

	// The triangle budget: last frame's read-back (a few frames late) sets how far the target size grows. Triangles fall
	// with 1/size^2, so the size follows the square root of the count, and it moves slowly so the lag does not overshoot.
	{
		const auto& stats = bisection.GetStats();
		if (settings.TriangleBudget <= 0.0f) {
			budgetCoarsening = 1.0f;
		} else if (stats.listed > 0) {
			const float trianglesK = float(stats.listed) * BisectionTessellator::GridTriangles / 1000.0f;
			const float budget = settings.TriangleBudget;
			const float target = budgetCoarsening * std::sqrt(trianglesK / budget);
			if (trianglesK > budget)
				budgetCoarsening = std::min(target, budgetCoarsening * 1.1f);
			else if (trianglesK < 0.8f * budget)
				budgetCoarsening = std::max(target, budgetCoarsening / 1.03f);
			budgetCoarsening = std::clamp(budgetCoarsening, 1.0f, 8.0f);
		}
	}

	// The vertices are evaluated as the tessellated draws evaluate them: this frame's constants (tessellated: Tess0.x
	// set, so the breakers curl), textures, the surf's state and the river flowmap.
	auto context = globals::d3d::context;
	GpuData d = frameData;
	d.Tess0.x = 1.0f;
	d.Draw0.x = 0.0f;
	d.Draw0.w = budgetCoarsening;
	gpuBuffer->Update(d);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	context->CSSetConstantBuffers(7, 1, cbs);
	ID3D11Buffer* frameBuffers[1] = { *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, srvs);
	ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
	context->CSSetShaderResources(8, 1, &flowmap);
	ID3D11SamplerState* linearClamp = linearClampSampler.get();
	ID3D11SamplerState* samplers[2] = { linearClamp, oceanSampler.get() };
	context->CSSetSamplers(WaterSamplerBase, 2, samplers);
	context->CSSetSamplers(8, 1, &linearClamp);

	BisectionTessellator::FrameParams p;
	p.frame = globals::state->frameCount;
	const auto& camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	p.cameraX = camera.x;
	p.cameraY = camera.y;
	p.cameraZ = camera.z;
	const auto& viewProj = globals::game::frameBufferCached.GetCameraViewProj();
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			p.viewProj[r][c] = viewProj.m[r][c];
	// How far the waves can carry the surface, for the frustum culling: not the hard clamp (DisplacementLimit: twice a
	// five-sigma crest), which kept everything within some 20 m of the camera, behind it too. The ocean moves a vertex
	// about 3.5 sigma sideways at most (Params2.z is 5 sigma times the choppiness, in units) and 4 sigma up or down; the
	// shore train's crests and lips, its run-up and the ripples on top. A culled triangle can only show as a moment's
	// sliver at the screen's edge.
	const float shoreWaves = 2.0f * (std::max(d.Shore1.z, 0.0f) + std::max(d.Shore1.w, 0.0f));
	const float oceanReach = std::max(d.Params2.z - 1.0f, 0.0f);
	const float choppy = std::max(d.Params2.x, 1.0f);
	const float shoreReach = shoreWaves * std::max(1.0f, d.Tune4.y / 1.55f);
	p.cullMarginXY = 0.7f * oceanReach + shoreReach + 32.0f;
	p.cullMarginZ = 0.8f * oceanReach / choppy + shoreReach + std::max(d.Shore0.w, 0.0f) + 4.0f * std::max(d.Ripple1.x, 0.0f) + 32.0f;
	// A still view keeps the subdivision: the split test reads the camera-relative position and slowly moving constants
	// only (TessellationSpacing), so refining it again changes nothing until the camera moves. It still refines every
	// TessReuseFrames, for the sea state, the shore field's blend and the ripple grid's origin. The view must have been
	// still for TessStillFrames first, so the refinements that followed the last movement have converged.
	TessellationView view;
	view.camera[0] = camera.x;
	view.camera[1] = camera.y;
	view.camera[2] = camera.z;
	const auto& unjittered = globals::game::frameBufferCached.GetCameraViewProjUnjittered();
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			view.viewProj[r][c] = unjittered.m[r][c];
	view.tess0 = d.Tess0;
	view.surface0 = d.Surface0;
	view.budget = budgetCoarsening;
	tessStillFrames = SameTessellationView(view, lastTessView) ? tessStillFrames + 1 : 0;
	lastTessView = view;
	constexpr uint32_t TessStillFrames = 4;
	constexpr uint32_t TessReuseFrames = 8;
	p.reuseSubdivision = tessStillFrames >= TessStillFrames && tessReusedFrames < TessReuseFrames && SameTessellationView(view, refinedTessView);
	// The meshes registered last frame.
	bisection.Build(p, waterMeshes);
	frameData.Draw0.w = budgetCoarsening;
	if (bisection.GetStats().reused) {
		++tessReusedFrames;
	} else {
		tessReusedFrames = 0;
		refinedTessView = view;
	}

	ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
	context->CSSetShaderResources(8, 1, nullSrvs);
	ID3D11Buffer* nullCbs[1] = { nullptr };
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->CSSetSamplers(WaterSamplerBase, 2, nullSamplers);
	context->CSSetSamplers(8, 1, nullSamplers);
}

bool PBRWater::SameTessellationView(const TessellationView& a, const TessellationView& b)
{
	// A unit (1.4 cm) of camera movement and 1e-4 of the view-projection (about 0.006 degrees) change no split: the
	// spacing follows the distance, and the frustum test has the waves' reach as its margin.
	for (int i = 0; i < 3; ++i)
		if (std::abs(a.camera[i] - b.camera[i]) > 1.0f)
			return false;
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			if (std::abs(a.viewProj[r][c] - b.viewProj[r][c]) > 1e-4f)
				return false;
	// The target size, the finest factor and the projection (Tess0.yzw) and the adaptive tolerance and coarsening
	// (Surface0.yz); not Surface0.w, the rain, which the split test does not read.
	return a.tess0.y == b.tess0.y && a.tess0.z == b.tess0.z && a.tess0.w == b.tess0.w && a.surface0.y == b.surface0.y && a.surface0.z == b.surface0.z &&
	       std::abs(a.budget - b.budget) <= 0.01f * b.budget;
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
	// Full precision for the travel times and distances: they become the waves' phase (omega tau, tens of radians) and the
	// nearest point of the coast, where half floats would step them visibly. The slownesses (0.05-1 s/m) and the rest
	// are fine as halves. Packed by the worker that built the field (ShoreField::PackForGpu): a field from elsewhere is
	// packed here.
	std::shared_ptr<PBRWaterModel::ShoreField> packed;
	const PBRWaterModel::ShoreField* source = field.get();
	if (field->gpuTimes.size() != count * 4 || field->gpuRays.size() != count * 4 || field->gpuRoom.size() != count * 2) {
		packed = std::make_shared<PBRWaterModel::ShoreField>(*field);
		packed->PackForGpu();
		source = packed.get();
	}
	const float* times = source->gpuTimes.data();
	const uint16_t* rays = source->gpuRays.data();
	const uint16_t* room = source->gpuRoom.data();

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
	result.times = create(DXGI_FORMAT_R32G32B32A32_FLOAT, times, static_cast<UINT>(field->width * 4 * sizeof(float)), "PBRWater::ShoreTimes");
	result.rays = create(DXGI_FORMAT_R16G16B16A16_FLOAT, rays, static_cast<UINT>(field->width * 4 * sizeof(uint16_t)), "PBRWater::ShoreRays");
	result.room = create(DXGI_FORMAT_R16G16_FLOAT, room, static_cast<UINT>(field->width * 2 * sizeof(uint16_t)), "PBRWater::ShoreRoom");
	if (result.times && result.rays && result.room)
		result.field = std::move(field);
	else
		result = {};
	return result;
}

void PBRWater::UpdateSwash(float dt)
{
	if (!gpuBuffer || frameData.Shore0.x < 0.5f) {
		frameData.Swash0 = {};
		frameData.Foam2 = {};
		PBRWaterProfiling::Skipped("PBRWater::ShoreFoam");
		PBRWaterProfiling::Skipped("PBRWater::SwashStep");
		PBRWaterProfiling::Skipped("PBRWater::SwashOut");
		PBRWaterProfiling::Skipped("PBRWater::ShoreWetness");
		return;
	}

	if (swashResetRequested.exchange(false, std::memory_order_acq_rel))
		swash.Reset();

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
	context->CSSetShaderResources(WaterSRVBase, 13, srvs);  // WaterSRVBase + 0..12: the swash's own history is bound by the simulation
	context->CSSetShaderResources(WaterSRVBase + 14, 1, &srvs[14]);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(WaterSamplerBase, 2, samplers);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	ShoreSimulation::Settings swashSettings;
	// The swash runs wherever the waves are still at full height (about a metre per texel at the default fade).
	swashSettings.extent = std::clamp(2.0f * settings.DisplacementFadeStart, 8960.0f, 32768.0f);
	// The surf simulation first: the wet level follows it. Off, the train runs to the waterline (Foam2.w = 0).
	// Asleep while no breaking beach is near the camera (MainThreadUpdate): the surf's grids then sit still.
	const bool surfWanted = settings.SurfQuality > 0;
	const bool surf = surfWanted && surfNearby.load(std::memory_order_acquire);
	if (surf) {
		if (surfAsleep)
			swash.ResetFoam();
		surfAsleep = false;
		swash.SetQuality(static_cast<uint32_t>(settings.SurfQuality - 1));
		swash.UpdateFoam(cameraPos.x, cameraPos.y, dt);
	} else {
		surfAsleep = surfWanted;
		PBRWaterProfiling::Skipped("PBRWater::ShoreFoam");
		PBRWaterProfiling::Skipped("PBRWater::SwashStep");
		PBRWaterProfiling::Skipped("PBRWater::SwashOut");
	}
	const bool foamReady = surf && swash.IsFoamReady();
	d.Foam2 = { swash.GetFoamOriginX(), swash.GetFoamOriginY(), 1.0f / swash.GetFoamExtent(), foamReady ? 1.0f : 0.0f };
	d.Shore0.z = foamReady ? swash.GetFoamBlendCode() : 1.0f;
	d.Shore2.z = swash.GetFoamShiftCode();
	gpuBuffer->Update(d);
	ID3D11ShaderResourceView* swashSrv = d.Foam2.w > 0.5f ? swash.GetFoamSRV() : nullptr;
	context->CSSetShaderResources(WaterSRVBase + 17, 1, &swashSrv);
	// The wet grid: how far up the shore it is wet, and the water's height for the lit objects' caustics. Without Wet
	// Shores nothing reads the wetness, and the caustics follow the still water.
	if (settings.WetShores)
		swash.Update(cameraPos.x, cameraPos.y, dt, swashSettings);
	else
		PBRWaterProfiling::Skipped("PBRWater::ShoreWetness");

	ID3D11ShaderResourceView* nullSrvs[13] = {};
	context->CSSetShaderResources(WaterSRVBase, 13, nullSrvs);
	context->CSSetShaderResources(WaterSRVBase + 14, 1, nullSrvs);
	context->CSSetShaderResources(WaterSRVBase + 17, 1, nullSrvs);
	ID3D11Buffer* nullCbs[1] = { nullptr };
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->CSSetSamplers(WaterSamplerBase, 2, nullSamplers);

	// The draws read the wet state the simulation just wrote.
	frameData.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), settings.WetShores && swash.IsReady() ? 1.0f : 0.0f };
	frameData.Foam2 = d.Foam2;
	frameData.Shore0.z = d.Shore0.z;
	frameData.Shore2.z = d.Shore2.z;

	// Lit terrain and objects read the wet sand, the foam on it and the water height from it.
	ID3D11ShaderResourceView* wetSrv = frameData.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	context->PSSetShaderResources(SwashWetSlot, 1, &wetSrv);
}

uint32_t PBRWater::SprayPoolSize() const
{
	constexpr uint32_t sizes[3] = { 16384, 65536, 131072 };
	return sizes[std::clamp(settings.SprayQuality, 0, 2)];
}

void PBRWater::UpdateSpray(float dt)
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!gpuBuffer || !settings.EnableSpray || !spray.IsReady() || !snap || !snap->exterior || !globals::shaderCache->IsEnabled()) {
		PBRWaterProfiling::Skipped("PBRWater::SprayEmit");
		PBRWaterProfiling::Skipped("PBRWater::SpraySim");
		return;
	}

	// The emit pass evaluates the waves as the draws do: this frame's constants, textures and the surf's state.
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
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, srvs);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(WaterSamplerBase, 2, samplers);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	SprayParticles::FrameInput input;
	input.dt = dt;
	const float windSpeed = d.Params0.y;
	input.windSpeed = windSpeed;
	// The wind the spray meets a metre or so over the water, not the 10 m wind the weather gives: the log profile over a
	// rough sea, U(z) / U(10 m) = ln(z / z0) / ln(10 m / z0) with z0 ~ 0.2 mm (Charnock), about 0.8 at 1 m, and the spray
	// thrown up in the lee of its own crest meets less still.
	constexpr float NearSurfaceWind = 0.6f;
	input.windVelocity = { d.Params1.x * windSpeed * NearSurfaceWind * UnitsPerMetre, d.Params1.y * windSpeed * NearSurfaceWind * UnitsPerMetre };
	input.gravity = d.Params0.w;
	input.cameraX = cameraPos.x;
	input.cameraY = cameraPos.y;
	input.cellSize = CellSize;
	input.cellX = std::floor(cameraPos.x / CellSize);
	input.cellY = std::floor(cameraPos.y / CellSize);
	OccluderCB levels{};
	FillOccluderLevels(levels, input.cellX, input.cellY);
	for (size_t i = 0; i < input.cellLevels.size(); ++i)
		input.cellLevels[i] = levels.cellLevels[i].x;

	if (d.Foam2.w > 0.5f && swash.GetSpraySourceSRV()) {
		input.shoreSource = swash.GetSpraySourceSRV();
		input.shoreStage = swash.GetStageSRV();
		input.shorePrep = swash.GetPrepSRV();
		input.shoreOriginX = swash.GetFoamOriginX();
		input.shoreOriginY = swash.GetFoamOriginY();
		input.shoreTexelSize = swash.GetFoamTexelSize();
		input.shoreGridSize = swash.GetFoamGridSize();
	}

	SprayParticles::Settings spraySettings;
	spraySettings.emitters = &settings.SprayEmitters;
	spraySettings.amount = settings.SprayAmount;
	spraySettings.range = settings.SprayRange;
	spraySettings.poolSize = SprayPoolSize();
	spray.Update(input, spraySettings);

	ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
	ID3D11Buffer* nullCbs[1] = { nullptr };
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->CSSetSamplers(WaterSamplerBase, 2, nullSamplers);
}

void PBRWater::DrawSpray()
{
	if (!loaded || !settings.EnableSpray || !spray.IsReady() || !globals::shaderCache->IsEnabled())
		return;
	auto* state = globals::state;
	if (sprayDrawFrame == state->frameCount)
		return;
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior)
		return;
	// From under the water the surface hides it.
	if (const auto* water = RE::TESWaterSystem::GetSingleton(); water && water->playerUnderwater)
		return;
	sprayDrawFrame = state->frameCount;
	auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	spray.Draw(depth);
}

PBRWater::SharedSettings PBRWater::GetCommonBufferData() const
{
	SharedSettings data{};
	if (!loaded)
		return data;
	if (frameData.Swash0.w > 0.5f) {
		data.SwashGrid = frameData.Swash0;
		// y: the highest the water can reach above the still level (PBRWater::DisplacementLimit, mirrored), so lit pixels
		// above it skip the grid fetch.
		const auto& f = frameData;
		const float limit = 2.0f * std::max(f.Params2.z, 0.0f) +
		                    2.0f * (std::max(f.Shore1.z, 0.0f) + std::max(f.Shore1.w, 0.0f)) * std::max(1.0f, f.Tune4.y / 1.55f) +
		                    std::max(f.Shore0.w, 0.0f) + 4.0f * std::max(f.Ripple1.x, 0.0f) + 1.0f;
		data.Wetness = { settings.WetShores ? 1.0f : 0.0f, limit, 0.0f, 0.0f };
	}
	// Drawn this frame or the last (the reflection passes update the shared data before Skylighting's probes).
	const bool occluding = occluderFrame != UINT32_MAX && globals::state->frameCount - occluderFrame <= 1;
	data.Skylighting = { occluding ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	return data;
}

void PBRWater::CompileSurfaceNoise()
{
	noiseCS = nullptr;
	if (auto* cs = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SurfaceNoiseCS.hlsl", {}, "cs_5_0")))
		noiseCS.attach(cs);
	else
		logger::warn("[PBR Water] Surface noise shader failed to compile; the vertices evaluate the noise themselves");
}

void PBRWater::UpdateSurfaceNoise()
{
	if (!gpuBuffer || !noiseCS || !noiseTexture || frameData.Noise0.z <= 0.0f) {
		frameData.Noise0.w = 0.0f;
		PBRWaterProfiling::Skipped("PBRWater::SurfaceNoise");
		return;
	}
	PBRWaterProfiling::Scope pass("PBRWater::SurfaceNoise");
	auto context = globals::d3d::context;
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	ID3D11UnorderedAccessView* uav = noiseTexture->uav.get();
	context->CSSetConstantBuffers(7, 1, cbs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(noiseCS.get(), nullptr, 0);
	context->Dispatch(SurfaceNoiseSize / 8, SurfaceNoiseSize / 8, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(7, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	// The draws read what was just baked.
	frameData.Noise0.w = 1.0f;
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
	PBRWaterProfiling::SkipUnlessRun row("PBRWater::SkylightingOccluder");
	ID3D11DepthStencilView* dsv = occlusion.dsv.get();
	if (!loaded || !settings.EnableSkyOcclusion || !dsv || !occluderVS || !occluderIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer || !globals::shaderCache->IsEnabled())
		return;
	// Calm water is the flat plane the probes are lifted above anyway: only waves need drawing.
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	row.ran = true;
	PBRWaterProfiling::Scope pass("PBRWater::SkylightingOccluder");

	OccluderCB cb{};
	cb.occlusionViewProj = occlusionViewProj;
	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	cb.eye = { eye.x, eye.y, eye.z, OccluderExtent };
	cb.grid = { static_cast<float>(OccluderQuads), OccluderExtent / OccluderQuads, eyeCellX, eyeCellY };
	if (!FillOccluderLevels(cb, eyeCellX, eyeCellY))
		return;
	DrawWaveGrid(dsv, occlusion.desc.Width, occlusion.desc.Height, cb, occluderIndices.get(), DXGI_FORMAT_R16_UINT, OccluderQuads * OccluderQuads * 6);
	occluderFrame = globals::state->frameCount;
}

bool PBRWater::FillOccluderLevels(OccluderCB& cb, float eyeCellX, float eyeCellY) const
{
	// Called by up to three passes a frame around the same cell: the 25 lookups are done once.
	const uint32_t frame = globals::state->frameCount;
	if (levelCache.frame == frame && levelCache.cellX == eyeCellX && levelCache.cellY == eyeCellY) {
		std::copy(std::begin(levelCache.levels), std::end(levelCache.levels), std::begin(cb.cellLevels));
		return levelCache.anyWater;
	}
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return false;
	// The still water of the cells around the eye, as SharedData::GetWaterData sees them.
	const auto eye = Util::GetEyePosition();
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
	std::copy(std::begin(cb.cellLevels), std::end(cb.cellLevels), std::begin(levelCache.levels));
	levelCache.frame = frame;
	levelCache.cellX = eyeCellX;
	levelCache.cellY = eyeCellY;
	levelCache.anyWater = anyWater;
	return anyWater;
}

void PBRWater::DrawWaveGrid(ID3D11DepthStencilView* dsv, UINT width, UINT height, const OccluderCB& cb, ID3D11Buffer* indices, DXGI_FORMAT indexFormat, UINT indexCount)
{
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
	context->VSGetShaderResources(WaterSRVBase, WaterSRVCount, savedSRVs);
	ID3D11SamplerState* savedSamplers[2] = {};
	context->VSGetSamplers(WaterSamplerBase, 2, savedSamplers);
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

	// Depth only, with the caller's transform.
	context->OMSetRenderTargets(0, nullptr, dsv);
	context->OMSetDepthStencilState(occluderDepthState.get(), 0);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(occluderRasterizer.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetIndexBuffer(indices, indexFormat, 0);
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
	context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, srvs);
	ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
	context->VSSetShaderResources(8, 1, &flowmap);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->VSSetSamplers(WaterSamplerBase, 2, samplers);

	context->DrawIndexed(indexCount, 0, 0);
	drawResourcesFrame = UINT32_MAX;

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
	context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, savedSRVs);
	context->VSSetSamplers(WaterSamplerBase, 2, savedSamplers);
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
}

void PBRWater::UpdateSunMap(bool waves)
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == sunMapFrame)
		return;
	sunMapFrame = frame;
	frameData.SunMap0.w = 0.0f;
	PBRWaterProfiling::SkipUnlessRun row("PBRWater::WaveHeightfield");
	// Calm water casts no shadow on itself: the heightfield is only drawn where waves stand.
	ID3D11DepthStencilView* dsv = sunMapTexture ? sunMapTexture->dsv.get() : nullptr;
	if (!waves || !dsv || !occluderVS || !sunMapIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer)
		return;
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	row.ran = true;
	PBRWaterProfiling::Scope pass("PBRWater::WaveHeightfield");

	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	constexpr float spacing = SunMapExtent / SunMapQuads;
	OccluderCB cb{};
	cb.eye = { eye.x, eye.y, eye.z, SunMapExtent };
	cb.grid = { static_cast<float>(SunMapQuads), spacing, eyeCellX, eyeCellY };
	if (!FillOccluderLevels(cb, eyeCellX, eyeCellY))
		return;
	// Seen from straight above. The grid is centred on the eye snapped to its spacing (SkylightingOccluderVS); heights are
	// stored as depth against the still water of the eye's cell, the highest surface nearest, so LESS_EQUAL keeps the
	// top of a curling lip.
	const float centreX = std::floor(eye.x / spacing) * spacing;
	const float centreY = std::floor(eye.y / spacing) * spacing;
	const float eyeLevel = cb.cellLevels[12].x;
	const float reference = eyeLevel > -1.0e8f ? eyeLevel : eye.z;
	const float scale = 2.0f / SunMapExtent;
	REX::W32::XMFLOAT4X4 m{};
	m.m[0][0] = scale;
	m.m[0][3] = -scale * (centreX - eye.x);
	m.m[1][1] = scale;
	m.m[1][3] = -scale * (centreY - eye.y);
	m.m[2][2] = -0.5f / SunMapRange;
	m.m[2][3] = 0.5f + (reference - eye.z) * 0.5f / SunMapRange;
	m.m[3][3] = 1.0f;
	cb.occlusionViewProj = m;

	auto context = globals::d3d::context;
	// Out of the pixel stage before it is drawn into.
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->PSSetShaderResources(WaveHeightfieldSlot, 1, &nullSrv);
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	DrawWaveGrid(dsv, SunMapSize, SunMapSize, cb, sunMapIndices.get(), DXGI_FORMAT_R32_UINT, SunMapQuads * SunMapQuads * 6);

	frameData.SunMap0 = { centreX, centreY, 1.0f / SunMapExtent, 1.0f };
	frameData.SunMap1 = { reference, SunMapRange, 0.0f, 0.0f };
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
		// y: with waves the surface is real geometry and Water Effects' parallax over the normal maps is skipped (Water.hlsl
		// GetWaterNormal): its march cost a tenth of the surface pass for detail the waves already carry.
		d.Params2 = { o.choppiness, settings.WaveHeight > 0.0f ? 1.0f : 0.0f, active ? o.displacementBound : 0.0f, static_cast<float>(std::max(ocean.GetMipCount(), 1u) - 1) };
		d.RefCamPos = { static_cast<float>(snap->refX), static_cast<float>(snap->refY), static_cast<float>(snap->refZ), 0.0f };
		// Shore waves need the coastline maps the snapshot blends between on the GPU. The clock runs at this frame's
		// (eased) frequency, but each map's lag omega tau uses the frequency it was traced for: with the eased one, tau
		// (up to a minute far out) turned every change of the sea into crests racing or stalling until the frequency
		// settled. A map is rebuilt when they differ by more than a few percent.
		const auto& shore = snap->shore;
		const bool shoreReady = shore.enabled && snap->shoreField && shoreTextures.field == snap->shoreField;
		const bool blending = shoreReady && snap->shoreBlend < 1.0f && snap->shorePrevious && previousShoreTextures.field == snap->shorePrevious;
		d.Shore0 = { shoreReady ? 1.0f : 0.0f, blending ? snap->shoreBlend : 1.0f, 1.0f, shore.enabled ? shore.maxRunup : 0.0f };
		d.Shore1 = { shore.crestWobble, settings.ShoreCurlDetail, shore.swellHeight, shore.windSeaHeight };
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

	// Tessellation: on-screen size uses the internal render resolution, so it scales with upscalers.
	float renderHeight = static_cast<float>(globals::game::graphicsState->screenHeight);
	if (globals::features::upscaling.loaded)
		renderHeight *= globals::features::upscaling.resolutionScale.y;
	const float projY = std::abs(globals::game::frameBufferCached.GetCameraProj()._22);
	d.Tess0 = { 0.0f, settings.TessellationTriangleSize, settings.TessellationMaxFactor, 0.5f * renderHeight * projY };
	d.Draw0 = { 0.0f, static_cast<float>(settings.WireframeMode), static_cast<float>(settings.DebugView), 0.0f };
	// The loaded cells: their water meets the flat LOD water at the grid's border, so the waves fade out before it.
	// Read from the cells the grid holds: TES::currentGridX/Y lag the grid (seen two cells off its centre with the
	// player on its edge), which faded the waves out over loaded water along straight cell lines.
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

	d.Light0 = { settings.Roughness, settings.EnableSubsurface ? settings.Subsurface : 0.0f, settings.VanillaFresnel, settings.SunSpecular };
	d.Light1 = { settings.Visibility, settings.EnableFoam ? 1.0f : 0.0f, settings.RefractionDistortion, settings.EnablePointLightSpecular ? settings.PointLightSpecular : 0.0f };
	// The open sea's foam test, also applied to the shore waves' crests and folds (PBRWater::EvaluateWavesFiltered); z the
	// same Amount and Coverage as a gain on the shore's foam coverages, which have no Jacobian (1 at the defaults).
	d.Foam1 = { settings.FoamBias, settings.FoamThreshold, settings.FoamAmount * std::max(settings.FoamBias, 0.0f) / 0.85f, settings.RiverWaveDamping };

	// Water clarity and the light in the water.
	d.Clarity0 = { settings.SedimentDensity / UnitsPerMetre, settings.Turbidity, settings.TurbidityPatchiness, settings.TurbidityPatchSize * UnitsPerMetre };
	d.Clarity1 = { settings.SedimentColor.x, settings.SedimentColor.y, settings.SedimentColor.z, settings.ShoreResuspension };
	d.Clarity2 = { settings.RiverTurbidity, weatherTurbidity.load(std::memory_order_acquire), settings.EnableRipples ? settings.WadingSilt : 0.0f, settings.SedimentLayerHeight * UnitsPerMetre };
	// Sediment plumes drift and reshape over hours; a daily wrap keeps their coordinates precise.
	d.Optics0 = { settings.ScatteringAnisotropy, settings.DownwellingAttenuation, std::fmod(globals::state->timer, 86400.0f), (snap && snap->ocean) ? snap->ocean->significantHeight : 0.0f };

	// Wind and rain on the surface.
	// yz: the adaptive tessellation (PBRWater::AdaptiveCoarsening).
	d.Surface0 = { settings.WindRoughness, settings.AdaptiveTessellation ? settings.TessellationErrorPixels : 0.0f, settings.TessellationMaxCoarsening,
		rainIntensity.load(std::memory_order_acquire) };
	{
		// The surface noise bake around the camera (UpdateSurfaceNoise): four texels across a cell of the sediment patches'
		// finest octave (their third, a quarter of their size), between 0.5 m and 16 m. 128^2 texels cost what as many
		// vertices did, and cover the tessellated water (640 m at the defaults). In whole texels, so the samples stay put as
		// the camera moves. Only the sediment is in it: without Water Clarity nothing is baked (Noise0.z = 0).
		const float finest = settings.TurbidityPatchSize / 4.12f * UnitsPerMetre;
		const float texel = std::clamp(finest / 4.0f, 0.5f * UnitsPerMetre, 16.0f * UnitsPerMetre);
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const float half = 0.5f * static_cast<float>(SurfaceNoiseSize);
		const float originX = (std::floor(cameraPos.x / texel) - half) * texel;
		const float originY = (std::floor(cameraPos.y / texel) - half) * texel;
		// w is set once this frame's bake has run.
		d.Noise0 = { originX, originY, settings.EnableClarity ? 1.0f / (texel * static_cast<float>(SurfaceNoiseSize)) : 0.0f, 0.0f };
	}
	d.Surface1 = { 0.0f, settings.EnableClarity ? 1.0f : 0.0f, settings.FetchScale, settings.VanillaNormalMaps ? 0.0f : 1.0f };

	// The artistic scales the shaders read (PBRWater.hlsli Tune0..Tune5).
	d.Tune0 = { settings.RiverCurrentSpeed, settings.FlowPeriodScale, settings.FlowPeriodMin, settings.FlowPeriodMax };
	d.Tune1 = { settings.FoamOffset0, settings.FoamOffset1, settings.FoamOffset2, settings.FoamOffset3 };
	d.Tune2 = { settings.FoamCascadeWeightFull, settings.FoamDepthFalloff, settings.FoamFadeDistance, settings.FoamRoughnessModifier };
	d.Tune3 = { settings.RippleHorizDisplace, settings.RippleDisplaceClamp, settings.ShoreTrainCycles, settings.ShoreResidualFoam };
	d.Tune4 = { settings.ShoreWaveVariation, PBRWaterModel::ShoreCrestLength(settings.DirectionalSpread), settings.DetailNormalCalm, settings.DetailNormalGale };
	d.Tune5 = { settings.PointLightEmitterRadius, settings.FoamFeather, settings.FoamBubbleSize, settings.ShoreFoamLifetime };
	d.Swash1 = { settings.SwashChurnHeight * UnitsPerMetre, settings.SwashChurnDepth, settings.SwashChurnSize * UnitsPerMetre, settings.SwashChurnSpeed };

	// The worldspace flowmap calms waves on rivers, looked up by position in every water pass.
	worldFlowmap = nullptr;
	if (globals::features::unifiedWater.GetWorldFlowmap(worldFlowmap, d.Flow0) && !worldFlowmap)
		d.Flow0 = {};

	frameData = d;
}

// ============================================================================
// Per-draw (render thread)
// ============================================================================

namespace
{
	/// Other views' water (CheapOffscreenWater): no shore waves, surf, foam, sediment, subsurface, point lights, ripples or
	/// wave shadows. They see it small or blurred.
	void CheapOffscreenGates(PBRWater::GpuData& d)
	{
		d.Shore0.x = 0.0f;
		d.Foam2.w = 0.0f;
		d.Swash0.w = 0.0f;
		d.Light1.y = 0.0f;
		d.Surface1.y = 0.0f;
		d.Light0.y = 0.0f;
		d.Light1.w = 0.0f;
		d.Ripple0.w = 0.0f;
		d.SunMap0.w = 0.0f;
		d.Draw0.z = 0.0f;
	}
}

void PBRWater::SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	if (!gpuBuffer || !waterShader || !pass || !pass->geometry)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;

	UpdateFrameConstants();

	const uint32_t vertexDescriptor = state->modifiedVertexDescriptor;
	const uint32_t technique = WaterTechnique(vertexDescriptor);
	const bool waves = WavesReach(*pass->geometry);

	const auto snap = snapshot.load(std::memory_order_acquire);
	const bool hasWaves = waves && snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled || settings.EnableRipples);

	// The tessellated mesh is drawn by our own vertex shader (its modes, Tess0.x): while that is still
	// compiling the vanilla shaders are bound, and the draw stays the engine's.
	auto customShadersLive = [&] {
		return shaderCache->IsEnabled() && shaderCache->GetVertexShader(*waterShader, vertexDescriptor) &&
		       shaderCache->GetPixelShader(*waterShader, state->modifiedPixelDescriptor);
	};

	// The tessellated mesh is built for the main camera: every pass of the main view draws it (depth and motion, surface,
	// point lights, underwater), each the same vertices. The other views (reflections, cubemaps, the map) see the water
	// small or not at all, and draw it untessellated, as does a draw the mesh does not cover yet (its first frame, a mesh
	// too large to tessellate).
	const bool mainView = state->inWorld && !(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections));
	bool cached = false;
	uint32_t cachedStart = 0;
	if (settings.EnableTessellation && bisection.IsReady() && hasWaves && IsTessellatedTechnique(technique) && mainView && customShadersLive()) {
		// Next frame's mesh covers this draw; this frame's does if the draw was there last frame too.
		waterMeshes.Register(*pass->geometry);
		const auto camera = globals::game::frameBufferCached.GetCameraPosAdjust();
		cached = bisection.BuiltFor(state->frameCount, camera) && bisection.Lookup(*pass->geometry, cachedStart);
	}

	ID3D11GeometryShader* geometryShader = nullptr;
	if (settings.WireframeMode > 0 && !IsStencilTechnique(technique) && customShadersLive())
		geometryShader = shaderCache->GetGeometryShader(*waterShader, vertexDescriptor);

	// Vertex spacing of this mesh, for filtering waves the untessellated grid cannot carry.
	float spacing = 0.0f;
	uint32_t triangleCount = 0;
	if (const auto triShape = pass->geometry->AsTriShape()) {
		triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
		if (hasWaves && !cached && triangleCount)
			spacing = pass->geometry->worldBound.radius * std::numbers::sqrt2_v<float> / std::max(std::sqrt(triangleCount * 0.5f), 1.0f);
	}

	// The water draws and which of them the compute mesh drew (Debug section).
	if (state->frameCount != drawStatsFrame) {
		lastDrawStats = drawStats;
		drawStats = {};
		drawStatsFrame = state->frameCount;
	}
	++drawStats.draws;
	++drawStats.byTechnique[DrawPassIndex(technique)];
	if (cached)
		++drawStats.computeDraws;

	// Most water is flat distant tiles, hundreds of them: anything done per draw is paid hundreds of times per pass. The
	// constants differ between draws only in the vertex shader's mode (Tess0.x) and, without tessellation, the spacing: the
	// modes are uploaded once and swapped by binding another buffer.
	GpuData& d = frameData;
	if (std::memcmp(&uploadedDrawData, &d, sizeof(GpuData)) != 0) {
		uploadedDrawData = d;
		GpuData drawData = d;
		// The tessellated mesh's vertex mode (BisectionTessellator::Draw).
		drawData.Tess0.x = 1.0f;
		drawBuffer->Update(drawData);
		drawData.Tess0.x = 2.0f;
		swallowDrawBuffer->Update(drawData);
		drawData.Tess0.x = 3.0f;
		deferredDrawBuffer->Update(drawData);
		drawData.Tess0.x = 0.0f;
		flatDrawBuffer->Update(drawData);
		// Costly gates off for other views (CheapOffscreenWater); always uploaded so toggling the setting never binds stale data.
		CheapOffscreenGates(drawData);
		offscreenDrawBuffer->Update(drawData);
		uploadedSpacing = -1.0f;
		GatherWaterSRVs(d, drawSRVs);
		drawResourcesFrame = UINT32_MAX;
	}
	// The engine's own draw of a mesh the tessellator draws runs the vertex shader in its culling mode (Tess0.x = 2): it
	// rasterises nothing, and RestoreDraw draws the mesh with the tessellated mode's constants, which the pixel stage gets
	// now.
	const bool cheap = !mainView && settings.CheapOffscreenWater;
	ID3D11Buffer* cb = cached ? drawBuffer->CB() : (cheap ? offscreenDrawBuffer->CB() : flatDrawBuffer->CB());
	if (spacing > 0.0f) {
		// Neighbouring tiles share a spacing: upload only when it (or the view's gates) changes.
		if (spacing != uploadedSpacing || cheap != uploadedSpacingCheap) {
			GpuData drawData = d;
			drawData.Tess0.x = 0.0f;
			drawData.Draw0.x = spacing;
			if (cheap)
				CheapOffscreenGates(drawData);
			spacingDrawBuffer->Update(drawData);
			uploadedSpacing = spacing;
			uploadedSpacingCheap = cheap;
		}
		cb = spacingDrawBuffer->CB();
	}
	ID3D11Buffer* vertexCb = cached ? swallowDrawBuffer->CB() : cb;
	context->VSSetConstantBuffers(7, 1, &vertexCb);
	context->PSSetConstantBuffers(7, 1, &cb);

	// These slots are free in every stage of the water shaders (Physical Sky, View Atmosphere and Landscape Seams use
	// the t110-t127 range, Lighting uses s12-s13).
	ID3D11SamplerState* linearClamp = linearClampSampler.get();
	ID3D11SamplerState* samplers[2] = { linearClamp, oceanSampler.get() };
	if (drawResourcesFrame != state->frameCount) {
		drawResourcesFrame = state->frameCount;
		ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
		context->VSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
		context->PSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
		context->VSSetSamplers(WaterSamplerBase, 2, samplers);
		// The vertex program reads the worldspace flowmap to calm waves on rivers.
		context->VSSetShaderResources(8, 1, &flowmap);
		context->VSSetSamplers(8, 1, &linearClamp);
	} else {
		// True PBR and Terrain Helper bind their pixel textures in t80-t97, with the lit geometry drawn between.
		ID3D11ShaderResourceView* bound[WaterSRVCount] = {};
		context->PSGetShaderResources(WaterSRVBase, WaterSRVCount, bound);
		bool intact = true;
		for (uint32_t i = 0; i < WaterSRVCount; ++i)
			intact = intact && bound[i] == drawSRVs[i];
		if (!intact)
			context->PSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
		for (auto* view : bound) {
			if (view)
				view->Release();
		}
	}
	context->PSSetSamplers(WaterSamplerBase, 2, samplers);
	// The waves' heightfield is the pixel stage's alone, in a slot nothing else uses: bound with every draw.
	ID3D11ShaderResourceView* heightfield = d.SunMap0.w > 0.5f && sunMapTexture ? sunMapTexture->srv.get() : nullptr;
	context->PSSetShaderResources(WaveHeightfieldSlot, 1, &heightfield);

	// The renderer binds textures and constant buffers lazily, right before the draw call, so the
	// device still holds the previous draw's state here: read this draw's from the shadow state.
	auto& shadow = globals::game::shadowState->GetRuntimeData();
	// That flush also re-binds every sampler slot the renderer has marked modified (Lighting uses 14-15),
	// which would replace ours with a point or wrap sampler depending on what was drawn before: drop those
	// pending updates for this draw. RestoreDraw() marks them again so the next draw gets the game's.
	shadow.PSSamplerModifiedBits &= ~(3u << WaterSamplerBase);

	cachedDrawPending = cached;
	cachedDrawStart = cachedStart;
	cachedDrawTechnique = technique;
	cachedDrawGeometry = pass->geometry;

	if (geometryShader) {
		context->GSSetConstantBuffers(7, 1, &cb);
		context->GSSetShader(geometryShader, nullptr, 0);
		geometryShaderBound = true;
	}
}

void PBRWater::SetupBatch()
{
	if (!gpuBuffer)
		return;
	UpdateFrameConstants();
	// Before the batch's pass and before the draws upload the constants, which say where the heightfield is.
	const auto snap = snapshot.load(std::memory_order_acquire);
	UpdateSunMap(settings.EnableWaveShadows && snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled) && globals::shaderCache->IsEnabled());
	// A batch the engine never closed (none should be) ends here rather than swallowing the next.
	EndDrawBatch();
	BeginDrawBatch(WaterTechnique(globals::state->modifiedVertexDescriptor));
}

void PBRWater::BeginDrawBatch(uint32_t technique)
{
	drawBatchOpen = true;
	PBRWaterProfiling::Begin(DrawPassNames[globals::state->inWorld ? 0 : 1][DrawPassIndex(technique)]);
}

void PBRWater::EndDrawBatch()
{
	if (!std::exchange(drawBatchOpen, false))
		return;
	EndDeferredBatch();
	PBRWaterProfiling::End();
}

bool PBRWater::WavesReach(const RE::NiAVObject& geometry) const
{
	// The waves fade out inside the loaded cells (LoadedGridFade) and with the distance (DisplacementDistanceFade).
	const auto& bound = geometry.worldBound;
	const float4& grid = frameData.Grid0;
	if (grid.z > grid.x && (bound.center.x + bound.radius < grid.x || bound.center.x - bound.radius > grid.z || bound.center.y + bound.radius < grid.y || bound.center.y - bound.radius > grid.w))
		return false;
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float dx = bound.center.x - eye.x;
	const float dy = bound.center.y - eye.y;
	const float reach = frameData.Params1.w + bound.radius;
	return dx * dx + dy * dy < reach * reach;
}

void PBRWater::GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const
{
	srvs[0] = settings.EnableRipples ? ripples.GetSRV() : nullptr;
	srvs[1] = fetchTexture ? fetchTexture->srv.get() : nullptr;
	srvs[2] = (d.Terrain1.z > 0.5f && terrainTexture) ? terrainTexture->srv.get() : nullptr;
	srvs[3] = settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr;
	srvs[4] = shoreProfileTexture ? shoreProfileTexture->srv.get() : nullptr;
	srvs[5] = ocean.GetDisplacementSRV();
	srvs[6] = ocean.GetDerivativesSRV();
	srvs[7] = ocean.GetSurfaceSRV();
	srvs[8] = ocean.GetPreviousDisplacementSRV();
	const bool shore = d.Shore0.x > 0.5f && shoreTextures.field;
	srvs[9] = shore ? shoreTextures.times->srv.get() : nullptr;
	srvs[10] = shore ? shoreTextures.rays->srv.get() : nullptr;
	srvs[11] = ocean.GetFoamSRV();
	srvs[12] = shore && d.Shore0.y < 1.0f && previousShoreTextures.field ? previousShoreTextures.times->srv.get() : nullptr;
	srvs[13] = shore && d.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	srvs[14] = shore ? shoreTextures.room->srv.get() : nullptr;
	srvs[15] = foamTexture ? foamTexture->srv.get() : nullptr;
	srvs[16] = d.Noise0.w > 0.5f && noiseTexture ? noiseTexture->srv.get() : nullptr;
	srvs[17] = shore && d.Foam2.w > 0.5f ? swash.GetFoamSRV() : nullptr;
}

void PBRWater::RestoreDraw()
{
	auto context = globals::d3d::context;
	// Rebound by the renderer's next flush: Lighting uses these slots.
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= (3u << WaterSamplerBase);
	if (std::exchange(cachedDrawPending, false)) {
		// The engine's draw rasterised nothing (the vertex shader's culling mode): the compute mesh, with all of the
		// draw's state, the vertex shader in its compute mode. With the deferred surface the stencil pass also records its
		// pixels, and the surface and point light passes shade those instead of drawing the triangles again.
		const bool deferred = settings.DeferredSurface && settings.WireframeMode == 0;
		if (deferred && IsStencilTechnique(cachedDrawTechnique))
			DrawRecordedStencil();
		else if (!(deferred && cachedDrawTechnique < 8 && DrawDeferredSurface()))
			DrawCachedMesh();
	}
	if (geometryShaderBound) {
		context->GSSetShader(nullptr, nullptr, 0);
		geometryShaderBound = false;
	}
}

bool PBRWater::EnsureDeferredRecords()
{
	auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
	if (!depth)
		return false;
	if (!deferredTilesCS) {
		deferredTilesCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredTilesCS.hlsl", {}, "cs_5_0")));
		if (!deferredTilesCS)
			return false;
	}
	D3D11_TEXTURE2D_DESC depthDesc{};
	depth->GetDesc(&depthDesc);
	if (deferredTags && deferredTags->desc.Width == depthDesc.Width && deferredTags->desc.Height == depthDesc.Height)
		return true;

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = depthDesc.Width;
	desc.Height = depthDesc.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	const float zero[4] = {};
	const std::tuple<std::unique_ptr<Texture2D>*, DXGI_FORMAT, const char*> targets[] = {
		{ &deferredTags, DXGI_FORMAT_R32_UINT, "PBRWater::DeferredTags" },
		{ &deferredWaves, DXGI_FORMAT_R16G16B16A16_FLOAT, "PBRWater::DeferredWaves" },
		{ &deferredNoise, DXGI_FORMAT_R8G8_UNORM, "PBRWater::DeferredNoise" },
		{ &deferredTexCoords, DXGI_FORMAT_R32G32_FLOAT, "PBRWater::DeferredTexCoords" },
	};
	for (const auto& [target, format, name] : targets) {
		desc.Format = srvDesc.Format = rtvDesc.Format = format;
		*target = std::make_unique<Texture2D>(desc, name);
		(*target)->CreateSRV(srvDesc);
		(*target)->CreateRTV(rtvDesc);
		context->ClearRenderTargetView((*target)->rtv.get(), zero);
	}

	// The shore train per 2x2 pixels (DeferredShoreCS). Its shader failing to compile only costs the pixels the train.
	if (!deferredShoreCS)
		deferredShoreCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredShoreCS.hlsl", {}, "cs_5_0")));
	{
		D3D11_TEXTURE2D_DESC halfDesc = desc;
		halfDesc.Width = (desc.Width + 1) / 2;
		halfDesc.Height = (desc.Height + 1) / 2;
		halfDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
		halfDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		D3D11_SHADER_RESOURCE_VIEW_DESC halfSrv = srvDesc;
		halfSrv.Format = halfDesc.Format;
		D3D11_UNORDERED_ACCESS_VIEW_DESC halfUav{};
		halfUav.Format = halfDesc.Format;
		halfUav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		const char* names[3] = { "PBRWater::DeferredShore0", "PBRWater::DeferredShore1", "PBRWater::DeferredShore2" };
		for (uint32_t i = 0; i < 3; ++i) {
			deferredShore[i] = std::make_unique<Texture2D>(halfDesc, names[i]);
			deferredShore[i]->CreateSRV(halfSrv);
			deferredShore[i]->CreateUAV(halfUav);
		}
		deferredShoreFrame = UINT32_MAX;
	}

	// The distant tiles shaded at half rate (DeferredFar.hlsl): one pixel of every 2x2, and which one, at half size.
	{
		D3D11_TEXTURE2D_DESC farDesc = desc;
		farDesc.Width = (desc.Width + 1) / 2;
		farDesc.Height = (desc.Height + 1) / 2;
		farDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		D3D11_SHADER_RESOURCE_VIEW_DESC farSrv = srvDesc;
		farSrv.Format = farDesc.Format;
		D3D11_RENDER_TARGET_VIEW_DESC farRtv = rtvDesc;
		farRtv.Format = farDesc.Format;
		deferredFar = std::make_unique<Texture2D>(farDesc, "PBRWater::DeferredFar");
		deferredFar->CreateSRV(farSrv);
		deferredFar->CreateRTV(farRtv);
		deferredFarFrame = UINT32_MAX;
	}

	// The tile lists: DrawInstancedIndirect's arguments per list, and room for every tile of the screen per list.
	deferredMaxTiles = ((desc.Width + DeferredTile - 1) / DeferredTile) * ((desc.Height + DeferredTile - 1) / DeferredTile);
	D3D11_BUFFER_DESC argsDesc{};
	argsDesc.ByteWidth = DeferredLists * 4 * sizeof(uint32_t);
	argsDesc.Usage = D3D11_USAGE_DEFAULT;
	argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	deferredArgs = nullptr;
	deferredArgsUAV = nullptr;
	DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, nullptr, deferredArgs.put()));
	Util::SetResourceName(deferredArgs.get(), "PBRWater::DeferredTileArgs");
	D3D11_UNORDERED_ACCESS_VIEW_DESC argsUav{};
	argsUav.Format = DXGI_FORMAT_R32_TYPELESS;
	argsUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	argsUav.Buffer.NumElements = DeferredLists * 4;
	argsUav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(deferredArgs.get(), &argsUav, deferredArgsUAV.put()));
	Util::SetResourceName(deferredArgsUAV.get(), "PBRWater::DeferredTileArgs UAV");

	D3D11_BUFFER_DESC tilesDesc{};
	tilesDesc.ByteWidth = DeferredLists * deferredMaxTiles * sizeof(uint32_t);
	tilesDesc.Usage = D3D11_USAGE_DEFAULT;
	tilesDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	tilesDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	tilesDesc.StructureByteStride = sizeof(uint32_t);
	deferredTiles = std::make_unique<Buffer>(tilesDesc, nullptr, "PBRWater::DeferredTiles");
	D3D11_SHADER_RESOURCE_VIEW_DESC tilesSrv{};
	tilesSrv.Format = DXGI_FORMAT_UNKNOWN;
	tilesSrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	tilesSrv.Buffer.NumElements = DeferredLists * deferredMaxTiles;
	deferredTiles->CreateSRV(tilesSrv);
	D3D11_UNORDERED_ACCESS_VIEW_DESC tilesUav{};
	tilesUav.Format = DXGI_FORMAT_UNKNOWN;
	tilesUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	tilesUav.Buffer.NumElements = DeferredLists * deferredMaxTiles;
	deferredTiles->CreateUAV(tilesUav);

	deferredMeshes.clear();
	deferredMeshesFrame = UINT32_MAX;
	deferredTilesFrame = UINT32_MAX;
	return true;
}

void PBRWater::DrawCachedMesh()
{
	ID3D11Buffer* cb = drawBuffer->CB();
	globals::d3d::context->VSSetConstantBuffers(7, 1, &cb);
	bisection.Draw(cachedDrawStart);
}

namespace
{
	/// The render targets and depth target bound now, with references held for the scope.
	struct BoundTargets
	{
		ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		UINT count = 0;

		explicit BoundTargets(ID3D11DeviceContext* context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
			while (count < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && rtv[count])
				++count;
		}
		~BoundTargets()
		{
			for (auto* view : rtv) {
				if (view)
					view->Release();
			}
			if (dsv)
				dsv->Release();
		}
		BoundTargets(const BoundTargets&) = delete;
		BoundTargets& operator=(const BoundTargets&) = delete;
	};
}

void PBRWater::DrawRecordedStencil()
{
	auto context = globals::d3d::context;
	const uint32_t frame = globals::state->frameCount;
	BoundTargets bound(context);
	// The record's targets stay bound across the stencil batch (EndDeferredBatch puts the engine's back): the engine
	// binds its own again only when it changes targets.
	const bool ours = deferredTags && bound.rtv[2] == deferredTags->rtv.get();
	const char* reason = nullptr;
	if (!bound.dsv)
		reason = "the stencil pass has no depth target";
	else if (!ours && (bound.rtv[2] || bound.rtv[3] || bound.rtv[4] || bound.rtv[5]))
		reason = "the stencil pass already uses render targets 2-5";
	else if (!cachedDrawGeometry)
		reason = "no geometry for the draw";
	else if (!EnsureDeferredRecords())
		reason = "the record could not be created (no depth buffer, or DeferredTilesCS did not compile)";
	else if (deferredMeshesFrame == frame && deferredMeshes.size() >= DeferredMeshes)
		reason = "more tessellated meshes than the record holds (32)";
	if (reason) {
		DeferredFallback(reason);
		DrawCachedMesh();
		return;
	}
	if (deferredMeshesFrame != frame) {
		// No mesh anywhere until this frame's stencil pass records one.
		deferredMeshes.clear();
		deferredMeshesFrame = frame;
		const float zero[4] = {};
		context->ClearRenderTargetView(deferredTags->rtv.get(), zero);
	}
	DeferredMesh mesh;
	mesh.startIndex = cachedDrawStart;
	// Texture coordinates that are one plane over the mesh are rebuilt per pixel: only the others are written.
	mesh.texCoords = !waterMeshes.AffineTexCoords(*cachedDrawGeometry, mesh.uvMapU, mesh.uvMapV);
	deferredMeshes.push_back(mesh);

	auto& batch = deferredBatch;
	if (!batch.recording) {
		batch.recording = true;
		batch.recordTargets = 0;
		batch.recordBlend = nullptr;
	}
	const UINT needed = mesh.texCoords ? 6 : 5;
	if (!ours || batch.recordTargets != needed) {
		ID3D11RenderTargetView* recorded[6] = { bound.rtv[0], bound.rtv[1], deferredTags->rtv.get(), deferredWaves->rtv.get(), deferredNoise->rtv.get(), deferredTexCoords->rtv.get() };
		context->OMSetRenderTargets(needed, recorded, bound.dsv);
		batch.recordTargets = needed;
	}
	// The engine's blend state need not write targets 2-5 (a zero write mask drops the record without a word): its own for
	// targets 0 and 1, the record's written whole. Set again only when the engine has set its own since.
	{
		winrt::com_ptr<ID3D11BlendState> blend;
		float factor[4] = {};
		UINT mask = 0xFFFFFFFF;
		context->OMGetBlendState(blend.put(), factor, &mask);
		if (blend.get() != batch.recordBlend || !batch.recordBlend) {
			batch.engineBlend = blend;
			std::copy_n(factor, 4, batch.blendFactor);
			batch.sampleMask = mask;
			batch.recordBlend = RecordBlendState(blend.get());
			context->OMSetBlendState(batch.recordBlend, factor, mask);
		}
	}

	DeferredDrawCB data;
	data.tag[0] = static_cast<uint32_t>(deferredMeshes.size());
	deferredCB->Update(data);
	ID3D11Buffer* drawCb = deferredCB->CB();
	context->PSSetConstantBuffers(13, 1, &drawCb);
	DrawCachedMesh();
	// The untessellated stencil draws in between write the record too, with no tag: where their water stands nearer,
	// none of the tessellated meshes owns the pixel.
	ID3D11Buffer* zeroCb = deferredZeroCB->CB();
	context->PSSetConstantBuffers(13, 1, &zeroCb);
	++drawStats.deferredRecorded;
}

void PBRWater::EndDeferredBatch()
{
	auto& batch = deferredBatch;
	if (!batch.recording && !batch.shading)
		return;
	auto context = globals::d3d::context;
	if (batch.recording) {
		BoundTargets bound(context);
		if (deferredTags && bound.rtv[2] == deferredTags->rtv.get())
			context->OMSetRenderTargets(2, bound.rtv, bound.dsv);
		winrt::com_ptr<ID3D11BlendState> blend;
		float factor[4] = {};
		UINT mask = 0xFFFFFFFF;
		context->OMGetBlendState(blend.put(), factor, &mask);
		if (blend.get() == batch.recordBlend)
			context->OMSetBlendState(batch.engineBlend.get(), batch.blendFactor, batch.sampleMask);
		ID3D11Buffer* nullCb = nullptr;
		context->PSSetConstantBuffers(13, 1, &nullCb);
		// The tiles are listed by the first surface draw (DrawDeferredSurface): sorting them by distance reads the depth,
		// which the stencil pass still has bound for writing here.
		batch.recording = false;
		batch.recordBlend = nullptr;
		batch.engineBlend = nullptr;
	}
	if (batch.shading) {
		ID3D11ShaderResourceView* nullSrvs[6] = {};
		context->PSSetShaderResources(38, 6, nullSrvs);
		context->PSSetShaderResources(46, 3, nullSrvs);
		context->VSSetShaderResources(101, 1, nullSrvs);
		ID3D11Buffer* nullCbs[3] = {};
		context->PSSetConstantBuffers(8, 3, nullCbs);
		context->PSSetConstantBuffers(13, 1, nullCbs);
		context->VSSetConstantBuffers(13, 1, nullCbs);
		ID3D11Buffer* cb = drawBuffer->CB();
		context->VSSetConstantBuffers(7, 1, &cb);
		context->PSSetConstantBuffers(7, 1, &cb);
		BoundTargets bound(context);
		if (bound.dsv && bound.dsv == batch.readOnly && batch.engineDSV)
			context->OMSetRenderTargets(bound.count, bound.rtv, batch.engineDSV.get());
		batch.shading = false;
		batch.readOnly = nullptr;
		batch.engineDSV = nullptr;
	}
}

ID3D11BlendState* PBRWater::RecordBlendState(ID3D11BlendState* engineState)
{
	auto& cached = recordBlendStates[engineState];
	if (!cached) {
		D3D11_BLEND_DESC desc{};
		if (engineState)
			engineState->GetDesc(&desc);
		else {
			desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			desc.RenderTarget[0].BlendOp = desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		if (!desc.IndependentBlendEnable) {
			for (uint32_t i = 1; i < 8; ++i)
				desc.RenderTarget[i] = desc.RenderTarget[0];
			desc.IndependentBlendEnable = TRUE;
		}
		for (uint32_t i = 2; i < 6; ++i) {
			auto& target = desc.RenderTarget[i];
			target.BlendEnable = FALSE;
			target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ZERO;
			target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		if (FAILED(globals::d3d::device->CreateBlendState(&desc, cached.put())))
			return engineState;
		Util::SetResourceName(cached.get(), "PBRWater::DeferredRecordBlend");
	}
	return cached.get();
}

void PBRWater::DeferredFallback(const char* reason)
{
	++drawStats.deferredForward;
	drawStats.deferredReason = reason;
}

void PBRWater::ClassifyDeferredTiles()
{
	auto context = globals::d3d::context;
	deferredTilesFrame = globals::state->frameCount;
	// No tiles for any mesh yet; DeferredTilesCS fills in the vertex counts as it lists them. Cleared on the GPU: a
	// CPU upload into a buffer the last frame's draws read can make the driver wait for them.
	const UINT zero[4] = {};
	context->ClearUnorderedAccessViewUint(deferredArgsUAV.get(), zero);

	// Distant tiles only where the surface pass can shade them at half rate.
	const bool distant = settings.DistantHalfRate && !deferredFarFailed && deferredFar;
	struct alignas(16) TilesCB
	{
		uint32_t width, height, maxTiles;
		float farDistance;
	} tilesData{ deferredTags->desc.Width, deferredTags->desc.Height, deferredMaxTiles,
		distant ? settings.DistantHalfRateDistance * static_cast<float>(PBRWaterModel::UnitsPerMetre) : 0.0f };
	if (!deferredTilesCB)
		deferredTilesCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TilesCB>(), "PBRWater::DeferredTilesCB");
	deferredTilesCB->Update(tilesData);

	winrt::com_ptr<ID3D11ComputeShader> savedShader;
	context->CSGetShader(savedShader.put(), nullptr, nullptr);
	ID3D11Buffer* savedCb = nullptr;
	context->CSGetConstantBuffers(0, 1, &savedCb);
	ID3D11Buffer* savedFrameCb = nullptr;
	context->CSGetConstantBuffers(12, 1, &savedFrameCb);
	ID3D11ShaderResourceView* savedSrvs[2] = {};
	context->CSGetShaderResources(0, 2, savedSrvs);
	ID3D11UnorderedAccessView* savedUavs[2] = {};
	context->CSGetUnorderedAccessViews(0, 2, savedUavs);

	ID3D11Buffer* cb = deferredTilesCB->CB();
	ID3D11Buffer* frameCb = *globals::game::perFrame.get();
	ID3D11ShaderResourceView* depthSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	ID3D11ShaderResourceView* srvs[2] = { deferredTags->srv.get(), depthSRV };
	ID3D11UnorderedAccessView* uavs[2] = { deferredArgsUAV.get(), deferredTiles->uav.get() };
	context->CSSetShader(deferredTilesCS.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetConstantBuffers(12, 1, &frameCb);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->Dispatch((tilesData.width + DeferredTile - 1) / DeferredTile, (tilesData.height + DeferredTile - 1) / DeferredTile, 1);

	context->CSSetShader(savedShader.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &savedCb);
	context->CSSetConstantBuffers(12, 1, &savedFrameCb);
	context->CSSetShaderResources(0, 2, savedSrvs);
	context->CSSetUnorderedAccessViews(0, 2, savedUavs, nullptr);
	if (savedCb)
		savedCb->Release();
	if (savedFrameCb)
		savedFrameCb->Release();
	for (auto* view : savedSrvs) {
		if (view)
			view->Release();
	}
	for (auto* uav : savedUavs) {
		if (uav)
			uav->Release();
	}
}

void PBRWater::BakeDeferredShore(ID3D11ShaderResourceView* depthSRV, ID3D11ShaderResourceView* flowmap)
{
	PBRWaterProfiling::Scope pass("PBRWater::DeferredShore");
	auto context = globals::d3d::context;
	deferredShoreFrame = globals::state->frameCount;
	struct alignas(16) ShoreCB
	{
		uint32_t width, height, halfWidth, halfHeight;
	} shoreData{ deferredTags->desc.Width, deferredTags->desc.Height, deferredShore[0]->desc.Width, deferredShore[0]->desc.Height };
	if (!deferredShoreCB)
		deferredShoreCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShoreCB>(), "PBRWater::DeferredShoreCB");
	deferredShoreCB->Update(shoreData);

	// The deferred draws' constants and views (the curl's spacing follows the draw: Tess0, Draw0.w), this pass's own.
	ID3D11Buffer* cbs[1] = { deferredShoreCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	ID3D11Buffer* waveCb = deferredDrawBuffer->CB();
	context->CSSetConstantBuffers(7, 1, &waveCb);
	ID3D11Buffer* frameBuffers[1] = { *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, drawSRVs);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(WaterSamplerBase, 2, samplers);
	ID3D11ShaderResourceView* srvs[4] = { deferredTags->srv.get(), deferredWaves->srv.get(), depthSRV, flowmap };
	context->CSSetShaderResources(0, 4, srvs);
	ID3D11UnorderedAccessView* uavs[3] = { deferredShore[0]->uav.get(), deferredShore[1]->uav.get(), deferredShore[2]->uav.get() };
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	context->CSSetShader(deferredShoreCS.get(), nullptr, 0);
	context->Dispatch((shoreData.halfWidth + 7) / 8, (shoreData.halfHeight + 7) / 8, 1);

	ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
	context->CSSetShaderResources(0, 4, nullSrvs);
	context->CSSetShaderResources(WaterSRVBase, WaterSRVCount, nullSrvs);
	ID3D11UnorderedAccessView* nullUavs[3] = {};
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	ID3D11Buffer* nullCbs[1] = {};
	context->CSSetConstantBuffers(0, 1, nullCbs);
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = {};
	context->CSSetSamplers(WaterSamplerBase, 2, nullSamplers);
	context->CSSetShader(nullptr, nullptr, 0);
}

bool PBRWater::DrawDeferredSurface()
{
	const uint32_t frame = globals::state->frameCount;
	if (deferredMeshesFrame != frame || !deferredTags) {
		DeferredFallback("the stencil pass recorded no mesh this frame");
		return false;
	}
	const auto mesh = std::find_if(deferredMeshes.begin(), deferredMeshes.end(), [&](const DeferredMesh& m) { return m.startIndex == cachedDrawStart; });
	if (mesh == deferredMeshes.end()) {
		DeferredFallback("the stencil pass did not record this mesh");
		return false;
	}
	const uint32_t slot = static_cast<uint32_t>(mesh - deferredMeshes.begin());

	auto context = globals::d3d::context;
	auto& batch = deferredBatch;
	BoundTargets bound(context);
	// The read-only depth and the record stay bound across the batch (EndDeferredBatch puts the engine's back); bound
	// again only when the engine has bound its own depth target since, which also drops the depth's view from the
	// pixel stage.
	if (!batch.shading || bound.dsv != batch.readOnly) {
		auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		ID3D11DepthStencilView* readOnly = nullptr;
		ID3D11DepthStencilView* engineDSV = nullptr;
		for (uint32_t i = 0; i < std::size(depth.views); ++i) {
			if (bound.dsv && depth.views[i] == bound.dsv) {
				readOnly = depth.readOnlyViews[i];
				engineDSV = bound.dsv;
			}
			if (bound.dsv && depth.readOnlyViews[i] == bound.dsv)
				readOnly = bound.dsv;  // the engine bound it already
		}
		if (!readOnly || !depth.depthSRV) {
			DeferredFallback(!bound.dsv ? "the surface pass has no depth target" : "the surface pass's depth target is not the main depth buffer");
			return false;
		}
		if (readOnly != bound.dsv) {
			context->OMSetRenderTargets(bound.count, bound.rtv, readOnly);
			batch.engineDSV.copy_from(engineDSV);
		}
		// Listed once a frame, with the depth bound read-only (DeferredTilesCS reads it to find the distant tiles).
		if (deferredTilesFrame != frame)
			ClassifyDeferredTiles();
		ID3D11ShaderResourceView* flowmap = frameData.Flow0.x != 0.0f ? worldFlowmap : nullptr;
		if (deferredShoreFrame != frame && deferredShoreCS && frameData.Shore0.x > 0.5f)
			BakeDeferredShore(depth.depthSRV, flowmap);
		ID3D11ShaderResourceView* srvs[6] = { deferredTags->srv.get(), deferredWaves->srv.get(), deferredNoise->srv.get(), deferredTexCoords->srv.get(), depth.depthSRV, flowmap };
		context->PSSetShaderResources(38, 6, srvs);
		ID3D11ShaderResourceView* shoreSrvs[3] = {};
		if (deferredShoreFrame == frame) {
			for (uint32_t i = 0; i < 3; ++i)
				shoreSrvs[i] = deferredShore[i]->srv.get();
		}
		context->PSSetShaderResources(46, 3, shoreSrvs);
		ID3D11ShaderResourceView* tilesSrv = deferredTiles->srv.get();
		context->VSSetShaderResources(101, 1, &tilesSrv);
		batch.shading = true;
		batch.readOnly = readOnly;
	}

	// The vertex program runs per pixel: the draw's vertex constants go to the pixel stage too (b8-b10).
	ID3D11Buffer* vertexCbs[3] = {};
	context->VSGetConstantBuffers(0, 3, vertexCbs);
	context->PSSetConstantBuffers(8, 3, vertexCbs);
	for (auto* buffer : vertexCbs) {
		if (buffer)
			buffer->Release();
	}

	D3D11_VIEWPORT viewport{};
	UINT viewports = 1;
	context->RSGetViewports(&viewports, &viewport);
	DeferredDrawCB data;
	data.tag[0] = slot + 1;
	data.tag[1] = deferredMaxTiles;
	data.tag[2] = mesh->texCoords ? 1u : 0u;
	data.tag[3] = deferredShoreFrame == frame ? 1u : 0u;
	data.viewport = { std::max(viewport.Width, 1.0f), std::max(viewport.Height, 1.0f), 0.0f, 0.0f };
	data.uvMapU = mesh->uvMapU;
	data.uvMapV = mesh->uvMapV;
	deferredCB->Update(data);
	ID3D11Buffer* drawCb = deferredCB->CB();
	context->VSSetConstantBuffers(13, 1, &drawCb);
	context->PSSetConstantBuffers(13, 1, &drawCb);
	ID3D11Buffer* tileCb = deferredDrawBuffer->CB();
	context->VSSetConstantBuffers(7, 1, &tileCb);
	context->PSSetConstantBuffers(7, 1, &tileCb);
	// The near tiles, then the distant ones: at half rate in the surface pass, at full rate in the point light passes
	// (and wherever the half-rate pass cannot run).
	data.mode[0] = slot;
	data.mode[1] = DeferredFull;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), slot * 4 * sizeof(uint32_t));
	if (!(cachedDrawTechnique == 0 && DrawDistantHalfRate(slot, data))) {
		data.mode[0] = DeferredMeshes + slot;
		deferredCB->Update(data);
		context->DrawInstancedIndirect(deferredArgs.get(), (DeferredMeshes + slot) * 4 * sizeof(uint32_t));
	}
	++drawStats.deferredShaded;
	return true;
}

bool PBRWater::DrawDistantHalfRate(uint32_t slot, DeferredDrawCB data)
{
	if (!settings.DistantHalfRate || deferredFarFailed || !deferredFar)
		return false;
	auto context = globals::d3d::context;
	BoundTargets bound(context);
	// Only the main view's surface: its targets are the record's size.
	if (bound.count == 0 || !bound.rtv[0])
		return false;
	{
		winrt::com_ptr<ID3D11Resource> resource;
		bound.rtv[0]->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (desc.Width != deferredTags->desc.Width || desc.Height != deferredTags->desc.Height)
			return false;
	}
	if (!deferredFarVS || !deferredFarPS) {
		deferredFarVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredFar.hlsl", { { "VSHADER", "" } }, "vs_5_0")));
		deferredFarPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\DeferredFar.hlsl", { { "PSHADER", "" } }, "ps_5_0")));
		if (!deferredFarVS || !deferredFarPS) {
			deferredFarFailed = true;  // from the next frame on the distant tiles are not listed apart
			return false;
		}
	}
	const uint32_t frame = globals::state->frameCount;
	if (deferredFarFrame != frame) {
		// Nothing shaded yet this frame: the upsampling reads a key of 0 where no mesh shaded.
		deferredFarFrame = frame;
		const float zero[4] = {};
		context->ClearRenderTargetView(deferredFar->rtv.get(), zero);
	}
	const uint32_t list = DeferredMeshes + slot;
	const UINT argsOffset = list * 4 * sizeof(uint32_t);

	// One pixel of every 2x2 into the half-size target, with the surface's own shaders: no depth (the record decides which
	// pixels are this mesh's water) and no blending (the key rides in alpha).
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&viewportCount, viewports);
	winrt::com_ptr<ID3D11BlendState> blend;
	float blendFactor[4] = {};
	UINT sampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(blend.put(), blendFactor, &sampleMask);

	ID3D11RenderTargetView* farRtv = deferredFar->rtv.get();
	context->OMSetRenderTargets(1, &farRtv, nullptr);
	const D3D11_VIEWPORT half{ 0.0f, 0.0f, static_cast<float>(deferredFar->desc.Width), static_cast<float>(deferredFar->desc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &half);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	data.viewport = { half.Width, half.Height, 0.0f, 0.0f };
	data.mode[0] = list;
	data.mode[1] = DeferredHalf;
	data.mode[2] = frame & 3;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), argsOffset);

	context->OMSetRenderTargets(bound.count, bound.rtv, bound.dsv);
	context->RSSetViewports(viewportCount, viewports);
	context->OMSetBlendState(blend.get(), blendFactor, sampleMask);

	// The distant tiles at full size, with the surface draw's blend, depth and stencil state: each pixel from the samples
	// around it.
	winrt::com_ptr<ID3D11VertexShader> engineVS;
	winrt::com_ptr<ID3D11PixelShader> enginePS;
	context->VSGetShader(engineVS.put(), nullptr, nullptr);
	context->PSGetShader(enginePS.put(), nullptr, nullptr);
	context->VSSetShader(deferredFarVS.get(), nullptr, 0);
	context->PSSetShader(deferredFarPS.get(), nullptr, 0);
	ID3D11ShaderResourceView* farSrv = deferredFar->srv.get();
	context->PSSetShaderResources(DeferredFarSlot, 1, &farSrv);
	data.viewport = { std::max(viewports[0].Width, 1.0f), std::max(viewports[0].Height, 1.0f), 0.0f, 0.0f };
	data.mode[1] = DeferredFull;
	data.mode[2] = 0;
	deferredCB->Update(data);
	context->DrawInstancedIndirect(deferredArgs.get(), argsOffset);

	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->PSSetShaderResources(DeferredFarSlot, 1, &nullSrv);
	context->VSSetShader(engineVS.get(), nullptr, 0);
	context->PSSetShader(enginePS.get(), nullptr, 0);
	return true;
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
			// Sky::windAngle is the weather's wind direction (radians) measured from north, counter-clockwise: the
			// engine's own wind (the vector it drives precipitation and the like with, AE 1.6.1170 0x140E13930, called
			// from TES's per-frame update with windSpeed and windAngle) is speed * (-sin a, cos a). As a maths angle
			// from +X that is a + pi/2. Read as one directly, every wind-driven field (fetch, the sea's direction,
			// swell exposure, onshore wind) was a quarter turn clockwise off the game's wind.
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
	// Seconds a new coastline map takes to take over: long enough that a map traced for a new period or moving with the
	// player only makes the crests drift a little for a moment, never jump.
	constexpr float BlendTime = 4.0f;
	const auto latest = enabled ? environment.GetShore() : nullptr;
	if (latest != currentShore) {
		// After a reset the new field takes over at once: blending its travel times in from a field traced for other
		// settings made the crests stall or race for seconds.
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

std::array<float, 24> PBRWater::ShoreSignature() const
{
	const auto& s = settings;
	return { s.EnableShoreWaves ? 1.0f : 0.0f, s.ShoreShortestPeriod, s.ShoreLongestPeriod, 0.0f, s.ShoreTrainCycles,
		s.ShoreWaveVariation, s.DirectionalSpread, s.ShoreCrestWobble, s.ShoreBoreWhelps, s.ShoreCurl, s.SwashLinger, s.FetchScale,
		s.SwellHeight, s.SwellPeriod, s.SwellDirection, s.SwellSpread, s.SwellPeakedness, s.WaveHeight, s.WindSpeedCalm,
		s.WindSpeedStorm, s.ShoreResidualFoam, s.ShoreCurlDetail, s.ShoreFoamLifetime, 0.0f };
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

			if (wantRipples)
				addSource(niCollision, center, radius, flatZ, wave, center);

			if (dt > 0.0f) {
				// The engine's own buoyancy floats objects on the flat plane (and lets heavy ones sink).
				// Add only the water's orbital motion, as a change in velocity relative to what was added
				// last frame: floating objects ride the waves, sinking ones keep sinking, and nothing is
				// pinned to the surface.
				const float strength = std::clamp(settings.BuoyancyStrength, 0.0f, 3.0f) / 3.0f;
				// Props are denser than the water parcels around them, so they lag its rise (0.75), and nothing is lifted once it
				// rides above the surface: following a fast-rising crest flung them into the air as it fell away.
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

	// The world's effects pass (Frame Annotations' "Effects"); Detours chains with its detour.
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));

	logger::info("[PBR Water] Installed hooks");
}

void PBRWater::TESWaterSystem_InitializeWater::thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool, bool isProcedural)
{
	func(waterSystem, waterTri, form, waterHeight, unk4, true, isProcedural);
}

void PBRWater::BSWaterShader_SetupGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	// Closed in RestoreGeometry, which the engine always pairs with this call.
	globals::state->BeginPerfEvent("PBR Water - Draw");
	func(waterShader, pass, renderFlags);
	globals::features::pbrWater.SetupDraw(waterShader, pass);
}

void PBRWater::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	globals::features::pbrWater.RestoreDraw();
	func(waterShader, pass, renderFlags);
	globals::state->EndPerfEvent();
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
	// After the original: its BeginTechnique sets the descriptors the batch is named by. Before any draw of the batch, so
	// the batch's pass encloses every draw's own markers (Frame Annotations) instead of interleaving with them.
	const bool ok = func(waterShader, technique);
	if (ok)
		globals::features::pbrWater.SetupBatch();
	return ok;
}

void PBRWater::BSWaterShader_RestoreTechnique::thunk(RE::BSShader* waterShader, uint32_t technique)
{
	func(waterShader, technique);
	globals::features::pbrWater.EndDrawBatch();
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
