#pragma once

#include "Feature.h"
#include "PBRWater/BisectionTessellator.h"
#include "PBRWater/FloatingObjects.h"
#include "PBRWater/FoamPattern.h"
#include "PBRWater/KelvinWake.h"
#include "PBRWater/OceanSimulation.h"
#include "PBRWater/ProjectedGrid.h"
#include "PBRWater/RippleSimulation.h"
#include "PBRWater/ShoreSimulation.h"
#include "PBRWater/SprayParticles.h"
#include "PBRWater/WaterEnvironment.h"
#include "PBRWater/WaterMeshes.h"
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
				T("feature.pbr_water.key_feature_7", "Water clarity that varies: drifting sediment, wave-stirred shallows and silt kicked up by wading"),
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
		bool EnableOceanWaves = true;  ///< the FFT sea; off: no spectrum, FFT, mips or gameplay mirror (shore waves and ripples are separate)
		float WaveHeight = 1.0f;
		float SwellHeight = 0.5f;      ///< metres, significant height of the swell from distant storms
		float SwellPeriod = 10.0f;     ///< seconds
		float SwellDirection = 35.0f;  ///< degrees from the wind direction
		float SwellPeakedness = 7.0f;  ///< JONSWAP peak enhancement of the swell: narrow-banded, long-travelled
		float SwellSpread = 30.0f;     ///< Mitsuyasu exponent of the swell: long, parallel crests
		float Choppiness = 1.0f;       ///< horizontal displacement scale; 1 = linear wave theory
		float DirectionalSpread = 0.6f;
		float FetchScale = 15.0f;  ///< Skyrim's distances are compressed: upwind open water counts this many times over

		// Shore
		bool EnableShoreWaves = true;      ///< waves shoal, break and run up beaches
		bool WetShores = true;             ///< the swash wets the sand and whatever it reaches (Wetness Effects)
		int SurfQuality = 1;               ///< the surf and swash simulation: 0 off (the train runs to the waterline), 1 low (34 cm cells), 2 high (17 cm)
		float ShoreShortestPeriod = 2.5f;  ///< s: the periods within which the shore train follows the sea
		float ShoreLongestPeriod = 3.5f;
		float ShoreTrainCycles = 7.0f;    ///< how many waves out from the shore the train reaches
		float ShoreWaveVariation = 1.0f;  ///< how much one wave's height varies from the next (0 all alike, 1 as random waves do)
		float ShoreResidualFoam = 0.5f;   ///< share of the breakers' white water they leave behind on the water (ShoreFoamCS)
		float ShoreFoamLifetime = 12.0f;  ///< s: how long the foam the surf leaves on the water takes to fade (e-folding)
		float ShoreCrestWobble = 0.35f;   ///< how far the crests bend and drift along the coast (waves)
		float ShoreBoreWhelps = 0.6f;     ///< height of the trailing crests (whelps) of weak bores, relative to the bore
		float ShoreCurl = 1.0f;           ///< how far plunging breakers curl over (0: lip only)
		float ShoreCurlDetail = 2.0f;     ///< tessellation multiplier where shore waves can curl
		float SwashLinger = 2.0f;         ///< backwash duration over the uprush's
		float SwashChurnHeight = 0.05f;   ///< m: the tallest the swash's churning lumps stand
		float SwashChurnDepth = 0.35f;    ///< the lumps' height as a share of the water's depth
		float SwashChurnSize = 0.35f;     ///< m: the lumps' size
		float SwashChurnSpeed = 1.7f;     ///< how fast the lumps boil, relative to the waves' clock

		// Surface and light
		bool EnableWaveShadows = true;         ///< the waves shade the water behind them from the sun (the heightfield pass and its per-pixel march)
		bool EnablePointLightSpecular = true;  ///< lamps and torches glint in the water (the per-pixel light loop)
		bool EnableSkyOcclusion = true;        ///< the waves occlude the sky in Skylighting's probes (the occluder grid pass)
		bool EnableEnvReflections = true;      ///< the cubemap reflection (off: the water form's reflection colour)
		bool EnableSSR = true;                 ///< screen-space reflections on the water
		bool EnableSunShadows = true;          ///< the shadow map and cloud shadows on the water
		bool EnableSkylighting = true;         ///< Skylighting's probes darken the sky the water sees
		bool EnableFog = true;                 ///< distance, height fog and aerial perspective on the water
		bool BatchedStencil = true;            ///< Unified Water's far tiles drawn in one instanced stencil draw instead of one engine draw each
		bool EnableWaterline = true;           ///< the mesh is held on the bed near the shore and the waterline found per pixel (terrain reads in the vertex, stencil and surface passes)
		float Roughness = 0.06f;
		float SunSpecular = 1.0f;
		float PointLightSpecular = 1.0f;
		float PointLightEmitterRadius = 8.0f;  ///< units (~a torch flame): Skyrim lights have no emitter size
		bool EnableSubsurface = true;          ///< sunlight scattered through the water (off: not evaluated)
		float Subsurface = 1.0f;
		float RefractionDistortion = 1.0f;
		float WindRoughness = 1.0f;      ///< scales the wind-driven micro-roughness (Cox-Munk)
		bool VanillaNormalMaps = false;  ///< read the water form's normal maps on top of the FFT waves (always read without them, and on rivers)
		float DetailNormalCalm = 0.75f;  ///< strength of the vanilla normal maps on top of the waves in calm air
		float DetailNormalGale = 1.3f;   ///< and at 8 m/s of wind and over

		// Water clarity
		bool EnableClarity = true;  ///< sediment: turbidity, shallows stirring, silt, storms; off: the water form's own clarity
		float Visibility = 1.0f;
		float Turbidity = 0.15f;  ///< suspended sediment everywhere (0 = crystal clear)
		float3 SedimentColor = { 0.34f, 0.29f, 0.2f };
		float SedimentDensity = 0.35f;        ///< extinction (1/m) per unit of turbidity
		float TurbidityPatchiness = 0.7f;     ///< how unevenly the sediment is spread
		float TurbidityPatchSize = 150.0f;    ///< metres
		float ShoreResuspension = 1.0f;       ///< sediment stirred up by the waves in the shallows
		float StormTurbidity = 1.0f;          ///< extra sediment in strong wind and rain
		float WadingSilt = 1.0f;              ///< silt kicked up by feet on the bed
		float SedimentLayerHeight = 1.5f;     ///< metres the stirred-up sediment reaches above the bed
		float SiltHalfLife = 20.0f;           ///< s: how long stirred-up silt takes to settle
		float ScatteringAnisotropy = 0.6f;    ///< Henyey-Greenstein g of the particles in the water
		float DownwellingAttenuation = 1.0f;  ///< scales how fast light fades on its way down

		// Foam
		bool EnableFoam = true;         ///< white water: the sea's, the breakers' and the surf's, in the pixel shader and the foam mips
		float FoamAmount = 1.0f;        ///< multiplier on the foam the open sea's crests add (FoamAdd), and on the shore's foam coverages (Foam1.z)
		float FoamBias = 0.85f;         ///< the Jacobian below which a texel foams; also moves the breakers' fold and scales the shore's foam (Foam1.z)
		float FoamThreshold = 0.0f;     ///< foam is added only above this biased Jacobian
		float FoamAdd = 0.1f;           ///< foam added per 1/60 s per unit of biased Jacobian
		float FoamDecayRate = 0.0175f;  ///< foam decays by exp(-rate) per 1/60 s
		float FoamOffset0 = 0.04f;      ///< foam offset of each cascade, longest waves first
		float FoamOffset1 = -0.04f;
		float FoamOffset2 = -0.46f;
		float FoamOffset3 = -0.38f;
		float FoamCascadeWeightFull = 1.0f;  ///< a cascade counts fully in the foam from this share of its amplitude
		float FoamDepthFalloff = 1.0f;       ///< exponent of the foam's fade with distance
		float FoamFadeDistance = 1000.0f;    ///< m: distance at which all foam has faded out
		float FoamRoughnessModifier = 0.0f;  ///< roughness added per unit of foam
		float FoamFeather = 0.12f;           ///< how soft the edge of the foam pattern is
		float FoamBubbleSize = 0.03f;        ///< m: the bubbles of the foam pattern (surf foam is a microfoam of centimetre bubbles)

		// Spray (SprayParticles)
		bool EnableSpray = true;
		float SprayAmount = 1.0f;   ///< multiplier on every emitter's rate
		float SprayRange = 128.0f;  ///< m around the camera in which the waves throw spray
		int SprayQuality = 1;       ///< particles alive at most: 0 = 16384, 1 = 65536, 2 = 131072
		SprayParticles::Emitters SprayEmitters = SprayParticles::DefaultEmitters();

		// Ripples
		bool EnableRipples = true;
		bool EnableWakes = true;     ///< Kelvin wakes behind the actors moving through the water (PBRWater/KelvinWake.h)
		bool UnderwaterVolume = true;  ///< the water around the camera under the surface absorbs and scatters as the surface's does, in place of the game's fog
		int LightShaftSamples = 12;    ///< steps of the underwater light shafts' march (0: no shafts)
		bool PhysicalBodyColour = true;  ///< the deep water's colour from its absorption and scattering (Optics.hlsli DeepReflectance), not the water form's
		float LightShaftFocus = 1.0f;  ///< how strongly the waves focus the shafts (caustic streaks)
		float WakeStrength = 1.0f;   ///< scales their height
		float RippleExtent = 64.0f;  ///< m covered around the camera
		float RippleHeight = 0.12f;  ///< m
		float RippleNormalStrength = 1.0f;
		float RippleHalfLife = 1.5f;       ///< s
		float RippleHorizDisplace = 3.0f;  ///< horizontal displacement strength (Crest 3)
		float RippleDisplaceClamp = 0.3f;  ///< largest horizontal displacement in simulation texels (Crest 0.3)

		// Floating objects and gameplay
		bool GameplayWaves = true;
		bool EnableFloatingObjects = true;
		bool CarryActors = true;
		bool HullMasks = true;  ///< the water is not drawn inside boats and ships (FloatingObjects::HullMask)
		float BuoyancyStrength = 1.0f;
		float FloatingRange = 150.0f;   ///< metres around the player in which objects float
		float MaxFloatingSize = 90.0f;  ///< metres: the longest hull that floats (the large Skyrim ships are ~65 m)
		float FloatingResponse = 1.0f;  ///< scales how much the hulls move

		// Performance
		bool EnableTessellation = true;
		bool DeferredSurface = true;             ///< shade the tessellated water once per pixel (PBRWater/Deferred.hlsli)
		bool DistantHalfRate = true;             ///< shade the deferred surface's distant tiles at a quarter of the pixels (DeferredFar.hlsl)
		float DistantHalfRateDistance = 60.0f;   ///< metres: where the distant tiles start
		float TessellationTriangleSize = 12.0f;  ///< target triangle edge on screen (pixels)
		bool ProjectedGrid = true;               ///< draw the sea as one grid cast from the screen onto its plane (PBRWater/ProjectedGrid.h)
		float GridCellSize = 8.0f;               ///< the projected grid's cell on screen (pixels)
		float TessellationMaxFactor = 32.0f;     ///< the finest triangle edge is 128 units (a cell mesh's grid) over this
		bool AdaptiveTessellation = true;        ///< coarsen the open water's mesh where the waves leave little to resolve (PBRWater::AdaptiveCoarsening)
		float TessellationErrorPixels = 0.5f;    ///< the height error the adaptive tessellation allows (pixels)
		float TessellationMaxCoarsening = 8.0f;  ///< the most it coarsens the open water's mesh
		float DisplacementFadeStart = 16384.0f;  ///< units
		float DisplacementFadeEnd = 32768.0f;
		int WaveResolution = 1;           ///< FFT size per cascade: 0 = 128, 1 = 256, 2 = 512
		bool CheapOffscreenWater = true;  ///< reflections, cubemaps and the map draw the water without shore waves, foam, clarity or lights
		int EngineReflections = 1;        ///< the engine's water reflection cubemap: 0 full scene, 1 sky only, 2 off (needs Dynamic Cubemaps, no Cloud Shadows)
		float TriangleBudget = 1000.0f;   ///< thousands of tessellated triangles a frame before the target size grows (0: none)
		bool LongWaveHalfRate = false;    ///< the two longest cascades' FFTs alternate frames

		// Debug
		int WireframeMode = 0;
		int DebugView = 0;
	};

	Settings settings;
	/** @brief Whether the water shaders were last compiled with PBRW_DEBUG; a change recompiles them (Prepass). */
	bool debugCompiled = false;

	virtual void DrawSettings() override;
	bool HasDebugSection() const override { return true; }
	void DrawDebugSection() override;
	/** @brief The Spray tab: the spray's switches and every emitter's parameters. */
	void DrawSpraySettings();
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
	/** @brief PBRW_DEBUG while a debug view or the wireframe is active: the water shaders carry that code only then. */
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() override;
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
	static constexpr uint32_t MaxHullMasks = 8;    ///< hulls the water is kept out of, nearest the camera (PBRWater.hlsli PBRW_MAX_HULLS)
	static constexpr uint32_t HullMaskStride = 7;  ///< float4 per hull mask (PBRWater.hlsli InsideHull)
	static_assert(FloatingObjects::HullStations == 12, "InsideHull reads the widths as three float4");

	struct GpuData
	{
		float4 Cascade0[PBRWaterModel::NumCascades];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera, w texel size (units)
		float4 Cascade1[PBRWaterModel::NumCascades];  // x mean omega (rad/s), y 1 / its PM weight at unlimited fetch, z mean wavenumber (rad/unit), w slope variance
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
		float4 Cascade2[PBRWaterModel::NumCascades];  // x height std (units), yzw unused
		float4 ShoreMap;                              // xy shore field corner (absolute units), zw 1 / extent (units)
		float4 Swash0;                                // xy swash grid corner (absolute units), z 1 / extent, w enabled
		float4 Shore2;                                // x wave clock (rad, wrapped), y the clock last frame, z unused, w peak period Tp (s; > 0: the clock is set)
		float4 Shore3;                                // x swash linger, y undular bore whelps strength, z breaker curl, w deep-water wavelength L0 (units)
		float4 Shore4;                                // x angular frequency the current shore field was traced for (rad/s), y the previous field's, zw unused
		float4 Shore6;                                // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                                 // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                                // xy surface noise grid corner (absolute units), z 1 / extent (units), w baked this frame
		float4 Tune1;                                 // foam offset of each cascade
		float4 Tune2;                                 // x foam cascade weight, y foam distance falloff exponent, z foam fade distance (m), w foam roughness modifier
		float4 Tune3;                                 // x ripple horizontal displacement, y its clamp (texels), z shore train reach (waves), w surf residual foam
		float4 Tune4;                                 // x shore wave height variation, y mean crest length (deep wavelengths), z detail normal strength calm, w gale
		float4 Tune5;                                 // x point light emitter radius (units), y foam feather, z foam bubble size (m), w shore foam lifetime (s)
		float4 Foam2;                                 // xy shore foam grid corner (absolute units), z 1 / extent, w enabled
		float4 SunMap0;                               // xy wave heightfield centre (absolute units), z 1 / extent, w drawn this frame
		float4 SunMap1;                               // x its reference level (absolute z), y height range either side (units), zw unused
		float4 Swash1;                                // swash churn: x tallest lump (units), y share of the depth, z lump size (units), w boil speed
		float4 Proj0;                                 // a projected grid's draw: x columns, y its level (absolute z), z cells listed, w its first vertex
		float4 Proj1;                                 // the cells (SeaLevels): xy first cell, zw cells across
		float4 Proj2;                                 // x its first cell, y its first patch, z a patch's cells across, w unused
		float4 Proj3;                                 // unused
		float4 WakeInfo;                              // x Kelvin wakes this frame
		float4 Wake0[PBRWaterWake::MaxSources];       // per wake: xy absolute position (units), z k0 = g / U^2 (1/unit), w peak height (units)
		float4 Wake1[PBRWaterWake::MaxSources];       // xy heading, zw unused

		float4 Hull0;                                    // x hull masks listed (HullData), yzw unused
		float4 HullData[MaxHullMasks * HullMaskStride];  // per hull: its frame's rows (axis, -axis . centre, centre relative to RefCamPos); bottom, top, front, back; the half widths at its stations
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

	/// Every pass the batch renderer draws: Unified Water's far stencil tiles are collected here instead (CollectStencilTile).
	template <int N>
	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags);
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
	/** @brief Trims or skips the engine's water reflection cubemap render (Settings::EngineReflections). */
	struct TESWaterReflections_UpdateActor
	{
		static bool thunk(RE::TESWaterReflections* reflections);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	/** @brief The engine reflection mode in effect: the setting, limited by what the other features need from the cubemap. */
	int EffectiveEngineReflections() const;

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

	/// The world's effects pass (vanilla particles and effect shaders), after the water: the sea spray draws first.
	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* accumulator, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	static constexpr uint32_t WaterSRVBase = 80;         ///< first slot of the water textures (t80-t97 in the vertex, pixel and compute stages)
	static constexpr uint32_t WaterSRVCount = 18;        ///< t80-t97
	static constexpr uint32_t WaterSamplerBase = 14;     ///< s14 (linear clamp) and s15 (ocean, anisotropic wrap)
	static constexpr uint32_t SwashWetSlot = 104;        ///< t104, pixel stage: the swash's wet sand for Lighting (PBRWaterShore)
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
	/** @brief Render thread: the SRVs of slots t80-t96. */
	void GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const;
	/// False for a water tile wholly outside the waves' reach: flat, so it needs no tessellation.
	bool WavesReach(const RE::NiAVObject& geometry) const;
	/** @brief One shore field on the GPU. */
	struct ShoreTextures
	{
		std::shared_ptr<const PBRWaterModel::ShoreField> field;
		std::unique_ptr<Texture2D> times;  ///< travel time to the waterline, signed distance to it, still water level
		std::unique_ptr<Texture2D> rays;   ///< grad of the travel time, beach slope, exposure to the swell
		std::unique_ptr<Texture2D> room;   ///< x room for the wave train (width of the water against the wavelength, coherent fronts), y the depth a wave's crest answers to (m; ShoreField::crestDepth)
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
	/** @brief Render thread: emits and flies the sea spray (needs this frame's constants and the surf's state). */
	void UpdateSpray(float dt);
	/** @brief Render thread, the world's effects pass: draws the sea spray once a frame, after the water. */
	void DrawSpray();
	/** @brief Particles alive at most for the SprayQuality setting. */
	uint32_t SprayPoolSize() const;
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
	std::unique_ptr<ConstantBuffer> drawBuffer;           ///< this frame's constants for the tessellated mesh (Tess0.x = 1: BisectionTessellator)
	std::unique_ptr<ConstantBuffer> flatDrawBuffer;       ///< for the untessellated draws (Tess0.x = 0)
	std::unique_ptr<ConstantBuffer> offscreenDrawBuffer;  ///< for the untessellated draws of other views (reflections, cubemaps, the map) with CheapOffscreenWater: Tess0.x = 0 and the costly gates off
	std::unique_ptr<ConstantBuffer> spacingDrawBuffer;    ///< untessellated draws filtered to their vertex spacing (Draw0.x)
	std::unique_ptr<ConstantBuffer> swallowDrawBuffer;    ///< for the engine's own draw of a mesh the tessellator draws instead (Tess0.x = 2: culled)
	std::array<std::unique_ptr<ConstantBuffer>, ProjectedGrid::MaxLevels> gridDrawBuffers;   ///< per projected grid: its cells (Tess0.x = 6)
	std::array<std::unique_ptr<ConstantBuffer>, ProjectedGrid::MaxLevels> patchDrawBuffers;  ///< and its breakers' patches (Tess0.x = 7)
	std::unique_ptr<ConstantBuffer> riverDrawBuffer[2];   ///< b8 of the vertex stage: the untessellated draws' river bit (Water.hlsl PBRWRiver), off and on
	std::unique_ptr<ConstantBuffer> deferredDrawBuffer;   ///< for the deferred surface's rectangle around a tessellated mesh (Tess0.x = 3)

	// The deferred surface (PBRWater/Deferred.hlsli): the stencil pass records each pixel of the tessellated water, and the
	// surface and point light passes shade those pixels once each instead of drawing the mesh's small triangles again.
	/// Mirrors DeferredCommon.hlsli: the tiles' size (pixels) and the most meshes recorded a frame (one bit each per tile).
	static constexpr uint32_t DeferredTile = 8;
	static constexpr uint32_t DeferredMeshes = 32;
	/// Each mesh has two tile lists: list m its near tiles, DeferredMeshes + m its distant ones (DeferredTilesCS).
	static constexpr uint32_t DeferredLists = 2 * DeferredMeshes;
	/// Mirrors DeferredCommon.hlsli: what a deferred draw does (DeferredDrawCB::mode[1]).
	static constexpr uint32_t DeferredFull = 0;
	static constexpr uint32_t DeferredHalf = 1;
	struct alignas(16) DeferredDrawCB
	{
		uint32_t tag[4] = {};  ///< x the mesh's tag (1..DeferredMeshes), y the tile list's room per list, z texture coordinates recorded
		float4 viewport{};     ///< xy the viewport's size (pixels)
		float4 uvMapU{};       ///< the mesh's texture coordinates over its model-space xy (WaterMeshes::AffineTexCoords)
		float4 uvMapV{};
		uint32_t mode[4] = {};  ///< x the tile list drawn, y DeferredFull or DeferredHalf, z the half-rate pass's sample rotation
	};
	/// A tessellated mesh the stencil pass recorded this frame.
	struct DeferredMesh
	{
		uint32_t startIndex = 0;  ///< its mesh slot (BisectionTessellator::Lookup)
		bool texCoords = false;   ///< its texture coordinates are recorded (not one plane over the mesh)
		float4 uvMapU{};
		float4 uvMapV{};
	};
	// The stencil pass's record (Deferred.hlsli): SV_Target2-5 there, t38-t41 of the surface.
	std::unique_ptr<Texture2D> deferredTags;              ///< R32_UINT: tag, vertex alpha, 16 bits of the depth
	std::unique_ptr<Texture2D> deferredWaves;             ///< R16G16B16A16_FLOAT: the waves' displacement, the waves without the surf
	std::unique_ptr<Texture2D> deferredTexCoords;         ///< R32G32_FLOAT: texture coordinates, of the meshes that need them
	std::unique_ptr<ConstantBuffer> deferredCB;           ///< b13 (DeferredDrawCB)
	winrt::com_ptr<ID3D11ComputeShader> deferredTilesCS;  ///< DeferredTilesCS: sorts the tiles by mesh
	std::unique_ptr<ConstantBuffer> deferredTilesCB;
	winrt::com_ptr<ID3D11Buffer> deferredArgs;  ///< per list: DrawInstancedIndirect's arguments
	winrt::com_ptr<ID3D11UnorderedAccessView> deferredArgsUAV;
	std::unique_ptr<Buffer> deferredTiles;  ///< per list: its tiles (t101 of the vertex stage)
	uint32_t deferredMaxTiles = 0;
	std::vector<DeferredMesh> deferredMeshes;  ///< this frame's recorded meshes, by tag - 1
	uint32_t deferredMeshesFrame = UINT32_MAX;
	uint32_t deferredTilesFrame = UINT32_MAX;
	// The shore train once per 2x2 pixels of the record (DeferredShoreCS): t46-t48 of the surface and point light passes.
	std::unique_ptr<Texture2D> deferredShore[3];          ///< R32G32B32A32_UINT at half the record's size
	winrt::com_ptr<ID3D11ComputeShader> deferredShoreCS;  ///< null when it failed to compile: the pixels evaluate the train
	std::unique_ptr<ConstantBuffer> deferredShoreCB;
	uint32_t deferredShoreFrame = UINT32_MAX;
	// The surface's material (PBRWater/SurfaceMaterialCS.hlsl, Material.hlsli): the waves, shore train and foam coverage of every
	// recorded pixel, evaluated once after the stencil pass, so the surface and point light passes' pixel shader (the
	// SIE::ShaderCache::WaterShaderFlags::DeferredMaterial permutation) holds only the lighting.
	std::unique_ptr<Texture2D> surfaceMaterial[2];          ///< R32G32B32A32_UINT and R32G32_UINT at the record's size: t75 and t107 of the surface and point light passes (24 bytes a pixel)
	winrt::com_ptr<ID3D11ComputeShader> surfaceMaterialCS;  ///< null when it failed to compile: the pixel shader evaluates the waves itself
	std::unique_ptr<ConstantBuffer> surfaceMaterialCB;
	uint32_t surfaceMaterialFrame = UINT32_MAX;        ///< the frame the material was last evaluated
	bool surfaceMaterialFailed = false;                ///< the compute shader or its textures could not be made: never taken
	static constexpr UINT SurfaceMaterialSlotA = 75;   ///< the material's first texture in the pixel stage (Deferred.hlsli)
	static constexpr UINT SurfaceMaterialSlotB = 107;  ///< and its second
	RE::BSShader* cachedDrawShader = nullptr;          ///< the water shader of the current draw (SetupDraw)
	uint32_t cachedDrawPixelDescriptor = 0;            ///< its pixel shader's descriptor
	/** @brief Render thread, the first deferred surface draw of a frame: evaluates the surface's material for the record's pixels; false if it could not. */
	bool BakeSurfaceMaterial(ID3D11ShaderResourceView* depthSRV);
	/** @brief Render thread, DrawDeferredSurface: the pixel shader of the current draw that reads the material, null while it compiles (or the material is not ready). */
	ID3D11PixelShader* GetMaterialPixelShader();
	// The distant tiles' half-rate shading (PBRWater/DeferredFar.hlsl): the surface pass shades one pixel of every 2x2 into
	// deferredFar, then upsamples it into the tiles with the surface draw's own state.
	std::unique_ptr<Texture2D> deferredFar;            ///< R16G16B16A16_FLOAT at half the record's size: colour, 1 + the shaded pixel
	winrt::com_ptr<ID3D11VertexShader> deferredFarVS;  ///< the distant tiles at full size
	winrt::com_ptr<ID3D11PixelShader> deferredFarPS;   ///< the upsampling
	bool deferredFarFailed = false;                    ///< the upsampling failed to compile: the distant tiles are shaded at full rate
	uint32_t deferredFarFrame = UINT32_MAX;            ///< the frame deferredFar was last cleared
	static constexpr UINT DeferredFarSlot = 49;        ///< the half-rate target in the upsampling's pixel stage (DeferredFar.hlsl t49)
	/** @brief Render thread, DrawDeferredSurface: shades the slot's distant tiles at half rate and upsamples them; false to shade them at full rate. */
	bool DrawDistantHalfRate(uint32_t slot, DeferredDrawCB data);
	/** @brief Render thread, the first deferred surface draw of a frame: evaluates the shore train per 2x2 pixels of the record. */
	void BakeDeferredShore(ID3D11ShaderResourceView* depthSRV);
	/** @brief Render thread: (re)creates the stencil pass's records at the depth buffer's size; false when there is none. */
	bool EnsureDeferredRecords();
	/** @brief Render thread, RestoreDraw: draws the cached mesh in the stencil pass, recording its pixels. */
	void DrawRecordedStencil();
	/** @brief Render thread: sorts the screen's tiles by the meshes the stencil pass recorded (DeferredTilesCS). */
	void ClassifyDeferredTiles();
	/** @brief Render thread, RestoreDraw: shades the pixels the stencil pass recorded for the cached mesh; false to draw it forward. */
	bool DrawDeferredSurface();
	/// The deferred surface's state, bound once for a batch of water draws instead of around every draw.
	struct DeferredBatchState
	{
		bool recording = false;                        ///< the stencil batch: the record's targets and blend state are bound
		bool shading = false;                          ///< a surface or point light batch: the read-only depth and the record are bound
		UINT recordTargets = 0;                        ///< targets bound while recording: 5, or 6 with texture coordinates
		ID3D11BlendState* recordBlend = nullptr;       ///< the blend state bound while recording (one of recordBlendStates)
		winrt::com_ptr<ID3D11BlendState> engineBlend;  ///< the engine's, put back at the batch's end
		float blendFactor[4] = {};
		UINT sampleMask = 0xFFFFFFFF;
		ID3D11DepthStencilView* readOnly = nullptr;        ///< the read-only depth view bound while shading
		winrt::com_ptr<ID3D11DepthStencilView> engineDSV;  ///< the engine's writable one, put back at the batch's end
	};
	DeferredBatchState deferredBatch;
	std::unique_ptr<ConstantBuffer> deferredZeroCB;  ///< b13 between the recorded draws: tag 0, no record
	/** @brief Render thread, EndDrawBatch: puts back the engine's state. */
	void EndDeferredBatch();
	/// The water meshes the tessellated draws drew, for the tessellator to subdivide.
	WaterMeshes waterMeshes;
	BisectionTessellator bisection;

	// The projected grids (PBRWater/ProjectedGrid.h): drawn in place of every water mesh at the sea's level and the lakes'.
	ProjectedGrid projectedGrid;
	std::shared_ptr<const CellLevels> gridCells;                   ///< the worldspace cells the grids were given
	std::unordered_map<uint32_t, uint32_t> gridPassFrame;          ///< technique * 8 + grid slot -> the frame its pass last drew the grid
	static constexpr uint32_t GridMeshStart = 0xFFFFFFFFu;         ///< cachedDrawStart of grid slot 0 (minus the slot: the deferred surface's mesh key)
	static constexpr UINT SeaLevelsSlot = 28;                      ///< pixel stage view of the grids' cells (PBRWater.hlsli SeaLevels)
	static constexpr float GridMaxDistance = 1000000.0f;           ///< the farthest a grid is cast (units)
	static bool IsGridKey(uint32_t start) { return start > GridMeshStart - ProjectedGrid::MaxLevels; }
	/** @brief The slot of the projected grid that draws `geometry` this frame (a water mesh at its level, not a river's), or -1. */
	int GridSlotOf(RE::BSGeometry& geometry) const;
	/** @brief Projected grid `slot` in the current draw's state: its cells, then its breakers' patches (Water.hlsl). */
	void DrawGrid(uint32_t slot);
	float budgetCoarsening = 1.0f;  ///< render thread: the target size's growth that holds the tessellated triangles to TriangleBudget (Draw0.w)
	/** @brief Render thread, RestoreDraw: draws the current draw's tessellated mesh. */
	void DrawCachedMesh();
	/** @brief Render thread (Prepass): subdivides and evaluates this frame's tessellated water, for every pass to draw. */
	void UpdateTessellation();
	GpuData frameData{};
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
	std::atomic<std::shared_ptr<const std::vector<PBRWaterWake::Source>>> wakeSources;  ///< main thread -> render thread: this frame's wakes
	winrt::com_ptr<ID3D11ShaderResourceView> kelvinPattern;             ///< the Kelvin wake pattern (t91)

	// The water around the camera under the surface (UnderwaterVolume.hlsl), in place of the game's underwater fog.
	winrt::com_ptr<ID3D11VertexShader> underwaterVS;
	winrt::com_ptr<ID3D11PixelShader> underwaterPS;
	winrt::com_ptr<ID3D11BlendState> underwaterBlend;  ///< dual-source: added + existing * transmittance
	std::unique_ptr<ConstantBuffer> underwaterCB;
	uint32_t underwaterFrame = UINT32_MAX;
	// The light shafts at half resolution (UnderwaterShaftsCS.hlsl): the march, its denoising, two histories.
	winrt::com_ptr<ID3D11PixelShader> underwaterMarchPS;
	winrt::com_ptr<ID3D11ComputeShader> underwaterTemporalCS;
	std::array<winrt::com_ptr<ID3D11ComputeShader>, 2> underwaterBlurCS;          ///< horizontal, vertical
	std::array<std::unique_ptr<Texture2D>, 4> underwaterShaftTargets;              ///< march (and the result), history 0, history 1, blur middle
	uint32_t underwaterHistoryFrame = UINT32_MAX;                                   ///< the frame the last history was written
	std::unordered_map<RE::TESWaterForm*, float> originalUnderwaterFog;  ///< main thread: each water form's fog amount before it was zeroed
	/** @brief Main thread: zeroes every water form's underwater fog amount while the volume replaces it, restores it otherwise. */
	void ApplyUnderwaterFogOverride();
	/** @brief The water between the camera and what each pixel shows, while the camera is under water (before the water's passes). */
	void DrawUnderwaterVolume();
	/** @brief Compiles the underwater volume's shaders (at setup, and again when the shader cache is cleared). */
	void CompileUnderwater();
	std::unordered_map<const void*, RE::NiPoint3> lastWaveVelocities;  ///< main thread: wave velocity added per body
	uint32_t uploadedTerrainGeneration = UINT32_MAX;
	std::shared_ptr<const PBRWaterModel::Bathymetry> uploadedTerrain;
	winrt::com_ptr<ID3D11SamplerState> linearClampSampler;
	winrt::com_ptr<ID3D11SamplerState> oceanSampler;

	RippleSimulation ripples;
	ShoreSimulation swash;                 ///< render thread
	SprayParticles spray;                  ///< render thread (its definitions load on the main thread)
	uint32_t sprayDrawFrame = UINT32_MAX;  ///< State frame the spray was last drawn in
	ShoreTextures shoreTextures;           ///< render thread: the current shore field
	ShoreTextures previousShoreTextures;   ///< render thread: the one it replaces, while they blend
	OceanSimulation ocean;                 ///< render thread

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
	struct LevelCache
	{
		uint32_t frame = UINT32_MAX;
		float cellX = 0.0f, cellY = 0.0f;
		bool anyWater = false;
		float4 levels[25]{};
	};
	mutable LevelCache levelCache;  ///< FillOccluderLevels' last scan
	/** @brief Render thread: draws the displaced water grid `cb` describes, depth only, into `dsv`; restores the state it changes. */
	void DrawWaveGrid(ID3D11DepthStencilView* dsv, UINT width, UINT height, const OccluderCB& cb, ID3D11Buffer* indices, DXGI_FORMAT indexFormat, UINT indexCount);

	// The waves' heightfield around the eye, which the water marches towards the sun through for their sun shadows (render thread)
	static constexpr uint32_t SunMapQuads = 256;    ///< grid quads per side (23 cm: shorter waves' shadows are left to the march's bias)
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
	std::atomic<bool> surfNearby{ true };           ///< main thread -> render thread: a breaking beach lies within the surf grid
	float surfIdleTime = 0.0f;                      ///< main thread: s since the last beach seen near the camera
	bool surfAsleep = false;                        ///< render thread: the surf passes are skipped
	float smoothedWindSpeed = -1.0f;
	float smoothedWindDirX = 1.0f;
	float smoothedWindDirY = 0.0f;
	std::atomic<std::shared_ptr<const PBRWaterModel::WaveSnapshot>> snapshot;
	std::atomic<float> renderDelta{ 0.0f };
	std::atomic<uint32_t> neededCascades{ 0xF };  ///< main thread: cascades with any weight in the water around the camera
	uint32_t cascadeMaskCalls = 0;                ///< main thread: MainThreadUpdate calls, the mask is sampled every 10th
	std::atomic<float> weatherTurbidity{ 0.0f };
	float smoothedWeatherTurbidity = 0.0f;
	std::atomic<float> rainIntensity{ 0.0f };
	std::atomic<float> cameraSurface{ 0.0f };  ///< displaced water height above the camera (absolute z)
	std::atomic<bool> cameraSurfaceValid{ false };
	std::atomic<uint32_t> cameraSurfaceFrame{ 0 };  ///< frame the main thread worked it out in
	float smoothedRain = 0.0f;

	// Render thread

	// Per-draw state (render thread)
	bool geometryShaderBound = false;
	/// The tessellated mesh for the current draw (its slot), issued in RestoreDraw once the engine's state is bound.
	bool cachedDrawPending = false;
	uint32_t cachedDrawStart = 0;
	uint32_t cachedDrawTechnique = 0;
	RE::BSGeometry* cachedDrawGeometry = nullptr;
	bool cachedDrawRiver = false;  ///< the current draw's mesh is a river's: never taken by the deferred surface
	/// The water draws of one frame, for the Debug section.
	struct DrawStats
	{
		uint32_t draws = 0;
		uint32_t computeDraws = 0;             ///< draws of the tessellated mesh
		uint32_t byTechnique[6] = {};          ///< draws per technique (DrawPassIndex)
		uint32_t deferredRecorded = 0;         ///< meshes the stencil pass recorded for the deferred surface
		uint32_t deferredShaded = 0;           ///< surface and point light draws shaded from the record
		uint32_t deferredForward = 0;          ///< tessellated draws that fell back to drawing their triangles
		const char* deferredReason = nullptr;  ///< why the last of those did
	};
	/** @brief Render thread: a tessellated draw the deferred surface could not take, and why (Debug section). */
	void DeferredFallback(const char* reason);
	/// The stencil pass's blend state with the record's targets (2-5) written whole and unblended, by the engine's state.
	std::unordered_map<ID3D11BlendState*, winrt::com_ptr<ID3D11BlendState>> recordBlendStates;
	/** @brief Render thread: the blend state for recording under `engineState` (recordBlendStates). */
	ID3D11BlendState* RecordBlendState(ID3D11BlendState* engineState);
	GpuData uploadedDrawData{};                              ///< what the draw buffers hold
	float uploadedSpacing = -1.0f;                           ///< the spacing spacingDrawBuffer holds
	bool uploadedSpacingCheap = false;                       ///< whether spacingDrawBuffer holds CheapOffscreenGates
	ID3D11ShaderResourceView* drawSRVs[WaterSRVCount] = {};  ///< the views for those constants
	uint32_t drawResourcesFrame = UINT32_MAX;                ///< the frame the views were last bound in
	static constexpr uint32_t StencilInstanceSlot = 103;     ///< vertex stage view of the instanced stencil tiles (Water.hlsl)
	static constexpr uint32_t MaxStencilInstances = 4096;
	std::unique_ptr<ConstantBuffer> instancedDrawBuffer;  ///< the instanced stencil tiles' constants (Tess0.x = 5)
	std::unique_ptr<StructuredBuffer> stencilInstanceBuffer;
	std::vector<float4> stencilInstances;  ///< collected tiles: absolute translate, scale
	std::vector<float4> stencilUpload;     ///< MaxStencilInstances entries, what the buffer upload copies
	struct StencilBatch
	{
		bool ready = false;            ///< the engine's last draw was a far Unified Water stencil tile: its state draws the rest
		uint32_t technique = 0;        ///< that draw's technique (the same permutation)
		void* rendererData = nullptr;  ///< that draw's mesh (every Unified Water tile shares it)
		uint32_t indexCount = 0;
	} stencilBatch;
	ID3D11Buffer* boundVertexCb = nullptr;  ///< the vertex stage's b7 as the draws left it (BindDrawResources resets)
	ID3D11Buffer* boundRiverCb = nullptr;   ///< the vertex stage's b8 as the draws left it (BindDrawResources resets)
	ID3D11Buffer* boundPixelCb = nullptr;   ///< the pixel stage's b7 as the draws left it
	DrawStats drawStats;                    ///< this frame so far
	bool drawBatchOpen = false;             ///< a profiler pass is open for the current batch of water draws
	/** @brief Render thread, a batch of water draws begins: the frame's wave heightfield, then the batch's pass. */
	void SetupBatch();
	/**
	 * @brief Takes a far, flat Unified Water tile of the main view's stencil batch for the instanced draw.
	 * @return true when the tile was taken and the engine must not draw it
	 */
	bool CollectStencilTile(RE::BSRenderPass* pass, uint32_t technique);
	/** @brief After the engine drew a pass: whether its state can draw the collected tiles (one far Unified Water tile in the stencil batch). */
	void AfterEnginePass(RE::BSRenderPass* pass, uint32_t technique);
	/** @brief Draws the collected stencil tiles in one instanced draw with the state of the last tile the engine drew. */
	void FlushStencilInstances();
	/** @brief Uploads the draw constant buffers when this frame's constants changed (once per batch). */
	void UploadDrawConstants();
	/** @brief Binds the views and samplers every water draw reads, and forgets which constant buffers are bound. */
	void BindDrawResources();
	/** @brief Render thread: opens the profiler's pass for this batch of water draws, named by its technique. */
	void BeginDrawBatch(uint32_t technique);
	/** @brief Render thread: closes it (RestoreTechnique). */
	void EndDrawBatch();
	// Main thread update cost (MainThreadUpdate), for the Debug section.
	std::atomic<float> mainUpdateMs{ 0.0f };      ///< smoothed (about 1 s)
	std::atomic<float> mainUpdatePeakMs{ 0.0f };  ///< decaying peak
	DrawStats lastDrawStats;                      ///< the last complete frame
	uint32_t drawStatsFrame = UINT32_MAX;
};
