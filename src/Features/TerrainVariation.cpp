#include "TerrainVariation.h"
#include "../FeatureBuffer.h"
#include "../Globals.h"
#include "../State.h"
#include "../Util.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

#include <pystring/pystring.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <array>
#include <sstream>

namespace
{
	using json = nlohmann::json;
	namespace fs = std::filesystem;

	constexpr wchar_t kBombTextureRoot[] = L"Data\\Shaders\\TerrainVariation\\Bombs";
	constexpr uint32_t kMaxBombSprites = 256u;

	std::string ToLower(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value;
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainVariation::Settings,
	enableTilingFix,
	enableLODTerrainTilingFix,
	enableBombing,
	debugDraw,
	randomSeed,
	density,
	cellSize,
	radiusMin,
	radiusMax,
	fadeStart,
	fadeEnd,
	intensity,
	colorMatchStrength,
	normalBlend,
	biomeAffinity,
	debugFadeStart,
	debugFadeEnd)

void TerrainVariation::DrawSettings()
{
	bool reloadRequested = false;

	if (ImGui::Checkbox("Enable Terrain Tiling Fix", reinterpret_cast<bool*>(&settings.enableTilingFix))) {
		reloadRequested = true;
	}
	if (ImGui::Checkbox("Enable LOD Terrain Tiling Fix", reinterpret_cast<bool*>(&settings.enableLODTerrainTilingFix))) {
		reloadRequested = true;
	}
	if (ImGui::Checkbox("Enable Texture Bombing", reinterpret_cast<bool*>(&settings.enableBombing))) {
		reloadRequested = true;
	}

	ImGui::Separator();

	bool debugOverlay = settings.debugDraw != 0;
	if (ImGui::Checkbox("Debug Bomb Overlay", &debugOverlay)) {
		settings.debugDraw = debugOverlay ? 1u : 0u;
	}

	int randomSeed = static_cast<int>(settings.randomSeed);
	if (ImGui::InputInt("Random Seed", &randomSeed)) {
		settings.randomSeed = static_cast<uint32_t>(randomSeed < 0 ? 0 : randomSeed);
	}

	ImGui::SliderFloat("Bomb Density", &settings.density, 0.0f, 3.0f);
	ImGui::SliderFloat("Bomb Intensity", &settings.intensity, 0.0f, 2.0f);
	ImGui::SliderFloat("Color Match Strength", &settings.colorMatchStrength, 0.0f, 2.0f);
	ImGui::SliderFloat("Normal Blend", &settings.normalBlend, 0.0f, 1.0f);
	ImGui::SliderFloat("Biome Affinity", &settings.biomeAffinity, 0.0f, 1.0f);

	ImGui::InputFloat("Cell Size", &settings.cellSize);
	ImGui::InputFloat("Radius Min", &settings.radiusMin);
	ImGui::InputFloat("Radius Max", &settings.radiusMax);
	ImGui::InputFloat("Fade Start", &settings.fadeStart);
	ImGui::InputFloat("Fade End", &settings.fadeEnd);
	ImGui::InputFloat("Debug Fade Start", &settings.debugFadeStart);
	ImGui::InputFloat("Debug Fade End", &settings.debugFadeEnd);

	ImGui::Text("Loaded Sprites: %u", settings.bombSpriteCount);
	if (ImGui::Button("Reload Bomb Textures")) {
		reloadRequested = true;
	}

	settings.density = std::clamp(settings.density, 0.0f, 3.0f);
	settings.intensity = std::clamp(settings.intensity, 0.0f, 2.0f);
	settings.colorMatchStrength = std::clamp(settings.colorMatchStrength, 0.0f, 2.0f);
	settings.normalBlend = std::clamp(settings.normalBlend, 0.0f, 1.0f);
	settings.biomeAffinity = std::clamp(settings.biomeAffinity, 0.0f, 1.0f);
	settings.cellSize = std::max(32.0f, settings.cellSize);
	settings.radiusMin = std::max(16.0f, settings.radiusMin);
	settings.radiusMax = std::max(settings.radiusMax, settings.radiusMin + 1.0f);
	settings.fadeStart = std::max(0.0f, settings.fadeStart);
	settings.fadeEnd = std::max(settings.fadeEnd, settings.fadeStart + 1.0f);
	settings.debugFadeStart = std::max(0.0f, settings.debugFadeStart);
	settings.debugFadeEnd = std::max(settings.debugFadeEnd, settings.debugFadeStart + 1.0f);
	settings.debugDraw = settings.debugDraw ? 1u : 0u;

	if (reloadRequested) {
		ReloadBombTextures();
	}

	UpdateShaderSettings();
}

void TerrainVariation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainVariation::RestoreDefaultSettings()
{
	settings = {};
	ReloadBombTextures();
}

bool TerrainVariation::DrawFailLoadMessage() const
{
	return false;
}

void TerrainVariation::ReloadBombTextures()
{
	ReleaseBombResources();

	std::error_code ec;
	fs::path basePath = fs::path(kBombTextureRoot);
	if (!fs::exists(basePath, ec)) {
		if (settings.enableBombing) {
			logger::warn("TerrainVariation: bomb texture directory '{}' not found.", basePath.string());
		}
		settings.bombSpriteCount = 0;
		UpdateShaderSettings();
		return;
	}

	EnumerateBombTextures(basePath);

	if (bombSpriteSources.empty()) {
		settings.bombSpriteCount = 0;
		logger::info("TerrainVariation: no bomb textures discovered under '{}'.", basePath.string());
		UpdateShaderSettings();
		return;
	}

	if (!BuildTextureArray()) {
		ReleaseBombResources();
		settings.bombSpriteCount = 0;
		UpdateShaderSettings();
		return;
	}

	BuildMetadataBuffer();

	if (!bombTextureSRV || !bombMetadataSRV) {
		ReleaseBombResources();
		logger::warn("TerrainVariation: bomb resources incomplete after build. Effect disabled.");
		UpdateShaderSettings();
		return;
	}

	settings.bombSpriteCount = static_cast<uint32_t>(bombSpriteGPUData.size());

	logger::info("TerrainVariation: loaded {} bomb textures.", settings.bombSpriteCount);
	UpdateShaderSettings();
}

void TerrainVariation::ReleaseBombResources()
{
	bombTextureArray = nullptr;
	bombTextureSRV = nullptr;
	bombNormalArray = nullptr;
	bombNormalSRV = nullptr;
	bombRMAArray = nullptr;
	bombRMASRV = nullptr;
	bombHeightArray = nullptr;
	bombHeightSRV = nullptr;
	bombMetadataBuffer = nullptr;
	bombMetadataSRV = nullptr;
	bombSpriteSources.clear();
	bombSpriteGPUData.clear();
	settings.bombSpriteCount = 0;
}

void TerrainVariation::EnumerateBombTextures(const fs::path& basePath)
{
	std::error_code ec;
	fs::recursive_directory_iterator it(basePath, ec), end;
	if (ec) {
		logger::warn("TerrainVariation: failed to enumerate '%s' (%s).", basePath.string(), ec.message());
		return;
	}

	for (; it != end; it.increment(ec)) {
		if (ec) {
			logger::warn("TerrainVariation: directory iteration error (%s).", ec.message());
			continue;
		}
		if (!it->is_regular_file(ec)) {
			continue;
		}
		if (!it->path().has_extension() || !pystring::iequals(it->path().extension().string(), ".dds")) {
			continue;
		}

		BombSpriteSource sprite;
		sprite.ddsPath = it->path();
		sprite.metadataPath = sprite.ddsPath;
		sprite.metadataPath.replace_extension(".json");

		auto parentName = sprite.ddsPath.parent_path().filename().string();
		sprite.categoryMask = ResolveCategoryMask(parentName);

		try {
			DirectX::ScratchImage image;
			DX::ThrowIfFailed(DirectX::LoadFromDDSFile(sprite.ddsPath.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));

			DirectX::ScratchImage converted;
			DX::ThrowIfFailed(DirectX::Convert(image.GetImages(), image.GetImageCount(), image.GetMetadata(), DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, converted));

			const DirectX::Image* src = converted.GetImage(0, 0, 0);
			if (!src) {
				logger::warn("TerrainVariation: failed to read image data for '%s'.", sprite.ddsPath.string());
				continue;
			}

			const float* pixels = reinterpret_cast<const float*>(src->pixels);
			const size_t pixelCount = static_cast<size_t>(src->width) * static_cast<size_t>(src->height);
			if (pixelCount == 0) {
				continue;
			}

			double accum[3] = { 0.0, 0.0, 0.0 };
			for (size_t i = 0; i < pixelCount; ++i) {
				accum[0] += pixels[i * 4 + 0];
				accum[1] += pixels[i * 4 + 1];
				accum[2] += pixels[i * 4 + 2];
			}

			const double invCount = 1.0 / static_cast<double>(pixelCount);
			sprite.averageColor[0] = static_cast<float>(accum[0] * invCount);
			sprite.averageColor[1] = static_cast<float>(accum[1] * invCount);
			sprite.averageColor[2] = static_cast<float>(accum[2] * invCount);
		} catch (const DX::com_exception& e) {
			logger::warn("TerrainVariation: skipping '%s' (%s).", sprite.ddsPath.string(), e.what());
			continue;
		}

		ApplyMetadataOverrides(sprite);

		auto ensureExists = [](const std::filesystem::path& path) {
			std::error_code existsError;
			return !path.empty() && fs::exists(path, existsError);
		};

		if (!sprite.hasNormal || !sprite.hasRMA || !sprite.hasHeight) {
			const std::wstring stem = sprite.ddsPath.stem().wstring();
			auto resolveWithSuffixes = [&](std::filesystem::path& target, std::initializer_list<const wchar_t*> suffixes) {
				if (ensureExists(target)) {
					return;
				}
				const std::filesystem::path folder = sprite.ddsPath.parent_path();
				for (const auto* suffix : suffixes) {
					std::filesystem::path candidate = folder / (stem + suffix + L".dds");
					if (ensureExists(candidate)) {
						target = candidate;
						break;
					}
				}
			};

			resolveWithSuffixes(sprite.normalPath, { L"_n", L"_normal", L"_norm" });
			resolveWithSuffixes(sprite.rmaPath, { L"_rma", L"_rmao", L"_rmaos" });
			resolveWithSuffixes(sprite.heightPath, { L"_h", L"_height", L"_disp", L"_parallax" });

			sprite.hasNormal = ensureExists(sprite.normalPath);
			sprite.hasRMA = ensureExists(sprite.rmaPath);
			sprite.hasHeight = ensureExists(sprite.heightPath);
		}

		if (!sprite.hasNormal || !sprite.hasRMA || !sprite.hasHeight) {
			logger::warn(
				"TerrainVariation: '{}' missing required PBR textures (normal={}, RMA={}, height={}). Entry skipped.",
				sprite.ddsPath.filename().string(),
				sprite.hasNormal ? "present" : "missing",
				sprite.hasRMA ? "present" : "missing",
				sprite.hasHeight ? "present" : "missing");
			continue;
		}

		bombSpriteSources.push_back(std::move(sprite));
		if (bombSpriteSources.size() >= kMaxBombSprites) {
			logger::warn("TerrainVariation: reached sprite budget of {}. Remaining files ignored.", kMaxBombSprites);
			break;
		}
	}
}

bool TerrainVariation::BuildTextureArray()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	if (!device || !context) {
		return false;
	}

