#include "SceneDebugWindow.h"

#include <array>
#include <cfloat>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "I18n/I18n.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

// The window is developer-facing validation output. Its strings still go through T() like the rest of the editor.
namespace
{
	using DebugSnapshot = SceneSettingsManager::DebugSnapshot;
	using PeriodValues = std::array<std::optional<float>, SceneSettingsManager::kPeriodCount>;
	using PeriodWeights = std::array<float, SceneSettingsManager::kPeriodCount>;

	constexpr ImGuiTableFlags kDebugTableFlags =
		ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

	/// Weights at or below this do not contribute to a blend.
	constexpr float kWeightEpsilon = 1e-4f;

	constexpr const char* kMissingValue = "-";

	/// Window position at the 1080p baseline, offset from the Weather Debug window so the two do not stack.
	constexpr ImVec2 kWindowPosition{ 100.0f, 140.0f };
	/// Starting and smallest window sizes at the 1080p baseline; the window stays resizable.
	constexpr ImVec2 kWindowSize{ 640.0f, 560.0f };
	constexpr ImVec2 kWindowMinSize{ 320.0f, 200.0f };

	/// Starts a two-column label/value table for a debug section.
	bool BeginFieldTable(const char* id)
	{
		return ImGui::BeginTable(id, 2, kDebugTableFlags);
	}

