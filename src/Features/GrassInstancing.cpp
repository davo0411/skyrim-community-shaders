#include "GrassInstancing.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassInstancing::Settings,
	EnableInstancing,
	ShowDebugStats)

void GrassInstancing::SetupResources()
{
	collected.reserve(8192);
	allInstances.reserve(8192);
	batches.reserve(64);
	EnsureBufferCapacity(INITIAL_CAPACITY);
	logger::info("[GrassInstancing] Initialized with buffer capacity {}", INITIAL_CAPACITY);
}

void GrassInstancing::Reset()
{
	// Update smoothed stats
	const float alpha = 0.1f;
	displayStats.passesCollected = static_cast<uint32_t>(displayStats.passesCollected * (1.f - alpha) + currentStats.passesCollected * alpha);
	displayStats.drawCallsIssued = static_cast<uint32_t>(displayStats.drawCallsIssued * (1.f - alpha) + currentStats.drawCallsIssued * alpha);
	displayStats.instancesRendered = static_cast<uint32_t>(displayStats.instancesRendered * (1.f - alpha) + currentStats.instancesRendered * alpha);
	displayStats.largestBatch = static_cast<uint32_t>(displayStats.largestBatch * (1.f - alpha) + currentStats.largestBatch * alpha);

	currentStats.Reset();
	collected.clear();
	allInstances.clear();
	batches.clear();
	collecting = false;
	prevShaderType = RE::BSShader::Type::None;
}

