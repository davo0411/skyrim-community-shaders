#include "MeshShaderRuleManager.h"

#include "Globals.h"
#include "Util.h"

#include <algorithm>
#include <fstream>

// ========== JSON Serialization ==========

void from_json(const json& j, MeshShaderRuleManager::MatchCriteria& criteria)
{
	if (j.contains("editorId") && j["editorId"].is_array()) {
		criteria.editorIdPatterns = j["editorId"].get<std::vector<std::string>>();
	}
	if (j.contains("modelPath") && j["modelPath"].is_array()) {
		criteria.modelPathPatterns = j["modelPath"].get<std::vector<std::string>>();
	}
	if (j.contains("texturePath") && j["texturePath"].is_array()) {
		criteria.texturePathPatterns = j["texturePath"].get<std::vector<std::string>>();
	}
	if (j.contains("nodeName") && j["nodeName"].is_array()) {
		criteria.nodeNamePatterns = j["nodeName"].get<std::vector<std::string>>();
	}
	if (j.contains("textureSet") && j["textureSet"].is_array()) {
		criteria.textureSetPatterns = j["textureSet"].get<std::vector<std::string>>();
	}

	// Parse shader flags if present
	if (j.contains("shaderFlags") && j["shaderFlags"].is_array()) {
		for (const auto& flag : j["shaderFlags"]) {
			if (!flag.is_string())
				continue;

			std::string flagName = flag.get<std::string>();

			// Map common flag names to actual values
			// Using BSShaderProperty::EShaderPropertyFlag
			static const std::unordered_map<std::string, uint64_t> flagMap = {
				{ "kSkinned", 1ULL << 1 },
				{ "kTreeAnim", 1ULL << 7 },
				{ "kVertexLighting", 1ULL << 8 },
				{ "kSoftLighting", 1ULL << 9 },
				{ "kRimLighting", 1ULL << 10 },
				{ "kBackLighting", 1ULL << 11 },
				{ "kSpecular", 1ULL << 0 },
				{ "kGlowMap", 1ULL << 13 },
				{ "kParallax", 1ULL << 14 },
				{ "kEnvMap", 1ULL << 15 },
				{ "kDecal", 1ULL << 16 },
				{ "kMultiTextureLandscape", 1ULL << 17 },
			};

			bool negate = flagName.starts_with("!");
			if (negate) {
				flagName = flagName.substr(1);
			}

			auto it = flagMap.find(flagName);
			if (it != flagMap.end()) {
				if (negate) {
					criteria.forbiddenShaderFlags |= it->second;
				} else {
					criteria.requiredShaderFlags |= it->second;
				}
			}
		}
	}

	criteria.CompilePatterns();
}

void from_json(const json& j, MeshShaderRuleManager::Rule& rule)
{
	if (j.contains("name") && j["name"].is_string()) {
		rule.name = j["name"].get<std::string>();
	}
	if (j.contains("profile") && j["profile"].is_string()) {
		rule.profileName = j["profile"].get<std::string>();
	}
	if (j.contains("priority") && j["priority"].is_number_integer()) {
		rule.priority = j["priority"].get<int>();
	}
	if (j.contains("enabled") && j["enabled"].is_boolean()) {
		rule.enabled = j["enabled"].get<bool>();
	}
	if (j.contains("match") && j["match"].is_object()) {
		rule.match = j["match"].get<MeshShaderRuleManager::MatchCriteria>();
	}
	if (j.contains("settings") && j["settings"].is_object()) {
		rule.settings = j["settings"];
	}
}

// ========== MatchCriteria Implementation ==========

void MeshShaderRuleManager::MatchCriteria::CompilePatterns()
{
	editorIdMatchers.Clear();
	editorIdMatchers.AddPatterns(editorIdPatterns);

	modelPathMatchers.Clear();
	modelPathMatchers.AddPatterns(modelPathPatterns);

	texturePathMatchers.Clear();
	texturePathMatchers.AddPatterns(texturePathPatterns);

	nodeNameMatchers.Clear();
	nodeNameMatchers.AddPatterns(nodeNamePatterns);

	textureSetMatchers.Clear();
	textureSetMatchers.AddPatterns(textureSetPatterns);
}

