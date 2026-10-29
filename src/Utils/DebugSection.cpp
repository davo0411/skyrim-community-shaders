#include "DebugSection.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "Utils/UI.h"

namespace Util
{
	DebugSection::DebugSection() :
		visible(ShowDebugSections())
	{
		if (!visible)
			return;

		ImGui::Spacing();
		MenuFonts::FontRoleGuard guard(Menu::FontRole::Subheading);
		ImGui::SeparatorText(T("menu.features.debug", "Debug"));
	}

	void DebugSection::Hint(const char* text) const
	{
		if (!visible)
			return;

		const auto& disabled = globals::menu->GetSettings().Theme.StatusPalette.Disable;
		ImGui::PushStyleColor(ImGuiCol_Text, disabled);
		ImGui::TextWrapped("%s", text);
		ImGui::PopStyleColor();
	}
}
