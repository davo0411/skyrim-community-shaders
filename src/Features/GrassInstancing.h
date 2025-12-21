#pragma once

#include "Buffer.h"

/**
 * GrassInstancing - True GPU Instancing for Grass Rendering
 *
 * Reduces thousands of grass draw calls to a handful of DrawIndexedInstanced calls.
 *
 * Architecture:
 * 1. Hook RenderPassImmediately - intercept grass render passes
 * 2. Extract instance transform from each pass's geometry
 * 3. Group by texture+technique (same GPU state = same batch)
 * 4. Flush: upload instances to StructuredBuffer, DrawIndexedInstanced per batch
 *
 * Shader support:
 * - RunGrass.hlsl has GRASS_INSTANCING define
 * - When enabled, reads instance data from StructuredBuffer<GrassInstance> using SV_InstanceID
 * - Original vertex attribute path remains for compatibility
 */
struct GrassInstancing : Feature
{
public:

	virtual inline std::string GetName() override { return "Grass Instancing"; }
	virtual inline std::string GetShortName() override { return "GrassInstancing"; }
	virtual std::string_view GetCategory() const override { return "Performance"; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"True GPU instancing - reduces grass draw calls from thousands to tens.",
			{ "Batches grass by texture and technique",
				"DrawIndexedInstanced for massive draw call reduction",
				"StructuredBuffer instance transforms",
				"Compatible with Grass Collision and Grass Lighting" }
		};
	}

	struct Settings
	{
		bool EnableInstancing = true;
		bool ShowDebugStats = false;
	};

	Settings settings;

	// Instance data - matches shader StructuredBuffer layout
	struct alignas(16) GrassInstance
	{
		float4 Data1;  // xyz = world position, w = wind multiplier
		float4 Data2;  // xyz = rotation col 0, w = rotation col 2.y
		float4 Data3;  // xyz = rotation col 1, w = rotation col 2.z
		float4 Data4;  // x = rotation col 2.x, y = scale, zw = unused
	};
	static_assert(sizeof(GrassInstance) == 64);

	// Collected pass before batching
	struct CollectedPass
	{
		RE::BSRenderPass* pass;
		uint32_t technique;
		bool alphaTest;
		uint32_t renderFlags;
		uint64_t batchKey;
		GrassInstance instance;
	};

	// Batch for instanced rendering
	struct Batch
	{
		RE::BSRenderPass* templatePass;  // Use for geometry & state setup
		uint32_t technique;
		bool alphaTest;
		uint32_t renderFlags;
		uint32_t instanceOffset;  // Offset into instance buffer
		uint32_t instanceCount;
	};

	// Per-frame collection
	std::vector<CollectedPass> collected;
	std::vector<GrassInstance> allInstances;
	std::vector<Batch> batches;
	bool collecting = false;
	RE::BSShader::Type prevShaderType = RE::BSShader::Type::None;

	// GPU instance buffer
	winrt::com_ptr<ID3D11Buffer> instanceBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> instanceSRV;
	uint32_t bufferCapacity = 0;
	static constexpr uint32_t INITIAL_CAPACITY = 8192;
	static constexpr uint32_t SRV_SLOT = 20;  // t20 for vertex shader

	// Statistics
	struct Stats
	{
		uint32_t passesCollected = 0;
		uint32_t drawCallsIssued = 0;
		uint32_t instancesRendered = 0;
		uint32_t largestBatch = 0;
		void Reset()
		{
			passesCollected = 0;
			drawCallsIssued = 0;
			instancesRendered = 0;
			largestBatch = 0;
		}
	};
	Stats currentStats;
	Stats displayStats;

	// Feature overrides
	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void PostPostLoad() override;
	virtual bool SupportsVR() override { return true; }

	// Core methods
	void OnShaderChange(RE::BSShader::Type newType);
	void StartCollection();
	void StopCollectionAndFlush();
	bool TryCollectPass(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags);
	void BuildBatches();
	void RenderAllBatches();
	void RenderSingleBatch(Batch& batch);

	// Helpers
	void EnsureBufferCapacity(uint32_t needed);
	void UploadInstanceBuffer();
	bool ExtractInstance(RE::BSGeometry* geom, GrassInstance& out);
	uint64_t MakeBatchKey(RE::BSRenderPass* pass, uint32_t technique);

	struct Hooks
	{
		struct RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install();
	};
};