bool MeshShaderRuleManager::MatchCriteria::HasAnyCriteria() const
{
	return !editorIdPatterns.empty() ||
	       !modelPathPatterns.empty() ||
	       !texturePathPatterns.empty() ||
	       !nodeNamePatterns.empty() ||
	       !textureSetPatterns.empty() ||
	       requiredShaderFlags != 0 ||
	       forbiddenShaderFlags != 0;
}

// ========== MeshShaderRuleManager Implementation ==========

size_t MeshShaderRuleManager::DiscoverRules()
{
	std::unique_lock lock(rulesMutex);

	rules.clear();
	featureRuleMap.clear();

	std::filesystem::path rulesDir(RulesDirectory);

	if (!std::filesystem::exists(rulesDir)) {
		logger::info("[MeshShaderRuleManager] Rules directory does not exist: {}", rulesDir.string());
		rulesLoaded = true;
		return 0;
	}

	logger::info("[MeshShaderRuleManager] Discovering rules in: {}", rulesDir.string());

	size_t totalRules = 0;
	size_t filesProcessed = 0;

	try {
		for (const auto& entry : std::filesystem::directory_iterator(rulesDir)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".json") {
				continue;
			}

			filesProcessed++;

			// Skip hidden files
			std::string filename = entry.path().filename().string();
			if (filename.empty() || filename[0] == '.' || filename[0] == '~') {
				continue;
			}

			try {
				size_t rulesFromFile = LoadRulesFromFile(entry.path());
				totalRules += rulesFromFile;

				if (rulesFromFile > 0) {
					logger::info("[MeshShaderRuleManager] Loaded {} rules from {}", rulesFromFile, filename);
				}
			} catch (const std::exception& e) {
				logger::warn("[MeshShaderRuleManager] Failed to load {}: {}", filename, e.what());
			}
		}
	} catch (const std::filesystem::filesystem_error& e) {
		logger::error("[MeshShaderRuleManager] Error accessing rules directory: {}", e.what());
	}

	// Sort rules within each feature by priority (descending)
	for (auto& [featureName, indices] : featureRuleMap) {
		std::sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
			return rules[a].priority > rules[b].priority;
		});
	}

	rulesLoaded = true;
	logger::info("[MeshShaderRuleManager] Discovered {} rules from {} files", totalRules, filesProcessed);

	return totalRules;
}

size_t MeshShaderRuleManager::LoadRulesFromFile(const std::filesystem::path& path)
{
	std::ifstream file(path);
	if (!file.is_open()) {
		throw std::runtime_error("Could not open file");
	}

	json root;
	try {
		file >> root;
	} catch (const json::parse_error& e) {
		throw std::runtime_error(std::string("JSON parse error: ") + e.what());
	}

	// Get feature name from file
	std::string featureName;
	if (root.contains("feature") && root["feature"].is_string()) {
		featureName = root["feature"].get<std::string>();
	} else {
		// Try to extract from filename: {ModName}_{FeatureName}.json or {FeatureName}.json
		std::string filename = path.stem().string();
		size_t underscore = filename.find('_');
		if (underscore != std::string::npos) {
			featureName = filename.substr(underscore + 1);
		} else {
			featureName = filename;
		}
	}

	if (featureName.empty()) {
		throw std::runtime_error("Could not determine feature name");
	}

	// Parse rules array
	if (!root.contains("rules") || !root["rules"].is_array()) {
		throw std::runtime_error("Missing or invalid 'rules' array");
	}

	size_t rulesLoaded = 0;
	std::string sourceFile = path.filename().string();

	for (const auto& ruleJson : root["rules"]) {
		auto parsedRule = ParseRule(ruleJson, featureName, sourceFile);
		if (parsedRule) {
			size_t index = rules.size();
			rules.push_back(std::move(*parsedRule));
			featureRuleMap[featureName].push_back(index);
			rulesLoaded++;
		}
	}

	return rulesLoaded;
}

