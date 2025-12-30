#include "Features/InverseSquareLighting/LightEditor.h"
#include "Features/LightLimitFix.h"
#include "Menu.h"
#include "Utils/FileSystem.h"

#include <fstream>

void LightEditor::DrawSettings()
{
	ImGui::Checkbox("Enable Light Editor", &enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Allows for modifying lights in real-time to preview changes. "
			"Changes cannot be saved directly and it is not intended for gameplay use.");
	}

	if (!enabled)
		return;

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Checkbox("Disable Regular Falloff Lights", &disableRegularLights);
	ImGui::Checkbox("Disable Inverse Square Falloff Lights", &disableInvSqLights);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	int selectedFilter = static_cast<int>(filterOption);
	if (ImGui::Combo("Filter By", &selectedFilter, FilterOptionLabels, static_cast<int>(FilterOption::Count))) {
		filterOption = static_cast<FilterOption>(selectedFilter);
	}

	int selectedSort = static_cast<int>(sortOption);
	if (ImGui::Combo("Sort By", &selectedSort, SortOptionLabels, static_cast<int>(SortOption::Count))) {
		sortOption = static_cast<SortOption>(selectedSort);
	}

	if (ImGui::BeginCombo("Lights", selected.isSelected ? GetLightName(selected).c_str() : "Select a light")) {
		for (auto& light : lights) {
			const auto displayName = GetLightName(light);
			const bool isSelected = light == selected;

			if (ImGui::Selectable(displayName.c_str(), isSelected))
				selected = light;

			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (!selected.isSelected)
		return;

	if (selected.isRef || selected.isAttached) {
		ImGui::Text("Owner: 0x%08X | %s", selected.id, displayInfo.ownerEditorId.c_str());
		ImGui::Text("Owner last edited by: %s", displayInfo.ownerLastEditedBy.c_str());
		ImGui::Text("Base Object: 0x%08X | %s", displayInfo.baseObjectFormId, selected.name.c_str());
		ImGui::Text("LIGH: 0x%08X | %s", displayInfo.lighFormId, displayInfo.lighEditorId.c_str());
		ImGui::Text("Cell: %s", displayInfo.cellEditorId.c_str());

		// Light Placer source information
		if (displayInfo.isLightPlacerLight) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Light Placer Light");
			if (!displayInfo.lightPlacerSource.empty()) {
				ImGui::Text("Source JSON: %s", displayInfo.lightPlacerSource.c_str());
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("This light was placed by Light Placer from the specified JSON configuration file.");
				}
			}
			if (!displayInfo.lightPlacerNodeName.empty()) {
				ImGui::Text("Node: %s", displayInfo.lightPlacerNodeName.c_str());
			}
		}
	} else {
		ImGui::Text("Memory Address: %p", selected.ptr);
		ImGui::Text("NiLight Name: %s", selected.name.c_str());

		// Check for Light Placer indicator in other lights
		if (displayInfo.isLightPlacerLight) {
			ImGui::Spacing();
			ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Light Placer Light");
			if (!displayInfo.lightPlacerSource.empty()) {
				ImGui::Text("Source JSON: %s", displayInfo.lightPlacerSource.c_str());
			}
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Action buttons row
	if (ImGui::Button("Revert Changes")) {
		current = original;
		current.pos = { 0, 0, 0 };
		waitFrames = 1;
	}

	ImGui::SameLine();

	if (ImGui::Button("Copy to Clipboard")) {
		CopyLightDataToClipboard();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Copy current light settings as Light Placer JSON snippet to clipboard.");
		ImGui::Text("You can paste this directly into a Light Placer configuration file.");
	}

	// Show copy confirmation feedback
	if (showCopyConfirmation) {
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "Copied!");
		copyConfirmationTimer -= ImGui::GetIO().DeltaTime;
		if (copyConfirmationTimer <= 0.0f) {
			showCopyConfirmation = false;
		}
	}

	// Export button - only show for attached/other lights that can be exported
	if (selected.isAttached || displayInfo.isLightPlacerLight) {
		ImGui::SameLine();
		if (ImGui::Button("Export to JSON...")) {
			showExportPopup = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Export this light configuration to a Light Placer JSON file.");
		}
	}

	// Export popup modal
	if (showExportPopup) {
		ImGui::OpenPopup("Export Light to JSON");
		showExportPopup = false;
	}

	if (ImGui::BeginPopupModal("Export Light to JSON", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Export light settings to Light Placer format");
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		static char outputFilename[256] = "exported_light.json";
		ImGui::InputText("Filename", outputFilename, sizeof(outputFilename));

		ImGui::Spacing();

		// Preview the JSON that will be exported
		if (ImGui::CollapsingHeader("Preview JSON", ImGuiTreeNodeFlags_DefaultOpen)) {
			std::string snippet = GenerateLightPlacerSnippet();
			ImGui::TextWrapped("%s", snippet.c_str());
		}

		ImGui::Spacing();

		if (ImGui::Button("Export", ImVec2(120, 0))) {
			auto configPath = GetLightPlacerConfigPath();
			auto outputPath = configPath / outputFilename;
			ExportToLightPlacerJson(outputPath);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}

		// Show last export message
		if (!lastExportMessage.empty()) {
			ImGui::Spacing();
			ImVec4 msgColor = lastExportSuccess ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f) : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
			ImGui::TextColored(msgColor, "%s", lastExportMessage.c_str());
		}

		ImGui::EndPopup();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	ImGui::CheckboxFlags("Inverse Square Light", reinterpret_cast<uint32_t*>(&current.data.flags), static_cast<uint32_t>(LightLimitFix::LightFlags::InverseSquare));

	ImGui::Spacing();
	ImGui::Spacing();

	ImGui::ColorEdit3("Color", &current.data.diffuse.red);
	ImGui::SliderFloat("Intensity", &current.data.fade, 0.01f, 16.f, "%.3f");

	const auto isInvSq = current.data.flags.any(LightLimitFix::LightFlags::InverseSquare);

	if (isInvSq)
		ImGui::BeginDisabled();
	ImGui::SliderFloat("Radius", &current.data.radius, 2.f, 8096.f, "%.0f");
	if (isInvSq)
		ImGui::EndDisabled();

	if (isInvSq) {
		ImGui::SliderFloat("Size", &current.data.size, 0.01f, 10.0f, "%.3f");
		ImGui::SliderFloat("Cutoff", &current.data.cutoffOverride, 0.01f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	}

	ImGui::Spacing();
	ImGui::Spacing();

	if (!selected.isOther && current.data.lighFormId != 0) {
		ImGui::Text("X: %.2f, Y: %.2f, Z: %.2f", displayInfo.pos.x, displayInfo.pos.y, displayInfo.pos.z);
		ImGui::Spacing();
		ImGui::SliderFloat3("Position Offset", &current.pos.x, -500.f, 500.f, "%.0f");

		ImGui::Spacing();
		ImGui::Spacing();

		auto* flags = reinterpret_cast<uint32_t*>(&current.tesFlags);
		ImGui::Spacing();
		ImGui::Text("Light Flags");
		ImGui::CheckboxFlags("Dynamic", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kDynamic));
		ImGui::CheckboxFlags("Negative", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kNegative));
		ImGui::CheckboxFlags("Flicker", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kFlicker));
		ImGui::CheckboxFlags("Flicker Slow", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kFlickerSlow));
		ImGui::CheckboxFlags("Pulse", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kPulse));
		ImGui::CheckboxFlags("Pulse Slow", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kPulseSlow));
		ImGui::CheckboxFlags("Hemi Shadow", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kHemiShadow));
		ImGui::CheckboxFlags("Omni Shadow", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kOmniShadow));
		ImGui::CheckboxFlags("Portal Strict", flags, static_cast<uint32_t>(RE::TES_LIGHT_FLAGS::kPortalStrict));
	}
}

