#pragma once

#include <ctime>
#include <filesystem>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

/**
 * @class SettingsOverrideManager
 * @brief Manages layered JSON override system for Community Shaders features
 *
 * This singleton class handles discovery, application, and management of feature
 * setting overrides from external mod files. It provides a non-destructive way
 * for mod authors to ship preset configurations that users can choose to apply.
 *
 * @section override_files Override File Format
 * Override files follow the naming convention:
 * - Feature-specific: `{ModName}_{FeatureShortName}.json`
 * - Global: `{ModName}_Global.json`
 *
 * @section override_states Override States
 * Each feature can be in one of these override states:
 * - **None**: No override file exists for this feature
 * - **Available**: Override file exists but hasn't been applied yet
 * - **Active**: Override is currently applied to settings
 * - **Broken**: Override file exists but failed to load/parse
 * - **Outdated**: Override was applied but the file has changed since
 *
 * @section user_workflow User Workflow
 * Users can:
 * 1. View all available overrides in the Overwrites tab
 * 2. Apply overrides to import preset settings
 * 3. Break overrides to stop tracking (keeps current settings)
 * 4. Export their current settings as new override files
 *
 * @see Feature for feature registration
 * @see State::Load for settings loading flow
 */
class SettingsOverrideManager
{
public:
	/**
	 * @enum OverrideStatus
	 * @brief Represents the current state of an override for a feature
	 */
	enum class OverrideStatus
	{
		None,       ///< No override file exists for this feature
		Available,  ///< Override file exists but hasn't been applied
		Active,     ///< Override is currently applied and tracked
		Broken,     ///< Override file exists but failed to load/parse
		Outdated    ///< Override file changed since last application
	};

	/**
	 * @struct OverrideInfo
	 * @brief Contains metadata and data for a single override file
	 */
	struct OverrideInfo
	{
		std::string modName;       ///< Name of the mod providing this override
		std::string featureName;   ///< Target feature short name (empty for global)
		std::string filePath;      ///< Full path to the override file
		json overrideData;         ///< The actual override values
		bool isGlobal = false;     ///< True if this affects multiple features

		// Metadata from override file
		std::string version;       ///< Version string from override metadata
		std::string description;   ///< Description from override metadata
		bool enabled = true;       ///< Whether this override is enabled

		// Tracking for application state
		std::string fileHash;          ///< Hash of file content for change detection
		std::time_t firstApplied = 0;  ///< Timestamp when first applied
		bool loadFailed = false;       ///< True if loading/parsing failed
		std::string errorMessage;      ///< Error message if loadFailed is true
	};

	/**
	 * @struct FeatureOverrideStatus
	 * @brief Summary of override state for a single feature
	 */
	struct FeatureOverrideStatus
	{
		std::string featureName;       ///< Feature short name
		OverrideStatus status;         ///< Current override status
		std::string modName;           ///< Mod name if override exists
		std::string description;       ///< Override description
		bool hasUserModifications;     ///< True if user changed settings after override
		std::time_t lastApplied;       ///< When override was last applied
	};

	static SettingsOverrideManager* GetSingleton()
	{
		static SettingsOverrideManager instance;
		return &instance;
	}

	/**
	 * @brief Discovers all override files in the overrides directory
	 * @return Number of override files discovered
	 */
	size_t DiscoverOverrides();

	/**
	 * @brief Applies overrides to settings JSON, respecting applied override tracking
	 * @param baseSettings The default settings to start with
	 * @param appliedOverrides Reference to tracking data for applied overrides
	 * @return Modified settings with overrides applied (only new/changed overrides)
	 */
	size_t ApplyNewOverrides(json& baseSettings, json& appliedOverrides);

	/**
	 * @brief Applies overrides to a specific feature's settings JSON
	 * @param featureName The short name of the feature
	 * @param featureJson The feature's JSON settings to modify
	 * @return Number of overrides applied
	 */
	size_t ApplyOverrides(const std::string& featureName, json& featureJson);

	/**
	 * @brief Applies feature-specific overrides respecting applied override tracking
	 * @param featureName The short name of the feature
	 * @param featureJson The feature's JSON settings to modify
	 * @param appliedOverrides Reference to tracking data for applied overrides
	 * @return Number of new overrides applied
	 */
	size_t ApplyNewFeatureOverrides(const std::string& featureName, json& featureJson, json& appliedOverrides);

	/**
	 * @brief Applies global overrides to the main settings JSON
	 * @param mainJson The main settings JSON to modify
	 * @return Number of global overrides applied
	 */
	size_t ApplyGlobalOverrides(json& mainJson);

	/**
	 * @brief Gets list of all discovered overrides
	 * @return Vector of override information
	 */
	const std::vector<OverrideInfo>& GetOverrides() const { return overrides; }

	/**
	 * @brief Gets overrides for a specific feature
	 * @param featureName The short name of the feature
	 * @return Vector of override information for the feature
	 */
	std::vector<const OverrideInfo*> GetFeatureOverrides(const std::string& featureName) const;

