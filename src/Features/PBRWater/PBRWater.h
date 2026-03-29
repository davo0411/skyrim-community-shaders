#pragma once

#include "Feature.h"
#include <cstdint>
#include <limits>
#include <unordered_map>

struct PBRWater : public Feature
{
	virtual inline std::string GetName() override { return "PBR Water"; }
	virtual inline std::string GetShortName() override { return "PBRWater"; }
	virtual inline std::string_view GetCategory() const override { return "Water"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("-"); }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Comprehensive feature for Physically Based Rendering for water surfaces.",
			{
				"Gerstner Wave system for 3D Vertex-displaced waves.",
				"Real Tessellation shaders for high-detail water surfaces.",
				"New BRDF specular reflections model.",
				"Advanced Water Light Scattering effects.",
				"Depth-based foam generation.",
				"Actor wading ripple effects.",
			}
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "PBR_WATER"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Water || t == RE::BSShader::Type::Lighting;
	}

	static constexpr uint32_t MAX_ACTOR_RIPPLES = 32;

	// ---- Settings Structs ----

	struct GeneralSettings
	{
		bool ShowWireframe = false;
		bool WireframeRawMode = false;
	};

	struct TessellationSettings
	{
		bool EnableTessellation = true;
		float TessellationMaxDistance = 5000.0f;
		float TessellationMinFactor = 0.1f;
		float TessellationMaxFactor = 12.0f;
	};

	struct WaveSettings
	{
		float WaveIntensity = 0.3f;
		float WaveAmplitude = 0.7f;
		float WaveSpeed = 0.5f;
		float WaveSteepness = 5.0f;
		float WaveFadeStart = 4096.0f;
		float WaveFadeEnd = 8192.0f;

		float Wave1Amplitude = 0.8f;
		float Wave1Wavelength = 60.0f;
		float Wave1Steepness = 0.4f;
		float Wave1AngleOffset = 0.0f;

		float Wave2Amplitude = 0.5f;
		float Wave2Wavelength = 35.0f;
		float Wave2Steepness = 0.35f;
		float Wave2AngleOffset = 0.6f;

		float Wave3Amplitude = 0.25f;
		float Wave3Wavelength = 18.0f;
		float Wave3Steepness = 0.3f;
		float Wave3AngleOffset = -0.7f;

		float Wave4Amplitude = 0.12f;
		float Wave4Wavelength = 8.0f;
		float Wave4Steepness = 0.25f;
		float Wave4AngleOffset = 0.44f;

		float Wave5Amplitude = 0.06f;
		float Wave5Wavelength = 4.0f;
		float Wave5Steepness = 0.2f;
		float Wave5AngleOffset = -0.44f;

		float Wave6Amplitude = 0.03f;
		float Wave6Wavelength = 2.0f;
		float Wave6Steepness = 0.15f;
		float Wave6AngleOffset = 1.22f;

		// Deprecated (kept for JSON load): shallow depth range is fixed in shader (meters)
		float ShoreBlendStart = 50.0f;
		float ShoreBlendEnd = 500.0f;
		float ShoreWaveStrength = 1.0f;
	};

	struct LightingSettings
	{
		bool EnableLightingOverrides = false;
		float FresnelBias = 0.02f;
		float FresnelPower = 5.0f;
		float ReflectionStrength = 1.0f;
		float RefractionStrength = 1.0f;
		float WaterTransparency = 1.0f;
		float AbsorptionDensity = 0.15f;
		float ScatteringCoeff = 0.05f;
		float SpecularIntensity = 1.0f;
		float SunSpecularPower = 250.0f;
		float SunSpecularMagnitude = 1.0f;
		float SunSparklePower = 50.0f;
		float SunSparkleMagnitude = 1.0f;
		float SpecularRadius = 128.0f;
		float SpecularBrightness = 1.0f;
	};

	struct FogSettings
	{
		float AboveWaterFogDistNear = 0.0f;
		float AboveWaterFogDistFar = 163840.0f;
		float AboveWaterFogAmount = 1.0f;
		float UnderwaterFogDistNear = 0.0f;
		float UnderwaterFogDistFar = 4096.0f;
		float UnderwaterFogAmount = 1.0f;
	};

	struct DepthSettings
	{
		float DepthReflections = 1.0f;
		float DepthRefractions = 1.0f;
		float DepthNormals = 1.0f;
		float DepthSpecularLighting = 1.0f;
	};

	struct RippleSettings
	{
		bool EnableActorRipples = true;
		float RippleStrength = 1.0f;
		float RippleRadius = 512.0f;
		float RippleWaveSpeed = 4.0f;
		float RippleWaveFreq1 = 0.08f;
		float RippleWaveFreq2 = 0.12f;
		float RippleWaveFreq3 = 0.18f;
		float RippleNormalStrength = 2.0f;
	};

	struct FoamSettings
	{
		bool EnableFoam = true;
		float FoamIntensity = 1.0f;
		float FoamIntensityFlowmap = 1.0f;
		float FoamThreshold = 0.3f;
		float FoamSharpness = 2.0f;
		float FoamIntersectionRange = 100.0f;
		float FoamIntersectionIntensity = 1.0f;
	};

	struct BRDFSettings
	{
		bool EnableBRDFSpecular = true;
		bool EnableWaterScattering = true;
	};

	// Shader buffer data - must match PBRWaterSettings in SharedData.hlsli
	struct alignas(16) ShaderBRDFSettings
	{
		uint32_t EnableBRDFSpecular;
		uint32_t EnableWaterScattering;
		uint32_t _padding[2];
	};

	struct Settings
	{
		GeneralSettings general;
		TessellationSettings tessellation;
		WaveSettings waves;
		LightingSettings lighting;
		FogSettings fog;
		DepthSettings depth;
		RippleSettings ripples;
		FoamSettings foam;
		BRDFSettings brdf;
	};

	// ---- GPU Constant Buffer Structs ----

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
		float FoamIntersectionRange;
		float FoamIntersectionIntensity;
		float FoamPad_c22z;
		float FoamPad_c22w;
		float FoamPad_c23x;
		float FoamPad_c23y;
		float FoamPad_c23z;

		// Legacy padding (shader ignores; shallow depth is fixed in GerstnerWaves.hlsli)
		float ShallowWaveDepthMin;
		float ShallowWaveDepthMax;
		float ShoreWavePad0;
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

	struct alignas(16) ActorRippleData
	{
		float PosX;
		float PosY;
		float Speed;
		float InWater;
		float VelocityX;
		float VelocityY;
		float WaterDepth;
		float pad0;
	};

	struct alignas(16) ActorRippleBuffer
	{
		ActorRippleData actors[MAX_ACTOR_RIPPLES];
		uint32_t numActors;
		uint32_t pad0[3];
	};
