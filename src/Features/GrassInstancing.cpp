#include "GrassInstancing.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassInstancing::Settings,
	EnableGrassInstancing,
	EnableTreeInstancing,
	ShowDebugStats)

// TreeAnim flag in Lighting shader descriptor
static constexpr uint32_t TREE_ANIM_FLAG = 1 << 26;

void GrassInstancing::SetupResources()
{
	// Grass buffers
	grassCollected.reserve(8192);
	grassInstances.reserve(8192);
	grassBatches.reserve(64);
	EnsureGrassBufferCapacity(INITIAL_CAPACITY);

	// Tree buffers
	treeCollected.reserve(4096);
	treeInstances.reserve(4096);
	treeBatches.reserve(64);
	EnsureTreeBufferCapacity(INITIAL_CAPACITY);

	logger::info("[VegetationInstancing] Initialized grass and tree buffers");
}

void GrassInstancing::Reset()
{
	// Update smoothed stats
	const float alpha = 0.1f;

	// Grass stats
	displayStats.grassPassesCollected = static_cast<uint32_t>(displayStats.grassPassesCollected * (1.f - alpha) + currentStats.grassPassesCollected * alpha);
	displayStats.grassDrawCallsIssued = static_cast<uint32_t>(displayStats.grassDrawCallsIssued * (1.f - alpha) + currentStats.grassDrawCallsIssued * alpha);
	displayStats.grassInstancesRendered = static_cast<uint32_t>(displayStats.grassInstancesRendered * (1.f - alpha) + currentStats.grassInstancesRendered * alpha);
	displayStats.grassLargestBatch = static_cast<uint32_t>(displayStats.grassLargestBatch * (1.f - alpha) + currentStats.grassLargestBatch * alpha);

	// Tree stats
	displayStats.treePassesCollected = static_cast<uint32_t>(displayStats.treePassesCollected * (1.f - alpha) + currentStats.treePassesCollected * alpha);
	displayStats.treeDrawCallsIssued = static_cast<uint32_t>(displayStats.treeDrawCallsIssued * (1.f - alpha) + currentStats.treeDrawCallsIssued * alpha);
	displayStats.treeInstancesRendered = static_cast<uint32_t>(displayStats.treeInstancesRendered * (1.f - alpha) + currentStats.treeInstancesRendered * alpha);
	displayStats.treeLargestBatch = static_cast<uint32_t>(displayStats.treeLargestBatch * (1.f - alpha) + currentStats.treeLargestBatch * alpha);

	currentStats.Reset();

	// Clear grass state
	grassCollected.clear();
	grassInstances.clear();
	grassBatches.clear();
	collectingGrass = false;
	prevShaderType = RE::BSShader::Type::None;

	// Clear tree state
	treeCollected.clear();
	treeInstances.clear();
	treeBatches.clear();
	collectingTrees = false;
	currentTreeTechnique = 0;
}

void GrassInstancing::DrawSettings()
{
	if (ImGui::TreeNodeEx("Vegetation Instancing", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Grass Instancing", &settings.EnableGrassInstancing);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Batches grass into instanced draw calls");

		ImGui::Checkbox("Enable Tree Instancing", &settings.EnableTreeInstancing);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Batches trees into instanced draw calls");

		ImGui::Checkbox("Show Stats", &settings.ShowDebugStats);

		if (settings.ShowDebugStats) {
			ImGui::Separator();

			// Grass stats
			if (displayStats.grassPassesCollected > 0) {
				ImGui::Text("=== Grass ===");
				ImGui::Text("Original Draw Calls: %u", displayStats.grassPassesCollected);
				ImGui::Text("Batched Draw Calls: %u", displayStats.grassDrawCallsIssued);
				ImGui::Text("Instances: %u", displayStats.grassInstancesRendered);
				ImGui::Text("Largest Batch: %u", displayStats.grassLargestBatch);
				float grassReduction = 100.f * (1.f - (float)displayStats.grassDrawCallsIssued / (float)displayStats.grassPassesCollected);
				ImGui::Text("Reduction: %.1f%%", grassReduction);
			}

			// Tree stats
			if (displayStats.treePassesCollected > 0) {
				ImGui::Text("=== Trees ===");
				ImGui::Text("Original Draw Calls: %u", displayStats.treePassesCollected);
				ImGui::Text("Batched Draw Calls: %u", displayStats.treeDrawCallsIssued);
				ImGui::Text("Instances: %u", displayStats.treeInstancesRendered);
				ImGui::Text("Largest Batch: %u", displayStats.treeLargestBatch);
				float treeReduction = 100.f * (1.f - (float)displayStats.treeDrawCallsIssued / (float)displayStats.treePassesCollected);
				ImGui::Text("Reduction: %.1f%%", treeReduction);
			}
		}

		ImGui::TreePop();
	}
}