	auto loadTexture2D = [&](const std::filesystem::path& path,
		winrt::com_ptr<ID3D11Texture2D>& outTexture,
		D3D11_TEXTURE2D_DESC& referenceDesc,
		bool& referenceInitialised,
		const char* label) -> bool {
		if (path.empty()) {
			logger::warn("TerrainVariation: missing %s texture path.", label);
			return false;
		}

		winrt::com_ptr<ID3D11Resource> resource;
		try {
			DX::ThrowIfFailed(DirectX::CreateDDSTextureFromFile(device, context, path.c_str(), resource.put(), nullptr));
		} catch (const DX::com_exception& e) {
			logger::warn("TerrainVariation: failed to load %s texture '%s' (%s).", label, path.string(), e.what());
			return false;
		}

		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(resource.as(texture))) {
			logger::warn("TerrainVariation: %s texture '%s' is not 2D.", label, path.string());
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		if (!referenceInitialised) {
			referenceDesc = desc;
			referenceDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
			referenceDesc.CPUAccessFlags = 0;
			referenceDesc.MiscFlags &= ~(UINT)D3D11_RESOURCE_MISC_TEXTURECUBE;
			referenceInitialised = true;
		} else if (desc.Width != referenceDesc.Width || desc.Height != referenceDesc.Height || desc.Format != referenceDesc.Format || desc.MipLevels != referenceDesc.MipLevels) {
			logger::warn("TerrainVariation: %s texture '%s' dimensions or mip count mismatch. Skipping sprite.", label, path.string());
			return false;
		}
		outTexture = std::move(texture);
		return true;
	};