std::optional<MeshShaderRuleManager::Rule> MeshShaderRuleManager::ParseRule(
	const json& ruleJson,
	const std::string& featureName,
	const std::string& sourceFile)
{
	if (!ruleJson.is_object()) {
		return std::nullopt;
	}

	Rule rule;
	rule.featureName = featureName;
	rule.sourceFile = sourceFile;

	try {
		rule = ruleJson.get<Rule>();
		rule.featureName = featureName;
		rule.sourceFile = sourceFile;
	} catch (const std::exception& e) {
		logger::warn("[MeshShaderRuleManager] Failed to parse rule: {}", e.what());
		return std::nullopt;
	}

	// Validate rule
	if (rule.profileName.empty()) {
		logger::warn("[MeshShaderRuleManager] Rule '{}' has no profile name", rule.name);
		return std::nullopt;
	}

	if (!rule.match.HasAnyCriteria()) {
		logger::warn("[MeshShaderRuleManager] Rule '{}' has no match criteria", rule.name);
		return std::nullopt;
	}

	return rule;
}

void MeshShaderRuleManager::RegisterRule(const Rule& rule)
{
	std::unique_lock lock(rulesMutex);

	size_t index = rules.size();
	rules.push_back(rule);
	featureRuleMap[rule.featureName].push_back(index);

	// Re-sort rules for this feature
	auto& indices = featureRuleMap[rule.featureName];
	std::sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
		return rules[a].priority > rules[b].priority;
	});

	// Invalidate cache since rules changed
	InvalidateCache();
}

void MeshShaderRuleManager::ClearRules()
{
	std::unique_lock lock(rulesMutex);
	rules.clear();
	featureRuleMap.clear();
	InvalidateCache();
}

std::vector<const MeshShaderRuleManager::Rule*> MeshShaderRuleManager::GetRulesForFeature(
	const std::string& featureName) const
{
	std::shared_lock lock(rulesMutex);

	std::vector<const Rule*> result;

	auto it = featureRuleMap.find(featureName);
	if (it != featureRuleMap.end()) {
		result.reserve(it->second.size());
		for (size_t index : it->second) {
			result.push_back(&rules[index]);
		}
	}

	return result;
}

MeshShaderRuleManager::ResolvedRule MeshShaderRuleManager::ResolveForGeometry(
	const std::string& featureName,
	RE::BSGeometry* geometry,
	RE::BSShaderProperty* property)
{
	if (!geometry) {
		return {};
	}

	// Check cache first
	CacheKey cacheKey{ featureName, geometry };
	{
		std::shared_lock cacheLock(cacheMutex);
		auto cacheIt = resolutionCache.find(cacheKey);
		if (cacheIt != resolutionCache.end()) {
			cacheStats.hits++;
			return cacheIt->second;
		}
	}

	cacheStats.misses++;

	ResolvedRule result;

	// Priority 1: Check NiExtraData
	auto extraDataProfile = GetExtraDataProfile(geometry, featureName);
	if (extraDataProfile) {
		result.profileName = *extraDataProfile;
		result.ruleName = "NiExtraData";
		result.matched = true;
		result.priority = INT_MAX;  // Highest priority

		if (debugLogging) {
			logger::info("[MeshShaderRuleManager] {} matched via NiExtraData: {}",
				geometry->name.c_str(), result.profileName);
		}
	} else {
		// Priority 2: Check rules
		std::shared_lock rulesLock(rulesMutex);

		auto it = featureRuleMap.find(featureName);
		if (it != featureRuleMap.end()) {
			// Extract geometry info once for all rule checks
			GeometryInfo info = ExtractGeometryInfo(geometry, property);

			for (size_t index : it->second) {
				const Rule& rule = rules[index];
				if (!rule.enabled) {
					continue;
				}

				if (MatchesCriteria(info, rule.match)) {
					result.profileName = rule.profileName;
					result.ruleName = rule.name;
					result.settings = rule.settings;
					result.matched = true;
					result.priority = rule.priority;

					if (debugLogging) {
						logger::info("[MeshShaderRuleManager] {} matched rule '{}': {}",
							geometry->name.c_str(), rule.name, result.profileName);
					}
					break;  // First match wins (rules are sorted by priority)
				}
			}
		}
	}

	// Cache the result
	{
		std::unique_lock cacheLock(cacheMutex);
		resolutionCache[cacheKey] = result;
		cacheStats.entries = resolutionCache.size();
	}

	return result;
}