std::string LightEditor::GetLightName(LightInfo& lightInfo)
{
	std::string prefix = lightInfo.isLightPlacer ? "[LP] " : "";

	if (lightInfo.isRef)
		return fmt::format("{}0x{:08X} - {}", prefix, lightInfo.id, lightInfo.name.c_str());
	if (lightInfo.isAttached)
		return fmt::format("{}0x{:08X}|{} - {}", prefix, lightInfo.id, lightInfo.index, lightInfo.name.c_str());

	// For Light Placer lights, try to extract a cleaner name from the full LP name
	if (lightInfo.isLightPlacer) {
		std::string name = lightInfo.name.c_str();
		// Extract the light EditorID from LP_Light[path|lightEDID]#index format
		size_t pipePos = name.find('|');
		size_t bracketEnd = name.find(']');
		if (pipePos != std::string::npos && bracketEnd != std::string::npos) {
			std::string lightEditorId = name.substr(pipePos + 1, bracketEnd - pipePos - 1);
			// Also extract the source path
			size_t bracketStart = name.find('[');
			if (bracketStart != std::string::npos) {
				std::string sourcePath = name.substr(bracketStart + 1, pipePos - bracketStart - 1);
				return fmt::format("[LP] {} ({})", lightEditorId, sourcePath);
			}
			return fmt::format("[LP] {}", lightEditorId);
		}
	}

	return fmt::format("{}{:p} - {}", prefix, lightInfo.ptr, lightInfo.name.c_str());
}