	std::vector<BombSpriteSource> filteredSources;
	filteredSources.reserve(bombSpriteSources.size());

	std::vector<winrt::com_ptr<ID3D11Texture2D>> albedoSlices;
	std::vector<winrt::com_ptr<ID3D11Texture2D>> normalSlices;
	std::vector<winrt::com_ptr<ID3D11Texture2D>> rmaSlices;
	std::vector<winrt::com_ptr<ID3D11Texture2D>> heightSlices;
	albedoSlices.reserve(bombSpriteSources.size());
	normalSlices.reserve(bombSpriteSources.size());
	rmaSlices.reserve(bombSpriteSources.size());
	heightSlices.reserve(bombSpriteSources.size());

	D3D11_TEXTURE2D_DESC albedoDesc{};
	D3D11_TEXTURE2D_DESC normalDesc{};
	D3D11_TEXTURE2D_DESC rmaDesc{};
	D3D11_TEXTURE2D_DESC heightDesc{};
	bool albedoInitialised = false;
	bool normalInitialised = false;
	bool rmaInitialised = false;
	bool heightInitialised = false;

	for (const auto& sprite : bombSpriteSources) {
		winrt::com_ptr<ID3D11Texture2D> albedoTexture;
		winrt::com_ptr<ID3D11Texture2D> normalTexture;
		winrt::com_ptr<ID3D11Texture2D> rmaTexture;
		winrt::com_ptr<ID3D11Texture2D> heightTexture;

		if (!loadTexture2D(sprite.ddsPath, albedoTexture, albedoDesc, albedoInitialised, "albedo")) {
			continue;
		}
		if (!loadTexture2D(sprite.normalPath, normalTexture, normalDesc, normalInitialised, "normal")) {
			continue;
		}
		if (!loadTexture2D(sprite.rmaPath, rmaTexture, rmaDesc, rmaInitialised, "RMA")) {
			continue;
		}
		if (!loadTexture2D(sprite.heightPath, heightTexture, heightDesc, heightInitialised, "height")) {
			continue;
		}

		filteredSources.push_back(sprite);
		albedoSlices.push_back(std::move(albedoTexture));
		normalSlices.push_back(std::move(normalTexture));
		rmaSlices.push_back(std::move(rmaTexture));
		heightSlices.push_back(std::move(heightTexture));
	}