std::optional<std::string> MeshShaderRuleManager::GetExtraDataProfile(
	RE::BSGeometry* geometry,
	const std::string& featureName)
{
	if (!geometry) {
		return std::nullopt;
	}

	auto extraData = geometry->GetExtraData(RE::BSFixedString(ExtraDataName.data()));
	if (!extraData) {
		return std::nullopt;
	}

	// Check if it's string extra data
	if (extraData->GetRTTI() == globals::rtti::NiStringExtraDataRTTI.get()) {
		auto* stringData = static_cast<RE::NiStringExtraData*>(extraData);
		if (stringData->value) {
			std::string value = stringData->value;

			// Format: "FeatureName:ProfileName" or just "ProfileName"
			size_t colonPos = value.find(':');
			if (colonPos != std::string::npos) {
				std::string extraFeature = value.substr(0, colonPos);
				if (extraFeature == featureName) {
					return value.substr(colonPos + 1);
				}
			} else {
				// No feature specified, apply to any feature
				return value;
			}
		}
	}

	return std::nullopt;
}

bool MeshShaderRuleManager::MatchesCriteria(
	const GeometryInfo& info,
	const MatchCriteria& criteria)
{
	if (!info.valid) {
		return false;
	}

	// Check shader flags first (fastest)
	if (criteria.requiredShaderFlags != 0) {
		if ((info.shaderFlags & criteria.requiredShaderFlags) != criteria.requiredShaderFlags) {
			return false;
		}
	}

	if (criteria.forbiddenShaderFlags != 0) {
		if ((info.shaderFlags & criteria.forbiddenShaderFlags) != 0) {
			return false;
		}
	}

	// For matching, we require at least one pattern match from any non-empty criteria
	bool hasAnyPatternCriteria = false;
	bool matchedAnyPattern = false;

	// Check editor ID
	if (!criteria.editorIdMatchers.IsEmpty()) {
		hasAnyPatternCriteria = true;
		if (!info.editorId.empty() && criteria.editorIdMatchers.MatchAny(info.editorId)) {
			matchedAnyPattern = true;
		}
	}

	// Check model path
	if (!criteria.modelPathMatchers.IsEmpty()) {
		hasAnyPatternCriteria = true;
		if (!info.modelPath.empty() && criteria.modelPathMatchers.MatchAny(info.modelPath)) {
			matchedAnyPattern = true;
		}
	}

	// Check texture paths
	if (!criteria.texturePathMatchers.IsEmpty()) {
		hasAnyPatternCriteria = true;
		for (const auto& texPath : info.texturePaths) {
			if (criteria.texturePathMatchers.MatchAny(texPath)) {
				matchedAnyPattern = true;
				break;
			}
		}
	}

	// Check node name
	if (!criteria.nodeNameMatchers.IsEmpty()) {
		hasAnyPatternCriteria = true;
		if (!info.nodeName.empty() && criteria.nodeNameMatchers.MatchAny(info.nodeName)) {
			matchedAnyPattern = true;
		}
	}

	// Check texture set
	if (!criteria.textureSetMatchers.IsEmpty()) {
		hasAnyPatternCriteria = true;
		if (!info.textureSetEditorId.empty() && criteria.textureSetMatchers.MatchAny(info.textureSetEditorId)) {
			matchedAnyPattern = true;
		}
	}

	// If there were pattern criteria, we need at least one match
	// If only flag criteria, they've already been checked above
	return !hasAnyPatternCriteria || matchedAnyPattern;
}

