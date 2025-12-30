#include "OverwritesTabRenderer.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>

#include "Feature.h"
#include "FeatureIssues.h"
#include "Globals.h"
#include "Menu.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Util.h"

OverwritesTabRenderer::OverwriteUIState OverwritesTabRenderer::uiState;

void OverwritesTabRenderer::RenderOverwritesTab()
{
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	if (!overrideManager) {
		ImGui::TextDisabled("Override system not available.");
		return;
	}

	// Ensure overrides are discovered
	if (overrideManager->GetOverrides().empty()) {
		overrideManager->DiscoverOverrides();
	}

	ImGui::BeginChild("##OverwritesContent", ImVec2(0, 0), false);
	{
		// Header text explaining the system
		ImGui::TextWrapped(
			"Manage feature setting overrides from mod preset files. "
			"Overrides allow mod authors to ship recommended settings that you can apply or ignore.");
		ImGui::Spacing();

		// Bulk actions section
		RenderBulkActions();

		ImGui::Spacing();
		Util::SeparatorTextWithFont("Feature Overrides", Menu::FontRole::Subheading);
		ImGui::Spacing();

		// Feature list
		RenderFeatureList();

		// Modals
		RenderExportModal();
		RenderConflictModal();
	}
	ImGui::EndChild();
}

void OverwritesTabRenderer::RenderBulkActions()
{
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	auto statuses = overrideManager->GetAllFeatureOverrideStatuses();

	// Count statuses for display
	int activeCount = 0;
	int availableCount = 0;
	int brokenCount = 0;

	for (const auto& status : statuses) {
		switch (status.status) {
		case SettingsOverrideManager::OverrideStatus::Active:
			activeCount++;
			break;
		case SettingsOverrideManager::OverrideStatus::Available:
		case SettingsOverrideManager::OverrideStatus::Outdated:
			availableCount++;
			break;
		case SettingsOverrideManager::OverrideStatus::Broken:
			brokenCount++;
			break;
		default:
			break;
		}
	}

	// Status summary
	auto& theme = globals::menu->GetTheme();
	
	ImGui::Text("Status: ");
	ImGui::SameLine();
	ImGui::TextColored(theme.StatusPalette.SuccessColor, "%d Active", activeCount);
	ImGui::SameLine();
	ImGui::Text(" | ");
	ImGui::SameLine();
	ImGui::TextColored(theme.StatusPalette.InfoColor, "%d Available", availableCount);
	if (brokenCount > 0) {
		ImGui::SameLine();
		ImGui::Text(" | ");
		ImGui::SameLine();
		ImGui::TextColored(theme.StatusPalette.Error, "%d Broken", brokenCount);
	}

	ImGui::Spacing();

	// Bulk action buttons
	float buttonWidth = 140.0f;
	
	if (ImGui::Button("Apply All", ImVec2(buttonWidth, 0))) {
		// Apply all available overrides
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded && overrideManager->HasFeatureOverrides(feature->GetShortName())) {
				feature->ReapplyOverrideSettings();
			}
		}
		globals::state->Save(State::ConfigMode::USER);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Apply all available overrides to their respective features.");
	}

	ImGui::SameLine();
	
	if (ImGui::Button("Break All", ImVec2(buttonWidth, 0))) {
		overrideManager->BreakAllOverrides();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Stop tracking all overrides. Your current settings will be preserved "
			"and won't be overwritten on next load.");
	}

	ImGui::SameLine();

	if (ImGui::Button("Export Modified", ImVec2(buttonWidth, 0))) {
		uiState.showExportModal = true;
		uiState.selectedFeature.clear();  // Export all
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Export all features with modified settings as override files.");
	}

	ImGui::SameLine();

	if (ImGui::Button("Refresh", ImVec2(80.0f, 0))) {
		overrideManager->RefreshOverrides();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Rescan the Overrides folder for new or changed override files.");
	}
}

