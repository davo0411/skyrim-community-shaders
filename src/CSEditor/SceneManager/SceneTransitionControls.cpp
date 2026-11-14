#include "SceneTransitionControls.h"

#include <imgui.h>

#include "I18n/I18n.h"
#include "SceneSettingsManager.h"
#include "Utils/UI.h"

// The key strings predate the move from the Scene Manager feature page, and are kept so existing translations still apply.
namespace
{
	using TransitionControl = SceneSettingsManager::TransitionControl;

	/// One transition slider, with a Reset on its line while the user's own value is set.
	void DrawControl(SceneSettingsManager& manager, TransitionControl control, const char* label, const char* format,
		ImGuiSliderFlags flags, const char* tooltip, const char* resetLabel)
	{
		const auto& spec = SceneSettingsManager::GetTransitionControlSpec(control);
		auto value = manager.GetTransitionControl(control);
		if (ImGui::SliderFloat(label, &value, spec.min, spec.max, format, ImGuiSliderFlags_AlwaysClamp | flags))
			manager.SetTransitionControl(control, value, true);
		// A drag held still would otherwise outlive the debounce and write the file mid-gesture.
		if (ImGui::IsItemActive())
			manager.HoldDeferredSceneChanges();
		Util::AddTooltip(tooltip);

		if (!manager.GetUserTransitionControl(control))
			return;
		ImGui::SameLine();
		const bool reset = Util::WarningButton(resetLabel);
		Util::AddTooltip(T("feature.scene_manager.transition_reset_tooltip",
			"Drop your value and use the active preset's, or the default when it sets none."));
		if (reset)
			manager.SetTransitionControl(control, std::nullopt);
	}
}

void SceneTransitionControls::Draw()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return;

	DrawControl(*manager, TransitionControl::TimeOfDayHours,
		T("feature.scene_manager.time_of_day_transition", "Time of Day Transition"), "%.2f h", ImGuiSliderFlags_None,
		T("feature.scene_manager.time_of_day_transition_tooltip",
			"Hours spent blending between time of day periods, kept inside the sky's own colour transitions.\n"
			"0 switches between periods instantly.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.time_of_day_transition_reset", "Reset##TimeOfDayTransition"));
	DrawControl(*manager, TransitionControl::LocationSeconds,
		T("feature.scene_manager.location_transition", "Location Transition"), "%.1f s", ImGuiSliderFlags_Logarithmic,
		T("feature.scene_manager.location_transition_tooltip",
			"Seconds location overrides take to ease in and out when you move between places.\n"
			"A single setting can override this from its own transition field in the CS Editor.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.location_transition_reset", "Reset##LocationTransition"));
	DrawControl(*manager, TransitionControl::WeatherScale,
		T("feature.scene_manager.weather_transition_length", "Weather Transition Length"), "%.2fx", ImGuiSliderFlags_Logarithmic,
		T("feature.scene_manager.weather_transition_length_tooltip",
			"Multiplies how long every weather takes to blend into the next, keeping each weather's own pacing.\n"
			"A longer transition also delays a queued weather change, which waits for the running one to finish.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.weather_transition_length_reset", "Reset##WeatherTransitionLength"));
	DrawControl(*manager, TransitionControl::Ease,
		T("feature.scene_manager.transition_ease", "Transition Easing"), "%.2f", ImGuiSliderFlags_None,
		T("feature.scene_manager.transition_ease_tooltip",
			"Softens the start and end of time of day, location and weather transitions, sky included.\n"
			"0 keeps a constant-speed blend.\n"
			"The active preset supplies this until you set your own."),
		T("feature.scene_manager.transition_ease_reset", "Reset##TransitionEase"));
}
