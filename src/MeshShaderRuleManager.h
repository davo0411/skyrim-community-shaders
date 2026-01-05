#pragma once

#include "Utils/GlobMatcher.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

/**
 * @brief Manages shader rules for mesh-specific shader targeting
 *
 * This system allows features to apply specific shader profiles to meshes
 * based on various matching criteria:
 * - NiExtraData attached to geometry (highest priority)
 * - Editor ID patterns
 * - Texture path patterns
 * - Model path patterns
 * - Node name patterns
 *
 * Rules are loaded from JSON files in Data\ShaderRules\ and can be
 * provided by the base mod or by other mods for compatibility.
 */
class MeshShaderRuleManager
{
public:
	static constexpr std::string_view ExtraDataName = "CS_ShaderProfile";
	static constexpr std::string_view RulesDirectory = "Data\\ShaderRules";

	/**
	 * @brief Criteria for matching geometry to a rule
	 */
	struct MatchCriteria
	{
		std::vector<std::string> editorIdPatterns;     // Match form editor ID
		std::vector<std::string> modelPathPatterns;    // Match .nif file path
		std::vector<std::string> texturePathPatterns;  // Match diffuse texture path
		std::vector<std::string> nodeNamePatterns;     // Match NIF node name
		std::vector<std::string> textureSetPatterns;   // Match texture set editor ID

		// Shader property flags
		uint64_t requiredShaderFlags = 0;
		uint64_t forbiddenShaderFlags = 0;

		// Pre-compiled matchers (populated during rule loading)
		Util::GlobMatcherSet editorIdMatchers;
		Util::GlobMatcherSet modelPathMatchers;
		Util::GlobMatcherSet texturePathMatchers;
		Util::GlobMatcherSet nodeNameMatchers;
		Util::GlobMatcherSet textureSetMatchers;

		/**
		 * @brief Compile pattern strings into matcher objects
		 */
		void CompilePatterns();

		/**
		 * @brief Check if any criteria are defined
		 */
		bool HasAnyCriteria() const;
	};

	/**
	 * @brief A shader rule definition
	 */
	struct Rule
	{
		std::string name;         // Human-readable name for debugging
		std::string featureName;  // Which feature this rule applies to
		std::string profileName;  // The profile to apply (feature-specific meaning)
		std::string sourceFile;   // File this rule was loaded from

		MatchCriteria match;
		int priority = 0;  // Higher priority rules are checked first

		json settings;  // Feature-specific settings (optional)

		bool enabled = true;
	};

	/**
	 * @brief Result of resolving a rule for geometry
	 */
	struct ResolvedRule
	{
		std::string profileName;
		std::string ruleName;
		json settings;
		bool matched = false;
		int priority = 0;
	};

	/**
	 * @brief Cached geometry information to avoid repeated lookups
	 */
	struct GeometryInfo
	{
		std::string nodeName;
		std::string editorId;
		std::string modelPath;
		std::vector<std::string> texturePaths;
		std::string textureSetEditorId;
		uint64_t shaderFlags = 0;
		bool valid = false;
	};

	static MeshShaderRuleManager* GetSingleton()
	{
		static MeshShaderRuleManager instance;
		return &instance;
	}

	// ========== Rule Management ==========

	/**
	 * @brief Discover and load all rule files from Data\ShaderRules\
	 * @return Number of rules loaded
	 */
	size_t DiscoverRules();

	/**
	 * @brief Load rules from a specific JSON file
	 * @param path Path to the JSON file
	 * @return Number of rules loaded from the file
	 */
	size_t LoadRulesFromFile(const std::filesystem::path& path);

	/**
	 * @brief Register a rule programmatically
	 * @param rule The rule to register
	 */
	void RegisterRule(const Rule& rule);

	/**
	 * @brief Clear all loaded rules
	 */
	void ClearRules();

	/**
	 * @brief Get all rules for a specific feature
	 * @param featureName The feature name
	 * @return Vector of pointers to matching rules
	 */
	std::vector<const Rule*> GetRulesForFeature(const std::string& featureName) const;

	// ========== Rule Resolution ==========