void GrassInstancing::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassInstancing::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassInstancing::RestoreDefaultSettings()
{
	settings = {};
}

void GrassInstancing::PostPostLoad()
{
	Hooks::Install();
}

// ============================================================================
// GRASS INSTANCING
// ============================================================================

void GrassInstancing::OnGrassShaderChange(RE::BSShader::Type newType)
{
	if (prevShaderType == RE::BSShader::Type::Grass && newType != RE::BSShader::Type::Grass) {
		StopGrassCollectionAndFlush();
	}

	if (prevShaderType != RE::BSShader::Type::Grass && newType == RE::BSShader::Type::Grass) {
		StartGrassCollection();
	}

	prevShaderType = newType;
}

void GrassInstancing::StartGrassCollection()
{
	if (!settings.EnableGrassInstancing)
		return;

	collectingGrass = true;
	grassCollected.clear();
	grassInstances.clear();
	grassBatches.clear();
}

void GrassInstancing::StopGrassCollectionAndFlush()
{
	if (!collectingGrass)
		return;

	collectingGrass = false;

	if (grassCollected.empty())
		return;

	BuildGrassBatches();
	RenderGrassBatches();
}

bool GrassInstancing::TryCollectGrassPass(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags)
{
	if (!collectingGrass || !settings.EnableGrassInstancing)
		return false;

	if (!pass || !pass->shader || !pass->geometry)
		return false;

	if (pass->shader->shaderType.get() != RE::BSShader::Type::Grass)
		return false;

	// Extract instance transform from geometry
	GrassInstance inst;
	if (!ExtractGrassInstance(pass->geometry, inst))
		return false;

	// Collect the pass
	CollectedGrassPass cp;
	cp.pass = pass;
	cp.technique = technique;
	cp.alphaTest = alphaTest;
	cp.renderFlags = renderFlags;
	cp.batchKey = MakeGrassBatchKey(pass, technique);
	cp.instance = inst;
	grassCollected.push_back(cp);

	currentStats.grassPassesCollected++;
	return true;
}

uint64_t GrassInstancing::MakeGrassBatchKey(RE::BSRenderPass* pass, uint32_t technique)
{
	// Key = technique (high 32) | texture pointer hash (low 32)
	uint64_t key = static_cast<uint64_t>(technique) << 32;

	if (pass->shaderProperty && pass->shaderProperty->material) {
		auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(pass->shaderProperty->material);
		if (mat->diffuseTexture && mat->diffuseTexture->rendererTexture) {
			uintptr_t ptr = reinterpret_cast<uintptr_t>(mat->diffuseTexture->rendererTexture->resourceView);
			key |= (ptr & 0xFFFFFFFF);
		}
	}

	return key;
}

void GrassInstancing::BuildGrassBatches()
{
	if (grassCollected.empty())
		return;

	// Sort by batch key
	std::sort(grassCollected.begin(), grassCollected.end(), [](const CollectedGrassPass& a, const CollectedGrassPass& b) {
		return a.batchKey < b.batchKey;
	});

	// Build batches
	grassBatches.clear();
	grassInstances.clear();

	uint64_t currentKey = grassCollected[0].batchKey;
	GrassBatch currentBatch;
	currentBatch.templatePass = grassCollected[0].pass;
	currentBatch.technique = grassCollected[0].technique;
	currentBatch.alphaTest = grassCollected[0].alphaTest;
	currentBatch.renderFlags = grassCollected[0].renderFlags;
	currentBatch.instanceOffset = 0;
	currentBatch.instanceCount = 0;

	for (const auto& cp : grassCollected) {
		if (cp.batchKey != currentKey) {
			if (currentBatch.instanceCount > 0) {
				currentStats.grassLargestBatch = std::max(currentStats.grassLargestBatch, currentBatch.instanceCount);
				grassBatches.push_back(currentBatch);
			}

			currentKey = cp.batchKey;
			currentBatch.templatePass = cp.pass;
			currentBatch.technique = cp.technique;
			currentBatch.alphaTest = cp.alphaTest;
			currentBatch.renderFlags = cp.renderFlags;
			currentBatch.instanceOffset = static_cast<uint32_t>(grassInstances.size());
			currentBatch.instanceCount = 0;
		}

		grassInstances.push_back(cp.instance);
		currentBatch.instanceCount++;
	}

	if (currentBatch.instanceCount > 0) {
		currentStats.grassLargestBatch = std::max(currentStats.grassLargestBatch, currentBatch.instanceCount);
		grassBatches.push_back(currentBatch);
	}

	currentStats.grassInstancesRendered = static_cast<uint32_t>(grassInstances.size());
}

