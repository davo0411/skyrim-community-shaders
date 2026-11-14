#include "FeatureOverwritesPanel.h"

#include "BaselineManager.h"
#include "CSEditor/Browser/BrowserWidgets.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "IconsTabler.h"
#include "SceneSettingsManager.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <tuple>
#include <unordered_map>

#define I18N_KEY_PREFIX "feature.scene_manager.overwrites."

namespace
{
	constexpr int kColumnCount = 4;
	constexpr int kActionButtonCount = 1;

	/** @brief One Baseline file as the table shows it, copied so actions that rediscover mid-frame are safe. */
	struct OverwriteRow
	{
		std::string feature;
		std::string featureName;
		std::string packId;
		std::string filePath;
		/// Newline-joined keys the feature does not have; while any exist the file is not applied.
		std::string unknownKeys;
		size_t settingCount = 0;
	};

	SceneSettingsManager::RevisionCache<std::vector<OverwriteRow>> rowCache;

	std::vector<OverwriteRow> BuildRows()
	{
		auto* manager = BaselineManager::GetSingleton();
		std::vector<OverwriteRow> rows;
		for (const auto& info : manager->GetFiles()) {
			if (!manager->IsApplicable(info))
				continue;
			auto* feature = Feature::FindFeatureByShortName(info.featureName);
			OverwriteRow row{ .feature = feature ? feature->GetDisplayName() : info.featureName,
				.featureName = info.featureName,
				.packId = info.packId,
				.filePath = info.filePath,
				.settingCount = Util::Settings::GetExportSettings(info.featureName, info.values).size() };
			auto unknownKeys = info.unknownKeys;
			if (feature && unknownKeys.empty()) {
				// Rediscovery clears the verdict until the next load applies the file, so recheck against the live shape.
				json shape;
				feature->SaveSettings(shape);
				Util::Settings::CollectUnknownSettingKeys(info.values, shape, "", unknownKeys);
			}
			for (const auto& key : unknownKeys)
				row.unknownKeys += (row.unknownKeys.empty() ? "" : "\n") + key;
			rows.push_back(std::move(row));
		}
		std::ranges::sort(rows, {}, [](const OverwriteRow& row) { return std::tie(row.feature, row.packId); });
		return rows;
	}

	void DrawStatus(const OverwriteRow& row)
	{
		if (!row.unknownKeys.empty()) {
			ImGui::TextColored(Util::Colors::GetWarning(), "%s", T(TKEY("status_not_applied"), "Not applied"));
			const auto tooltip = std::format("{}\n{}",
				T(TKEY("status_not_applied_tooltip"), "Names settings this feature does not have, so none of it is applied:"), row.unknownKeys);
			Util::AddTooltip(tooltip.c_str());
		} else {
			ImGui::TextColored(Util::Colors::GetSuccess(), "%s", T(TKEY("status_applied"), "Applied"));
		}
	}

	/** @brief Edit; the pack itself is managed above, in the Presets page's pack actions. */
	void DrawActions(const OverwriteRow& row)
	{
		auto* feature = Feature::FindFeatureByShortName(row.featureName);
		const Util::DisableGuard notEditable(!feature || !FeatureOverwritesPanel::HasExportableSettings(feature));
		if (Util::IconButton("##edit", Icons::TI(ICON_TI_PENCIL), T(TKEY("edit_tooltip"), "Edit: opens the export on this file with its settings ticked.")))
			FeatureOverwritesPanel::BeginExportInto(feature, row.packId);
	}

	void DrawRow(const OverwriteRow& row)
	{
		const float rowHeight = BrowserUI::RowHeight();
		ImGui::PushID(row.filePath.c_str());
		ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
		ImGui::TableNextColumn();
		BrowserUI::RowStripe(ImGui::GetColorU32(row.unknownKeys.empty() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning()), rowHeight);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(row.feature.c_str());
		Util::AddTooltip(row.filePath.c_str());

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%zu", row.settingCount);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		DrawStatus(row);

		ImGui::TableNextColumn();
		DrawActions(row);
		ImGui::PopID();
	}

	void DrawTable(const std::vector<OverwriteRow>& rows, const std::string& packId)
	{
		BrowserUI::RowShadeScope shade;
		if (!ImGui::BeginTable("##OverwriteFiles", kColumnCount, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp))
			return;
		const float actionsWidth = Util::IconButtonSize() * kActionButtonCount + ImGui::GetStyle().ItemSpacing.x * (kActionButtonCount - 1);
		ImGui::TableSetupColumn(T(TKEY("feature"), "Feature"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("settings"), "Settings"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("status"), "Status"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
		ImGui::TableHeadersRow();
		for (const auto& row : rows)
			if (row.packId == packId)
				DrawRow(row);
		ImGui::EndTable();
	}
}

bool FeatureOverwritesPanel::HasExportableSettings(Feature* feature)
{
	assert(feature);
	// The settings schema is fixed per feature, so one SaveSettings probe is enough.
	static std::unordered_map<Feature*, bool> cache;
	auto [it, inserted] = cache.try_emplace(feature, false);
	if (inserted && feature->UsesMainSettings()) {
		json settings;
		feature->SaveSettings(settings);
		it->second = settings.is_object() && !settings.empty();
	}
	return it->second;
}

bool FeatureOverwritesPanel::DrawPackFiles(const std::string& packId)
{
	const auto& rows = rowCache.Get(BaselineManager::GetSingleton()->GetRevision(), BuildRows);
	if (std::ranges::none_of(rows, [&packId](const OverwriteRow& row) { return row.packId == packId; }))
		return false;

	ImGui::SeparatorText(T(TKEY("pack_files"), "Baseline files"));
	DrawTable(rows, packId);
	return true;
}

#undef I18N_KEY_PREFIX