	/**
	 * @brief Resolve which rule applies to a geometry for a given feature
	 *
	 * Resolution order (first match wins within priority):
	 * 1. NiExtraData on geometry (CS_ShaderProfile)
	 * 2. Pattern matching against rule criteria
	 *
	 * @param featureName The feature requesting resolution
	 * @param geometry The geometry being rendered
	 * @param property The shader property (optional, for texture info)
	 * @return Resolved rule information
	 */
	ResolvedRule ResolveForGeometry(
		const std::string& featureName,
		RE::BSGeometry* geometry,
		RE::BSShaderProperty* property = nullptr);

	/**
	 * @brief Check if geometry has explicit shader profile extra data
	 * @param geometry The geometry to check
	 * @param featureName The feature to check for
	 * @return Profile name if found, empty optional otherwise
	 */
	std::optional<std::string> GetExtraDataProfile(
		RE::BSGeometry* geometry,
		const std::string& featureName);

	// ========== Cache Management ==========

	/**
	 * @brief Invalidate all cached geometry lookups
	 * Call this on cell change or when rules are reloaded
	 */
	void InvalidateCache();

	/**
	 * @brief Get cache statistics for debugging
	 */
	struct CacheStats
	{
		size_t hits = 0;
		size_t misses = 0;
		size_t entries = 0;
	};
	CacheStats GetCacheStats() const;

	// ========== Geometry Information Extraction ==========

	/**
	 * @brief Extract all relevant information from geometry for matching
	 * @param geometry The geometry to analyze
	 * @param property The shader property (optional)
	 * @return Extracted geometry information
	 */
	GeometryInfo ExtractGeometryInfo(
		RE::BSGeometry* geometry,
		RE::BSShaderProperty* property = nullptr);

	// ========== Debugging ==========

	/**
	 * @brief Enable/disable debug logging for rule resolution
	 */
	void SetDebugLogging(bool enabled) { debugLogging = enabled; }

	/**
	 * @brief Get total number of loaded rules
	 */
	size_t GetRuleCount() const { return rules.size(); }

private:
	MeshShaderRuleManager() = default;
	~MeshShaderRuleManager() = default;
	MeshShaderRuleManager(const MeshShaderRuleManager&) = delete;
	MeshShaderRuleManager& operator=(const MeshShaderRuleManager&) = delete;

	// ========== Internal Methods ==========

	/**
	 * @brief Parse a single rule from JSON
	 */
	std::optional<Rule> ParseRule(const json& ruleJson, const std::string& featureName, const std::string& sourceFile);

	/**
	 * @brief Check if geometry matches a rule's criteria
	 */
	bool MatchesCriteria(const GeometryInfo& info, const MatchCriteria& criteria);

	/**
	 * @brief Get editor ID for a geometry's base object
	 */
	std::optional<std::string> GetEditorIdForGeometry(RE::BSGeometry* geometry);

	/**
	 * @brief Get model path for a geometry's base object
	 */
	std::optional<std::string> GetModelPathForGeometry(RE::BSGeometry* geometry);

	/**
	 * @brief Get texture paths from a shader property
	 */
	std::vector<std::string> GetTexturePathsForProperty(RE::BSShaderProperty* property);

	/**
	 * @brief Get texture set editor ID from a shader property
	 */
	std::optional<std::string> GetTextureSetEditorId(RE::BSShaderProperty* property);

	// ========== Data ==========

	mutable std::shared_mutex rulesMutex;
	std::vector<Rule> rules;

	// Feature name -> indices into rules vector (sorted by priority)
	std::unordered_map<std::string, std::vector<size_t>> featureRuleMap;

	// Cache for geometry resolution
	// Key: feature name + geometry pointer
	struct CacheKey
	{
		std::string featureName;
		RE::BSGeometry* geometry;

		bool operator==(const CacheKey& other) const
		{
			return featureName == other.featureName && geometry == other.geometry;
		}
	};

	struct CacheKeyHash
	{
		size_t operator()(const CacheKey& key) const
		{
			size_t h1 = std::hash<std::string>{}(key.featureName);
			size_t h2 = std::hash<void*>{}(key.geometry);
			return h1 ^ (h2 << 1);
		}
	};

	mutable std::shared_mutex cacheMutex;
	mutable std::unordered_map<CacheKey, ResolvedRule, CacheKeyHash> resolutionCache;
	mutable CacheStats cacheStats;

	bool debugLogging = false;
	bool rulesLoaded = false;
};

// JSON serialization support
void from_json(const json& j, MeshShaderRuleManager::MatchCriteria& criteria);
void from_json(const json& j, MeshShaderRuleManager::Rule& rule);
