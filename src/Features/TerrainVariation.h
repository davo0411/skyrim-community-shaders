#pragma once

struct TerrainVariation : Feature
{
private:
	static constexpr std::string_view MOD_ID = "148123";

public:
	virtual inline std::string GetName() override { return "Terrain Variation"; }
	virtual inline std::string GetShortName() override { return "TerrainVariation"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_VARIATION"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override
	{
		// TERRAIN_VARIATION acts purely as an inclusion gate for the optional Nexus-shipped HLSLI
		// (the file is absent when the feature is uninstalled). All path divergence inside Lighting
		// / ExtendedMaterials is now driven by SharedData::terrainVariationSettings.enableTerrainVariation
		// so a single compiled landscape permutation handles both states.
		return loaded && shaderType == RE::BSShader::Type::Lighting;
	}
	virtual bool IsCore() const override { return false; };
	virtual bool SupportsVR() override { return true; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Terrain Variation reduces the repeating pattern effect on terrain textures.\n"
			"This technique creates more natural-looking terrain by adding variation to texture sampling.",
			{ "Reduces terrain texture tiling",
				"Adjustable distance-based blending",
				"Improved terrain visual quality",
				"Compatible with Extended Materials parallax" }
		};
	}

	struct alignas(16) Settings
	{
		// Runtime master switch for the stochastic terrain variation path. Mirrors the
		// installed-state of the feature so a single LANDSCAPE shader permutation can branch
		// on SharedData::terrainVariationSettings.enableTerrainVariation. GetCommonBufferData()
		// zeroes this on the GPU side when the feature is not loaded, guaranteeing the
		// vanilla SampleBias / SampleLevel fallback path is taken without touching the
		// user-facing serialised value.
		uint32_t enableTerrainVariation = 1;
		uint32_t enableLODTerrainTilingFix = 1;
		uint32_t pad[2]{};
	};

	STATIC_ASSERT_ALIGNAS_16(Settings);

	Settings settings;

	// Returns settings safe to upload to the FeatureData cbuffer. When the feature isn't
	// loaded the master toggle is forced off so shader-side runtime branches stay on the
	// vanilla fallback even if the persisted default is 1.
	inline Settings GetCommonBufferData() const
	{
		Settings out = settings;
		if (!loaded)
			out.enableTerrainVariation = 0;
		return out;
	}

	virtual void DrawSettings() override;
	virtual bool DrawFailLoadMessage() const override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void PostPostLoad() override;
};