	/**
	 * @brief Checks if there are any overrides available for a specific feature
	 * @param featureName The short name of the feature
	 * @return True if the feature has overrides available
	 */
	bool HasFeatureOverrides(const std::string& featureName) const;

	/**
	 * @brief Manually reapplies all overrides for a specific feature to the provided JSON
	 * @param featureName The short name of the feature
	 * @param featureJson JSON object to apply overrides to
	 * @return Number of overrides applied
	 */
	size_t ReapplyFeatureOverrides(const std::string& featureName, json& featureJson);

	/**
	 * @brief Enables or disables a specific override
	 * @param modName Name of the mod
	 * @param featureName Feature name (empty for global)
	 * @param isEnabled Whether to enable the override
	 */
	void SetOverrideEnabled(const std::string& modName, const std::string& featureName, bool isEnabled);

	/**
	 * @brief Clears all cached overrides and forces rediscovery
	 */
	void RefreshOverrides();

	/**
	 * @brief Gets the overrides directory path
	 */
	std::filesystem::path GetOverridesDirectory() const;

	/**
	 * @brief Loads applied overrides tracking data
	 * @return JSON object containing tracking data for previously applied overrides
	 */
	json LoadAppliedOverridesTracking() const;

	/**
	 * @brief Saves applied overrides tracking data
	 * @param appliedOverrides JSON object containing tracking data
	 */
	void SaveAppliedOverridesTracking(const json& appliedOverrides) const;

	/**
	 * @brief Gets the path to the applied overrides tracking file
	 */
	std::filesystem::path GetAppliedOverridesTrackingPath() const;

	/**
	 * @brief Checks if override system is enabled
	 */
	bool IsEnabled() const { return enabled; }

	/**
	 * @brief Enables or disables the entire override system
	 */
	void SetEnabled(bool enable) { enabled = enable; }

	/**
	 * @brief Reports an override failure to the Feature Issues system
	 * @param modName Name of the mod
	 * @param featureName Feature name (empty for global overrides)
	 * @param errorMessage Description of the failure
	 */
	void ReportOverrideFailure(const std::string& modName, const std::string& featureName, const std::string& errorMessage);

	// =====================================================================
	// New Override Management API
	// =====================================================================

	/**
	 * @brief Gets the override status for a specific feature
	 * @param featureName The short name of the feature
	 * @return FeatureOverrideStatus containing current state information
	 */
	FeatureOverrideStatus GetFeatureOverrideStatus(const std::string& featureName) const;

	/**
	 * @brief Gets override status for all known features
	 * @return Vector of FeatureOverrideStatus for all features with overrides
	 */
	std::vector<FeatureOverrideStatus> GetAllFeatureOverrideStatuses() const;

	/**
	 * @brief Breaks (removes) override tracking for a feature
	 * 
	 * This removes the override from the applied tracking, allowing user
	 * modifications to persist without being overwritten on next load.
	 * The override file itself is not deleted.
	 * 
	 * @param featureName The short name of the feature
	 * @return True if tracking was removed successfully
	 */
	bool BreakOverride(const std::string& featureName);

	/**
	 * @brief Breaks override tracking for all features
	 * @return Number of overrides broken
	 */
	size_t BreakAllOverrides();

	/**
	 * @brief Forces reapplication of an override for a feature
	 * 
	 * This reloads the override file and applies it to the feature's
	 * current settings, updating the tracking data.
	 * 
	 * @param featureName The short name of the feature
	 * @param featureJson The feature's JSON settings to modify
	 * @return True if override was applied successfully
	 */
	bool ForceApplyOverride(const std::string& featureName, json& featureJson);

	/**
	 * @brief Forces reapplication of all available overrides
	 * @return Number of overrides successfully applied
	 */
	size_t ForceApplyAllOverrides();

	/**
	 * @brief Exports feature settings as a new override file
	 * 
	 * Creates a new override JSON file from the provided settings.
	 * 
	 * @param featureName The short name of the feature
	 * @param featureJson The feature's current JSON settings
	 * @param modName Name to use for the override file (defaults to "User")
	 * @param description Optional description for the override
	 * @return True if export was successful
	 */
	bool ExportFeatureOverride(const std::string& featureName, const json& featureJson, 
		const std::string& modName = "User", const std::string& description = "");

	/**
	 * @brief Exports all modified feature settings as override files
	 * @param modName Name to use for the override files
	 * @return Number of override files created
	 */
	size_t ExportAllModifiedSettings(const std::string& modName = "User");

	/**
	 * @brief Checks if a feature has been modified from its override values
	 * @param featureName The short name of the feature
	 * @param currentSettings The feature's current JSON settings
	 * @return True if settings differ from the applied override
	 */
	bool HasUserModifications(const std::string& featureName, const json& currentSettings) const;