	if (albedoSlices.empty()) {
		logger::warn("TerrainVariation: no compatible bomb textures loaded.");
		return false;
	}

	bombSpriteSources = std::move(filteredSources);

	auto createArray = [&](const std::vector<winrt::com_ptr<ID3D11Texture2D>>& slices,
		D3D11_TEXTURE2D_DESC desc,
		winrt::com_ptr<ID3D11Texture2D>& outTexture,
		winrt::com_ptr<ID3D11ShaderResourceView>& outSRV,
		const char* label) -> bool {
		desc.ArraySize = static_cast<UINT>(slices.size());
		winrt::com_ptr<ID3D11Texture2D> arrayTexture;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, arrayTexture.put()))) {
			logger::error("TerrainVariation: failed to create %s texture array.", label);
			return false;
		}

		for (size_t slice = 0; slice < slices.size(); ++slice) {
			for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
				const UINT dstIndex = D3D11CalcSubresource(mip, static_cast<UINT>(slice), desc.MipLevels);
				context->CopySubresourceRegion(arrayTexture.get(), dstIndex, 0, 0, 0, slices[slice].get(), mip, nullptr);
			}
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MostDetailedMip = 0;
		srvDesc.Texture2DArray.MipLevels = desc.MipLevels;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.ArraySize = desc.ArraySize;

		winrt::com_ptr<ID3D11ShaderResourceView> arraySRV;
		if (FAILED(device->CreateShaderResourceView(arrayTexture.get(), &srvDesc, arraySRV.put()))) {
			logger::error("TerrainVariation: failed to create %s SRV.", label);
			return false;
		}

		outTexture = std::move(arrayTexture);
		outSRV = std::move(arraySRV);
		return true;
	};

	if (!createArray(albedoSlices, albedoDesc, bombTextureArray, bombTextureSRV, "albedo")) {
		return false;
	}
	if (!createArray(normalSlices, normalDesc, bombNormalArray, bombNormalSRV, "normal")) {
		return false;
	}
	if (!createArray(rmaSlices, rmaDesc, bombRMAArray, bombRMASRV, "RMA")) {
		return false;
	}
	if (!createArray(heightSlices, heightDesc, bombHeightArray, bombHeightSRV, "height")) {
		return false;
	}

	return true;
}

