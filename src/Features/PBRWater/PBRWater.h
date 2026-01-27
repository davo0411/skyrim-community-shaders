#pragma once

struct PBRWater : public Feature
{
	virtual inline std::string GetName() override { return "PBR Water"; }
	virtual inline std::string GetShortName() override { return "PBRWater"; }
	virtual inline std::string_view GetCategory() const override { return "Lighting"; }
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
			}
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "PBR_WATER"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override { return t == RE::BSShader::Type::Lighting; };

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	// Resources
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	struct Settings
	{
		bool EnableBRDFSpecular = true;
		bool EnableWaterScattering = true;
	} settings;
};
