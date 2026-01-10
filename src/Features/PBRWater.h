#pragma once

#include "Feature.h"
#include "Water/WaterSettings.h"
#include "Water/WaterTessellation.h"
#include "Water/WaterWaves.h"
#include "Water/WaterRipples.h"

struct PBRWater : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "PBR Water"; }
	virtual inline std::string GetShortName() override { return "PBRWater"; }
	virtual inline std::string_view GetCategory() const override { return "Water"; }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Physically-based water rendering with tessellation and Gerstner waves.",
			{
				"Hardware tessellation for dynamic mesh density",
				"Gerstner wave simulation for realistic water movement",
				"PBR lighting overrides for water surfaces",
				"Actor wading ripples and foam generation",
			}
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "PBR_WATER"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Water; }

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void PostPostLoad() override;
	virtual void Reset() override;

	////////////////////////////////////////////////// Shader Hooks
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

	////////////////////////////////////////////////// Settings Types
	using GeneralSettings = UnifiedWaterSettings::GeneralSettings;
	using TessellationSettings = UnifiedWaterTessellation::TessellationSettings;
	using WaveSettings = UnifiedWaterWaves::WaveSettings;
	using RippleSettings = UnifiedWaterRipples::RippleSettings;
	using FoamSettings = UnifiedWaterSettings::FoamSettings;

	struct Settings
	{
		UnifiedWaterSettings::GeneralSettings general{};
		UnifiedWaterTessellation::TessellationSettings tessellation{};
		UnifiedWaterWaves::WaveSettings waves{};
		UnifiedWaterRipples::RippleSettings ripples{};
		UnifiedWaterSettings::FoamSettings foam{};
	};

	////////////////////////////////////////////////// Per-Frame Data
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
		float WaveFadeStart;
		float WaveFadeEnd;
		float TessPadding3;
		
		// Player ripples data
		float PlayerPosX;
		float PlayerPosY;
		float PlayerPosZ;
		float PlayerSpeed;
		float PlayerInWater;
		float PlayerVelocityX;
		float PlayerVelocityY;
		float PlayerWaterDepth;
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
		
		// Terrain heightmap parameters
		float TerrainHeightmapEnabled;
		float TerrainScaleX;
		float TerrainScaleY;
		float TerrainOffsetX;
		float TerrainOffsetY;
		float TerrainZRangeMin;
		float TerrainZRangeMax;
		float TerrainRawZMin;        // Raw heightmap pos0.z for water depth
		float TerrainRawZMax;        // Raw heightmap pos1.z for water depth
		float TerrainPad0;
		float TerrainPad1;
		float TerrainPad2;
	};

	using ActorRippleData = UnifiedWaterRipples::ActorRippleData;
	using ActorRippleBuffer = UnifiedWaterRipples::ActorRippleBuffer;
	using TessellationParams = UnifiedWaterTessellation::TessellationParams;
#pragma warning(pop)

	struct alignas(16) PerTile
	{
		float PrevData[4];  // x/y = prev normal, z = prev distance, w = prev segments per axis
		float TileData[4];  // x/y = tile cell coords, z = LOD level, w = tile span (cells)
	};

	////////////////////////////////////////////////// Member Data
	Settings settings;
	ConstantBuffer* perFrame = nullptr;
	ConstantBuffer* perTile = nullptr;
	ConstantBuffer* actorRippleBuffer = nullptr;

	// Timing state
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

	////////////////////////////////////////////////// API for shader hooks
	const Settings& GetSettings() const { return settings; }
	ConstantBuffer* GetPerFrameBuffer() { return perFrame; }
	ConstantBuffer* GetPerTileBuffer() { return perTile; }
	ConstantBuffer* GetActorRippleBuffer() { return actorRippleBuffer; }

	void UpdatePerFrameData(PerFrame& data, float waterSurfaceHeight = 0.0f);
	void UpdatePerTileData(const RE::BSRenderPass* pass, PerTile& data);

	void ResetTimingState();
};