void LightEditor::GatherLights()
{
	if (!enabled || !Menu::GetSingleton()->ShouldSwallowInput())
		return;

	if (waitFrames > 0) {
		waitFrames--;
		return;
	}

	bool foundSelected = false;

	auto addLight = [&](const RE::NiPointer<RE::BSLight>& light) {
		const auto bsLight = light.get();
		if (!bsLight)
			return;

		const auto niLight = bsLight->light.get();
		if (!niLight)
			return;

		LightInfo current;
		RE::TESObjectLIGH* ligh = nullptr;

		const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);
		const auto refr = niLight->GetUserData();
		if (refr) {
			if (refr->IsDisabled())
				return;
			if (auto* objRef = refr->GetObjectReference()) {
				if (objRef->GetFormType() == RE::FormType::Light)
					ligh = objRef->As<RE::TESObjectLIGH>();
				current.id = refr->GetFormID();
				current.name = clib_util::editorID::get_editorID(objRef);
				current.index = lightsAttached[refr]++;
			}
		}

		current.isRef = ligh != nullptr;

		if (!current.isRef && runtimeData->lighFormId != 0)
			ligh = RE::TESForm::LookupByID(runtimeData->lighFormId)->As<RE::TESObjectLIGH>();

		if (ligh && ligh->data.flags.any(RE::TES_LIGHT_FLAGS::kSpotlight, RE::TES_LIGHT_FLAGS::kSpotShadow))
			return;

		current.isOther = ligh == nullptr;
		current.isAttached = refr && !current.isRef && !current.isOther;

		// Detect Light Placer lights by checking the NiLight name prefix
		std::string lightName = niLight->name.c_str();
		current.isLightPlacer = lightName.starts_with("LP_Light[") || lightName.starts_with("LP_Node[");

		const bool isRefMatch = current.isRef && filterOption == FilterOption::RefLights;
		const bool isAttachedMatch = current.isAttached && filterOption == FilterOption::AttachedLights;
		const bool isOtherMatch = current.isOther && !current.isLightPlacer && filterOption == FilterOption::OtherLights;
		const bool isLightPlacerMatch = current.isLightPlacer && filterOption == FilterOption::LightPlacerLights;

		if (!(isRefMatch || isAttachedMatch || isOtherMatch || isLightPlacerMatch))
			return;

		if (current.isRef) {
			current.position = refr->GetPosition();
		} else if (current.isAttached) {
			current.position = niLight->parent->world.translate;
		}
		if (current.isOther || current.isLightPlacer) {
			current.ptr = reinterpret_cast<void*>(niLight);
			current.name = niLight->name;
			current.position = niLight->parent ? niLight->parent->world.translate : RE::NiPoint3();
			current.index = 0;
		}

		current.isSelected = selected == current;

		lights.push_back(current);

		if (!current.isSelected)
			return;
		selected = current;
		foundSelected = true;
		UpdateSelectedLight(refr, ligh, niLight);
	};

	lights.clear();
	lightsAttached.clear();

	const auto smState = globals::game::smState;
	const auto shadowSceneNode = smState->shadowSceneNode[0];

	const auto& activeLights = shadowSceneNode->GetRuntimeData().activeLights;

	for (auto& light : activeLights) {
		addLight(light);
	}

	const auto& activeShadowLights = shadowSceneNode->GetRuntimeData().activeShadowLights;

	for (auto& light : activeShadowLights) {
		addLight(light);
	}

	if (!foundSelected) {
		previous = selected;
		selected = {};
	}

	SortLights();
}

