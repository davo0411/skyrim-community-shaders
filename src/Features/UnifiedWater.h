#pragma once
#include "OverlayFeature.h"
#include "UnifiedWater/Flowmap.h"
#include "UnifiedWater/WaterCache.h"
#include "UnifiedWater/WaterSettings.h"
#include "UnifiedWater/WaterTessellation.h"
#include "UnifiedWater/WaterWaves.h"
#include "UnifiedWater/WaterRipples.h"
#include <cstdint>
#include <limits>
#include <unordered_map>

// Ensure BGS terrain classes are available
#include "RE/B/BGSTerrainBlock.h"
#include "RE/B/BGSTerrainNode.h"
#include "RE/C/Console.h"

struct UnifiedWater : OverlayFeature
{
	virtual inline std::string GetName() override { return "Unified Water"; }
	virtual inline std::string GetShortName() override { return "UnifiedWater"; }
	virtual inline std::string_view GetShaderDefineName() override { return "UNIFIED_WATER"; }
	virtual std::string_view GetCategory() const override { return "Water"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Enhanced water rendering system with improved wave simulation and foam generation.",
			{ "Optimized water meshes for better performance",
				"Gerstner wave simulation for realistic water movement",
				"Advanced depth-based foam generation",
				"Enhanced flowmap support for dynamic water flow",
				"Seamless integration with existing water effects" }
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	// Re-export types for backwards compatibility and JSON serialization
	using GeneralSettings = UnifiedWaterSettings::GeneralSettings;
	using TessellationSettings = UnifiedWaterTessellation::TessellationSettings;
	using WaveSettings = UnifiedWaterWaves::WaveSettings;
	using LightingSettings = UnifiedWaterSettings::LightingSettings;
	using FogSettings = UnifiedWaterSettings::FogSettings;
	using DepthSettings = UnifiedWaterSettings::DepthSettings;
	using RippleSettings = UnifiedWaterRipples::RippleSettings;
	using FoamSettings = UnifiedWaterSettings::FoamSettings;
	using Settings = UnifiedWaterSettings::Settings;

#pragma warning(push)
#pragma warning(disable: 4324)
	struct alignas(16) PerFrame
	{
		float WaveIntensity;
		float WaveAmplitude;
		float WaveSpeed;
		float WaveSteepness;
		float GameTimeHours;
		float RealTimeSeconds;
		float TimeScale;
		float CellWorldSize;
		float PrevGameTimeHours;
		float PrevRealTimeSeconds;
		float PrevTimeScale;
		
		// Water Lighting Overrides
		float EnableLightingOverrides;
		float FresnelBias;
		float FresnelPower;
		float ReflectionStrength;
		float RefractionStrength;
		float WaterTransparency;
		float AbsorptionDensity;
		float ScatteringCoeff;
		float SpecularIntensity;
		
		// Sun Specular Overrides
		float SunSpecularPower;
		float SunSpecularMagnitude;
		float SunSparklePower;
		float SunSparkleMagnitude;
		float SpecularRadius;
		float SpecularBrightness;
		
		// Fog Overrides
		float AboveWaterFogDistNear;
		float AboveWaterFogDistFar;
		float AboveWaterFogAmount;
		float UnderwaterFogDistNear;
		float UnderwaterFogDistFar;
		float UnderwaterFogAmount;
		
		// Depth Properties
		float DepthReflections;
		float DepthRefractions;
		float DepthNormals;
		float DepthSpecularLighting;
		
		// Debug visualizer
		float WireframeEnabled;
		float PerFramePad0;
		float PerFramePad1;
		float PerFramePad2;
		
		// Wave 1 (Primary) - Large swells
		float Wave1Amplitude;
		float Wave1Wavelength;
		float Wave1Steepness;
		float Wave1AngleOffset;
		
		// Wave 2 (Secondary) - Medium waves
		float Wave2Amplitude;
		float Wave2Wavelength;
		float Wave2Steepness;
		float Wave2AngleOffset;
		
		// Wave 3 (Detail) - Small waves
		float Wave3Amplitude;
		float Wave3Wavelength;
		float Wave3Steepness;
		float Wave3AngleOffset;
		
		// Wave 4 (Fine Ripple 1) - Sub-meter detail
		float Wave4Amplitude;
		float Wave4Wavelength;
		float Wave4Steepness;
		float Wave4AngleOffset;
		
		// Wave 5 (Fine Ripple 2) - Micro ripples
		float Wave5Amplitude;
		float Wave5Wavelength;
		float Wave5Steepness;
		float Wave5AngleOffset;
		
		// Wave 6 (Fine Ripple 3) - Tiny surface detail
		float Wave6Amplitude;
		float Wave6Wavelength;
		float Wave6Steepness;
		float Wave6AngleOffset;
		
		// Tessellation control
		float TessellationEnabled;
		float WaveFadeStart;      // Distance where waves start fading
		float WaveFadeEnd;        // Distance where waves fully fade
		float TessPadding3;
		
		// Player ripples data
		float PlayerPosX;
		float PlayerPosY;
		float PlayerPosZ;
		float PlayerSpeed;
		float PlayerInWater;
		float PlayerVelocityX;  // Actual velocity for wake direction
		float PlayerVelocityY;
		float PlayerWaterDepth;  // Depth below water surface
		float RippleStrength;
		float RippleRadius;
		float RippleWaveSpeed;
		float RippleWaveFreq1;
		float RippleWaveFreq2;
		float RippleWaveFreq3;
		float RippleNormalStrength;
		
		// Foam System
		float FoamEnabled;
		float FoamIntensity;
		float FoamIntensityFlowmap;
		float FoamThreshold;
		float FoamSharpness;
		float FoamLargeWaveSlopeRequirement;
		float FoamSmallWaveSlopeMultiplier;
		float FoamSmallWaveBaseOffset;
		float FoamSmallWaveHeightRange;
		float FoamPad0;
		float FoamPad1;
		float FoamPad2;
		
		// Depth-based wave control
		float ShallowWaveDepthMin;
		float ShallowWaveDepthMax;
		float ShoreWaveDepthThreshold;
		float ShoreWaveStrength;
		
		// Terrain heightmap parameters (for vertex shader depth estimation)
		float TerrainHeightmapEnabled;
		float TerrainScaleX;
		float TerrainScaleY;
		float TerrainOffsetX;
		float TerrainOffsetY;
		float TerrainZRangeMin;
		float TerrainZRangeMax;
		float TerrainPad0;
	};

	// Re-export types from modules for backwards compatibility
	using ActorRippleData = UnifiedWaterRipples::ActorRippleData;
	using ActorRippleBuffer = UnifiedWaterRipples::ActorRippleBuffer;
	using TessellationParams = UnifiedWaterTessellation::TessellationParams;
#pragma warning(pop)

	struct alignas(16) PerTile
	{
		float PrevData[4];  // x/y = prev normal, z = prev distance, w = prev segments per axis
		float TileData[4];  // x/y = tile cell coords, z = LOD level, w = tile span (cells)
	};

	Settings settings;
	ConstantBuffer* perFrame = nullptr;
	ConstantBuffer* perTile = nullptr;
	ConstantBuffer* actorRippleBuffer = nullptr;

	float lastGameTimeHours = 0.0f;
	float lastRealTimeSeconds = 0.0f;
	float lastTimeScale = 1.0f;
	float currentGameTimeHours = 0.0f;
	float currentRealTimeSeconds = 0.0f;
	float currentTimeScale = 1.0f;
	std::uint32_t lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	bool hasLastTimingSample = false;

	struct PrevTileData
	{
		float normalX = 0.0f;
		float normalY = 0.0f;
		float distance = 10000.0f;
		float segmentsPerAxis = 32.0f;
	};

	std::unordered_map<std::uint64_t, PrevTileData> prevTileData;
	
	virtual void SetupResources() override;
	virtual void Reset() override;

	struct TESWaterSystem_InitializeWater_SetWaterShaderMaterialParams
	{
		static void thunk(RE::TESWaterForm* form, RE::BSWaterShaderMaterial* material);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TESWaterSystem_InitializeWater
	{
		static void thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool noDisplacement, bool isProcedural);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShaderMaterial_ComputeCRC32
	{
		static int32_t thunk(RE::BSWaterShaderMaterial* material, uint32_t srcHash);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BGSTerrainBlock_Attach
	{
		static void thunk(RE::BGSTerrainBlock* block);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BGSTerrainBlock_Detach
	{
		static void thunk(RE::BGSTerrainBlock* block);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BGSTerrainNode_UpdateWaterMeshSubVisibility
	{
		static void thunk(const RE::BGSTerrainNode* node, RE::BSMultiBoundNode* waterParent);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TES_SetWorldSpace
	{
		static void thunk(RE::TES* tes, RE::TESWorldSpace* worldSpace, bool isExterior);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TES_DestroySkyCell
	{
		static void thunk(RE::TES* tes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSWaterShader_RestoreGeometry
	{
		static void thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TESWaterSystem_UpdateDisplacementMeshPosition
	{
		static void thunk(RE::TESWaterSystem* waterSystem);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	virtual void DrawSettings() override;

	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override;

	virtual void DataLoaded() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; }

	virtual void PostPostLoad() override;

private:
	RE::NiPointer<RE::BSTriShape> waterMesh;
	std::uint16_t baseVertexCount = 0;
	std::uint16_t baseTriangleCount = 0;
	std::uint16_t optimisedVertexCount = 0;
	std::uint16_t optimisedTriangleCount = 0;
	RE::NiPointer<RE::BSTriShape> optimisedWaterMesh;
	Flowmap* flowmap = nullptr;
	WaterCache* waterCache = nullptr;

	RE::NiNode** gWaterLOD = nullptr;
	RE::NiPointer<RE::NiSourceTexture>* gFlowMapSourceTex = nullptr;
	int32_t* gFlowMapSize = nullptr;
	float4* gDisplacementCellTexCoordOffset = nullptr;
	RE::NiPoint2* gDisplacementMeshPos = nullptr;
	RE::NiPoint2* gDisplacementMeshFlowCellOffset = nullptr;
	
	void SetFlowmapTex() const;
	static bool LoadOrderChanged();
};