void TerrainVariation::BuildMetadataBuffer()
{
	auto device = globals::d3d::device;
	if (!device) {
		return;
	}

	bombSpriteGPUData.clear();
	bombSpriteGPUData.reserve(bombSpriteSources.size());

	for (size_t i = 0; i < bombSpriteSources.size(); ++i) {
		const auto& source = bombSpriteSources[i];
		BombSpriteGPU gpu{};

		gpu.averageColorMask[0] = source.averageColor[0];
		gpu.averageColorMask[1] = source.averageColor[1];
		gpu.averageColorMask[2] = source.averageColor[2];

		static_assert(sizeof(uint32_t) == sizeof(float));
		std::memcpy(&gpu.averageColorMask[3], &source.categoryMask, sizeof(uint32_t));

		gpu.params[0] = std::max(0.1f, source.sizeScale);
		gpu.params[1] = std::clamp(source.normalStrength, 0.0f, 1.0f);
		gpu.params[2] = std::clamp(source.heightStrength, 0.0f, 1.0f);
		uint32_t flags = 0;
		if (source.hasNormal) {
			flags |= 1u;
		}
		if (source.hasRMA) {
			flags |= 1u << 1;
		}
		if (source.hasHeight) {
			flags |= 1u << 2;
		}
		std::memcpy(&gpu.params[3], &flags, sizeof(uint32_t));

		gpu.materialParams[0] = std::max(0.0f, source.roughnessScale);
		gpu.materialParams[1] = std::max(0.0f, source.metalnessScale);
		gpu.materialParams[2] = std::max(0.0f, source.aoScale);
		gpu.materialParams[3] = std::max(0.0f, source.heightScale);

		gpu.parallaxParams[0] = std::max(0.0f, source.parallaxScale);
		gpu.parallaxParams[1] = 0.0f;
		gpu.parallaxParams[2] = 0.0f;
		gpu.parallaxParams[3] = 0.0f;

		bombSpriteGPUData.push_back(gpu);
	}

	if (bombSpriteGPUData.empty()) {
		return;
	}

	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = static_cast<UINT>(bombSpriteGPUData.size() * sizeof(BombSpriteGPU));
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.StructureByteStride = sizeof(BombSpriteGPU);
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	D3D11_SUBRESOURCE_DATA init{};
	init.pSysMem = bombSpriteGPUData.data();

	winrt::com_ptr<ID3D11Buffer> buffer;
	if (FAILED(device->CreateBuffer(&desc, &init, buffer.put()))) {
		logger::error("TerrainVariation: failed to create metadata buffer.");
		return;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.ViewDimension = D3D_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = static_cast<UINT>(bombSpriteGPUData.size());

	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	if (FAILED(device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put()))) {
		logger::error("TerrainVariation: failed to create metadata SRV.");
		return;
	}

	bombMetadataBuffer = std::move(buffer);
	bombMetadataSRV = std::move(srv);
}