void LightEditor::UpdateSelectedLight(RE::TESObjectREFR* refr, RE::TESObjectLIGH* ligh, RE::NiLight* niLight)
{
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);
	auto tesFlags = ligh ? &ligh->data.flags : nullptr;

	if (previous != selected) {
		original.tesFlags = tesFlags ? static_cast<ISLCommon::TES_LIGHT_FLAGS_EXT>(tesFlags->underlying()) : static_cast<ISLCommon::TES_LIGHT_FLAGS_EXT>(0);
		original.data = *runtimeData;
		original.pos = selected.isRef ? refr->GetPosition() : niLight->parent->local.translate;
		current = original;
		current.pos = { 0, 0, 0 };
		previous = selected;
	}

	runtimeData->diffuse = current.data.diffuse;
	runtimeData->fade = current.data.fade;

	if (current.data.flags.any(LightLimitFix::LightFlags::InverseSquare)) {
		current.data.radius = runtimeData->radius;
		runtimeData->cutoffOverride = std::clamp(current.data.cutoffOverride, 0.01f, 1.0f);
		runtimeData->size = std::clamp(current.data.size, 0.1f, 50.0f);
	} else {
		runtimeData->radius = current.data.radius;
		runtimeData->cutoffOverride = current.data.cutoffOverride;
	}

	if (selected.isRef) {
		const auto currentPos = refr->GetPosition();
		const auto newPos = original.pos + current.pos;
		if (currentPos != newPos) {
			refr->SetPosition(newPos);
			waitFrames = 1;
		}
		displayInfo.pos = newPos;
	} else if (selected.isAttached) {
		const auto currentPos = niLight->parent->local.translate;
		const auto newPos = original.pos + current.pos;
		if (currentPos != newPos) {
			niLight->parent->local.translate = newPos;
			RE::NiUpdateData updateData;
			niLight->parent->Update(updateData);
			waitFrames = 1;
		}
		displayInfo.pos = newPos;
	}

	if (!selected.isOther && refr && tesFlags && current.tesFlags.underlying() != tesFlags->underlying()) {
		*tesFlags = static_cast<RE::TES_LIGHT_FLAGS>(current.tesFlags.underlying());
		refr->Disable();
		refr->Enable(false);
		waitFrames = 1;
	}

	if (current.data.flags.any(LightLimitFix::LightFlags::InverseSquare))
		runtimeData->flags.set(LightLimitFix::LightFlags::InverseSquare);
	else
		runtimeData->flags.reset(LightLimitFix::LightFlags::InverseSquare);

	displayInfo.ownerFormId = refr ? refr->GetFormID() : 0;
	displayInfo.ownerEditorId = refr ? clib_util::editorID::get_editorID(refr) : "Unknown";
	displayInfo.baseObjectFormId = refr && refr->GetBaseObject() ? refr->GetBaseObject()->formID : 0;
	displayInfo.ownerLastEditedBy = refr && refr->GetDescriptionOwnerFile() ? refr->GetDescriptionOwnerFile()->fileName : "Unknown";
	displayInfo.cellEditorId = refr && refr->GetParentCell() ? refr->GetParentCell()->GetFormEditorID() : "Unknown";
	displayInfo.lighFormId = ligh ? ligh->GetFormID() : 0;
	displayInfo.lighEditorId = ligh ? clib_util::editorID::get_editorID(ligh) : "Unknown";

	// Detect Light Placer source information
	DetectLightPlacerSource(niLight);
}

