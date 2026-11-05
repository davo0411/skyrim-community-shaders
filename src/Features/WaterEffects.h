#pragma once

#include <winrt/base.h>

/** @brief Enhances water rendering with realistic caustics and underwater lighting effects. */
struct WaterEffects : Feature
{
public:
	winrt::com_ptr<ID3D11ShaderResourceView> causticsView;
	virtual inline std::string GetName() override { return "Water Effects"; }
	virtual std::string GetDisplayName() override { return T("feature.water_effects.name", "Water Effects"); }
	/** @brief Returns the short identifier used for file paths and logging. */
	virtual inline std::string GetShortName() override { return "WaterEffects"; }
	virtual inline std::string_view GetShaderDefineName() override { return "WATER_EFFECTS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }

	/** @brief Returns a summary description and list of key features for the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.water_effects.description", "Water Effects enhances water rendering with realistic caustics and underwater lighting effects.\nThis feature adds dynamic light patterns and improved water visual quality."),
			{ T("feature.water_effects.key_feature_1", "Realistic water caustics"),
				T("feature.water_effects.key_feature_2", "Enhanced underwater lighting"),
				T("feature.water_effects.key_feature_3", "Dynamic light patterns on water surfaces"),
				T("feature.water_effects.key_feature_4", "Improved water visual fidelity"),
				T("feature.water_effects.key_feature_5", "Atmospheric underwater effects") } };
	};

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	/** @brief Loads the water caustics DDS texture from disk. */
	virtual void SetupResources() override;

	/** @brief Binds the caustics texture SRV to the pixel shader for the current frame. */
	virtual void Prepass() override;

	virtual bool IsCore() const override { return true; };

	/** @brief User settings of the caustics on everything under water (Crest-style light projection and focal blur). */
	struct Settings
	{
		float CausticsProjection = 4.0f;     ///< divisor of the projection along the light (Crest 4)
		float CausticsMinLightZ = 0.2f;      ///< lowest sun elevation (sine) the projection follows
		float CausticsFocalDepth = 2.0f;     ///< m: water depth at which caustics are sharpest (Crest 2)
		float CausticsDepthOfField = 0.33f;  ///< m: range of depth over which they stay sharp (Crest 0.33)
		float CausticsMaxBlur = 6.0f;        ///< most mip levels of defocus blur
	};

	Settings settings;

	/** @brief The caustics' constants for the shared feature buffer; matches WaterEffectsSettings in SharedData.hlsli. */
	struct alignas(16) SharedSettings
	{
		float4 Caustics0;  // x projection divisor, y lowest sun elevation (sine), z focal depth (m), w depth of field (m)
		float4 Caustics1;  // x most mip levels of defocus blur, yzw unused
	};
	STATIC_ASSERT_ALIGNAS_16(SharedSettings);

	/** @brief Render thread: the caustics' constants for the shared feature buffer. */
	SharedSettings GetCommonBufferData() const;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
};
