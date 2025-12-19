#include "WaterTessellation.h"

#include "Buffer.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "Util.h"

namespace UnifiedWaterTessellation
{
	// Static shader storage
	static winrt::com_ptr<ID3D11HullShader> waterHullShader;
	static winrt::com_ptr<ID3D11DomainShader> waterDomainShader;
	static winrt::com_ptr<ID3D11GeometryShader> waterGeometryShader;

	// Async shader compilation state
	static std::atomic<bool> tessellationShadersReady{ false };
	static std::atomic<bool> tessellationShadersCompiling{ false };
	static std::future<void> shaderCompileFuture;

	// Constant buffer
	static ConstantBuffer* tessellationParams = nullptr;

	// Per-pass state tracking (thread_local for safety)
	static thread_local bool tessellationActiveForPass = false;
	static thread_local D3D11_PRIMITIVE_TOPOLOGY originalTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

	winrt::com_ptr<ID3D11HullShader>& GetHullShader()
	{
		return waterHullShader;
	}

	winrt::com_ptr<ID3D11DomainShader>& GetDomainShader()
	{
		return waterDomainShader;
	}

	winrt::com_ptr<ID3D11GeometryShader>& GetGeometryShader()
	{
		return waterGeometryShader;
	}

	ConstantBuffer* GetTessellationParamsBuffer()
	{
		return tessellationParams;
	}

	void SetTessellationParamsBuffer(ConstantBuffer* buffer)
	{
		tessellationParams = buffer;
	}

	bool AreShadersReady()
	{
		return tessellationShadersReady.load();
	}

	bool AreShadersCompiling()
	{
		return tessellationShadersCompiling.load();
	}

	bool& GetTessellationActiveForPass()
	{
		return tessellationActiveForPass;
	}

	D3D11_PRIMITIVE_TOPOLOGY& GetOriginalTopology()
	{
		return originalTopology;
	}

	bool IsTechniqueCompatible(uint32_t technique)
	{
		// Tessellation is only compatible with SPECULAR techniques (0-7)
		// UNDERWATER (8), LOD (9), STENCIL (10), SIMPLE (11) have different VS_OUTPUT structures
		// Our HS/DS are compiled with SPECULAR + FLOWMAP + BLEND_NORMALS defines
		return (technique < 8);
	}

	void CompileShadersAsync()
	{
		// Don't start if already compiling or ready
		if (tessellationShadersCompiling.load() || tessellationShadersReady.load()) {
			return;
		}

		tessellationShadersCompiling.store(true);
		logger::info("[Unified Water] [Tessellation] Starting async shader compilation");

		// Launch compilation on a separate thread
		shaderCompileFuture = std::async(std::launch::async, []() {
			// Compile tessellation shaders
			// Using SPECULAR + FLOWMAP + BLEND_NORMALS as the common water permutation
			// NUM_SPECULAR_LIGHTS must match what the game's VS uses for correct VS_OUTPUT structure
			std::vector<std::pair<const char*, const char*>> tessDefines = {
				{ "HSHADER", "" },
				{ "UNIFIED_WATER", "" },
				{ "SPECULAR", "" },
				{ "NUM_SPECULAR_LIGHTS", "0" },
				{ "FLOWMAP", "" },
				{ "BLEND_NORMALS", "" },
				{ "NORMAL_TEXCOORD", "" }
			};

			bool allSuccess = true;
			constexpr std::string_view featureName = "UnifiedWater";
			const std::wstring sourcePath = L"Data\\Shaders\\Water.hlsl";

			// Hull shader
			if (auto* hullShader = static_cast<ID3D11HullShader*>(
					SIE::SShaderCache::CompileAndCacheCustomShader(sourcePath, tessDefines, SIE::ShaderClass::Hull, featureName, "WaterHull"))) {
				waterHullShader.attach(hullShader);
				logger::debug("[Unified Water] [Tessellation] Hull shader ready");
			} else {
				logger::error("[Unified Water] [Tessellation] Failed to compile hull shader");
				allSuccess = false;
			}

			// Domain shader with same defines but DSHADER instead of HSHADER
			tessDefines[0] = { "DSHADER", "" };

			if (auto* domainShader = static_cast<ID3D11DomainShader*>(
					SIE::SShaderCache::CompileAndCacheCustomShader(sourcePath, tessDefines, SIE::ShaderClass::Domain, featureName, "WaterDomain"))) {
				waterDomainShader.attach(domainShader);
				logger::debug("[Unified Water] [Tessellation] Domain shader ready");
			} else {
				logger::error("[Unified Water] [Tessellation] Failed to compile domain shader");
				allSuccess = false;
			}

			// Geometry shader for proper per-triangle barycentric coordinates (needed for wireframe view)
			tessDefines[0] = { "GSHADER", "" };

			if (auto* geometryShader = static_cast<ID3D11GeometryShader*>(
					SIE::SShaderCache::CompileAndCacheCustomShader(sourcePath, tessDefines, SIE::ShaderClass::Geometry, featureName, "WaterGeometry"))) {
				waterGeometryShader.attach(geometryShader);
				logger::debug("[Unified Water] [Tessellation] Geometry shader ready");
			} else {
				logger::error("[Unified Water] [Tessellation] Failed to compile geometry shader");
				allSuccess = false;
			}

			tessellationShadersCompiling.store(false);
			tessellationShadersReady.store(allSuccess);

			if (allSuccess) {
				logger::info("[Unified Water] [Tessellation] All shaders ready");
			} else {
				logger::warn("[Unified Water] [Tessellation] Some shaders failed to compile");
			}
		});
	}