void LightEditor::SortLights()
{
	// Disable FormID/EditorID sorting for light types that don't have meaningful IDs
	if ((filterOption == FilterOption::OtherLights || filterOption == FilterOption::LightPlacerLights) &&
		(sortOption == SortOption::FormID || sortOption == SortOption::EditorID))
		sortOption = SortOption::None;

	switch (sortOption) {
	case SortOption::Distance:
		{
			const auto playerPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
			std::ranges::sort(lights, [&](const LightInfo& a, const LightInfo& b) {
				return a.position.GetSquaredDistance(playerPos) < b.position.GetSquaredDistance(playerPos);
			});
			break;
		}
	case SortOption::FormID:
		std::ranges::sort(lights, [](const LightInfo& a, const LightInfo& b) {
			return (a.id * 10 + a.index) < (b.id * 10 + b.index);
		});
		break;
	case SortOption::EditorID:
		std::ranges::sort(lights, [](const LightInfo& a, const LightInfo& b) {
			return a.name < b.name;
		});
		break;
	case SortOption::None:
	default:
		break;
	}
}

// Light Placer Integration

void LightEditor::DetectLightPlacerSource(RE::NiLight* niLight)
{
	displayInfo.isLightPlacerLight = false;
	displayInfo.lightPlacerSource.clear();
	displayInfo.lightPlacerNodeName.clear();

	if (!niLight || niLight->name.empty())
		return;

	std::string lightName = niLight->name.c_str();

	// Light Placer uses naming convention: LP_Light[path|lightEDID]#index or LP_Node[path|...]#index
	constexpr std::string_view LP_LIGHT_PREFIX = "LP_Light[";
	constexpr std::string_view LP_NODE_PREFIX = "LP_Node[";

	bool isLPLight = lightName.starts_with(LP_LIGHT_PREFIX);
	bool isLPNode = lightName.starts_with(LP_NODE_PREFIX);

	if (!isLPLight && !isLPNode)
		return;

	displayInfo.isLightPlacerLight = true;

	// Extract the path from the naming convention
	// Format: LP_Light[path|lightEDID]#index or LP_Node[path|nodeName(x,y,z)]#index
	size_t bracketStart = lightName.find('[');
	size_t pipePos = lightName.find('|');
	size_t bracketEnd = lightName.find(']');

	if (bracketStart != std::string::npos && pipePos != std::string::npos && bracketEnd != std::string::npos) {
		// Extract the JSON file path (between [ and |)
		displayInfo.lightPlacerSource = lightName.substr(bracketStart + 1, pipePos - bracketStart - 1);

		// Extract node/light name info (between | and ])
		displayInfo.lightPlacerNodeName = lightName.substr(pipePos + 1, bracketEnd - pipePos - 1);
	}
}

std::filesystem::path LightEditor::GetLightPlacerConfigPath()
{
	// Light Placer configs are stored in Data/LightPlacer/
	return Util::PathHelpers::GetDataPath() / "LightPlacer";
}

bool LightEditor::IsLightPlacerInstalled()
{
	return std::filesystem::exists(GetLightPlacerConfigPath());
}

nlohmann::json LightEditor::GenerateLightPlacerJson() const
{
	nlohmann::json lightData;

	// Light EditorID
	if (displayInfo.lighFormId != 0) {
		lightData["light"] = displayInfo.lighEditorId;
	}

	// Color (normalized RGB)
	lightData["color"] = { current.data.diffuse.red, current.data.diffuse.green, current.data.diffuse.blue };

	// Fade/Intensity
	lightData["fade"] = current.data.fade;

	// Radius
	lightData["radius"] = current.data.radius;

	// ISL-specific fields
	if (current.data.flags.any(LightLimitFix::LightFlags::InverseSquare)) {
		lightData["cutoff"] = current.data.cutoffOverride;
		lightData["size"] = current.data.size;

		// Build flags string
		std::string flagsStr = "InverseSquare";

		// Add other relevant flags
		if (current.tesFlags.any(ISLCommon::TES_LIGHT_FLAGS_EXT::kInverseSquare)) {
			// Already included
		}

		lightData["flags"] = flagsStr;
	}

	// Position offset (if modified)
	if (current.pos.x != 0 || current.pos.y != 0 || current.pos.z != 0) {
		lightData["offset"] = { current.pos.x, current.pos.y, current.pos.z };
	}

	return lightData;
}