void GrassInstancing::EnsureGrassBufferCapacity(uint32_t needed)
{
	if (grassBufferCapacity >= needed)
		return;

	auto* device = globals::d3d::device;
	if (!device)
		return;

	uint32_t newCap = needed;
	newCap--;
	newCap |= newCap >> 1;
	newCap |= newCap >> 2;
	newCap |= newCap >> 4;
	newCap |= newCap >> 8;
	newCap |= newCap >> 16;
	newCap++;

	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = newCap * sizeof(GrassInstance);
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(GrassInstance);

	grassInstanceBuffer = nullptr;
	grassInstanceSRV = nullptr;

	HRESULT hr = device->CreateBuffer(&desc, nullptr, grassInstanceBuffer.put());
	if (FAILED(hr)) {
		logger::error("[VegetationInstancing] Failed to create grass buffer: {:x}", (uint32_t)hr);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = newCap;

	hr = device->CreateShaderResourceView(grassInstanceBuffer.get(), &srvDesc, grassInstanceSRV.put());
	if (FAILED(hr)) {
		logger::error("[VegetationInstancing] Failed to create grass SRV: {:x}", (uint32_t)hr);
		grassInstanceBuffer = nullptr;
		return;
	}

	grassBufferCapacity = newCap;
}

bool GrassInstancing::ExtractGrassInstance(RE::BSGeometry* geom, GrassInstance& out)
{
	if (!geom)
		return false;

	auto& runtime = geom->GetGeometryRuntimeData();
	auto* renderer = runtime.rendererData;
	if (!renderer || !renderer->rawVertexData)
		return false;

	auto vertexDesc = runtime.vertexDesc;
	if (!vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_INSTANCEDATA))
		return false;

	uint32_t offset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_INSTANCEDATA);
	float* data = reinterpret_cast<float*>(renderer->rawVertexData + offset);

	out.Data1 = float4(data[0], data[1], data[2], data[3]);
	out.Data2 = float4(data[4], data[5], data[6], data[7]);
	out.Data3 = float4(data[8], data[9], data[10], data[11]);
	out.Data4 = float4(data[12], data[13], data[14], data[15]);

	return true;
}

void GrassInstancing::RenderGrassBatches()
{
	if (grassBatches.empty())
		return;

	auto* ctx = globals::d3d::context;
	if (!ctx)
		return;

	uint32_t maxBatchSize = 0;
	for (const auto& batch : grassBatches) {
		maxBatchSize = std::max(maxBatchSize, batch.instanceCount);
	}
	EnsureGrassBufferCapacity(maxBatchSize);

	if (!grassInstanceBuffer)
		return;

	for (auto& batch : grassBatches) {
		RenderGrassBatch(batch);
	}
}

