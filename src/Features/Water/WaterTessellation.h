#pragma once

#include <atomic>
#include <d3d11.h>
#include <future>
#include <winrt/base.h>
#include <nlohmann/json.hpp>

class ConstantBuffer;

namespace UnifiedWaterTessellation
{
	struct TessellationSettings
	{
		bool EnableTessellation = true;
		float TessellationMinDistance = 256.0f;
		float TessellationMaxDistance = 6144.0f;
		float TessellationMinFactor = 0.1f;
		float TessellationMaxFactor = 16.0f;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		TessellationSettings,
		EnableTessellation,
		TessellationMinDistance,
		TessellationMaxDistance,
		TessellationMinFactor,
		TessellationMaxFactor)

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

	// Shader management
	winrt::com_ptr<ID3D11HullShader>& GetHullShader();
	winrt::com_ptr<ID3D11DomainShader>& GetDomainShader();
	winrt::com_ptr<ID3D11GeometryShader>& GetGeometryShader();

	// Constant buffer for tessellation parameters
	ConstantBuffer* GetTessellationParamsBuffer();
	void SetTessellationParamsBuffer(ConstantBuffer* buffer);

	// Async shader compilation
	void CompileShadersAsync();
	bool AreShadersReady();
	bool AreShadersCompiling();

	// State tracking for SetupGeometry/RestoreGeometry
	bool& GetTessellationActiveForPass();
	D3D11_PRIMITIVE_TOPOLOGY& GetOriginalTopology();

	// Update tessellation constant buffer with current camera/settings
	void UpdateTessellationParams(const TessellationSettings& settings, ID3D11DeviceContext* context);

	// Bind tessellation shaders and setup pipeline
	void BindTessellationShaders(ID3D11DeviceContext* context, const TessellationSettings& settings, ConstantBuffer* perFrameBuffer);

	// Unbind tessellation shaders and restore original state
	void UnbindTessellationShaders(ID3D11DeviceContext* context);

	// Bind just geometry shader for wireframe visualization
	void BindGeometryShaderOnly(ID3D11DeviceContext* context);

	// Check if tessellation is supported for a given render technique
	bool IsTechniqueCompatible(uint32_t technique);
}
