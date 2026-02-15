#pragma once

struct SkyScattering : Feature
{
public:
	static constexpr uint32_t MAX_CLOUD_LAYERS = 32;

	struct alignas(16) Settings
	{
		uint32_t Enabled = 1;
		float Opacity = 1.0f;
		uint32_t NumLayers = 16;
		float CloudShadowStrength = 0.6f;

		float3 ScatterTint = { 1.0f, 0.85f, 0.6f };
		float ScatterAmount = 0.3f;

		float SilverIntensity = 0.5f;
		float SilverSpread = 0.15f;
		float AmbientDarkening = 0.4f;
		float pad;
	};

	Settings settings;

	// Metadata
	virtual inline std::string GetName() override { return "Sky Scattering"; }
	virtual inline std::string GetShortName() override { return "SkyScattering"; }
	virtual inline std::string_view GetCategory() const override { return "Sky"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SKY_SCATTERING"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Adds volumetric sky scattering by rendering clouds into a layered cubemap array, enabling realistic inter-cloud shadowing.",
			{ "Cubemap array with up to 32 cloud layers",
				"Per-layer cloud shadowing from above",
				"Volumetric cloud scattering approximation",
				"Cloud shadow texture from final layer" }
		};
	}

	// Resources
	Texture2D* texCubemapArray = nullptr;         ///< Cubemap array for layered cloud rendering
	Texture2D* texCubemapArrayCopy = nullptr;     ///< Copy for safe SRV binding

	/// Per-face RTVs for each layer: [layer][face]
	ID3D11RenderTargetView* layerRTVs[MAX_CLOUD_LAYERS][6] = {};
	ID3D11RenderTargetView* layerCopyRTVs[MAX_CLOUD_LAYERS][6] = {};

	ID3D11BlendState* scatterBlendState = nullptr;

	uint32_t currentCloudLayer = 0;
	bool overrideSky = false;

	// Feature interface
	virtual void SetupResources() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void CheckResourcesSide(int side);
	void SkyShaderHacks();
	void ModifySky(RE::BSRenderPass* Pass);
	void AdvanceCloudLayer();

	virtual void ReflectionsPrepass() override;
	virtual void EarlyPrepass() override;

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSSkyShader_SetupMaterial
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);
			logger::info("[Sky Scattering] Installed hooks");
		}
	};
};