std::string LightEditor::GenerateLightPlacerSnippet() const
{
	auto lightJson = GenerateLightPlacerJson();

	// Wrap in the Light Placer array format for a single light
	nlohmann::json wrapper;
	wrapper["data"] = lightJson;

	// If we have node information from LP source
	if (!displayInfo.lightPlacerNodeName.empty()) {
		// Try to extract node name vs point info
		if (displayInfo.lightPlacerNodeName.find('(') != std::string::npos) {
			// Contains position info, likely a node attachment
			size_t parenPos = displayInfo.lightPlacerNodeName.find('(');
			std::string nodeName = displayInfo.lightPlacerNodeName.substr(0, parenPos);
			wrapper["nodes"] = nlohmann::json::array({ nodeName });
		} else {
			wrapper["nodes"] = nlohmann::json::array({ displayInfo.lightPlacerNodeName });
		}
	}

	nlohmann::json lightsArray = nlohmann::json::array({ wrapper });

	return lightsArray.dump(2);
}

void LightEditor::CopyLightDataToClipboard()
{
	std::string snippet = GenerateLightPlacerSnippet();

	// Use ImGui clipboard
	ImGui::SetClipboardText(snippet.c_str());

	// Show confirmation feedback
	showCopyConfirmation = true;
	copyConfirmationTimer = 2.0f;  // Show for 2 seconds

	logger::info("[LightEditor] Copied light data to clipboard:\n{}", snippet);
}

void LightEditor::ExportToLightPlacerJson(const std::filesystem::path& outputPath)
{
	try {
		// Ensure directory exists
		Util::FileHelpers::EnsureDirectoryExists(outputPath.parent_path());

		nlohmann::json existingConfig;

		// Check if file already exists and try to merge
		if (std::filesystem::exists(outputPath)) {
			std::ifstream existingFile(outputPath);
			if (existingFile.is_open()) {
				try {
					existingFile >> existingConfig;
					existingFile.close();
				} catch (const nlohmann::json::parse_error&) {
					// File exists but isn't valid JSON, we'll overwrite it
					existingConfig = nlohmann::json::array();
				}
			}
		}

		// Ensure it's an array
		if (!existingConfig.is_array()) {
			existingConfig = nlohmann::json::array();
		}

		// Create the new light entry in Light Placer format
		nlohmann::json newEntry;

		// Add model path if we have reference info
		if (!displayInfo.ownerEditorId.empty() && displayInfo.ownerEditorId != "Unknown") {
			// For attached lights, we need the model path
			// This is a simplified version - in practice, you'd need to look up the model
			newEntry["formIDs"] = nlohmann::json::array({ fmt::format("0x{:08X}", displayInfo.ownerFormId) });
		}

		// Create the lights array with our light data
		nlohmann::json lightWrapper;
		lightWrapper["data"] = GenerateLightPlacerJson();

		// Add node attachment info if available
		if (!displayInfo.lightPlacerNodeName.empty()) {
			if (displayInfo.lightPlacerNodeName.find('(') != std::string::npos) {
				size_t parenPos = displayInfo.lightPlacerNodeName.find('(');
				std::string nodeName = displayInfo.lightPlacerNodeName.substr(0, parenPos);
				lightWrapper["nodes"] = nlohmann::json::array({ nodeName });
			} else {
				lightWrapper["nodes"] = nlohmann::json::array({ displayInfo.lightPlacerNodeName });
			}
		}

		newEntry["lights"] = nlohmann::json::array({ lightWrapper });

		existingConfig.push_back(newEntry);

		// Write to file
		std::ofstream outFile(outputPath);
		if (outFile.is_open()) {
			outFile << existingConfig.dump(2);
			outFile.close();

			lastExportSuccess = true;
			lastExportMessage = fmt::format("Exported to: {}", outputPath.filename().string());
			logger::info("[LightEditor] Exported light to: {}", outputPath.string());
		} else {
			lastExportSuccess = false;
			lastExportMessage = "Failed to open file for writing";
			logger::error("[LightEditor] Failed to export light to: {}", outputPath.string());
		}
	} catch (const std::exception& e) {
		lastExportSuccess = false;
		lastExportMessage = fmt::format("Export failed: {}", e.what());
		logger::error("[LightEditor] Export failed: {}", e.what());
	}
}