	void UpdateTessellationParams(const TessellationSettings& settings, ID3D11DeviceContext* context)
	{
		if (!tessellationParams)
			return;

		TessellationParams params{};
		params.TessellationMinDistance = settings.TessellationMinDistance;
		params.TessellationMaxDistance = settings.TessellationMaxDistance;
		params.TessellationMinFactor = settings.TessellationMinFactor;
		params.TessellationMaxFactor = settings.TessellationMaxFactor;

		// Get camera world position using established utility function
		auto cameraPos = Util::GetEyePosition(0);
		params.CameraWorldPosX = cameraPos.x;
		params.CameraWorldPosY = cameraPos.y;
		params.CameraWorldPosZ = cameraPos.z;
		params.DetailHeightScale = 0.0f;  // Unused - kept for cbuffer compatibility

		tessellationParams->Update(params);

		// Bind tessellation constant buffer to HS and DS
		ID3D11Buffer* tessBuffers[1] = { tessellationParams->CB() };
		context->HSSetConstantBuffers(9, 1, tessBuffers);
		context->DSSetConstantBuffers(9, 1, tessBuffers);
	}

	void BindTessellationShaders(ID3D11DeviceContext* context, const TessellationSettings& settings, ConstantBuffer* perFrameBuffer)
	{
		if (!AreShadersReady())
			return;

		// Update tessellation parameters
		UpdateTessellationParams(settings, context);

		// Save original topology for RestoreGeometry
		context->IAGetPrimitiveTopology(&originalTopology);

		// Set patch list topology for tessellation (3 control points per patch)
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		// Bind hull, domain, and geometry shaders
		context->HSSetShader(waterHullShader.get(), nullptr, 0);
		context->DSSetShader(waterDomainShader.get(), nullptr, 0);
		context->GSSetShader(waterGeometryShader.get(), nullptr, 0);

		// Bind VS constant buffers to DS as well (DS needs the same transforms)
		ID3D11Buffer* vsBuffers[3] = { nullptr, nullptr, nullptr };
		context->VSGetConstantBuffers(0, 3, vsBuffers);
		context->DSSetConstantBuffers(0, 3, vsBuffers);

		// Bind the FrameBuffer cbuffer (b12) to HS and DS - needed for CameraPosAdjust
		ID3D11Buffer* frameBuffer[1] = { nullptr };
		context->PSGetConstantBuffers(12, 1, frameBuffer);
		if (frameBuffer[0]) {
			context->HSSetConstantBuffers(12, 1, frameBuffer);
			context->DSSetConstantBuffers(12, 1, frameBuffer);
		}

		// Bind the UnifiedWater per-frame buffer to HS/DS
		if (perFrameBuffer) {
			ID3D11Buffer* perFrameBuffers[1] = { perFrameBuffer->CB() };
			context->HSSetConstantBuffers(7, 1, perFrameBuffers);
			context->DSSetConstantBuffers(7, 1, perFrameBuffers);
		}

		// Bind normal textures to DS for tessellation
		ID3D11ShaderResourceView* normalSRVs[3] = { nullptr, nullptr, nullptr };
		ID3D11SamplerState* normalSamplers[3] = { nullptr, nullptr, nullptr };
		context->PSGetShaderResources(4, 3, normalSRVs);
		context->PSGetSamplers(4, 3, normalSamplers);
		context->DSSetShaderResources(4, 3, normalSRVs);
		context->DSSetSamplers(4, 3, normalSamplers);

		for (int i = 0; i < 3; i++) {
			if (normalSRVs[i])
				normalSRVs[i]->Release();
			if (normalSamplers[i])
				normalSamplers[i]->Release();
		}

		// Bind flowmap textures (slots 8-9) for flowmap water
		ID3D11ShaderResourceView* flowmapSRVs[2] = { nullptr, nullptr };
		ID3D11SamplerState* flowmapSamplers[2] = { nullptr, nullptr };
		context->PSGetShaderResources(8, 2, flowmapSRVs);
		context->PSGetSamplers(8, 2, flowmapSamplers);
		context->DSSetShaderResources(8, 2, flowmapSRVs);
		context->DSSetSamplers(8, 2, flowmapSamplers);
		for (int i = 0; i < 2; i++) {
			if (flowmapSRVs[i])
				flowmapSRVs[i]->Release();
			if (flowmapSamplers[i])
				flowmapSamplers[i]->Release();
		}

		tessellationActiveForPass = true;
	}

	void UnbindTessellationShaders(ID3D11DeviceContext* context)
	{
		if (!tessellationActiveForPass)
			return;

		// Unbind hull, domain, and geometry shaders
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);

		// Restore original topology
		if (originalTopology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			context->IASetPrimitiveTopology(originalTopology);
		}

		tessellationActiveForPass = false;
		originalTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	}

	void BindGeometryShaderOnly(ID3D11DeviceContext* context)
	{
		if (!AreShadersReady())
			return;

		// Bind only the geometry shader for tri visualization without tessellation
		// GS assigns proper per-triangle barycentric coordinates needed for wireframe rendering
		context->GSSetShader(waterGeometryShader.get(), nullptr, 0);
		tessellationActiveForPass = true;  // Reuse flag to trigger cleanup in RestoreGeometry
	}
}
