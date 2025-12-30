#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * @class OverwritesTabRenderer
 * @brief Renders the Overwrites management tab in the General settings
 *
 * This component provides a user-friendly interface for managing feature
 * setting overrides. Users can:
 * - View all features and their override status
 * - Apply or break individual overrides
 * - Export current settings as new override files
 * - Perform bulk operations on all overrides
 *
 * @section override_ui Override Status Indicators
 * - Green checkmark: Override is active and applied
 * - Yellow warning: Override file changed, reapply available
 * - Red X: Override file failed to load (broken)
 * - Grey dash: No override available for this feature
 *
 * @see SettingsOverrideManager for override business logic
 * @see SettingsTabRenderer for integration point
 */
class OverwritesTabRenderer
{
public:
	/**
	 * @struct OverwriteUIState
	 * @brief Tracks UI state for the overwrites tab
	 */
	struct OverwriteUIState
	{
		std::string selectedFeature;           ///< Currently selected feature name
		std::string exportModName = "User";    ///< Mod name for export operations
		std::string exportDescription;         ///< Description for export operations
		bool showExportModal = false;          ///< Whether export modal is open
		bool showConflictModal = false;        ///< Whether conflict resolution modal is open
		std::string conflictFeature;           ///< Feature with conflict being resolved
	};

	/**
	 * @brief Renders the complete Overwrites tab content
	 * 
	 * Main entry point for the overwrites management UI. Renders the feature
	 * list with status indicators and action buttons.
	 */
	static void RenderOverwritesTab();

private:
	/**
	 * @brief Renders the header section with bulk action buttons
	 */
	static void RenderBulkActions();

	/**
	 * @brief Renders the feature list with override status
	 */
	static void RenderFeatureList();

	/**
	 * @brief Renders action buttons for a single feature
	 * @param featureName The short name of the feature
	 */
	static void RenderFeatureActions(const std::string& featureName);

	/**
	 * @brief Renders the export settings modal dialog
	 */
	static void RenderExportModal();

	/**
	 * @brief Renders the conflict resolution modal dialog
	 */
	static void RenderConflictModal();

	/// Persistent UI state across frames
	static OverwriteUIState uiState;
};
