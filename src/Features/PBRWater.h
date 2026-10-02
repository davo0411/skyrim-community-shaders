#pragma once

#include "Feature.h"
#include "PBRWater/RippleSimulation.h"
#include "PBRWater/WaterEnvironment.h"
#include "PBRWater/WaveModel.h"

#include <atomic>
#include <memory>
#include <mutex>

/**
 * @brief Physically based water: spectral Gerstner waves on tessellated geometry, PBR lighting,
 * foam, shoreline waves, interactive ripples and gameplay that follows the rendered surface.
 */
struct PBRWater : Feature
{
	virtual inline std::string GetName() override { return "PBR Water"; }
	virtual std::string GetDisplayName() override { return T("feature.pbr_water.name", "PBR Water"); }
	virtual inline std::string GetShortName() override { return "PBRWater"; }
	virtual inline std::string_view GetShaderDefineName() override { return "PBR_WATER"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Water; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.pbr_water.description", "Physically based water with real waves, tessellated geometry, foam and interactive ripples."),
			{ T("feature.pbr_water.key_feature_1", "Wind-driven wave spectrum that follows the weather, sized by open-water fetch and depth"),
				T("feature.pbr_water.key_feature_2", "GPU tessellation of the water surface, stable with upscalers and frame generation"),
				T("feature.pbr_water.key_feature_3", "PBR specular, Fresnel, light absorption and subsurface scattering"),
				T("feature.pbr_water.key_feature_4", "Shoreline breaking waves and foam on shores, crests, objects and wakes"),
				T("feature.pbr_water.key_feature_5", "Ripple simulation for every actor and physics object, replacing the vanilla wading mesh"),
				T("feature.pbr_water.key_feature_6", "Swimming and floating objects ride the rendered waves") } };
	}

	struct Settings
	{
		// Geometry
		bool EnableTessellation = true;
		float TessellationTriangleSize = 10.0f;  ///< target triangle edge on screen (pixels)
		float TessellationMaxFactor = 32.0f;
		float DisplacementFadeStart = 16384.0f;  ///< units
		float DisplacementFadeEnd = 32768.0f;

		// Waves
		float WindSpeedCalm = 3.0f;    ///< m/s when the weather has no wind
		float WindSpeedStorm = 22.0f;  ///< m/s at the strongest weather wind
		float WaveHeight = 1.0f;
		float Choppiness = 0.75f;
		float DirectionalSpread = 0.6f;
		bool UseFetch = true;
		bool UseBathymetry = true;
		float RiverWaveDamping = 0.85f;

		// Shore
		float ShoreWaveHeight = 0.6f;
		float ShoreSlope = 0.04f;
		float ShoreSteepness = 0.6f;
		float ShoreOnsetDepth = 6.0f;  ///< metres

		// Lighting
		float Roughness = 0.06f;
		float SunSpecular = 1.0f;
		float PointLightSpecular = 1.0f;
		float Subsurface = 1.0f;
		float VanillaFresnel = 0.0f;
		float Visibility = 1.0f;
		float RefractionDistortion = 1.0f;

		// Foam
		float FoamAmount = 1.0f;
		float ShoreFoamWidth = 0.35f;  ///< metres
		float FoamPersistence = 1.5f;  ///< seconds a crest's foam trail lasts
		float CrestFoamThreshold = 0.55f;
		float FoamScale = 1.2f;  ///< metres
		float BreakingFoam = 1.0f;
		float WakeFoam = 1.0f;
		float FoamAlbedo = 0.85f;

		// Ripples
		bool EnableRipples = true;
		bool PhysicsObjectRipples = true;
		float RippleExtent = 64.0f;  ///< metres
		float RippleHeight = 0.12f;  ///< metres
		float RippleNormalStrength = 1.0f;
		float RippleSpeed = 1.2f;  ///< m/s
		float RippleHalfLife = 1.5f;

		// Gameplay
		bool GameplayWaves = true;
		float BuoyancyStrength = 1.0f;

		// Debug
		int WireframeMode = 0;
		int DebugView = 0;
	};

	Settings settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void RegisterWeatherVariables() override;

	/**
	 * @brief Clamps every setting to its UI range. Saved files, Ctrl+click typed values and weather
	 * blends can all push values outside it, and an out-of-range wave or ripple height displaces the
	 * mesh by kilometres.
	 */
	void SanitizeSettings();

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	/** @brief Main thread, once per frame: wind, phases, environment, ripple sources, buoyancy. */
	void MainThreadUpdate();

	/** @brief Wave displacement height above the flat plane at a world position (any thread). */
	bool GetWaveHeight(const RE::NiPoint3& position, float flatWaterZ, float& height) const;

	// ---- GPU constants, must match PBRWaterData in PBRWater.hlsli ----
	struct GpuData
	{
		float4 WaveDirK[PBRWaterModel::MaxWaves];
		float4 WaveAmp[PBRWaterModel::MaxWaves];
		float4 WaveExtra[PBRWaterModel::MaxWaves];
		float4 Params0;
		float4 Params1;
		float4 Params2;
		float4 RefCamPos;
		float4 Tess0;
		float4 Draw0;
		float4 Fetch0;
		float4 Fetch1;
		float4 Terrain0;
		float4 Terrain1;
		float4 Shore0;
		float4 Shore1;
		float4 Ripple0;
		float4 Ripple1;
		float4 Ripple2;
		float4 Light0;
		float4 Light1;
		float4 Foam0;
		float4 Foam1;
	};
	STATIC_ASSERT_ALIGNAS_16(GpuData);

	// ---- Hooks ----
	struct TESWaterSystem_InitializeWater
	{
		static void thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool noDisplacement, bool isProcedural);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TESObjectCELL_GetWaterHeight
	{
		static bool thunk(RE::TESObjectCELL* cell, const RE::NiPoint3& position, float& waterHeight);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MainUpdate
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	void UpdateFrameConstants();
	void SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass);
	void RestoreDraw();
	void UpdateFetchTexture();
	void GatherInteractions(const PBRWaterModel::WaveSnapshot& snapshot, float dt);
	PBRWaterModel::SpectrumParams CurrentSpectrumParams(float windSpeed) const;

	std::unique_ptr<ConstantBuffer> gpuBuffer;
	GpuData frameData{};
	uint32_t frameDataFrame = UINT32_MAX;

	std::unique_ptr<Texture2D> fetchTexture;
	uint32_t uploadedFetchGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::FetchField> uploadedFetch;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;

	RippleSimulation ripples;
	WaterEnvironment environment;

	// Main thread state
	PBRWaterModel::PhaseIntegrator phases;
	float smoothedWindSpeed = -1.0f;
	float smoothedWindDirX = 1.0f;
	float smoothedWindDirY = 0.0f;
	std::atomic<std::shared_ptr<const PBRWaterModel::WaveSnapshot>> snapshot;
	std::atomic<float> renderDelta{ 0.0f };

	// Per-draw tessellation state (render thread)
	bool tessellationBound = false;
	bool geometryShaderBound = false;
	bool tessellationFailureLogged = false;
};
