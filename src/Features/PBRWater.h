#pragma once

#include "Feature.h"
#include "PBRWater/RippleSimulation.h"
#include "PBRWater/WaterEnvironment.h"
#include "PBRWater/WaveModel.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

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
	// Image space: the SAO composite, which fogs the opaque scene, hosts the underwater view.
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Water || t == RE::BSShader::Type::ImageSpace; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.pbr_water.description", "Physically based water with real waves, tessellated geometry, foam and interactive ripples."),
			{ T("feature.pbr_water.key_feature_1", "Wind-driven wave spectrum that follows the weather, sized by open-water fetch and depth"),
				T("feature.pbr_water.key_feature_2", "GPU tessellation of the water surface, stable with upscalers and frame generation"),
				T("feature.pbr_water.key_feature_3", "PBR specular, Fresnel, light absorption and light transmitted through wave crests"),
				T("feature.pbr_water.key_feature_7", "Water clarity that varies: drifting sediment, wave-stirred shallows, muddy rivers and silt kicked up by wading"),
				T("feature.pbr_water.key_feature_8", "Volumetric underwater view with depth-dependent light, sun glow, light shafts and a waterline meniscus"),
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
		float ScatteringAnisotropy = 0.6f;    ///< Henyey-Greenstein g of the particles in the water
		float DownwellingAttenuation = 1.0f;  ///< scales how fast light fades on its way down
		float WindRoughness = 1.0f;           ///< scales the wind-driven micro-roughness (Cox-Munk)
		float Gusts = 0.6f;                   ///< how strongly gusts vary the wind over the water
		float GustSize = 40.0f;               ///< metres
		float Bioluminescence = 0.0f;         ///< plankton glow in churned water at night (off by default)
		float3 BioluminescenceColor = { 0.1f, 0.75f, 0.95f };

		// Water clarity
		float Turbidity = 0.15f;            ///< suspended sediment everywhere (0 = crystal clear)
		float TurbidityPatchiness = 0.7f;   ///< how unevenly it is spread
		float TurbidityPatchSize = 150.0f;  ///< metres
		float SedimentDensity = 0.35f;      ///< extinction (1/m) per unit of turbidity
		float3 SedimentColor = { 0.34f, 0.29f, 0.2f };
		float ShoreResuspension = 1.0f;    ///< sediment stirred up by the waves in the shallows
		float RiverTurbidity = 0.8f;       ///< silt carried by river currents
		float StormTurbidity = 1.0f;       ///< extra sediment in strong wind and rain
		float WadingSilt = 1.0f;           ///< silt kicked up by feet on the bed
		float SedimentLayerHeight = 1.5f;  ///< metres the stirred-up sediment reaches above the bed

		// Underwater
		bool EnableUnderwater = true;
		float UnderwaterVisibility = 1.0f;
		float LightShafts = 1.0f;
		float LightShaftDepth = 15.0f;  ///< metres over which the shafts fade
		float SunGlow = 1.0f;
		float Meniscus = 1.0f;
		int UnderwaterSamples = 12;

		// Foam
		float FoamAmount = 1.0f;
		float ShoreFoamWidth = 0.35f;  ///< metres
		float FoamPersistence = 1.5f;  ///< seconds a crest's foam trail lasts
		float CrestFoamThreshold = 0.55f;
		float FoamScale = 1.2f;  ///< metres
		float BreakingFoam = 1.0f;
		float WakeFoam = 1.0f;
		float WindStreaks = 1.0f;  ///< windrows: foam streaks in strong wind, slicks in light wind
		float FoamAlbedo = 0.85f;
		float FoamDrift = 1.0f;  ///< scales the wind's surface (Stokes) drift that carries foam and bubbles

		// Whitecaps
		float WhitecapAmount = 1.0f;
		float WhitecapScale = 2.5f;   ///< metres
		float WhitecapStreak = 3.0f;  ///< stretch along the wind
		float BubbleAmount = 1.0f;

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
		float4 WavePast[PBRWaterModel::MaxWaves];
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
		float4 Clarity0;
		float4 Clarity1;
		float4 Clarity2;
		float4 Optics0;
		float4 Underwater0;
		float4 Underwater1;
		float4 Underwater2;
		float4 Underwater3;
		float4 Surface0;
		float4 Surface1;
		float4 Surface2;
		float4 Land0;  // xy texel (0,0) corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;  // x enabled, y vertex spacing (units)
		float4 Foam2;  // x whitecap amount, y whitecap scale (units), z streak stretch, w bubble amount
		float4 Foam3;  // xy foam drift (units, wrapped), zw extra bubble drift (units, wrapped)
	};
	STATIC_ASSERT_ALIGNAS_16(GpuData);

	/** @brief The water around the camera, for the underwater view. Published by the main thread. */
	struct CameraWater
	{
		bool underwater = false;   ///< the camera may be below the (displaced) surface
		bool nearSurface = false;  ///< the waterline can cross the lens
		float flatZ = 0.0f;        ///< absolute height of the flat water plane
		float band = 0.0f;         ///< how far the waves and ripples can move the surface (units)
		float3 shallow{};          ///< water form colours (gamma, weather multiplier applied)
		float3 deep{};
		float visibility = 2048.0f;  ///< underwater fog distance of the water form (units)
	};

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

	/** @brief ISSAOComposite (the pass that fogs the opaque scene), one hook per vtable variant. */
	template <int Variant>
	struct ISSAOComposite_Render
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Render thread: binds the water constants and textures for the underwater composite. */
	void BindUnderwaterComposite();