void OverwritesTabRenderer::RenderFeatureList()
{
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	auto& featureList = Feature::GetFeatureList();

	// Create sorted list
	std::vector<Feature*> sortedFeatures(featureList.begin(), featureList.end());
	std::sort(sortedFeatures.begin(), sortedFeatures.end(),
		[](Feature* a, Feature* b) { return a->GetName() < b->GetName(); });

	// Table for feature list
	ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
	                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;

	float availableHeight = ImGui::GetContentRegionAvail().y - 20.0f;
	if (ImGui::BeginTable("##OverridesTable", 4, tableFlags, ImVec2(0, availableHeight))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Override Source", ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
		ImGui::TableHeadersRow();

		for (Feature* feature : sortedFeatures) {
			if (!feature->IsInMenu())
				continue;

			const std::string& featureName = feature->GetShortName();
			auto status = overrideManager->GetFeatureOverrideStatus(featureName);

			ImGui::TableNextRow();

			// Status column
			ImGui::TableNextColumn();
			ImGui::TextColored(
				SettingsOverrideManager::GetStatusColor(status.status), "%s",
				SettingsOverrideManager::GetStatusIcon(status.status));
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", SettingsOverrideManager::GetStatusText(status.status));
			}

			// Feature name column
			ImGui::TableNextColumn();
			if (!feature->loaded) {
				ImGui::TextDisabled("%s", feature->GetName().c_str());
			} else {
				ImGui::Text("%s", feature->GetName().c_str());
			}

			// Override source column
			ImGui::TableNextColumn();
			if (status.status != SettingsOverrideManager::OverrideStatus::None) {
				ImGui::Text("%s", status.modName.c_str());
				if (!status.description.empty()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextWrapped("%s", status.description.c_str());
					}
				}
			} else {
				ImGui::TextDisabled("-");
			}

			// Actions column
			ImGui::TableNextColumn();
			RenderFeatureActions(featureName);
		}

		ImGui::EndTable();
	}
}

void OverwritesTabRenderer::RenderFeatureActions(const std::string& featureName)
{
	auto overrideManager = SettingsOverrideManager::GetSingleton();
	auto status = overrideManager->GetFeatureOverrideStatus(featureName);

	ImGui::PushID(featureName.c_str());

	float buttonWidth = 60.0f;
	bool hasOverride = status.status != SettingsOverrideManager::OverrideStatus::None;
	bool isActive = status.status == SettingsOverrideManager::OverrideStatus::Active;

	Feature* feature = FeatureIssues::FindFeature(featureName);
	bool isLoaded = feature && feature->loaded;

	// Apply button
	ImGui::BeginDisabled(!hasOverride || !isLoaded);
	if (ImGui::Button("Apply", ImVec2(buttonWidth, 0))) {
		if (feature && feature->ReapplyOverrideSettings()) {
			globals::state->Save(State::ConfigMode::USER);
		}
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (!hasOverride) {
			ImGui::Text("No override available for this feature.");
		} else if (!isLoaded) {
			ImGui::Text("Feature must be loaded to apply override.");
		} else {
			ImGui::Text("Apply override settings to this feature.");
		}
	}

	ImGui::SameLine();

	// Break button
	ImGui::BeginDisabled(!isActive);
	if (ImGui::Button("Break", ImVec2(buttonWidth, 0))) {
		overrideManager->BreakOverride(featureName);
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (!isActive) {
			ImGui::Text("No active override to break.");
		} else {
			ImGui::Text(
				"Stop tracking this override. Current settings will be preserved "
				"and won't be overwritten on next load.");
		}
	}

	ImGui::SameLine();

	// Export button
	ImGui::BeginDisabled(!isLoaded);
	if (ImGui::Button("Export", ImVec2(buttonWidth, 0))) {
		uiState.selectedFeature = featureName;
		uiState.showExportModal = true;
	}
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		if (!isLoaded) {
			ImGui::Text("Feature must be loaded to export settings.");
		} else {
			ImGui::Text("Export current settings as a new override file.");
		}
	}

	ImGui::PopID();
}