	/**
	 * @brief Gets the diff between current settings and override values
	 * @param featureName The short name of the feature
	 * @param currentSettings The feature's current JSON settings
	 * @return Vector of {path, currentValue, overrideValue} tuples
	 */
	std::vector<std::tuple<std::string, std::string, std::string>> GetSettingsDiff(
		const std::string& featureName, const json& currentSettings) const;

	/**
	 * @brief Gets list of all feature names that have override files
	 * @return Set of feature short names with available overrides
	 */
	std::unordered_set<std::string> GetFeaturesWithOverrides() const;

	/**
	 * @brief Marks an override as broken with an error message
	 * @param featureName The short name of the feature
	 * @param errorMessage Description of why the override is broken
	 */
	void MarkOverrideBroken(const std::string& featureName, const std::string& errorMessage);

	// =====================================================================
	// Status Display Helpers (for UI consistency)
	// =====================================================================

	/**
	 * @brief Gets a color for the override status (uses theme colors)
	 * @param status The override status
	 * @return ImVec4 color for the status
	 */
	static ImVec4 GetStatusColor(OverrideStatus status);

	/**
	 * @brief Gets a short icon/symbol for the override status
	 * @param status The override status
	 * @return Status icon string like "[OK]", "[+]", etc.
	 */
	static const char* GetStatusIcon(OverrideStatus status);

	/**
	 * @brief Gets descriptive text for the override status
	 * @param status The override status
	 * @return Human-readable status description
	 */
	static const char* GetStatusText(OverrideStatus status);

private:
	SettingsOverrideManager() = default;
	~SettingsOverrideManager() = default;
	SettingsOverrideManager(const SettingsOverrideManager&) = delete;
	SettingsOverrideManager& operator=(const SettingsOverrideManager&) = delete;

	/**
	 * @brief Loads a single override file
	 * @param filePath Path to the override file
	 * @return Override info if successful, nullptr otherwise
	 */
	std::unique_ptr<OverrideInfo> LoadOverrideFile(const std::filesystem::path& filePath);

	/**
	 * @brief Parses mod name and feature name from filename
	 * @param filename The override filename
	 * @return Pair of {modName, featureName} (featureName empty for global)
	 */
	std::pair<std::string, std::string> ParseOverrideFilename(const std::string& filename);

	/**
	 * @brief Validates override file format and content
	 * @param overrideJson The JSON to validate
	 * @param filePath Path to the file being validated (for error reporting)
	 * @return True if valid
	 */
	bool ValidateOverrideFormat(const json& overrideJson, const std::string& filePath = "");

	/**
	 * @brief Validates JSON data types and ranges for safety
	 * @param jsonData The JSON data to validate
	 * @param path Current path in the JSON (for error reporting)
	 * @param filePath Path to the file being validated (for error reporting)
	 * @return True if all data types are safe
	 */
	bool ValidateJsonDataTypes(const json& jsonData, const std::string& path = "", const std::string& filePath = "");

	/**
	 * @brief Sanitizes JSON data to prevent corruption
	 * @param jsonData The JSON data to sanitize
	 * @return Sanitized JSON data
	 */
	json SanitizeJsonData(const json& jsonData);

	/**
	 * @brief Recursively merges override JSON into target JSON
	 * @param target The target JSON to modify
	 * @param override The override JSON to apply
	 */
	void MergeJson(json& target, const json& override);

	/**
	 * @brief Computes JSON diff between two objects
	 * @param current Current JSON values
	 * @param original Original/override JSON values
	 * @param path Current JSON path for nested keys
	 * @param diffs Output vector for differences
	 */
	void ComputeJsonDiff(const json& current, const json& original, const std::string& path,
		std::vector<std::tuple<std::string, std::string, std::string>>& diffs) const;

	/**
	 * @brief Gets the tracking key for a feature override
	 * @param modName The mod name
	 * @param featureName The feature name
	 * @return Tracking key string
	 */
	std::string GetTrackingKey(const std::string& modName, const std::string& featureName) const;

	std::vector<OverrideInfo> overrides;
	std::unordered_map<std::string, std::vector<size_t>> featureOverrideMap;  // Maps feature name to override indices
	std::unordered_map<std::string, std::string> brokenOverrides;             // Maps feature name to error message
	mutable json cachedAppliedOverrides;                                      // Cached tracking data
	mutable bool trackingCacheDirty = true;                                   // Whether tracking cache needs reload
	bool enabled = true;
	bool discovered = false;

	static constexpr const char* GLOBAL_SUFFIX = "_Global.json";

	// Security limits for JSON validation
	static constexpr size_t MAX_JSON_DEPTH = 10;
	static constexpr size_t MAX_STRING_LENGTH = 1000;
	static constexpr size_t MAX_ARRAY_SIZE = 100;
	static constexpr size_t MAX_OBJECT_SIZE = 100;
	static constexpr double MAX_NUMERIC_VALUE = 1e6;
	static constexpr double MIN_NUMERIC_VALUE = -1e6;
};