MeshShaderRuleManager::GeometryInfo MeshShaderRuleManager::ExtractGeometryInfo(
	RE::BSGeometry* geometry,
	RE::BSShaderProperty* property)
{
	GeometryInfo info;

	if (!geometry) {
		return info;
	}

	info.valid = true;

	// Node name
	if (geometry->name.c_str()) {
		info.nodeName = Util::NormalizePath(geometry->name.c_str());
	}

	// Editor ID and model path from reference
	auto editorId = GetEditorIdForGeometry(geometry);
	if (editorId) {
		info.editorId = Util::ToLower(*editorId);
	}

	auto modelPath = GetModelPathForGeometry(geometry);
	if (modelPath) {
		info.modelPath = Util::NormalizePath(*modelPath);
	}

	// Texture info from property
	if (property) {
		info.texturePaths = GetTexturePathsForProperty(property);

		auto textureSetId = GetTextureSetEditorId(property);
		if (textureSetId) {
			info.textureSetEditorId = Util::ToLower(*textureSetId);
		}

		// Shader flags
		info.shaderFlags = static_cast<uint64_t>(property->flags.underlying());
	}

	return info;
}

std::optional<std::string> MeshShaderRuleManager::GetEditorIdForGeometry(RE::BSGeometry* geometry)
{
	if (!geometry) {
		return std::nullopt;
	}

	auto* userData = geometry->GetUserData();
	if (!userData) {
		return std::nullopt;
	}

	auto* baseObject = userData->GetObjectReference();
	if (!baseObject) {
		return std::nullopt;
	}

	const char* editorId = baseObject->GetFormEditorID();
	if (editorId && editorId[0] != '\0') {
		return std::string(editorId);
	}

	return std::nullopt;
}

std::optional<std::string> MeshShaderRuleManager::GetModelPathForGeometry(RE::BSGeometry* geometry)
{
	if (!geometry) {
		return std::nullopt;
	}

	auto* userData = geometry->GetUserData();
	if (!userData) {
		return std::nullopt;
	}

	auto* baseObject = userData->GetObjectReference();
	if (!baseObject) {
		return std::nullopt;
	}

	// Try to get model from TESModel interface
	if (auto* model = baseObject->As<RE::TESModel>()) {
		const char* modelPath = model->GetModel();
		if (modelPath && modelPath[0] != '\0') {
			return std::string(modelPath);
		}
	}

	return std::nullopt;
}

std::vector<std::string> MeshShaderRuleManager::GetTexturePathsForProperty(RE::BSShaderProperty* property)
{
	std::vector<std::string> paths;

	if (!property || !property->material) {
		return paths;
	}

	auto* baseMaterial = property->material;

	// Try to get texture set for lighting materials
	if (auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(baseMaterial)) {
		auto textureSet = lightingMaterial->GetTextureSet();
		if (textureSet) {
			// Get diffuse texture path (most commonly used for matching)
			const char* diffusePath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
			if (diffusePath && diffusePath[0] != '\0') {
				paths.push_back(Util::NormalizePath(diffusePath));
			}

			// Also get normal map path (useful for some materials)
			const char* normalPath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
			if (normalPath && normalPath[0] != '\0') {
				paths.push_back(Util::NormalizePath(normalPath));
			}
		}
	}

	return paths;
}

std::optional<std::string> MeshShaderRuleManager::GetTextureSetEditorId(RE::BSShaderProperty* property)
{
	if (!property || !property->material) {
		return std::nullopt;
	}

	if (auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(property->material)) {
		auto textureSet = lightingMaterial->GetTextureSet();
		if (textureSet) {
			// BSTextureSet is a TESForm in some cases, try to get editor ID
			if (auto* form = textureSet.get()) {
				if (auto* tesForm = skyrim_cast<RE::TESForm*>(form)) {
					const char* editorId = tesForm->GetFormEditorID();
					if (editorId && editorId[0] != '\0') {
						return std::string(editorId);
					}
				}
			}
		}
	}

	return std::nullopt;
}

void MeshShaderRuleManager::InvalidateCache()
{
	std::unique_lock lock(cacheMutex);
	resolutionCache.clear();
	cacheStats.entries = 0;
}

MeshShaderRuleManager::CacheStats MeshShaderRuleManager::GetCacheStats() const
{
	std::shared_lock lock(cacheMutex);
	return cacheStats;
}