void GrassInstancing::RenderGrassBatch(GrassBatch& batch)
{
	if (!batch.templatePass || batch.instanceCount == 0)
		return;

	auto* pass = batch.templatePass;
	auto* shader = pass->shader;
	auto* geom = pass->geometry;

	if (!shader || !geom)
		return;

	auto* ctx = globals::d3d::context;
	auto& runtime = geom->GetGeometryRuntimeData();
	auto* rendererData = runtime.rendererData;

	if (!rendererData)
		return;

	// Upload batch instances
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = ctx->Map(grassInstanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr))
			return;
		memcpy(mapped.pData, &grassInstances[batch.instanceOffset], batch.instanceCount * sizeof(GrassInstance));
		ctx->Unmap(grassInstanceBuffer.get(), 0);
	}

	// Bind instance SRV
	ID3D11ShaderResourceView* srv = grassInstanceSRV.get();
	ctx->VSSetShaderResources(GRASS_SRV_SLOT, 1, &srv);

	// Setup shader
	shader->SetupTechnique(batch.technique);
	shader->SetupGeometry(pass, batch.renderFlags);

	// Get buffers
	ID3D11Buffer* vertexBuffer = rendererData->vertexBuffer;
	ID3D11Buffer* indexBuffer = rendererData->indexBuffer;

	if (!vertexBuffer || !indexBuffer) {
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	uint32_t stride = runtime.vertexDesc.GetSize();
	uint32_t offset = 0;

	ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
	ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	uint32_t indexCount = 0;
	if (auto* triShape = geom->AsTriShape()) {
		indexCount = triShape->GetTrishapeRuntimeData().triangleCount * 3;
	} else {
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	ctx->DrawIndexedInstanced(indexCount, batch.instanceCount, 0, 0, 0);
	currentStats.grassDrawCallsIssued++;

	// Unbind
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(GRASS_SRV_SLOT, 1, &nullSRV);

	shader->RestoreGeometry(pass, batch.renderFlags);
}

// ============================================================================
// TREE INSTANCING
// ============================================================================

bool GrassInstancing::IsTreePass(uint32_t technique)
{
	return (technique & TREE_ANIM_FLAG) != 0;
}

void GrassInstancing::StartTreeCollection(uint32_t technique)
{
	if (!settings.EnableTreeInstancing)
		return;

	collectingTrees = true;
	currentTreeTechnique = technique;
	treeCollected.clear();
	treeInstances.clear();
	treeBatches.clear();
}

void GrassInstancing::StopTreeCollectionAndFlush()
{
	if (!collectingTrees)
		return;

	collectingTrees = false;

	if (treeCollected.empty())
		return;

	BuildTreeBatches();
	RenderTreeBatches();
}

bool GrassInstancing::TryCollectTreePass(RE::BSRenderPass* pass, uint32_t technique, uint32_t renderFlags)
{
	if (!settings.EnableTreeInstancing)
		return false;

	// Tree instancing uses SV_InstanceID which conflicts with VR stereo rendering
	if (REL::Module::IsVR())
		return false;

	if (!pass || !pass->shader || !pass->geometry)
		return false;

	if (pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return false;

	if (!IsTreePass(technique))
		return false;

	// Check if we're collecting and technique matches
	if (!collectingTrees) {
		StartTreeCollection(technique);
	} else if (technique != currentTreeTechnique) {
		// Technique changed, flush current batch and start new
		StopTreeCollectionAndFlush();
		StartTreeCollection(technique);
	}

	// Extract instance transform
	TreeInstance inst;
	if (!ExtractTreeInstance(pass->geometry, inst))
		return false;

	// Collect
	CollectedTreePass cp;
	cp.pass = pass;
	cp.technique = technique;
	cp.renderFlags = renderFlags;
	cp.batchKey = MakeTreeBatchKey(pass, technique);
	cp.instance = inst;
	treeCollected.push_back(cp);

	currentStats.treePassesCollected++;
	return true;
}

uint64_t GrassInstancing::MakeTreeBatchKey(RE::BSRenderPass* pass, uint32_t technique)
{
	// Key = geometry pointer (same mesh) + texture
	uint64_t key = 0;

	// Use geometry pointer as primary key (same tree model)
	if (pass->geometry) {
		auto& runtime = pass->geometry->GetGeometryRuntimeData();
		if (runtime.rendererData) {
			// Use vertex buffer pointer as mesh identifier
			uintptr_t vbPtr = reinterpret_cast<uintptr_t>(runtime.rendererData->vertexBuffer);
			key = (vbPtr & 0xFFFFFFFF) << 32;
		}
	}

	// Add texture as secondary key
	if (pass->shaderProperty && pass->shaderProperty->material) {
		auto* mat = static_cast<RE::BSLightingShaderMaterialBase*>(pass->shaderProperty->material);
		if (mat->diffuseTexture && mat->diffuseTexture->rendererTexture) {
			uintptr_t ptr = reinterpret_cast<uintptr_t>(mat->diffuseTexture->rendererTexture->resourceView);
			key |= (ptr & 0xFFFFFFFF);
		}
	}

	return key;
}

void GrassInstancing::BuildTreeBatches()
{
	if (treeCollected.empty())
		return;

	// Sort by batch key
	std::sort(treeCollected.begin(), treeCollected.end(), [](const CollectedTreePass& a, const CollectedTreePass& b) {
		return a.batchKey < b.batchKey;
	});

	treeBatches.clear();
	treeInstances.clear();

	uint64_t currentKey = treeCollected[0].batchKey;
	TreeBatch currentBatch;
	currentBatch.templatePass = treeCollected[0].pass;
	currentBatch.technique = treeCollected[0].technique;
	currentBatch.renderFlags = treeCollected[0].renderFlags;
	currentBatch.instanceOffset = 0;
	currentBatch.instanceCount = 0;

	for (const auto& cp : treeCollected) {
		if (cp.batchKey != currentKey) {
			if (currentBatch.instanceCount > 0) {
				currentStats.treeLargestBatch = std::max(currentStats.treeLargestBatch, currentBatch.instanceCount);
				treeBatches.push_back(currentBatch);
			}

			currentKey = cp.batchKey;
			currentBatch.templatePass = cp.pass;
			currentBatch.technique = cp.technique;
			currentBatch.renderFlags = cp.renderFlags;
			currentBatch.instanceOffset = static_cast<uint32_t>(treeInstances.size());
			currentBatch.instanceCount = 0;
		}

		treeInstances.push_back(cp.instance);
		currentBatch.instanceCount++;
	}

	if (currentBatch.instanceCount > 0) {
		currentStats.treeLargestBatch = std::max(currentStats.treeLargestBatch, currentBatch.instanceCount);
		treeBatches.push_back(currentBatch);
	}

	currentStats.treeInstancesRendered = static_cast<uint32_t>(treeInstances.size());
}

void GrassInstancing::EnsureTreeBufferCapacity(uint32_t needed)
{
	if (treeBufferCapacity >= needed)
		return;

	auto* device = globals::d3d::device;
	if (!device)
		return;

	uint32_t newCap = needed;
	newCap--;
	newCap |= newCap >> 1;
	newCap |= newCap >> 2;
	newCap |= newCap >> 4;
	newCap |= newCap >> 8;
	newCap |= newCap >> 16;
	newCap++;

	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = newCap * sizeof(TreeInstance);
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(TreeInstance);

	treeInstanceBuffer = nullptr;
	treeInstanceSRV = nullptr;

	HRESULT hr = device->CreateBuffer(&desc, nullptr, treeInstanceBuffer.put());
	if (FAILED(hr)) {
		logger::error("[VegetationInstancing] Failed to create tree buffer: {:x}", (uint32_t)hr);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = newCap;

	hr = device->CreateShaderResourceView(treeInstanceBuffer.get(), &srvDesc, treeInstanceSRV.put());
	if (FAILED(hr)) {
		logger::error("[VegetationInstancing] Failed to create tree SRV: {:x}", (uint32_t)hr);
		treeInstanceBuffer = nullptr;
		return;
	}

	treeBufferCapacity = newCap;
}

bool GrassInstancing::ExtractTreeInstance(RE::BSGeometry* geom, TreeInstance& out)
{
	if (!geom)
		return false;

	// Get world transform from NiAVObject
	const auto& world = geom->world;
	const auto& prevWorld = geom->previousWorld;

	// Pack NiTransform into 3x4 row-major matrix
	// NiMatrix3 is 3x3 rotation, NiPoint3 is translate, float is scale
	const auto& rot = world.rotate;
	const auto& trans = world.translate;
	float scale = world.scale;

	// World matrix rows (rotation * scale, translation in w)
	out.WorldRow0 = float4(rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, trans.x);
	out.WorldRow1 = float4(rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, trans.y);
	out.WorldRow2 = float4(rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, trans.z);

	// Previous world for motion vectors
	const auto& prevRot = prevWorld.rotate;
	const auto& prevTrans = prevWorld.translate;
	float prevScale = prevWorld.scale;

	out.PrevWorldRow0 = float4(prevRot.entry[0][0] * prevScale, prevRot.entry[0][1] * prevScale, prevRot.entry[0][2] * prevScale, prevTrans.x);
	out.PrevWorldRow1 = float4(prevRot.entry[1][0] * prevScale, prevRot.entry[1][1] * prevScale, prevRot.entry[1][2] * prevScale, prevTrans.y);
	out.PrevWorldRow2 = float4(prevRot.entry[2][0] * prevScale, prevRot.entry[2][1] * prevScale, prevRot.entry[2][2] * prevScale, prevTrans.z);

	return true;
}

void GrassInstancing::RenderTreeBatches()
{
	if (treeBatches.empty())
		return;

	auto* ctx = globals::d3d::context;
	if (!ctx)
		return;

	uint32_t maxBatchSize = 0;
	for (const auto& batch : treeBatches) {
		maxBatchSize = std::max(maxBatchSize, batch.instanceCount);
	}
	EnsureTreeBufferCapacity(maxBatchSize);

	if (!treeInstanceBuffer)
		return;

	for (auto& batch : treeBatches) {
		RenderTreeBatch(batch);
	}
}

void GrassInstancing::RenderTreeBatch(TreeBatch& batch)
{
	if (!batch.templatePass || batch.instanceCount == 0)
		return;

	auto* pass = batch.templatePass;
	auto* shader = pass->shader;
	auto* geom = pass->geometry;

	if (!shader || !geom)
		return;

	auto* ctx = globals::d3d::context;
	auto& runtime = geom->GetGeometryRuntimeData();
	auto* rendererData = runtime.rendererData;

	if (!rendererData)
		return;

	// Upload batch instances
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = ctx->Map(treeInstanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr))
			return;
		memcpy(mapped.pData, &treeInstances[batch.instanceOffset], batch.instanceCount * sizeof(TreeInstance));
		ctx->Unmap(treeInstanceBuffer.get(), 0);
	}

	// Bind instance SRV
	ID3D11ShaderResourceView* srv = treeInstanceSRV.get();
	ctx->VSSetShaderResources(TREE_SRV_SLOT, 1, &srv);

	// Setup shader
	shader->SetupTechnique(batch.technique);
	shader->SetupGeometry(pass, batch.renderFlags);

	// Get buffers
	ID3D11Buffer* vertexBuffer = rendererData->vertexBuffer;
	ID3D11Buffer* indexBuffer = rendererData->indexBuffer;

	if (!vertexBuffer || !indexBuffer) {
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	uint32_t stride = runtime.vertexDesc.GetSize();
	uint32_t offset = 0;

	ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
	ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	uint32_t indexCount = 0;
	if (auto* triShape = geom->AsTriShape()) {
		indexCount = triShape->GetTrishapeRuntimeData().triangleCount * 3;
	} else {
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	ctx->DrawIndexedInstanced(indexCount, batch.instanceCount, 0, 0, 0);
	currentStats.treeDrawCallsIssued++;

	// Unbind
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(TREE_SRV_SLOT, 1, &nullSRV);

	shader->RestoreGeometry(pass, batch.renderFlags);
}

// ============================================================================
// HOOKS
// ============================================================================

void GrassInstancing::Hooks::RenderPassImmediately::thunk(
	RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags)
{
	auto& inst = globals::features::grassInstancing;
	auto* cache = globals::shaderCache;

	if (!cache->IsEnabled() || !inst.loaded) {
		func(pass, technique, alphaTest, renderFlags);
		return;
	}

	if (pass && pass->shader) {
		auto type = pass->shader->shaderType.get();

		// Handle grass
		if (type == RE::BSShader::Type::Grass) {
			if (type != inst.prevShaderType) {
				inst.OnGrassShaderChange(type);
			}
			if (inst.TryCollectGrassPass(pass, technique, alphaTest, renderFlags)) {
				inst.prevShaderType = type;
				return;
			}
		}
		// Handle trees (Lighting shader with TREE_ANIM)
		else if (type == RE::BSShader::Type::Lighting && inst.IsTreePass(technique)) {
			if (inst.TryCollectTreePass(pass, technique, renderFlags)) {
				return;
			}
		}
		// Handle transitions
		else {
			// Flush grass if we were collecting
			if (inst.collectingGrass && type != RE::BSShader::Type::Grass) {
				inst.StopGrassCollectionAndFlush();
			}
			// Flush trees if we were collecting and hit non-tree
			if (inst.collectingTrees && (type != RE::BSShader::Type::Lighting || !inst.IsTreePass(technique))) {
				inst.StopTreeCollectionAndFlush();
			}
		}

		inst.prevShaderType = type;
	}

	func(pass, technique, alphaTest, renderFlags);
}

void GrassInstancing::Hooks::Install()
{
	stl::write_thunk_call<RenderPassImmediately>(
		REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

	logger::info("[VegetationInstancing] Hooks installed");
}
