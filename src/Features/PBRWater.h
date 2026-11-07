#pragma once

#include "Feature.h"
#include "PBRWater/FloatingObjects.h"
#include "PBRWater/FoamPattern.h"
#include "PBRWater/OceanSimulation.h"
#include "PBRWater/RippleSimulation.h"
#include "PBRWater/ShoreSimulation.h"
#include "PBRWater/WaterEnvironment.h"
#include "PBRWater/WaveModel.h"

#include <array>
#include <atomic>
#include <future>
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

	/**
	 * @brief User settings. Physics (gravity, the index of refraction, JONSWAP, Miche and McCowan limits, the foam's
	 * measured albedo...) stays in the code; everything here is a guess, an artistic scale or a quality knob. Lengths
	 * are metres unless the comment says units (1 m = 70 units). The defaults are what the water always looked like.
	 */
	struct Settings
	{
		// Waves
		float WindSpeedCalm = 3.0f;    ///< m/s when the weather has no wind
		float WindSpeedStorm = 22.0f;  ///< m/s at the strongest weather wind
		float WaveHeight = 1.0f;
		float SwellHeight = 0.5f;      ///< metres, significant height of the swell from distant storms
		float SwellPeriod = 10.0f;     ///< seconds
		float SwellDirection = 35.0f;  ///< degrees from the wind direction
		float SwellPeakedness = 7.0f;  ///< JONSWAP peak enhancement of the swell: narrow-banded, long-travelled
		float SwellSpread = 30.0f;     ///< Mitsuyasu exponent of the swell: long, parallel crests
		float Choppiness = 1.0f;       ///< horizontal displacement scale; 1 = linear wave theory
		float DirectionalSpread = 0.6f;
		float FetchScale = 15.0f;  ///< Skyrim's distances are compressed: upwind open water counts this many times over
		float Gusts = 0.6f;        ///< how strongly gusts vary the wind over the water
		float GustSize = 40.0f;    ///< metres
		float WindStreaks = 1.0f;  ///< windrows: slicks in light wind

		// Shore
		bool EnableShoreWaves = true;      ///< waves shoal, break and run up beaches
		bool WetShores = true;             ///< the swash wets the sand and whatever it reaches (Wetness Effects)
		float ShoreShortestPeriod = 2.5f;  ///< s: the periods within which the shore train follows the sea
		float ShoreLongestPeriod = 3.5f;
		float ShoreTimeScale = 0.7f;      ///< the shore clock runs this much slower than the waves' frequency
		float ShoreTrainCycles = 7.0f;    ///< how many waves out from the shore the train reaches
		float ShoreSetMin = 0.45f;        ///< wave sets: one wave's height runs from this factor
		float ShoreSetMax = 1.55f;        ///< to this one
		float ShoreResidualFoam = 0.5f;   ///< share of the breakers' white water they leave behind on the water (ShoreFoamCS)
		float ShoreFoamLifetime = 12.0f;  ///< s: how long the foam the surf leaves on the water takes to fade (e-folding)
		float ShoreCrestWobble = 0.35f;   ///< how far the crests bend and drift along the coast (waves)
		float ShoreSplitWaves = 0.6f;     ///< height of the shorter waves the breakers split into in the surf, relative to the main train
		float ShoreCurl = 1.0f;           ///< how far plunging breakers curl over (0: lip only)
		float ShoreCurlDetail = 2.0f;     ///< tessellation multiplier where shore waves can curl
		float SwashLinger = 2.0f;         ///< backwash duration over the uprush's

		// Surface and light
		float Roughness = 0.06f;
		float SunSpecular = 1.0f;
		float PointLightSpecular = 1.0f;
		float PointLightEmitterRadius = 8.0f;  ///< units (~a torch flame): Skyrim lights have no emitter size
		float Subsurface = 1.0f;
		float VanillaFresnel = 0.0f;
		float RefractionDistortion = 1.0f;
		float WindRoughness = 1.0f;      ///< scales the wind-driven micro-roughness (Cox-Munk)
		float DetailNormalCalm = 0.75f;  ///< strength of the vanilla normal maps on top of the waves in calm air
		float DetailNormalGale = 1.3f;   ///< and at 8 m/s of wind and over

		// Water clarity
		float Visibility = 1.0f;
		float Turbidity = 0.15f;  ///< suspended sediment everywhere (0 = crystal clear)
		float3 SedimentColor = { 0.34f, 0.29f, 0.2f };
		float SedimentDensity = 0.35f;        ///< extinction (1/m) per unit of turbidity
		float TurbidityPatchiness = 0.7f;     ///< how unevenly the sediment is spread
		float TurbidityPatchSize = 150.0f;    ///< metres
		float ShoreResuspension = 1.0f;       ///< sediment stirred up by the waves in the shallows
		float RiverTurbidity = 0.8f;          ///< silt carried by river currents
		float StormTurbidity = 1.0f;          ///< extra sediment in strong wind and rain
		float WadingSilt = 1.0f;              ///< silt kicked up by feet on the bed
		float SedimentLayerHeight = 1.5f;     ///< metres the stirred-up sediment reaches above the bed
		float SiltHalfLife = 20.0f;           ///< s: how long stirred-up silt takes to settle
		float ScatteringAnisotropy = 0.6f;    ///< Henyey-Greenstein g of the particles in the water
		float DownwellingAttenuation = 1.0f;  ///< scales how fast light fades on its way down

		// Foam
		float FoamAmount = 1.0f;        ///< multiplier on the foam the open sea's crests add (FoamAdd)
		float FoamBias = 0.85f;         ///< the Jacobian below which a texel foams
		float FoamThreshold = 0.0f;     ///< foam is added only above this biased Jacobian
		float FoamAdd = 0.1f;           ///< foam added per 1/60 s per unit of biased Jacobian
		float FoamDecayRate = 0.0175f;  ///< foam decays by exp(-rate) per 1/60 s
		float FoamOffset0 = 0.04f;      ///< foam offset of each cascade, longest waves first
		float FoamOffset1 = -0.04f;
		float FoamOffset2 = -0.46f;
		float FoamOffset3 = -0.38f;
		float FoamCascadeWeightFull = 1.0f;  ///< a cascade counts fully in the foam from this share of its amplitude
		float FoamDepthFalloff = 1.0f;       ///< exponent of the foam's fade with distance
		float FoamFadeDistance = 1000.0f;    ///< m: distance at which the open sea's foam has faded out
		float FoamRoughnessModifier = 0.0f;  ///< roughness added per unit of foam
		float FoamFeather = 0.12f;           ///< how soft the edge of the foam pattern is
		float FoamBubbleSize = 0.03f;        ///< m: the bubbles of the foam pattern (surf foam is a microfoam of centimetre bubbles)

		// Ripples and rivers
		bool EnableRipples = true;
		float RippleExtent = 64.0f;  ///< m covered around the camera
		float RippleHeight = 0.12f;  ///< m
		float RippleNormalStrength = 1.0f;
		float RippleHalfLife = 1.5f;       ///< s
		float RippleHorizDisplace = 3.0f;  ///< horizontal displacement strength (Crest 3)
		float RippleDisplaceClamp = 0.3f;  ///< largest horizontal displacement in simulation texels (Crest 0.3)
		float RiverCurrentSpeed = 1.5f;    ///< m/s at full flow strength: moves the ripples and the waves
		float RiverWaveDamping = 0.5f;
		float FlowPeriodScale = 3.0f;  ///< s of flow clock period per metre of a cascade's texel (Crest 3)
		float FlowPeriodMin = 1.0f;    ///< s
		float FlowPeriodMax = 16.0f;   ///< s

		// Floating objects and gameplay
		bool GameplayWaves = true;
		bool EnableFloatingObjects = true;
		bool CarryActors = true;
		float BuoyancyStrength = 1.0f;
		float FloatingRange = 150.0f;   ///< metres around the player in which objects float
		float MaxFloatingSize = 90.0f;  ///< metres: the longest hull that floats (the large Skyrim ships are ~65 m)
		float FloatingResponse = 1.0f;  ///< scales how much the hulls move

		// Performance
		bool EnableTessellation = true;
		float TessellationTriangleSize = 10.0f;  ///< target triangle edge on screen (pixels)
		float TessellationMaxFactor = 32.0f;
		float DisplacementFadeStart = 16384.0f;  ///< units
		float DisplacementFadeEnd = 32768.0f;
		int WaveResolution = 1;  ///< FFT size per cascade: 0 = 128, 1 = 256, 2 = 512

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
		float4 Cascade1[PBRWaterModel::NumCascades];  // x mean omega (rad/s), y 1 / its PM weight at unlimited fetch, z mean wavenumber (rad/unit), w unused
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
		float4 Shore1;  // x crest wobble (waves), y curl tessellation multiplier, z swell Hs (units), w open-sea wind sea Hs (units)
		float4 Ripple0;
		float4 Ripple1;
		float4 Ripple2;
		float4 Light0;
		float4 Light1;
		float4 Foam1;
		float4 Clarity0;
		float4 Clarity1;
		float4 Clarity2;
		float4 Optics0;
		float4 Surface0;
		float4 Surface1;
		float4 Flow0;                                 // worldspace flowmap UV = absolute xy * xz + yw (x = 0: no flowmap)
		float4 Cascade2[PBRWaterModel::NumCascades];  // x height std (units), y deep-water phase speed of the mean frequency (units/s)
		float4 ShoreMap;                              // xy shore field corner (absolute units), zw 1 / extent (units)
		float4 Swash0;                                // xy swash grid corner (absolute units), z 1 / extent, w enabled
		float4 Shore2;                                // x wave clock (rad, wrapped), y the clock last frame, z its rate (rad/s), w peak period Tp (s)
		float4 Shore3;                                // x swash linger, y surf splitting, z breaker curl, w deep-water wavelength L0 (units)
		float4 Shore4;                                // x angular frequency the current shore field was traced for (rad/s), y the previous field's, z the sea's own deep-water wavelength (units, run-up)
		float4 Shore6;                                // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                                 // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                                // xy surface noise grid corner (absolute units), z 1 / extent (units), w baked this frame
		float4 Tune0;                                 // x river current speed (m/s), y flow period per metre of texel (s/m), z shortest flow period (s), w longest (s)
		float4 Tune1;                                 // foam offset of each cascade
		float4 Tune2;                                 // x foam cascade weight, y foam distance falloff exponent, z foam fade distance (m), w foam roughness modifier
		float4 Tune3;                                 // x ripple horizontal displacement, y its clamp (texels), z shore train reach (waves), w surf residual foam
		float4 Tune4;                                 // x shore set min, y shore set max, z detail normal strength calm, w gale
		float4 Tune5;                                 // x point light emitter radius (units), y foam feather, z foam bubble size (m), w shore foam lifetime (s)
		float4 Foam2;                                 // xy shore foam grid corner (absolute units), z 1 / extent, w enabled
		float4 SunMap0;                               // xy wave heightfield centre (absolute units), z 1 / extent, w drawn this frame
		float4 SunMap1;                               // x its reference level (absolute z), y height range either side (units), zw unused
	};
	STATIC_ASSERT_ALIGNAS_16(GpuData);

	/** @brief What the lit objects need of the swash (Lighting.hlsl, wetness); matches PBRWaterSettings in SharedData.hlsli. */
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

	/// Opens the profiler's pass for a batch of water draws (one technique), after drawing the frame's wave heightfield.
	struct BSWaterShader_SetupTechnique
	{
		static bool thunk(RE::BSShader* waterShader, uint32_t technique);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/// Closes it.
	struct BSWaterShader_RestoreTechnique
	{
		static void thunk(RE::BSShader* waterShader, uint32_t technique);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * The engine's "camera under water" setter. Besides TESWaterSystem::playerUnderwater it writes a global copy, from
	 * which every water shader property takes its kUnderwater flag (the underwater technique). The engine also calls it
	 * from the player: touching the water with the camera above drew the surface as seen from below, a mirror.
	 */
	struct TESWaterSystem_SetUnderwater
	{
		static void thunk(RE::TESWaterSystem* waterSystem, bool underwater, float height);
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
	static constexpr uint32_t WaterSRVCount = 18;        ///< t110-t127
	static constexpr uint32_t WaveHeightfieldSlot = 27;  ///< t27, pixel stage: the waves' heightfield (sunMapTexture)
	static constexpr uint32_t SurfaceNoiseSize = 128;    ///< texels per side of the surface noise bake

	void UpdateFrameConstants();
	void SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass);
	void RestoreDraw();
	void UpdateFetchTexture();
	void UpdateTerrainTexture();
	void GatherInteractions(const PBRWaterModel::WaveSnapshot& snapshot, float dt);
	/** @brief Main thread: the still water level of the exterior cells around the eye, for the ripple simulation's bed depth. */
	RippleSimulation::WaterLevels GatherWaterLevels(const RE::NiPoint3& eye, bool exterior) const;
	float WeatherTurbidityTarget() const;
	/** @brief 0..1 how much of the current weather blend is rainy. */
	float RainFraction() const;
	PBRWaterModel::SpectrumParams CurrentSpectrumParams(float windSpeed, bool exterior) const;
	/** @brief FFT size for the WaveResolution setting. */
	uint32_t WaveResolutionSize() const;
	/** @brief Render thread: the SRVs of slots t110-t126. */
	void GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const;
	/// False for a water tile wholly outside the waves' reach: flat, so it needs no tessellation.
	bool WavesReach(const RE::NiAVObject& geometry) const;
	/** @brief One shore field on the GPU. */
	struct ShoreTextures
	{
		std::shared_ptr<const PBRWaterModel::ShoreField> field;
		std::unique_ptr<Texture2D> times;  ///< travel time to the waterline, signed distance to it, still water level
		std::unique_ptr<Texture2D> rays;   ///< grad of the travel time, beach slope, exposure to the swell
		std::unique_ptr<Texture2D> room;   ///< room for the wave train (width of the water against the wavelength, coherent fronts)
	};
	/** @brief Render thread: uploads the shore fields the snapshot blends between, when they change. */
	void UpdateShoreTextures();
	static ShoreTextures UploadShoreField(std::shared_ptr<const PBRWaterModel::ShoreField> field);
	/** @brief Main thread: a new shore field takes over from the current one over a few seconds. */
	void UpdateShoreField(float dt, bool enabled);
	/** @brief The settings that change the shore waves' timing or shape, in a fixed order (see UpdateShoreReset). */
	std::array<float, 24> ShoreSignature() const;
	/**
	 * @brief Main thread: after a shore or sea-state setting changes and then holds still for a moment, restarts the shore
	 * train from the new settings: true when this frame resets it.
	 */
	bool UpdateShoreReset(float dt);
	/** @brief Render thread: steps the swash history: how wet the shore is (needs this frame's constants). */
	void UpdateSwash(float dt);
	/** @brief Main thread: the game's camera-under-water state follows the displaced surface. */
	void UpdateCameraWater(const PBRWaterModel::WaveSnapshot& snapshot, bool exterior);
	/** @brief Render thread, before the world is drawn: applies the main thread's camera-under-water state again. */
	void ApplyCameraWater();
	/** @brief Render thread: puts back the samplers SetupDraw replaced. */
	/** @brief Render thread: compiles the Skylighting occluder's vertex shader. */
	void CompileOccluder();
	/** @brief Render thread: uploads the foam texture once its bake (started in SetupResources) is done. */
	void UpdateFoamTexture();
	/** @brief Render thread: compiles the surface noise bake's compute shader. */
	void CompileSurfaceNoise();
	/** @brief Render thread: bakes this frame's surface noise around the camera (needs this frame's constants). */
	void UpdateSurfaceNoise();

	std::unique_ptr<ConstantBuffer> gpuBuffer;
	std::unique_ptr<ConstantBuffer> drawBuffer;      ///< this frame's constants for the tessellated water draws
	std::unique_ptr<ConstantBuffer> flatDrawBuffer;  ///< and for the rest
	GpuData frameData{};
	ID3D11ShaderResourceView* worldFlowmap = nullptr;  ///< render thread: Unified Water's worldspace flowmap (not owned)
	uint32_t frameDataFrame = UINT32_MAX;

	std::unique_ptr<Texture2D> fetchTexture;
	std::unique_ptr<Texture2D> foamTexture;                  ///< t125: the baked foam pattern (FoamPattern.h)
	std::future<std::vector<PBRWaterFoam::Level>> foamBake;  ///< its bake, on a worker at start-up
	std::unique_ptr<Texture2D> noiseTexture;                 ///< t126: this frame's surface noise around the camera (SurfaceNoiseCS)
	winrt::com_ptr<ID3D11ComputeShader> noiseCS;
	uint32_t uploadedFetchGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::FetchField> uploadedFetch;
	std::unique_ptr<Texture2D> terrainTexture;
	std::unique_ptr<Texture2D> shoreProfileTexture;  ///< the shore wave through its life (ShoreProfile.h)
	/** @brief Where a body last met the water: its rest (Lagrangian) position and height above the surface. */
	struct SourceTrack
	{
		double originX = 0.0, originY = 0.0;
		float offset = 0.0f;
	};
	std::unordered_map<const void*, SourceTrack> lastSources;          ///< main thread: ripple source motion relative to the water
	std::unordered_map<const void*, RE::NiPoint3> lastWaveVelocities;  ///< main thread: wave velocity added per body
	uint32_t uploadedTerrainGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::Bathymetry> uploadedTerrain;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	winrt::com_ptr<ID3D11SamplerState> oceanSampler;

	RippleSimulation ripples;
	ShoreSimulation swash;                ///< render thread
	ShoreTextures shoreTextures;          ///< render thread: the current shore field
	ShoreTextures previousShoreTextures;  ///< render thread: the one it replaces, while they blend
	OceanSimulation ocean;                ///< render thread

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
	uint32_t occluderFrame = UINT32_MAX;  ///< State frame the occluder was last drawn in
	/** @brief Render thread: the still water level of the 5x5 exterior cells around the eye; false when none has water. */
	bool FillOccluderLevels(OccluderCB& cb, float eyeCellX, float eyeCellY) const;
	/** @brief Render thread: draws the displaced water grid `cb` describes, depth only, into `dsv`; restores the state it changes. */
	void DrawWaveGrid(ID3D11DepthStencilView* dsv, UINT width, UINT height, const OccluderCB& cb, ID3D11Buffer* indices, DXGI_FORMAT indexFormat, UINT indexCount);

	// The waves' heightfield around the eye, which the water marches towards the sun through for their sun shadows (render thread)
	static constexpr uint32_t SunMapQuads = 512;    ///< grid quads per side
	static constexpr float SunMapExtent = 4096.0f;  ///< units across, centred on the eye
	static constexpr uint32_t SunMapSize = 1024;    ///< texels per side
	static constexpr float SunMapRange = 4096.0f;   ///< height range either side of the reference level (units)
	winrt::com_ptr<ID3D11Buffer> sunMapIndices;
	std::unique_ptr<Texture2D> sunMapTexture;  ///< t27: the highest water surface over each point around the eye
	uint32_t sunMapFrame = UINT32_MAX;         ///< State frame the heightfield was last drawn in
	/** @brief Render thread, first water draw of a frame: draws the waves' heightfield and records it in frameData.SunMap0/1. */
	void UpdateSunMap(bool waves);
	std::atomic<bool> oceanReady{ false };  ///< the GPU ocean is live; until then gameplay sees no FFT waves
	WaterEnvironment environment;
	FloatingObjects floating;  ///< main thread

	// Main thread state
	PBRWaterModel::WaveClock clock;
	PBRWaterModel::OceanMirror mirror;
	PBRWaterModel::ShoreParams shoreState;  ///< last frame's shore wave train (clock and eased frequency)
	bool hasShoreState = false;
	std::shared_ptr<const PBRWaterModel::ShoreField> currentShore;   ///< the shore field the waves follow
	std::shared_ptr<const PBRWaterModel::ShoreField> previousShore;  ///< the one it is taking over from
	float shoreBlend = 1.0f;                                         ///< 0..1 from previousShore's travel times to currentShore's
	std::array<float, 24> shoreSignature{};                          ///< the settings the shore train's timing follows (ShoreSignature)
	bool hasShoreSignature = false;
	float shoreResetDelay = -1.0f;                  ///< s until the shore train resets after a settings change (< 0: none pending)
	bool shoreHardSwap = false;                     ///< the next shore field replaces the current one at once instead of blending in
	std::atomic_bool swashResetRequested{ false };  ///< render thread: clear the swash history on its next step
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
	bool tessellationFailureLogged = false;
	/// The water draws of one frame, for the Debug section.
	struct DrawStats
	{
		uint32_t draws = 0;
		uint32_t tessellated = 0;
		uint32_t patches = 0;          ///< triangles of the tessellated draws: the tessellator's patches
		uint32_t byTechnique[6] = {};  ///< draws per technique (DrawPassIndex)
	};
	GpuData uploadedDrawData{};                              ///< what the draw buffers hold
	ID3D11ShaderResourceView* drawSRVs[WaterSRVCount] = {};  ///< the views for those constants
	uint32_t drawResourcesFrame = UINT32_MAX;                ///< the frame the views were last bound in
	DrawStats drawStats;                                     ///< this frame so far
	bool drawBatchOpen = false;                              ///< a profiler pass is open for the current batch of water draws
	/** @brief Render thread, a batch of water draws begins: the frame's wave heightfield, then the batch's pass. */
	void SetupBatch();
	/** @brief Render thread: opens the profiler's pass for this batch of water draws, named by its technique. */
	void BeginDrawBatch(uint32_t technique);
	/** @brief Render thread: closes it (RestoreTechnique). */
	void EndDrawBatch();
	// Main thread update cost (MainThreadUpdate), for the Debug section.
	std::atomic<float> mainUpdateMs{ 0.0f };      ///< smoothed (about 1 s)
	std::atomic<float> mainUpdatePeakMs{ 0.0f };  ///< decaying peak
	DrawStats lastDrawStats;                      ///< the last complete frame
	uint32_t drawStatsFrame = UINT32_MAX;
	uint32_t tessellationMissingSince = UINT32_MAX;
};
