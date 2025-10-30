#pragma once

#include "Feature.h"

#include <filesystem>
#include <string>
#include <vector>

#include <winrt/base.h>

struct TerrainVariation : Feature
{
private:
	static constexpr std::string_view MOD_ID = "148123";

	enum class BombCategory : uint32_t
	{
		None = 0,
		Snow = 1u << 0,
		Dirt = 1u << 1,
		Rock = 1u << 2,
		Mud = 1u << 3,
		Moss = 1u << 4,
		Gravel = 1u << 5,
		Shared = 1u << 6
	};

	struct BombSpriteGPU
	{
		float averageColorMask[4];
		float params[4];          // x=size scale, y=normal strength, z=height strength, w=flags as float
		float materialParams[4];  // x=roughness scale, y=metalness scale, z=ao scale, w=height scale
		float parallaxParams[4];  // x=parallax scale, y=reserved, z=reserved, w=reserved
	};

	struct BombSpriteSource
	{
		std::filesystem::path ddsPath;
		std::filesystem::path metadataPath;
		std::filesystem::path normalPath;
		std::filesystem::path rmaPath;
		std::filesystem::path heightPath;
		uint32_t categoryMask = static_cast<uint32_t>(BombCategory::Shared);
		float averageColor[3] = { 0.5f, 0.5f, 0.5f };
		float sizeScale = 1.0f;
		float normalStrength = 0.0f;
		float heightStrength = 0.0f;
		float parallaxScale = 1.0f;
		float roughnessScale = 1.0f;
		float metalnessScale = 1.0f;
		float aoScale = 1.0f;
		float heightScale = 1.0f;
		bool hasNormal = false;
		bool hasRMA = false;
		bool hasHeight = false;
	};

	std::vector<BombSpriteGPU> bombSpriteGPUData;
	std::vector<BombSpriteSource> bombSpriteSources;

	winrt::com_ptr<ID3D11Texture2D> bombTextureArray;
	winrt::com_ptr<ID3D11ShaderResourceView> bombTextureSRV;
	winrt::com_ptr<ID3D11Texture2D> bombNormalArray;
	winrt::com_ptr<ID3D11ShaderResourceView> bombNormalSRV;
	winrt::com_ptr<ID3D11Texture2D> bombRMAArray;
	winrt::com_ptr<ID3D11ShaderResourceView> bombRMASRV;
	winrt::com_ptr<ID3D11Texture2D> bombHeightArray;
	winrt::com_ptr<ID3D11ShaderResourceView> bombHeightSRV;
	winrt::com_ptr<ID3D11Buffer> bombMetadataBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> bombMetadataSRV;

	void ReloadBombTextures();
	void ReleaseBombResources();
	void EnumerateBombTextures(const std::filesystem::path& basePath);
	bool BuildTextureArray();
	void BuildMetadataBuffer();
	uint32_t ResolveCategoryMask(const std::filesystem::path& folderName) const;
	void ApplyMetadataOverrides(BombSpriteSource& sprite);

public:
	virtual inline std::string GetName() override { return "Terrain Variation"; }
	virtual inline std::string GetShortName() override { return "TerrainVariation"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_VARIATION"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type shaderType) override
	{
		return (shaderType == RE::BSShader::Type::Lighting);
	}
	virtual bool IsCore() const override { return false; };
	virtual bool SupportsVR() override { return true; }
	virtual std::string_view GetCategory() const override { return "Landscape & Textures"; }

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

	struct Settings
	{
		uint enableTilingFix = true;
		uint enableLODTerrainTilingFix = true;
		uint enableBombing = true;
		uint debugDraw = 0;
		uint bombSpriteCount = 0;
		uint randomSeed = 1337;
		float density = 0.45f;
		float cellSize = 384.0f;
		float radiusMin = 120.0f;
		float radiusMax = 220.0f;
		float fadeStart = 350.0f;
		float fadeEnd = 2400.0f;
		float intensity = 0.85f;
		float colorMatchStrength = 0.35f;
		float normalBlend = 0.25f;
		float biomeAffinity = 0.5f;
		float debugFadeStart = 150.0f;
		float debugFadeEnd = 1200.0f;
		float pad0[2]{};
	} settings;

	virtual void DrawSettings() override;
	virtual bool DrawFailLoadMessage() const override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void SetupResources() override;
	virtual void Reset() override;
	virtual void Prepass() override;
	virtual void PostPostLoad() override;
	void UpdateShaderSettings();
};