private:
	void UpdateFrameConstants();
	void SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass);
	void RestoreDraw();
	void UpdateFetchTexture();
	void UpdateLandTexture();
	void GatherInteractions(const PBRWaterModel::WaveSnapshot& snapshot, float dt);
	std::shared_ptr<const CameraWater> FindCameraWater(const PBRWaterModel::WaveSnapshot& snapshot, bool exterior) const;
	float WeatherTurbidityTarget() const;
	/** @brief 0..1 how much of the current weather blend is rainy. */
	float RainFraction() const;
	PBRWaterModel::SpectrumParams CurrentSpectrumParams(float windSpeed) const;

	std::unique_ptr<ConstantBuffer> gpuBuffer;
	GpuData frameData{};
	uint32_t frameDataFrame = UINT32_MAX;

	std::unique_ptr<Texture2D> fetchTexture;
	uint32_t uploadedFetchGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::FetchField> uploadedFetch;
	std::unique_ptr<Texture2D> landTexture;
	std::unordered_map<const void*, RE::NiPoint3> lastSourcePositions;  ///< main thread: ripple source motion
	std::unordered_map<const void*, RE::NiPoint3> lastWaveVelocities;   ///< main thread: wave velocity added per body
	double foamDriftX = 0.0, foamDriftY = 0.0;                          ///< main thread: accumulated surface drift (units)
	double bubbleDriftX = 0.0, bubbleDriftY = 0.0;
	uint32_t uploadedLandGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::Bathymetry> uploadedLand;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;

	RippleSimulation ripples;
	WaterEnvironment environment;

	// Main thread state
	PBRWaterModel::PhaseIntegrator phases;
	float smoothedWindSpeed = -1.0f;
	float smoothedWindDirX = 1.0f;
	float smoothedWindDirY = 0.0f;
	std::atomic<std::shared_ptr<const PBRWaterModel::WaveSnapshot>> snapshot;
	std::atomic<std::shared_ptr<const CameraWater>> cameraWater;
	std::atomic<float> renderDelta{ 0.0f };
	std::atomic<float> weatherTurbidity{ 0.0f };
	float smoothedWeatherTurbidity = 0.0f;
	std::atomic<float> rainIntensity{ 0.0f };
	float smoothedRain = 0.0f;

	// Render thread
	uint32_t underwaterCompositeFrame = UINT32_MAX;  ///< frame the composite last fogged the scene under water

	// Per-draw tessellation state (render thread)
	bool tessellationBound = false;
	bool geometryShaderBound = false;
	bool tessellationFailureLogged = false;
	uint32_t tessellationMissingSince = UINT32_MAX;
};
