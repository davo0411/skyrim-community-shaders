#pragma once

#include "Buffer.h"

struct ExtendedMaterials : Feature
{
	virtual inline std::string GetName() override { return "Extended Materials"; }
	virtual inline std::string GetShortName() override { return "ExtendedMaterials"; }
	virtual inline std::string_view GetShaderDefineName() override { return "EXTENDED_MATERIALS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Extended Materials adds advanced material effects including parallax occlusion mapping and complex material blending.\n"
			"This feature enhances surface detail and depth perception for more realistic textures.",
			{ "Parallax occlusion mapping for depth",
				"Complex material blending",
				"Terrain heightmap support",
				"Parallax shadows",
				"Height-based texture blending" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct alignas(16) Settings
	{
		uint EnableComplexMaterial = 1;

		uint EnableParallax = 1;
		uint EnableTerrain = 0;
		uint EnableHeightBlending = 1;

		uint EnableShadows = 1;
		uint EnableParallaxWarpingFix = 1;

		// SSDM silhouette extrusion: screen-space displacement of mesh/terrain SIDES only,
		// layered on top of the texture-space POM interior (Lobel 2008). 0 = off.
		uint EnableSilhouette = 0;

		// Multiplier on the SSDM seed amplitude (apparent silhouette depth). 1 = matches POM height.
		float SilhouetteScale = 1.0f;
	};
	STATIC_ASSERT_ALIGNAS_16(Settings);

	Settings settings;

	virtual void DataLoaded() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	// ---- SSDM (screen-space silhouette extrusion) ----
	// Lighting writes duv (RG) + coverage (B) into texDisplacement mip0; the build pass downsamples
	// it into a pyramid; the solve writes absolute fetch-UV (RG), validity (Z), coverage (W) into texSSDM.
	void DrawSSDM();
	void RegisterDisplacementRT();
	ID3D11ShaderResourceView* GetSSDMOffsetSRV() const;
	// Mip0 duv+coverage written by Lighting; composite reads this to skip interior POM pixels.
	ID3D11ShaderResourceView* GetDisplacementSeedSRV() const;
	void ClearDisplacementTexture();

	bool SilhouetteActive() const { return settings.EnableParallax && settings.EnableSilhouette; }

	static constexpr int SSDM_MIP_LEVELS = 6;

	eastl::unique_ptr<Texture2D> texDisplacement;
	winrt::com_ptr<ID3D11RenderTargetView> rtvDisplacement;
	winrt::com_ptr<ID3D11UnorderedAccessView> uavDisplacement[SSDM_MIP_LEVELS];
	// Single-mip SRVs for SSDMBuildPyramid: full-chain SRV + mip UAV on the same texture is undefined in D3D11.
	winrt::com_ptr<ID3D11ShaderResourceView> srvDisplacementMip[SSDM_MIP_LEVELS];

	eastl::unique_ptr<Texture2D> texSSDM;

private:
	void CompileSSDMComputeShadersIfNeeded();

	struct alignas(16) SSDMSolveCB
	{
		float surfaceWidth;
		float surfaceHeight;
		float bufferWidth;
		float bufferHeight;
		float rcpBufferWidth;
		float rcpBufferHeight;
		float maxStepUv;
		float damping;
		std::int32_t numMips;
		std::int32_t numIters;
		std::int32_t pad0;
		std::int32_t pad1;
		std::int32_t pad2;
		std::int32_t pad3;
		std::int32_t pad4;
		std::int32_t pad5;
	};
	STATIC_ASSERT_ALIGNAS_16(SSDMSolveCB);
	static_assert(sizeof(SSDMSolveCB) == 64);

	winrt::com_ptr<ID3D11ComputeShader> ssdmBuildPyramidCS;
	winrt::com_ptr<ID3D11ComputeShader> ssdmSolveCS;
	eastl::unique_ptr<ConstantBuffer> cbufSSDMSolve;
};