uint32_t TerrainVariation::ResolveCategoryMask(const fs::path& folderName) const
{
	uint32_t mask = static_cast<uint32_t>(BombCategory::Shared);

	if (folderName.empty()) {
		return mask;
	}

	std::string name = ToLower(folderName.generic_string());
	std::replace_if(name.begin(), name.end(), [](char c) { return c == '_' || c == '-' || c == '+' || c == '/' || c == '\\'; }, ' ');

	std::istringstream stream(name);
	std::string token;
	uint32_t derivedMask = 0;

	while (stream >> token) {
		if (token == "snow") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Snow);
		} else if (token == "dirt" || token == "soil") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Dirt);
		} else if (token == "rock" || token == "stone") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Rock);
		} else if (token == "mud") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Mud);
		} else if (token == "moss") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Moss);
		} else if (token == "gravel" || token == "pebble") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Gravel);
		} else if (token == "shared" || token == "common" || token == "all") {
			derivedMask |= static_cast<uint32_t>(BombCategory::Shared);
		}
	}

	if (derivedMask != 0) {
		mask = derivedMask;
	}

	return mask;
}

void TerrainVariation::ApplyMetadataOverrides(BombSpriteSource& sprite)
{
	std::error_code ec;
	if (sprite.metadataPath.empty() || !fs::exists(sprite.metadataPath, ec)) {
		return;
	}

	std::ifstream file(sprite.metadataPath);
	if (!file.is_open()) {
		logger::warn("TerrainVariation: unable to open metadata '%s'.", sprite.metadataPath.string());
		return;
	}

	json data = json::parse(file, nullptr, false);
	if (!data.is_object()) {
		return;
	}

	if (auto categories = data.find("categories"); categories != data.end()) {
		uint32_t overrideMask = 0;
		if (categories->is_array()) {
			for (const auto& entry : *categories) {
				if (entry.is_string()) {
					overrideMask |= ResolveCategoryMask(std::filesystem::path(entry.get<std::string>()));
				}
			}
		} else if (categories->is_string()) {
			overrideMask = ResolveCategoryMask(std::filesystem::path(categories->get<std::string>()));
		}
		if (overrideMask != 0) {
			sprite.categoryMask = overrideMask;
		}
	}

	if (auto scale = data.find("sizeScale"); scale != data.end() && scale->is_number()) {
		sprite.sizeScale = std::max(0.1f, scale->get<float>());
	}

	const auto extractUnit = [](const json::iterator& it) {
		return std::clamp(it->get<float>(), 0.0f, 1.0f);
	};

	if (auto normalStrength = data.find("normalStrength"); normalStrength != data.end() && normalStrength->is_number()) {
		sprite.normalStrength = extractUnit(normalStrength);
	} else if (auto legacyInfluence = data.find("normalInfluence"); legacyInfluence != data.end() && legacyInfluence->is_number()) {
		sprite.normalStrength = extractUnit(legacyInfluence);
	}

	if (auto heightStrength = data.find("heightStrength"); heightStrength != data.end() && heightStrength->is_number()) {
		sprite.heightStrength = extractUnit(heightStrength);
	}

	const auto metadataDir = sprite.metadataPath.parent_path();
	const auto resolvePath = [&](const json::iterator& it, std::filesystem::path& target) {
		if (it == data.end() || !it->is_string()) {
			return false;
		}
		const std::string& relative = it.value().get_ref<const std::string&>();
		std::filesystem::path candidate = metadataDir / relative;
		if (!candidate.has_extension()) {
			candidate.replace_extension(".dds");
		}
		if (fs::exists(candidate)) {
			target = candidate;
			return true;
		}
		return false;
	};

	if (auto normalMap = data.find("normalMap"); resolvePath(normalMap, sprite.normalPath)) {
		sprite.hasNormal = true;
	}
	if (auto rmaMap = data.find("rmaMap"); resolvePath(rmaMap, sprite.rmaPath)) {
		sprite.hasRMA = true;
	}
	if (auto roughnessMetalAOMap = data.find("RMAOSMap"); resolvePath(roughnessMetalAOMap, sprite.rmaPath)) {
		sprite.hasRMA = true;
	}
	if (auto heightMap = data.find("heightMap"); resolvePath(heightMap, sprite.heightPath)) {
		sprite.hasHeight = true;
	}
	if (auto parallaxMap = data.find("parallaxMap"); resolvePath(parallaxMap, sprite.heightPath)) {
		sprite.hasHeight = true;
	}

	if (auto parallaxScale = data.find("parallaxScale"); parallaxScale != data.end() && parallaxScale->is_number()) {
		sprite.parallaxScale = std::max(0.0f, static_cast<float>(parallaxScale.value()));
	}
	if (auto heightScale = data.find("heightScale"); heightScale != data.end() && heightScale->is_number()) {
		sprite.heightScale = std::max(0.0f, static_cast<float>(heightScale.value()));
	}
	if (auto roughnessScale = data.find("roughnessScale"); roughnessScale != data.end() && roughnessScale->is_number()) {
		sprite.roughnessScale = std::max(0.0f, static_cast<float>(roughnessScale.value()));
	}
	if (auto metalnessScale = data.find("metalnessScale"); metalnessScale != data.end() && metalnessScale->is_number()) {
		sprite.metalnessScale = std::max(0.0f, static_cast<float>(metalnessScale.value()));
	}
	if (auto aoScale = data.find("aoScale"); aoScale != data.end() && aoScale->is_number()) {
		sprite.aoScale = std::max(0.0f, static_cast<float>(aoScale.value()));
	}
}

