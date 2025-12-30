#pragma once
#include "Features/InverseSquareLighting/Common.h"

#include <nlohmann/json.hpp>

struct LightEditor
{
	bool enabled;
	bool disableInvSqLights;
	bool disableRegularLights;

	void DrawSettings();
	void GatherLights();

private:
	struct LightInfo
	{
		bool isSelected = false;
		uint32_t id;
		void* ptr;
		uint32_t index;
		std::string name;
		bool isRef;
		bool isAttached;
		bool isOther;
		bool isLightPlacer = false;  // Light originated from Light Placer mod
		RE::NiPoint3 position;

		bool operator==(const LightInfo& other) const noexcept
		{
			return id == other.id && index == other.index;
		}
	};

	struct LightDisplayInfo
	{
		RE::FormID ownerFormId = 0;
		std::string ownerEditorId;
		RE::FormID baseObjectFormId = 0;
		std::string ownerLastEditedBy;
		std::string cellEditorId;
		RE::FormID lighFormId = 0;
		std::string lighEditorId;
		RE::NiPoint3 pos = {};
		// Light Placer specific info
		std::string lightPlacerSource;        // JSON file this light originated from
		std::string lightPlacerNodeName;      // Node name in the config
		bool isLightPlacerLight = false;      // Whether this is a Light Placer managed light
	};

	struct LightSettings
	{
		stl::enumeration<ISLCommon::TES_LIGHT_FLAGS_EXT, uint32_t> tesFlags;
		ISLCommon::RuntimeLightDataExt data = {};
		RE::NiPoint3 pos = {};
	};

	bool showAttachedLights = false;
	bool showEffectLights = false;
	int32_t waitFrames = 0;

	enum class FilterOption
	{
		RefLights,
		AttachedLights,
		LightPlacerLights,
		OtherLights,
		Count
	};

	const char* FilterOptionLabels[4] = {
		"Ref Lights",
		"Attached Lights",
		"Light Placer Lights",
		"Other Lights"
	};

	enum class SortOption
	{
		None,
		Distance,
		FormID,
		EditorID,
		Count
	};

	const char* SortOptionLabels[4] = {
		"None",
		"Distance",
		"FormID",
		"EditorID"
	};

	FilterOption filterOption = FilterOption::RefLights;
	SortOption sortOption = SortOption::Distance;

	std::vector<LightInfo> lights = {};
	std::unordered_map<RE::TESObjectREFR*, uint32_t> lightsAttached = {};

	LightInfo selected = {};
	LightInfo previous = {};

	LightDisplayInfo displayInfo = {};
	LightSettings original = {};
	LightSettings current = {};

	// Export/Save state
	bool showExportPopup = false;
	bool showCopyConfirmation = false;
	float copyConfirmationTimer = 0.0f;
	std::string lastExportMessage;
	bool lastExportSuccess = false;

	void SortLights();

	static std::string GetLightName(LightInfo& lightInfo);

	void UpdateSelectedLight(RE::TESObjectREFR* refr, RE::TESObjectLIGH* ligh, RE::NiLight* niLight);

	// Light Placer integration
	void DetectLightPlacerSource(RE::NiLight* niLight);
	static std::filesystem::path GetLightPlacerConfigPath();
	static bool IsLightPlacerInstalled();

	// Export/Save functionality
	nlohmann::json GenerateLightPlacerJson() const;
	void CopyLightDataToClipboard();
	void ExportToLightPlacerJson(const std::filesystem::path& outputPath);
	std::string GenerateLightPlacerSnippet() const;
};
