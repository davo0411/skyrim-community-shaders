#include "TerrainVariation.h"
#include "../Globals.h"
#include "../MeshShaderRuleManager.h"
#include "../State.h"
#include "../Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	TerrainVariation::Settings,
	enableTilingFix,
	enableLODTerrainTilingFix,
	enableMeshTargeting)

// ========== SetupGeometry Hook ==========

void TerrainVariation::BSLightingShader_SetupGeometry(RE::BSRenderPass* pass)
{
	auto& feature = globals::features::terrainVariation;
	if (!feature.loaded || !feature.settings.enableMeshTargeting) {
		return;
	}

	auto state = globals::state;
	if (!state) {
		return;
	}

	// Clear the mesh targeting bit (use locally-defined constant, not State.h enum)
	state->permutationData.ExtraFeatureDescriptor &= ~MeshTargetedBit;

	// Query MeshShaderRuleManager for this geometry
	auto* ruleManager = MeshShaderRuleManager::GetSingleton();
	if (!ruleManager) {
		return;
	}

	auto resolved = ruleManager->ResolveForGeometry(
		"TerrainVariation",
		pass->geometry,
		static_cast<RE::BSShaderProperty*>(pass->shaderProperty));

	if (resolved.matched) {
		// Set the bit to indicate this mesh should have terrain variation applied
		state->permutationData.ExtraFeatureDescriptor |= MeshTargetedBit;
	}
}

// ========== Hooks ==========

struct TerrainVariation::Hooks
{
	struct BSLightingShader_SetupGeometry_Hook
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			TerrainVariation::BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry_Hook>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[TerrainVariation] Installed hooks");
	}
};

// ========== UI ==========

void TerrainVariation::DrawSettings()
{
	if (ImGui::Checkbox("Enable Terrain Tiling Fix", (bool*)&settings.enableTilingFix)) {
		UpdateShaderSettings();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Reduces the repeating pattern effect on terrain textures.\n"
			"This technique creates more natural-looking terrain by adding variation to texture sampling.");
	}

	if (ImGui::Checkbox("Apply to LOD Terrain", (bool*)&settings.enableLODTerrainTilingFix)) {
		UpdateShaderSettings();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Applies the tiling fix to LOD terrain objects.\n"
			"This helps reduce the visible tiling effect on distant terrain.");
	}

	ImGui::Separator();

	ImGui::Checkbox("Enable Mesh Targeting", (bool*)&settings.enableMeshTargeting);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Apply terrain variation to specific meshes like grass cliffs.\n"
			"Configure rules in Data\\ShaderRules\\TerrainVariation.json");
	}

	if (settings.enableMeshTargeting) {
		auto* ruleManager = MeshShaderRuleManager::GetSingleton();
		if (ruleManager) {
			auto rules = ruleManager->GetRulesForFeature("TerrainVariation");
			if (!rules.empty()) {
				ImGui::Text("  Active rules: %zu", rules.size());
			}
		}
	}
}

void TerrainVariation::UpdateShaderSettings()
{
	if (!globals::state) {
		return;
	}

	// Mark the vertex descriptor as dirty to trigger an update
	if (globals::game::stateUpdateFlags) {
		globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_VERTEX_DESC);
	}
}

void TerrainVariation::PostPostLoad()
{
	Hooks::Install();
	logger::info("TerrainVariation: Feature initialized");
	UpdateShaderSettings();
}

void TerrainVariation::LoadSettings(json& o_json)
{
	settings = o_json;
	UpdateShaderSettings();
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