void TerrainVariation::SetupResources()
{
	ReloadBombTextures();
}

void TerrainVariation::Reset()
{
}

void TerrainVariation::Prepass()
{
	auto context = globals::d3d::context;
	if (!context) {
		return;
	}

	ID3D11ShaderResourceView* baseSRVs[2]{};
	ID3D11ShaderResourceView* normalSRV = nullptr;
	ID3D11ShaderResourceView* rmaSRV = nullptr;
	ID3D11ShaderResourceView* heightSRV = nullptr;

	if (settings.enableBombing && bombTextureSRV && bombMetadataSRV) {
		baseSRVs[0] = bombTextureSRV.get();
		baseSRVs[1] = bombMetadataSRV.get();
		normalSRV = bombNormalSRV ? bombNormalSRV.get() : nullptr;
		rmaSRV = bombRMASRV ? bombRMASRV.get() : nullptr;
		heightSRV = bombHeightSRV ? bombHeightSRV.get() : nullptr;
	}

	context->PSSetShaderResources(66, 2, baseSRVs);
	context->PSSetShaderResources(120, 1, &normalSRV);
	context->PSSetShaderResources(121, 1, &rmaSRV);
	context->PSSetShaderResources(122, 1, &heightSRV);
}

void TerrainVariation::PostPostLoad()
{
	ReloadBombTextures();
}

void TerrainVariation::UpdateShaderSettings()
{
	if (auto state = globals::state; state) {
		state->UpdateSharedData(state->inWorld, false);
	}
}