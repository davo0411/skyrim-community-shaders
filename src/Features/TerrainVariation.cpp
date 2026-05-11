#include "TerrainVariation.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainVariation::Settings,
	enableTerrainVariation,
	enableLODTerrainTilingFix)

void TerrainVariation::DrawSettings()
{
	{
		MenuFonts::FontRoleGuard bodyGuard(Menu::FontRole::Body);
		ImGui::TextWrapped(
			"Stochastic terrain variation reduces visible tiling on near and (optionally) LOD terrain.");
	}

	ImGui::Spacing();

	bool tvEnabled = settings.enableTerrainVariation != 0;
	if (ImGui::Checkbox("Enable Terrain Variation", &tvEnabled)) {
		settings.enableTerrainVariation = tvEnabled ? 1u : 0u;
		logger::info("TerrainVariation enable changed to: {}", settings.enableTerrainVariation != 0);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Master toggle for the runtime terrain variation path.\n"
			"When off, terrain samples fall back to vanilla SampleBias / SampleLevel\n"
			"without recompiling shaders.");
	}

	bool lodTilingFix = settings.enableLODTerrainTilingFix != 0;
	if (ImGui::Checkbox("Apply to LOD Terrain", &lodTilingFix)) {
		settings.enableLODTerrainTilingFix = lodTilingFix ? 1u : 0u;
		logger::info("TerrainVariation LOD setting changed to: {}", settings.enableLODTerrainTilingFix != 0);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Applies the tiling fix to LOD terrain objects.\n"
			"This helps reduce the visible tiling effect on distant terrain.");
	}
}

void TerrainVariation::PostPostLoad()
{
	logger::info("TerrainVariation: Feature initialized");
}

void TerrainVariation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void TerrainVariation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void TerrainVariation::RestoreDefaultSettings()
{
	settings = {};
}

bool TerrainVariation::DrawFailLoadMessage() const
{
	return false;
}