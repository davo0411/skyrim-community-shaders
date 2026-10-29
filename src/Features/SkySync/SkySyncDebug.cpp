#include "Features/SkySync.h"
#include "Globals.h"
#include "Utils/DebugSection.h"

void SkySync::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	static constexpr const char* CasterNames[] = { "Sun", "Masser", "Secunda", "Zenith" };
	static constexpr const char* PhaseNames[] = { "Full", "Waning Gibbous", "Waning Quarter", "Waning Crescent", "New", "Waxing Crescent", "Waxing Quarter", "Waxing Gibbous" };

	auto getPhase = [](const RE::Moon* moon) -> const char* {
		if (!moon || !moon->moonMesh)
			return "Unknown";
		if (const auto prop = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().shaderProperty.get())) {
			if (auto tex = prop->GetBaseTexture())
				return PhaseNames[static_cast<int>(Util::Moon::GetPhaseFromTexture(tex->name.c_str()))];
		}
		return "Unknown";
	};

	auto drawMoonEntry = [&](const char* label, Caster caster, const char* phase) {
		auto& color = colors[static_cast<int>(caster)];
		ImVec4 swatch = { color.x, color.y, color.z, 1.0f };
		ImGui::ColorButton(label, swatch, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, { ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight() });
		ImGui::SameLine();
		ImGui::Text("%s  [%s]  color (%.3f, %.3f, %.3f, %.3f)", label, phase, color.x, color.y, color.z, color.w);
	};

	const auto sky = globals::game::sky;
	drawMoonEntry("Masser", Caster::Masser, sky ? getPhase(sky->masser) : "Unknown");
	drawMoonEntry("Secunda", Caster::Secunda, sky ? getPhase(sky->secunda) : "Unknown");

	ImGui::Text("Dim: %.3f", currentDim);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("Shadow target: %s", CasterNames[static_cast<int>(shadowFader.target)]);
	ImGui::Text("Shadow dir:    (%.2f, %.2f, %.2f)", shadowFader.currentDir.x, shadowFader.currentDir.y, shadowFader.currentDir.z);
	ImGui::Text("Transition intensity factor: %.3f", shadowFader.intensityFactor);
	ImGui::Text("Caster fade: %.3f", shadowFader.casterFade);
	if (shadowFader.transitioning) {
		const float t = shadowFader.Progress(settings.ShadowTransitionDuration);
		ImGui::ProgressBar(t, { -1.0f, 0.0f }, "");
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::Text("Transitioning %.0f%%", t * 100.0f);
	} else {
		ImGui::TextDisabled("No transition");
	}
}
