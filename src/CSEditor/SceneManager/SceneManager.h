#pragma once

#include "Feature.h"
#include "SceneSettingsManager.h"

/**
 * @brief Core feature that hosts the SceneSettingsManager and forwards its lifecycle hooks.
 *
 * It has no page of its own. Like the CS Editor it stays out of the feature list; its controls live in the CS Editor
 * (Scene Manager page, Scene Debug window) and the Presets page.
 */
struct SceneManager : Feature, SceneSettingsManager
{
	std::string GetName() override { return "Scene Manager"; }
	std::string GetDisplayName() override { return T("feature.scene_manager.name", "Scene Manager"); }
	std::string GetShortName() override { return "SceneManager"; }
	std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	bool IsCore() const override { return true; }
	bool IsAlwaysEnabled() const override { return true; }
	bool IsInMenu() const override { return false; }
	bool UsesMainSettings() const override { return false; }

	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	void SetupResources() override;
	void DataLoaded() override;
	/** @brief Scene resolution, forwarded to SceneSettingsManager::Update; repeat calls within a frame are no-ops. */
	void Update();
};