	/// Starts a field row with its label, leaving the value column current.
	void BeginFieldRow(const char* label)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(label);
		ImGui::TableNextColumn();
	}

	void DrawField(const char* label, const std::string& value)
	{
		BeginFieldRow(label);
		ImGui::TextUnformatted(value.empty() ? kMissingValue : value.c_str());
	}

	void DrawFormIdField(const char* label, RE::FormID formId)
	{
		DrawField(label, std::format("{:08X}", formId));
	}

	void DrawFlag(const char* label, bool value)
	{
		BeginFieldRow(label);
		if (value)
			Util::Text::Success(T("cs_editor.debug.yes", "yes"));
		else
			Util::Text::Disabled(T("cs_editor.debug.no", "no"));
	}

	std::string FormatGameHour(float hour)
	{
		const auto wholeHours = static_cast<int>(hour);
		const auto minutes = static_cast<int>((hour - static_cast<float>(wholeHours)) * 60.0f);
		return std::format("{:.4f} ({:02d}:{:02d})", hour, wholeHours, minutes);
	}

	void DrawPeriodHeaders()
	{
		for (int period = 0; period < SceneSettingsManager::kPeriodCount; ++period)
			ImGui::TableSetupColumn(SceneSettingsManager::GetPeriodDisplayName(static_cast<SceneSettingsManager::TimeOfDayPeriod>(period)));
		ImGui::TableHeadersRow();
	}

	/// One row of per-period blend weights, greying out periods that contribute nothing.
	void DrawPeriodWeights(const char* id, const PeriodWeights& weights)
	{
		if (!ImGui::BeginTable(id, SceneSettingsManager::kPeriodCount, kDebugTableFlags))
			return;
		DrawPeriodHeaders();
		ImGui::TableNextRow();
		for (const auto weight : weights) {
			ImGui::TableNextColumn();
			if (weight > kWeightEpsilon)
				Util::Text::Success("%.3f", weight);
			else
				Util::Text::Disabled("%.3f", weight);
		}
		ImGui::EndTable();
	}

	/// One labelled row of per-period override values inside a 1 + period column table.
	void DrawPeriodValues(const char* label, const PeriodValues& values)
	{
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(label);
		for (const auto& value : values) {
			ImGui::TableNextColumn();
			if (value)
				ImGui::Text("%.4f", *value);
			else
				Util::Text::Disabled(kMissingValue);
		}
	}

	void DrawNameList(const std::vector<std::string>& names)
	{
		if (names.empty()) {
			Util::Text::Disabled(T("cs_editor.debug.none", "None"));
			return;
		}
		for (const auto& name : names)
			ImGui::BulletText("%s", SceneSettingsManager::GetFeatureDisplayName(name).c_str());
	}

	void DrawSceneContext(const DebugSnapshot& snapshot)
	{
		if (!BeginFieldTable("SceneContext"))
			return;
		DrawFlag(T("cs_editor.debug.player_and_cell_ready", "Player and cell ready"), snapshot.playerReady);
		DrawFlag(T("cs_editor.debug.interior", "Interior"), snapshot.interior);
		DrawFlag(T("cs_editor.debug.main_or_loading_menu_open", "Main or loading menu open"), snapshot.menuOpen);
		DrawField(T("cs_editor.debug.cell", "Cell"), snapshot.cellName);
		DrawField(T("cs_editor.debug.cell_editor_id", "Cell editor ID"), snapshot.cellEditorId);
		DrawFormIdField(T("cs_editor.debug.cell_form_id", "Cell form ID"), snapshot.cellId);
		DrawField(T("cs_editor.debug.location", "Location"), snapshot.locationName);
		DrawFormIdField(T("cs_editor.debug.location_form_id", "Location form ID"), snapshot.locationId);
		ImGui::EndTable();
	}

	void DrawTimeOfDay(const DebugSnapshot& snapshot)
	{
		if (BeginFieldTable("TimeOfDay")) {
			DrawField(T("cs_editor.debug.game_hour", "Game hour"), FormatGameHour(snapshot.gameHour));
			DrawField(T("cs_editor.debug.current_period", "Current period"), SceneSettingsManager::GetPeriodDisplayName(snapshot.period));
			DrawField(T("cs_editor.debug.hour_at_last_resolve", "Hour at last resolve"),
				snapshot.lastHour < 0.0f ? std::string(kMissingValue) : FormatGameHour(snapshot.lastHour));
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextUnformatted(T("cs_editor.debug.live_period_weights", "Live period weights"));
		DrawPeriodWeights("LiveWeights", snapshot.timeOfDayFactors);

		ImGui::Spacing();
		ImGui::TextUnformatted(T("cs_editor.debug.weights_used_by_the_last_resolve", "Weights used by the last resolve"));
		DrawPeriodWeights("ResolvedWeights", snapshot.blendFactors);
	}

	/// Load flags, what the last resolve saw, and the features that failed to apply or restore.
	void DrawResolverState(const DebugSnapshot& snapshot)
	{
		if (BeginFieldTable("ResolverState")) {
			DrawFlag(T("cs_editor.debug.scene_data_loaded", "Scene data loaded"), snapshot.dataLoaded);
			DrawFlag(T("cs_editor.debug.weather_data_loaded", "Weather data loaded"), snapshot.weatherDataLoaded);
			DrawFlag(T("cs_editor.debug.location_data_loaded", "Location data loaded"), snapshot.locationDataLoaded);
			DrawFlag(T("cs_editor.debug.game_data_ready", "Game data ready"), snapshot.gameDataReady);
			DrawFlag(T("cs_editor.debug.resolver_suspended", "Resolver suspended"), snapshot.resolverSuspended);
			DrawFlag(T("cs_editor.debug.resolver_dirty", "Resolver dirty"), snapshot.resolverDirty);
			DrawFlag(T("cs_editor.debug.active_entry_cache_dirty", "Active entry cache dirty"), snapshot.activeEntryCacheDirty);
			DrawFlag(T("cs_editor.debug.has_active_scene_entries", "Has active scene entries"), snapshot.hasActiveSceneEntries);
			DrawFlag(T("cs_editor.debug.deferred_save_pending", "Deferred save pending"), snapshot.deferredSceneChangesPending);
			DrawField(T("cs_editor.debug.scene_layer_suspend_depth", "Scene layer suspend depth"), std::format("{}", snapshot.sceneLayerSuspendDepth));
			DrawFlag(T("cs_editor.debug.interior_at_last_resolve", "Interior at last resolve"), snapshot.lastInterior);
			DrawFormIdField(T("cs_editor.debug.cell_at_last_resolve", "Cell at last resolve"), snapshot.lastCellId);
			DrawFormIdField(T("cs_editor.debug.location_at_last_resolve", "Location at last resolve"), snapshot.lastLocationId);
			DrawFormIdField(T("cs_editor.debug.current_at_last_resolve", "Current weather at last resolve"), snapshot.lastWeather.currentWeatherId);
			DrawFormIdField(T("cs_editor.debug.previous_at_last_resolve", "Previous weather at last resolve"), snapshot.lastWeather.previousWeatherId);
			DrawField(T("cs_editor.debug.lerp_at_last_resolve", "Weather lerp at last resolve"), std::format("{:.4f}", snapshot.lastWeather.lerp));
			ImGui::EndTable();
		}
		if (snapshot.interior)
			Util::Text::Disabled(T("cs_editor.debug.weather_is_not_blended_while_in_an_inter", "Weather is not blended while in an interior."));

		ImGui::Spacing();
		ImGui::TextUnformatted(T("cs_editor.debug.features_failing_to_apply", "Features failing to apply"));
		DrawNameList(snapshot.applyFailures);

		ImGui::Spacing();
		ImGui::TextUnformatted(T("cs_editor.debug.features_failing_to_restore", "Features failing to restore"));
		DrawNameList(snapshot.restoreFailures);
	}

	void DrawLocationTransitions(const DebugSnapshot& snapshot)
	{
		if (BeginFieldTable("LocationTransitions")) {
			DrawField(T("cs_editor.debug.global_duration", "Global duration"), std::format("{:.2f}s", snapshot.globalTransitionSeconds));
			DrawField(T("cs_editor.debug.pause_aware_clock", "Pause-aware clock"), std::format("{:.3f}", snapshot.transitionTime));
			DrawField(T("cs_editor.debug.last_tick", "Last tick"),
				snapshot.lastTransitionTick < 0.0f ? std::string(kMissingValue) :
													 std::format("{:.3f} ({:.3f} ago)", snapshot.lastTransitionTick,
														 snapshot.transitionTime - snapshot.lastTransitionTick));
			DrawField(T("cs_editor.debug.active_transitions", "Active transitions"), std::format("{}", snapshot.locationTransitions.size()));
			DrawField(T("cs_editor.debug.feature_batches", "Feature batches"), std::format("{}", snapshot.transitionBatchCount));
			DrawFlag(T("cs_editor.debug.batches_dirty", "Batches dirty"), snapshot.transitionBatchesDirty);
			ImGui::EndTable();
		}

		ImGui::Spacing();
		if (snapshot.locationTransitions.empty()) {
			Util::Text::Disabled(T("cs_editor.debug.no_transitions_are_currently_easing", "No transitions are currently easing."));
		} else if (ImGui::BeginTable("ActiveTransitions", 8, kDebugTableFlags)) {
			for (const auto* header :
				{ T("cs_editor.debug.feature", "Feature"), T("cs_editor.debug.path", "Path"), T("cs_editor.debug.setting", "Setting"), T("cs_editor.debug.start", "Start"), T("cs_editor.debug.current", "Current"), T("cs_editor.debug.target", "Target"), T("cs_editor.debug.progress", "Progress"), T("cs_editor.debug.direction", "Direction") })
				ImGui::TableSetupColumn(header);
			ImGui::TableHeadersRow();
			for (const auto& transition : snapshot.locationTransitions) {
				ImGui::TableNextRow();
				const std::string featureLabel = SceneSettingsManager::GetFeatureDisplayName(transition.feature);
				for (const auto* column : { &featureLabel, &transition.path, &transition.key }) {
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(column->empty() ? kMissingValue : column->c_str());
				}
				ImGui::TableNextColumn();
				ImGui::Text("%.4f", transition.startValue);
				ImGui::TableNextColumn();
				ImGui::Text("%.4f", transition.currentValue);
				ImGui::TableNextColumn();
				ImGui::Text("%.4f", transition.targetValue);
				ImGui::TableNextColumn();
				ImGui::Text("%.0f%% of %.2fs", transition.progress * 100.0f, transition.duration);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(transition.restoreAtEnd ? T("cs_editor.debug.restoring", "restoring") : T("cs_editor.debug.applying", "applying"));
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextUnformatted(T("cs_editor.debug.features_failing_to_apply_transitions", "Features failing to apply transitions"));
		DrawNameList(snapshot.transitionApplyFailures);
	}

	/// Every setting a scene currently drives: its pre-override value, what is applied, and the per-period inputs.
	void DrawResolvedSettings(const DebugSnapshot& snapshot)
	{
		if (snapshot.resolvedSettings.empty()) {
			Util::Text::Disabled(T("cs_editor.debug.no_settings_are_currently_overridden", "No settings are currently overridden."));
			return;
		}

		for (size_t index = 0; index < snapshot.resolvedSettings.size(); ++index) {
			const auto& setting = snapshot.resolvedSettings[index];
			ImGui::PushID(static_cast<int>(index));
			const auto header = std::format("{}{}.{}: {} (base {})",
				SceneSettingsManager::GetFeatureDisplayName(setting.feature),
				setting.path.empty() ? "" : std::format(".{}", setting.path),
				setting.key, setting.applied, setting.baseline);
			if (ImGui::TreeNode(header.c_str())) {
				if (ImGui::BeginTable("PeriodValues", SceneSettingsManager::kPeriodCount + 1, kDebugTableFlags)) {
					ImGui::TableSetupColumn(T("cs_editor.debug.layer", "Layer"));
					DrawPeriodHeaders();
					DrawPeriodValues(T("cs_editor.debug.time_of_day", "Time of day"), setting.timeOfDayValues);
					DrawPeriodValues(T("cs_editor.debug.current_weather", "Current weather"), setting.currentWeatherValues);
					DrawPeriodValues(T("cs_editor.debug.previous_weather", "Previous weather"), setting.previousWeatherValues);
					ImGui::EndTable();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	/** @brief Every resolver section, sampled each frame the window is visible. */
	void DrawSections(const SceneSettingsManager& manager)
	{
		const auto snapshot = manager.GetDebugSnapshot();
		ImGui::TextWrapped("%s", T("cs_editor.debug.live_debug_view_of_the_scene_resolver_sa", "Live debug view of the scene resolver. Sampled every frame while this window is open."));
		if (ImGui::CollapsingHeader(T("cs_editor.debug.scene_context", "Scene Context"), ImGuiTreeNodeFlags_DefaultOpen))
			DrawSceneContext(snapshot);
		if (ImGui::CollapsingHeader(T("cs_editor.debug.time_of_day_header", "Time of Day"), ImGuiTreeNodeFlags_DefaultOpen))
			DrawTimeOfDay(snapshot);
		if (ImGui::CollapsingHeader(T("cs_editor.debug.resolver_state", "Resolver State")))
			DrawResolverState(snapshot);
		if (ImGui::CollapsingHeader(T("cs_editor.debug.location_transitions", "Location Transitions"), ImGuiTreeNodeFlags_DefaultOpen))
			DrawLocationTransitions(snapshot);
		if (ImGui::CollapsingHeader(T("cs_editor.debug.applied_settings", "Applied Settings"), ImGuiTreeNodeFlags_DefaultOpen))
			DrawResolvedSettings(snapshot);
	}
}

void SceneDebugWindow::Draw(bool* open)
{
	if (!open || !*open)
		return;

	const float scale = Util::GetUIScale();
	ImGui::SetNextWindowPos(ImVec2(kWindowPosition.x * scale, kWindowPosition.y * scale), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(kWindowSize.x * scale, kWindowSize.y * scale), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(kWindowMinSize.x * scale, kWindowMinSize.y * scale), ImVec2(FLT_MAX, FLT_MAX));

	const std::string title = std::string(T("cs_editor.scene_debug", "Scene Debug")) + "##CSEditorSceneDebug";
	if (Util::BeginWithRoundedClose(title.c_str(), open, ImGuiWindowFlags_None)) {
		if (const auto* manager = SceneSettingsManager::GetSingleton())
			DrawSections(*manager);
	}
	ImGui::End();
}
