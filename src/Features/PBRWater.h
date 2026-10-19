#pragma once

#include "Feature.h"
#include "PBRWater/FloatingObjects.h"
#include "PBRWater/OceanSimulation.h"
#include "PBRWater/RippleSimulation.h"
#include "PBRWater/ShoreSimulation.h"
#include "PBRWater/WaterEnvironment.h"
#include "PBRWater/WaveModel.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

/**
 * @brief Physically based water: an FFT ocean on tessellated geometry, PBR lighting, foam, shoreline
 * waves, interactive ripples and gameplay that follows the rendered surface.
 */
struct PBRWater : Feature
{
	virtual inline std::string GetName() override { return "PBR Water"; }
	virtual std::string GetDisplayName() override { return T("feature.pbr_water.name", "PBR Water"); }
	virtual inline std::string GetShortName() override { return "PBRWater"; }
	virtual inline std::string_view GetShaderDefineName() override { return "PBR_WATER"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }
	// Image space: screen-space reflections reject hits on the displaced water surface.
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Water || t == RE::BSShader::Type::ImageSpace || t == RE::BSShader::Type::Lighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.pbr_water.description", "Physically based water with real waves, tessellated geometry, foam and interactive ripples."),
			{ T("feature.pbr_water.key_feature_1", "FFT ocean: thousands of waves from the weather's wind plus distant swell, sized by open-water fetch and depth"),
				T("feature.pbr_water.key_feature_2", "GPU tessellation of the water surface, stable with upscalers and frame generation"),
				T("feature.pbr_water.key_feature_3", "PBR specular, Fresnel, light absorption and light transmitted through wave crests"),
				T("feature.pbr_water.key_feature_7", "Water clarity that varies: drifting sediment, wave-stirred shallows, muddy rivers and silt kicked up by wading"),
				T("feature.pbr_water.key_feature_4", "Shoreline breaking waves, surf foam and wakes"),
				T("feature.pbr_water.key_feature_5", "Ripple simulation for every actor and physics object, replacing the vanilla wading mesh"),
				T("feature.pbr_water.key_feature_6", "Swimmers, floating props, boats, ships and ice floes ride the rendered waves, with no patches or configuration") } };
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
		float Choppiness = 1.0f;  ///< horizontal displacement scale; 1 = linear wave theory
		float DirectionalSpread = 0.6f;
		float SwellHeight = 0.5f;      ///< metres, significant height of the swell from distant storms
		float SwellPeriod = 10.0f;     ///< seconds
		float SwellDirection = 35.0f;  ///< degrees from the wind direction
		int WaveResolution = 1;        ///< FFT size per cascade: 0 = 128, 1 = 256, 2 = 512
		bool UseFetch = true;
		float FetchScale = 15.0f;  ///< Skyrim's distances are compressed: upwind open water counts this many times over
		bool UseBathymetry = true;
		float RiverWaveDamping = 0.5f;

		// Shore
		bool EnableShoreWaves = true;        ///< waves shoal, break and run up beaches
		float ShoreWaveHeight = 1.0f;        ///< scale on the height of the waves reaching the shore
		float SwashFoam = 1.0f;              ///< foam riding the swash up the beach
		bool WetShores = true;               ///< the swash wets the sand and whatever it reaches (Wetness Effects)
		bool UnderwaterFollowsWaves = true;  ///< the camera is under water when under the displaced surface

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

		// Foam
		float FoamAmount = 1.0f;
		float EdgeFoam = 1.0f;  ///< foam lingering at the water's edge
		float BreakingFoam = 1.0f;
		float WakeFoam = 1.0f;
		float WakeFoamLifetime = 8.0f;  ///< seconds foam from wakes and splashes lasts
		float SplashFoam = 1.0f;        ///< whitewater from bodies hitting the water
		float WindStreaks = 1.0f;       ///< windrows: slicks in light wind
		float FoamAlbedo = 0.85f;
		float FoamDrift = 1.0f;  ///< scales the wind's surface (Stokes) drift that carries foam and bubbles

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

		// Floating objects
		bool EnableFloatingObjects = true;
		float FloatingRange = 150.0f;   ///< metres around the player in which objects float
		float MaxFloatingSize = 90.0f;  ///< metres: the longest hull that floats (the large Skyrim ships are ~65 m)
		float FloatingResponse = 1.0f;  ///< scales how much the hulls move
		bool CarryActors = true;

		// Debug
		int WireframeMode = 0;
		int DebugView = 0;
	};

	Settings settings;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

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
	virtual void GameLoaded() override;
	virtual void SavingGame() override;

	/** @brief Main thread, once per frame: wind, sea state, wave clock, CPU mirror, environment, ripple sources, buoyancy. */
	void MainThreadUpdate();

	/** @brief Wave displacement height above the flat plane at a world position (any thread). */
	bool GetWaveHeight(const RE::NiPoint3& position, float flatWaterZ, float& height) const;

	// ---- GPU constants, must match PBRWaterData in PBRWater.hlsli ----
	struct GpuData
	{
		float4 Cascade0[PBRWaterModel::NumCascades];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera, w texel size (units)
		float4 Cascade1[PBRWaterModel::NumCascades];  // x mean omega (rad/s), y 1 / its PM weight at unlimited fetch, z mean wavenumber (rad/unit)
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
		float4 Shore0;  // x shore waves on (field ready), y blend from the previous shore field (0) to the current one (1), w largest run-up (units)
		float4 Shore1;  // x breaking foam strength, y swash foam strength, z swell Hs (units), w open-sea wind sea Hs (units)
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
		float4 Surface0;
		float4 Surface1;
		float4 Land0;                                 // xy texel (0,0) corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                                 // x enabled, y vertex spacing (units)
		float4 Foam3;                                 // xy foam drift (units, wrapped), zw extra bubble drift (units, wrapped)
		float4 Flow0;                                 // worldspace flowmap UV = absolute xy * xz + yw (x = 0: no flowmap)
		float4 Cascade2[PBRWaterModel::NumCascades];  // x height std (units), y deep-water phase speed of the mean frequency (units/s)
		float4 ShoreMap;                              // xy shore field corner (absolute units), zw 1 / extent (units)
		float4 Swash0;                                // xy swash grid corner (absolute units), z 1 / extent, w enabled
		float4 Shore2;                                // xyz wave clock of each shore wave component (rad, wrapped), w peak period Tp (s)
		float4 Shore3;                                // xyz the clocks last frame, w deep-water wavelength L0 (units)
		float4 Shore4;                                // xyz angular frequency each component was traced for in the current shore field (rad/s)
		float4 Shore5;                                // xyz clock shift applied when the current field took over (rad)
		float4 Shore6;                                // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Shore7;                                // xyz angular frequency each component was traced for in the previous shore field (rad/s)
		float4 Grid0;                                 // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
	};
	STATIC_ASSERT_ALIGNAS_16(GpuData);

	/** @brief What the lit objects need of the swash (Lighting.hlsl, wetness and caustics); matches PBRWaterSettings in SharedData.hlsli. */
	struct alignas(16) SharedSettings
	{
		float4 SwashGrid;    // xy grid corner (absolute units), z 1 / extent (units), w enabled
		float4 Wetness;      // x wet shores enabled, yzw unused
		float4 Skylighting;  // x the water surface is in Skylighting's occlusion maps, yzw unused
	};
	STATIC_ASSERT_ALIGNAS_16(SharedSettings);
	/** @brief Render thread: the swash data for the shared feature buffer. */
	SharedSettings GetCommonBufferData() const;

	/**
	 * @brief Render thread, Skylighting's probe update: draws the displaced water surface into this frame's sky
	 * occlusion map, so the waves occlude the sky like any other geometry (troughs see less of it past the crests).
	 * A depth-only draw of one camera-centred heightfield grid; nothing when the water is calm.
	 * @param occlusion         the occlusion map Skylighting just rendered (depth)
	 * @param occlusionViewProj Skylighting's occlusion transform (eye-relative position to occlusion clip space)
	 */
	void DrawSkylightingOccluder(Texture2D& occlusion, const REX::W32::XMFLOAT4X4& occlusionViewProj);

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
	static constexpr uint32_t WaterSRVCount = 15;  ///< t110-t124

	void UpdateFrameConstants();
	void SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass);
	void RestoreDraw();
	void UpdateFetchTexture();
	void UpdateLandTexture();
	void GatherInteractions(const PBRWaterModel::WaveSnapshot& snapshot, float dt);
	float WeatherTurbidityTarget() const;
	/** @brief 0..1 how much of the current weather blend is rainy. */
	float RainFraction() const;
	PBRWaterModel::SpectrumParams CurrentSpectrumParams(float windSpeed, bool exterior) const;
	/** @brief FFT size for the WaveResolution setting. */
	uint32_t WaveResolutionSize() const;
	/** @brief Render thread: the SRVs of slots t110-t124. */
	void GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const;
	/** @brief One shore field on the GPU. */
	struct ShoreTextures
	{
		std::shared_ptr<const PBRWaterModel::ShoreField> field;
		std::unique_ptr<Texture2D> times;     ///< arrival time of each component, still water level
		std::unique_ptr<Texture2D> slowness;  ///< grad T of components 0 and 1
		std::unique_ptr<Texture2D> beach;     ///< grad T of component 2, beach slope, exposure to the swell
		std::unique_ptr<Texture2D> room;      ///< room for the wave train (width of the water against the wavelength)
	};
	/** @brief Render thread: uploads the shore fields the snapshot blends between, when they change. */
	void UpdateShoreTextures();
	static ShoreTextures UploadShoreField(std::shared_ptr<const PBRWaterModel::ShoreField> field);
	/** @brief Main thread: a new shore field takes over from the current one over a few seconds, the clocks moving onto its reference. */
	void UpdateShoreField(float dt, bool enabled);
	/** @brief Render thread: steps the swash history: wet sand and the foam left on it (needs this frame's constants). */
	void UpdateSwash(float dt);
	/** @brief Main thread: the game's camera-under-water state follows the displaced surface. */
	void UpdateCameraWater(const PBRWaterModel::WaveSnapshot& snapshot, bool exterior);
	/** @brief Render thread, before the world is drawn: applies the main thread's camera-under-water state again. */
	void ApplyCameraWater();
	/** @brief Render thread: puts back the samplers SetupDraw replaced. */
	void RestoreSamplers();
	/** @brief Render thread: compiles the Skylighting occluder's vertex shader. */
	void CompileOccluder();

	std::unique_ptr<ConstantBuffer> gpuBuffer;
	GpuData frameData{};
	ID3D11ShaderResourceView* worldFlowmap = nullptr;  ///< render thread: Unified Water's worldspace flowmap (not owned)
	uint32_t frameDataFrame = UINT32_MAX;

	std::unique_ptr<Texture2D> fetchTexture;
	uint32_t uploadedFetchGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::FetchField> uploadedFetch;
	std::unique_ptr<Texture2D> landTexture;
	/** @brief Where a body last met the water: its rest (Lagrangian) position and height above the surface. */
	struct SourceTrack
	{
		double originX = 0.0, originY = 0.0;
		float offset = 0.0f;
	};
	std::unordered_map<const void*, SourceTrack> lastSources;          ///< main thread: ripple source motion relative to the water
	std::unordered_map<const void*, RE::NiPoint3> lastWaveVelocities;  ///< main thread: wave velocity added per body
	double foamDriftX = 0.0, foamDriftY = 0.0;                         ///< main thread: accumulated surface drift (units)
	double bubbleDriftX = 0.0, bubbleDriftY = 0.0;
	uint32_t uploadedLandGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::Bathymetry> uploadedLand;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	winrt::com_ptr<ID3D11SamplerState> oceanSampler;

	RippleSimulation ripples;
	ShoreSimulation swash;                                        ///< render thread
	ShoreTextures shoreTextures;                                  ///< render thread: the current shore field
	ShoreTextures previousShoreTextures;                          ///< render thread: the one it replaces, while they blend
	std::shared_ptr<const PBRWaterModel::ShoreField> drawnShore;  ///< render thread: the field last frame's constants used
	OceanSimulation ocean;                                        ///< render thread

	// Skylighting occluder (render thread)
	static constexpr uint32_t OccluderQuads = 176;     ///< grid quads per side
	static constexpr float OccluderExtent = 11000.0f;  ///< units across: Skylighting's 10000-unit probe grid and a margin
	struct alignas(16) OccluderCB
	{
		REX::W32::XMFLOAT4X4 occlusionViewProj;
		float4 eye;             // xyz absolute eye position, w grid extent (units)
		float4 grid;            // x quads per side, y spacing (units), zw absolute cell of the eye
		float4 cellLevels[25];  // x absolute still water level of cell (eye cell + (i - 2, j - 2)) at i + 5 j, < -1e8: no water
	};
	STATIC_ASSERT_ALIGNAS_16(OccluderCB);
	winrt::com_ptr<ID3D11VertexShader> occluderVS;
	winrt::com_ptr<ID3D11Buffer> occluderIndices;
	winrt::com_ptr<ID3D11DepthStencilState> occluderDepthState;
	winrt::com_ptr<ID3D11RasterizerState> occluderRasterizer;
	std::unique_ptr<ConstantBuffer> occluderCB;
	uint32_t occluderFrame = UINT32_MAX;    ///< State frame the occluder was last drawn in
	std::atomic<bool> oceanReady{ false };  ///< the GPU ocean is live; until then gameplay sees no FFT waves
	WaterEnvironment environment;
	FloatingObjects floating;  ///< main thread

	// Main thread state
	PBRWaterModel::WaveClock clock;
	PBRWaterModel::OceanMirror mirror;
	PBRWaterModel::ShoreParams shoreState;  ///< last frame's shore wave train (phases and eased frequency)
	bool hasShoreState = false;
	std::shared_ptr<const PBRWaterModel::ShoreField> currentShore;    ///< the shore field the waves follow
	std::shared_ptr<const PBRWaterModel::ShoreField> previousShore;   ///< the one it is taking over from
	float shoreBlend = 1.0f;                                          ///< 0..1 share of currentShore's train (the rest is previousShore's)
	std::array<double, PBRWaterModel::ShoreComponents> shoreShift{};  ///< clock shift applied when currentShore took over (rad)
	float smoothedWindSpeed = -1.0f;
	float smoothedWindDirX = 1.0f;
	float smoothedWindDirY = 0.0f;
	std::atomic<std::shared_ptr<const PBRWaterModel::WaveSnapshot>> snapshot;
	std::atomic<float> renderDelta{ 0.0f };
	std::atomic<float> weatherTurbidity{ 0.0f };
	float smoothedWeatherTurbidity = 0.0f;
	std::atomic<float> rainIntensity{ 0.0f };
	std::atomic<float> cameraSurface{ 0.0f };  ///< displaced water height above the camera (absolute z)
	std::atomic<bool> cameraSurfaceValid{ false };
	std::atomic<uint32_t> cameraSurfaceFrame{ 0 };  ///< frame the main thread worked it out in
	float smoothedRain = 0.0f;

	// Render thread

	// Per-draw tessellation state (render thread)
	bool tessellationBound = false;
	bool geometryShaderBound = false;
	/// Samplers s12-s13 as the game left them. Its renderer caches sampler state per slot and would not
	/// rebind a slot it believes unchanged, so ours must not outlive the water draw (Lighting uses both).
	std::array<ID3D11SamplerState*, 2> savedVSSamplers{};
	std::array<ID3D11SamplerState*, 2> savedPSSamplers{};
	bool samplersSaved = false;
	bool tessellationFailureLogged = false;
	uint32_t tessellationMissingSince = UINT32_MAX;
};