void OverwritesTabRenderer::RenderExportModal()
{
	if (!uiState.showExportModal) {
		return;
	}

	ImGui::SetNextWindowSize(ImVec2(400, 200), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Export Override", &uiState.showExportModal, ImGuiWindowFlags_AlwaysAutoResize)) {
		bool exportAll = uiState.selectedFeature.empty();
		
		if (exportAll) {
			ImGui::Text("Export all modified feature settings as override files.");
		} else {
			ImGui::Text("Export settings for: %s", uiState.selectedFeature.c_str());
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		static char modNameBuf[128] = "User";
		static char descriptionBuf[256] = "";

		ImGui::InputText("Preset Name", modNameBuf, sizeof(modNameBuf));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Name for the override file (e.g., 'MyPreset' creates 'MyPreset_Feature.json')");
		}

		ImGui::InputTextMultiline("Description", descriptionBuf, sizeof(descriptionBuf), ImVec2(0, 60));

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button("Export", ImVec2(100, 0))) {
			auto overrideManager = SettingsOverrideManager::GetSingleton();

			if (exportAll) {
				int exported = 0;
				for (auto* feature : Feature::GetFeatureList()) {
					if (feature->loaded) {
						json settings;
						feature->SaveSettings(settings);
						if (overrideManager->ExportFeatureOverride(
								feature->GetShortName(), settings, modNameBuf, descriptionBuf)) {
							exported++;
						}
					}
				}
			} else if (Feature* feature = FeatureIssues::FindFeature(uiState.selectedFeature)) {
				if (feature->loaded) {
					json settings;
					feature->SaveSettings(settings);
					overrideManager->ExportFeatureOverride(
						uiState.selectedFeature, settings, modNameBuf, descriptionBuf);
				}
			}

			uiState.showExportModal = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			uiState.showExportModal = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		// Open the popup if it should be shown but isn't open yet
		ImGui::OpenPopup("Export Override");
	}
}

void OverwritesTabRenderer::RenderConflictModal()
{
	if (!uiState.showConflictModal)
		return;

	ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal("Resolve Override Conflict", &uiState.showConflictModal)) {
		auto overrideManager = SettingsOverrideManager::GetSingleton();
		Feature* feature = FeatureIssues::FindFeature(uiState.conflictFeature);

		ImGui::Text("Override values differ from current settings for: %s", uiState.conflictFeature.c_str());
		ImGui::Spacing();

		if (feature && feature->loaded) {
			json currentSettings;
			feature->SaveSettings(currentSettings);
			auto diffs = overrideManager->GetSettingsDiff(uiState.conflictFeature, currentSettings);

			if (!diffs.empty()) {
				if (ImGui::BeginTable("##DiffTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
					ImGui::TableSetupColumn("Setting");
					ImGui::TableSetupColumn("Current");
					ImGui::TableSetupColumn("Override");
					ImGui::TableHeadersRow();

					for (const auto& [path, current, override] : diffs) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::Text("%s", path.c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%s", current.c_str());
						ImGui::TableNextColumn();
						ImGui::Text("%s", override.c_str());
					}

					ImGui::EndTable();
				}
			} else {
				ImGui::Text("No differences found.");
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		if (ImGui::Button("Apply Override", ImVec2(120, 0))) {
			if (feature) {
				feature->ReapplyOverrideSettings();
				globals::state->Save(State::ConfigMode::USER);
			}
			uiState.showConflictModal = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Keep Current", ImVec2(120, 0))) {
			overrideManager->BreakOverride(uiState.conflictFeature);
			uiState.showConflictModal = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Cancel", ImVec2(100, 0))) {
			uiState.showConflictModal = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	} else {
		ImGui::OpenPopup("Resolve Override Conflict");
	}
}