#pragma warning(pop)

	struct alignas(16) PerTile
	{
		float PrevData[4];
		float TileData[4];
	};

	struct alignas(16) TessellationParams
	{
		float TessellationMinDistance;
		float TessellationMaxDistance;
		float TessellationMinFactor;
		float TessellationMaxFactor;
		float CameraWorldPosX;
		float CameraWorldPosY;
		float CameraWorldPosZ;
		float DetailHeightScale;
	};

	// ---- Member Data ----

	Settings settings;
	ConstantBuffer* perFrame = nullptr;
	ConstantBuffer* perTile = nullptr;
	ConstantBuffer* tessellationParams = nullptr;
	ConstantBuffer* actorRippleBuffer = nullptr;

	winrt::com_ptr<ID3D11HullShader> waterHullShader;
	winrt::com_ptr<ID3D11DomainShader> waterDomainShader;
	winrt::com_ptr<ID3D11GeometryShader> waterGeometryShader;

	ShaderBRDFSettings GetShaderBRDFSettings() const
	{
		return {
			settings.brdf.EnableBRDFSpecular ? 1u : 0u,
			settings.brdf.EnableWaterScattering ? 1u : 0u,
			{ 0, 0 }
		};
	}

	// Timing tracking
	float lastGameTimeHours = 0.0f;
	float lastRealTimeSeconds = 0.0f;
	float lastTimeScale = 1.0f;
	float currentGameTimeHours = 0.0f;
	float currentRealTimeSeconds = 0.0f;
	float currentTimeScale = 1.0f;
	std::uint32_t lastTimingFrameIndex = std::numeric_limits<std::uint32_t>::max();
	bool hasLastTimingSample = false;

	// Player movement tracking
	RE::NiPoint3 lastPlayerPos{ 0.0f, 0.0f, 0.0f };
	RE::NiPoint2 playerVelocity{ 0.0f, 0.0f };
	float lastPlayerUpdateTime = 0.0f;
	bool hasPlayerMovementData = false;

	struct PrevTileData
	{
		float normalX = 0.0f;
		float normalY = 0.0f;
		float distance = 10000.0f;
		float segmentsPerAxis = 32.0f;
	};

	std::unordered_map<std::uint64_t, PrevTileData> prevTileData;

	// ---- Feature Overrides ----

	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;
	virtual void PostPostLoad() override;
	virtual void ClearShaderCache() override;

	// ---- Hooks ----
	// These chain with UnifiedWater's hooks via vfunc/detour ordering.
	// PostPostLoad order: PBR Water hooks first (P<U alphabetically), then
	// UnifiedWater hooks second and stores PBR's thunk in its func.
	// Result: UW::thunk -> PBR::thunk -> original game function.

	struct TESWaterSystem_InitializeWater
	{
		static void thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool noDisplacement, bool isProcedural);
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

	static inline thread_local bool tessellationActiveForPass = false;
	static inline thread_local D3D11_PRIMITIVE_TOPOLOGY originalTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
};