void GrassInstancing::DrawSettings()
{
	if (ImGui::TreeNodeEx("Grass Instancing", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable GPU Instancing", &settings.EnableInstancing);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Batches grass into instanced draw calls for major performance gains");

		ImGui::Checkbox("Show Stats", &settings.ShowDebugStats);

		if (settings.ShowDebugStats && displayStats.passesCollected > 0) {
			ImGui::Separator();
			ImGui::Text("Original Draw Calls: %u", displayStats.passesCollected);
			ImGui::Text("Batched Draw Calls: %u", displayStats.drawCallsIssued);
			ImGui::Text("Instances Rendered: %u", displayStats.instancesRendered);
			ImGui::Text("Largest Batch: %u", displayStats.largestBatch);

			float reduction = 100.f * (1.f - (float)displayStats.drawCallsIssued / (float)displayStats.passesCollected);
			ImGui::Text("Draw Call Reduction: %.1f%%", reduction);
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
// Core Logic
// ============================================================================

void GrassInstancing::OnShaderChange(RE::BSShader::Type newType)
{
	if (prevShaderType == RE::BSShader::Type::Grass && newType != RE::BSShader::Type::Grass) {
		StopCollectionAndFlush();
	}

	if (prevShaderType != RE::BSShader::Type::Grass && newType == RE::BSShader::Type::Grass) {
		StartCollection();
	}

	prevShaderType = newType;
}

void GrassInstancing::StartCollection()
{
	if (!settings.EnableInstancing)
		return;

	collecting = true;
	collected.clear();
	allInstances.clear();
	batches.clear();
}

void GrassInstancing::StopCollectionAndFlush()
{
	if (!collecting)
		return;

	collecting = false;

	if (collected.empty())
		return;

	BuildBatches();
	// Note: UploadInstanceBuffer is NOT called here
	// Each batch uploads its own slice in RenderSingleBatch
	RenderAllBatches();
}

bool GrassInstancing::TryCollectPass(RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags)
{
	if (!collecting || !settings.EnableInstancing)
		return false;

	if (!pass || !pass->shader || !pass->geometry)
		return false;

	if (pass->shader->shaderType.get() != RE::BSShader::Type::Grass)
		return false;

	// Extract instance transform from geometry
	GrassInstance inst;
	if (!ExtractInstance(pass->geometry, inst))
		return false;

	// Collect the pass
	CollectedPass cp;
	cp.pass = pass;
	cp.technique = technique;
	cp.alphaTest = alphaTest;
	cp.renderFlags = renderFlags;
	cp.batchKey = MakeBatchKey(pass, technique);
	cp.instance = inst;
	collected.push_back(cp);

	currentStats.passesCollected++;
	return true;
}

uint64_t GrassInstancing::MakeBatchKey(RE::BSRenderPass* pass, uint32_t technique)
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

void GrassInstancing::BuildBatches()
{
	if (collected.empty())
		return;

	// Sort by batch key to group same texture+technique together
	std::sort(collected.begin(), collected.end(), [](const CollectedPass& a, const CollectedPass& b) {
		return a.batchKey < b.batchKey;
	});

	// Build batches
	batches.clear();
	allInstances.clear();

	uint64_t currentKey = collected[0].batchKey;
	Batch currentBatch;
	currentBatch.templatePass = collected[0].pass;
	currentBatch.technique = collected[0].technique;
	currentBatch.alphaTest = collected[0].alphaTest;
	currentBatch.renderFlags = collected[0].renderFlags;
	currentBatch.instanceOffset = 0;
	currentBatch.instanceCount = 0;

	for (const auto& cp : collected) {
		if (cp.batchKey != currentKey) {
			// Finish current batch
			if (currentBatch.instanceCount > 0) {
				currentStats.largestBatch = std::max(currentStats.largestBatch, currentBatch.instanceCount);
				batches.push_back(currentBatch);
			}

			// Start new batch
			currentKey = cp.batchKey;
			currentBatch.templatePass = cp.pass;
			currentBatch.technique = cp.technique;
			currentBatch.alphaTest = cp.alphaTest;
			currentBatch.renderFlags = cp.renderFlags;
			currentBatch.instanceOffset = static_cast<uint32_t>(allInstances.size());
			currentBatch.instanceCount = 0;
		}

		allInstances.push_back(cp.instance);
		currentBatch.instanceCount++;
	}

	// Final batch
	if (currentBatch.instanceCount > 0) {
		currentStats.largestBatch = std::max(currentStats.largestBatch, currentBatch.instanceCount);
		batches.push_back(currentBatch);
	}

	currentStats.instancesRendered = static_cast<uint32_t>(allInstances.size());
}

// ============================================================================
// GPU Resources
// ============================================================================

void GrassInstancing::EnsureBufferCapacity(uint32_t needed)
{
	if (bufferCapacity >= needed)
		return;

	auto* device = globals::d3d::device;
	if (!device)
		return;

	// Round up to power of 2
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

	instanceBuffer = nullptr;
	instanceSRV = nullptr;

	HRESULT hr = device->CreateBuffer(&desc, nullptr, instanceBuffer.put());
	if (FAILED(hr)) {
		logger::error("[GrassInstancing] Failed to create buffer: {:x}", (uint32_t)hr);
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = newCap;

	hr = device->CreateShaderResourceView(instanceBuffer.get(), &srvDesc, instanceSRV.put());
	if (FAILED(hr)) {
		logger::error("[GrassInstancing] Failed to create SRV: {:x}", (uint32_t)hr);
		instanceBuffer = nullptr;
		return;
	}

	bufferCapacity = newCap;
	logger::debug("[GrassInstancing] Buffer resized to {} instances", newCap);
}

void GrassInstancing::UploadInstanceBuffer()
{
	if (allInstances.empty() || !instanceBuffer)
		return;

	EnsureBufferCapacity(static_cast<uint32_t>(allInstances.size()));

	auto* ctx = globals::d3d::context;
	if (!ctx)
		return;

	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		logger::error("[GrassInstancing] Failed to map buffer");
		return;
	}

	memcpy(mapped.pData, allInstances.data(), allInstances.size() * sizeof(GrassInstance));
	ctx->Unmap(instanceBuffer.get(), 0);
}

// ============================================================================
// Instance Extraction
// ============================================================================

bool GrassInstancing::ExtractInstance(RE::BSGeometry* geom, GrassInstance& out)
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

	// Get instance data offset within vertex structure
	uint32_t offset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_INSTANCEDATA);

	// Instance data is 4 float4s at this offset in the first vertex
	float* data = reinterpret_cast<float*>(renderer->rawVertexData + offset);

	out.Data1 = float4(data[0], data[1], data[2], data[3]);
	out.Data2 = float4(data[4], data[5], data[6], data[7]);
	out.Data3 = float4(data[8], data[9], data[10], data[11]);
	out.Data4 = float4(data[12], data[13], data[14], data[15]);

	return true;
}

// ============================================================================
// Rendering
// ============================================================================

void GrassInstancing::RenderAllBatches()
{
	if (batches.empty())
		return;

	auto* ctx = globals::d3d::context;
	if (!ctx)
		return;

	// Find largest batch and ensure buffer capacity
	uint32_t maxBatchSize = 0;
	for (const auto& batch : batches) {
		maxBatchSize = std::max(maxBatchSize, batch.instanceCount);
	}
	EnsureBufferCapacity(maxBatchSize);

	if (!instanceBuffer)
		return;

	for (auto& batch : batches) {
		RenderSingleBatch(batch);
	}
}

void GrassInstancing::RenderSingleBatch(Batch& batch)
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

	// Upload this batch's instances to buffer (SV_InstanceID starts at 0 per draw)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = ctx->Map(instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) {
			logger::error("[GrassInstancing] Failed to map instance buffer");
			return;
		}
		memcpy(mapped.pData, &allInstances[batch.instanceOffset], batch.instanceCount * sizeof(GrassInstance));
		ctx->Unmap(instanceBuffer.get(), 0);
	}

	// Bind instance buffer SRV
	ID3D11ShaderResourceView* srv = instanceSRV.get();
	ctx->VSSetShaderResources(SRV_SLOT, 1, &srv);

	// Setup shader technique
	shader->SetupTechnique(batch.technique);

	// Setup geometry (constant buffers, textures, etc.)
	shader->SetupGeometry(pass, batch.renderFlags);

	// Get vertex/index buffers from template geometry
	ID3D11Buffer* vertexBuffer = rendererData->vertexBuffer;
	ID3D11Buffer* indexBuffer = rendererData->indexBuffer;

	if (!vertexBuffer || !indexBuffer) {
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	// Get vertex stride
	uint32_t stride = runtime.vertexDesc.GetSize();
	uint32_t offset = 0;

	// Bind geometry buffers
	ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
	ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Get index count
	uint32_t indexCount = 0;
	if (auto* triShape = geom->AsTriShape()) {
		indexCount = triShape->GetTrishapeRuntimeData().triangleCount * 3;
	} else {
		logger::warn("[GrassInstancing] Non-BSTriShape geometry");
		shader->RestoreGeometry(pass, batch.renderFlags);
		return;
	}

	// DRAW INSTANCED - massive draw call reduction!
	ctx->DrawIndexedInstanced(
		indexCount,
		batch.instanceCount,
		0,   // StartIndexLocation
		0,   // BaseVertexLocation
		0);  // StartInstanceLocation (always 0, we upload per-batch)

	currentStats.drawCallsIssued++;

	// Unbind instance buffer
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->VSSetShaderResources(SRV_SLOT, 1, &nullSRV);

	// Restore geometry state
	shader->RestoreGeometry(pass, batch.renderFlags);
}

// ============================================================================
// Hooks
// ============================================================================

void GrassInstancing::Hooks::RenderPassImmediately::thunk(
	RE::BSRenderPass* pass, uint32_t technique, bool alphaTest, uint32_t renderFlags)
{
	auto& inst = globals::features::grassInstancing;
	auto* cache = globals::shaderCache;

	if (!cache->IsEnabled() || !inst.loaded || !inst.settings.EnableInstancing) {
		func(pass, technique, alphaTest, renderFlags);
		return;
	}

	// Track shader type transitions
	if (pass && pass->shader) {
		auto type = pass->shader->shaderType.get();
		if (type != inst.prevShaderType) {
			inst.OnShaderChange(type);
		}
	}

	// Try to collect for instancing
	if (inst.TryCollectPass(pass, technique, alphaTest, renderFlags)) {
		return;  // Collected, will be rendered in batch
	}

	// Not grass or collection failed, render normally
	func(pass, technique, alphaTest, renderFlags);
}

void GrassInstancing::Hooks::Install()
{
	stl::write_thunk_call<RenderPassImmediately>(
		REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

	logger::info("[GrassInstancing] Hooks installed");
}
