#include "PBRWater.h"

#include "Globals.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PBRWater::Settings,
	EnableBRDFSpecular,
	EnableWaterScattering)

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::DrawSettings()
{
	ImGui::Checkbox("Enable BRDF Specular", (bool*)&settings.EnableBRDFSpecular);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Enables physically based BRDF specular reflections on water surfaces");

	ImGui::Checkbox("Enable Water Scattering", (bool*)&settings.EnableWaterScattering);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Enables scattering effects in water");
}

void PBRWater::SetupResources()
{
}

void PBRWater::ClearShaderCache()
{
}
