#pragma once

#include "Buffer.h"

/**
 * GrassInstancing - True GPU Instancing for Grass and Tree Rendering
 *
 * Reduces thousands of vegetation draw calls to a handful of DrawIndexedInstanced calls.
 *
 * Architecture:
 * 1. Hook RenderPassImmediately - intercept grass and tree render passes
 * 2. Extract instance transform from each pass
 * 3. Group by texture+technique (same GPU state = same batch)
 * 4. Flush: upload instances to StructuredBuffer, DrawIndexedInstanced per batch
 *
 * Shader support:
 * - RunGrass.hlsl has GRASS_INSTANCING define
 * - Lighting.hlsl has TREE_INSTANCING define for TREE_ANIM passes
 * - Instance data read from StructuredBuffer using SV_InstanceID
 */
struct GrassInstancing : Feature
{
public:

	virtual inline std::string GetName() override { return "Vegetation Instancing"; }
	virtual inline std::string GetShortName() override { return "GrassInstancing"; }
	virtual std::string_view GetCategory() const override { return "Performance"; }
	virtual bool IsCore() const override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"True GPU instancing - reduces grass and tree draw calls from thousands to tens.",
			{ "Batches grass by texture and technique",
				"Batches trees by mesh and texture",
				"DrawIndexedInstanced for massive draw call reduction",
				"StructuredBuffer instance transforms" }
		};
	}

	struct Settings
	{
		bool EnableGrassInstancing = true;
		bool EnableTreeInstancing = true;
		bool ShowDebugStats = false;
	};

	Settings settings;

	// ========================================================================
	// GRASS INSTANCING
	// ========================================================================

	// Grass instance data - matches shader StructuredBuffer layout
	struct alignas(16) GrassInstance
	{
		float4 Data1;  // xyz = world position, w = wind multiplier
		float4 Data2;  // xyz = rotation col 0, w = rotation col 2.y
		float4 Data3;  // xyz = rotation col 1, w = rotation col 2.z
		float4 Data4;  // x = rotation col 2.x, y = scale, zw = unused
	};
	static_assert(sizeof(GrassInstance) == 64);

	// Collected grass pass before batching
	struct CollectedGrassPass
	{
		RE::BSRenderPass* pass;
		uint32_t technique;
		bool alphaTest;
		uint32_t renderFlags;
		uint64_t batchKey;
		GrassInstance instance;
	};

	// Grass batch for instanced rendering
	struct GrassBatch
	{
		RE::BSRenderPass* templatePass;
		uint32_t technique;
		bool alphaTest;
		uint32_t renderFlags;
		uint32_t instanceOffset;
		uint32_t instanceCount;
	};

	// Grass collection state
	std::vector<CollectedGrassPass> grassCollected;
	std::vector<GrassInstance> grassInstances;
	std::vector<GrassBatch> grassBatches;
	bool collectingGrass = false;
	RE::BSShader::Type prevShaderType = RE::BSShader::Type::None;

	// Grass GPU buffer
	winrt::com_ptr<ID3D11Buffer> grassInstanceBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> grassInstanceSRV;
	uint32_t grassBufferCapacity = 0;

	// ========================================================================
	// TREE INSTANCING
	// ========================================================================

	// Tree instance data - 3x4 world matrix (row major) + previous world matrix
	struct alignas(16) TreeInstance
	{
		float4 WorldRow0;      // World matrix row 0 (includes translate.x in w)
		float4 WorldRow1;      // World matrix row 1 (includes translate.y in w)
		float4 WorldRow2;      // World matrix row 2 (includes translate.z in w)
		float4 PrevWorldRow0;  // Previous world matrix row 0
		float4 PrevWorldRow1;  // Previous world matrix row 1
		float4 PrevWorldRow2;  // Previous world matrix row 2
	};
	static_assert(sizeof(TreeInstance) == 96);

	// Collected tree pass before batching
	struct CollectedTreePass
	{
		RE::BSRenderPass* pass;
		uint32_t technique;
		uint32_t renderFlags;
		uint64_t batchKey;
		TreeInstance instance;
	};

	// Tree batch for instanced rendering
	struct TreeBatch
	{
		RE::BSRenderPass* templatePass;
		uint32_t technique;
		uint32_t renderFlags;
		uint32_t instanceOffset;
		uint32_t instanceCount;
	};

	// Tree collection state
	std::vector<CollectedTreePass> treeCollected;
	std::vector<TreeInstance> treeInstances;
	std::vector<TreeBatch> treeBatches;
	bool collectingTrees = false;
	uint32_t currentTreeTechnique = 0;

	// Tree GPU buffer
	winrt::com_ptr<ID3D11Buffer> treeInstanceBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> treeInstanceSRV;
	uint32_t treeBufferCapacity = 0;

	// ========================================================================
	// SHARED
	// ========================================================================

	static constexpr uint32_t INITIAL_CAPACITY = 8192;
	static constexpr uint32_t GRASS_SRV_SLOT = 20;  // t20 for vertex shader
	static constexpr uint32_t TREE_SRV_SLOT = 21;   // t21 for vertex shader

	// Statistics
	struct Stats
	{
		uint32_t grassPassesCollected = 0;
		uint32_t grassDrawCallsIssued = 0;
		uint32_t grassInstancesRendered = 0;
		uint32_t grassLargestBatch = 0;
		uint32_t treePassesCollected = 0;
		uint32_t treeDrawCallsIssued = 0;
		uint32_t treeInstancesRendered = 0;
		uint32_t treeLargestBatch = 0;
		void Reset()
		{
			grassPassesCollected = 0;
			grassDrawCallsIssued = 0;
			grassInstancesRendered = 0;
			grassLargestBatch = 0;
			treePassesCollected = 0;
			treeDrawCallsIssued = 0;
			treeInstancesRendered = 0;
			treeLargestBatch = 0;
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

	// Grass methods
	void OnGrassShaderChange(RE::BSShader::Type newType);
	void StartGrassCollection();
	void StopGrassCollectionAndFlush();
	bool TryCollectGrassPass(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags);
	void BuildGrassBatches();
	void RenderGrassBatches();
	void RenderGrassBatch(GrassBatch& batch);
	bool ExtractGrassInstance(RE::BSGeometry* geom, GrassInstance& out);
	uint64_t MakeGrassBatchKey(RE::BSRenderPass* pass, uint32_t technique);
	void EnsureGrassBufferCapacity(uint32_t needed);

	// Tree methods
	bool IsTreePass(uint32_t technique);
	void StartTreeCollection(uint32_t technique);
	void StopTreeCollectionAndFlush();
	bool TryCollectTreePass(RE::BSRenderPass* pass, uint32_t technique, uint32_t renderFlags);
	void BuildTreeBatches();
	void RenderTreeBatches();
	void RenderTreeBatch(TreeBatch& batch);
	bool ExtractTreeInstance(RE::BSGeometry* geom, TreeInstance& out);
	uint64_t MakeTreeBatchKey(RE::BSRenderPass* pass, uint32_t technique);
	void EnsureTreeBufferCapacity(uint32_t needed);

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
