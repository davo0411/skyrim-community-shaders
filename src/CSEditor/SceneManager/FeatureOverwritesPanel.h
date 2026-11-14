#pragma once

#include <string>

struct Feature;

/// Baseline preset files: the per-pack file table on the Presets page, and the export that writes feature settings into a pack.
namespace FeatureOverwritesPanel
{
	/**
	 * @brief Draws the Baseline files a pack has in use, with whether each applies and a button to edit it.
	 * @param packId The pack whose files to list.
	 * @return False, drawing nothing, when the pack has no files in use.
	 */
	bool DrawPackFiles(const std::string& packId);

	/** @brief Opens the export popup on the next DrawExport; a non-null feature is preselected with every setting ticked and the feature list hidden. */
	void BeginExport(Feature* feature = nullptr);

	/**
	 * @brief Opens the export popup on an existing Baseline file, its settings ticked, so it can be edited.
	 * @param packId The pack the file belongs to.
	 */
	void BeginExportInto(Feature* feature, const std::string& packId);

	/** @brief Draws the export popup while open; only the caller that opened it draws it. */
	void DrawExport();

	/** @brief Icon button that opens the export on this feature, told apart from the Baseline preset export by glyph and tooltip. */
	void DrawExportButton(Feature* feature, const char* id);

	/** @brief Whether the feature saves any main settings that an overwrite could carry; cached per feature. */
	bool HasExportableSettings(Feature* feature);
}
