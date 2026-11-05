#include "WaterEffects.h"

#include <DDSTextureLoader.h>

#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.water_effects."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WaterEffects::Settings,
	CausticsProjection,
	CausticsMinLightZ,
	CausticsFocalDepth,
	CausticsDepthOfField,
	CausticsMaxBlur)

void WaterEffects::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\WaterEffects\\watercaustics.dds", nullptr, causticsView.put());
}

void WaterEffects::Prepass()
{
	auto context = globals::d3d::context;
	auto srv = causticsView.get();
	context->PSSetShaderResources(65, 1, &srv);
}

bool WaterEffects::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void WaterEffects::DrawSettings()
{
	auto& s = settings;
	Util::SliderFloatTip(T(TKEY("caustics_projection"), "Caustics Projection"), &s.CausticsProjection, 1.0f, 16.0f, "%.1f",
		T(TKEY("caustics_projection_tooltip"), "Caustics on the bed are read where the sunlight crossed the water's surface, so they shift along the light with depth. This divides that shift, flattening its angle. Default 4."));
	Util::SliderFloatTip(T(TKEY("caustics_min_light_z"), "Low Sun Limit"), &s.CausticsMinLightZ, 0.05f, 0.5f, "%.2f",
		T(TKEY("caustics_min_light_z_tooltip"), "The lowest sun elevation (its sine) the shift follows, so a low sun does not smear the pattern. Default 0.2."));
	Util::SliderFloatTip(T(TKEY("caustics_focal_depth"), "Focal Depth"), &s.CausticsFocalDepth, 0.0f, 20.0f, "%.2f m",
		T(TKEY("caustics_focal_depth_tooltip"), "Water depth at which caustics are sharpest. Default 2 m."));
	Util::SliderFloatTip(T(TKEY("caustics_depth_of_field"), "Depth of Field"), &s.CausticsDepthOfField, 0.01f, 10.0f, "%.2f m",
		T(TKEY("caustics_depth_of_field_tooltip"), "Range of depth around the focal depth over which caustics stay sharp; they blur beyond it. Default 0.33 m."), ImGuiSliderFlags_Logarithmic);
	Util::SliderFloatTip(T(TKEY("caustics_max_blur"), "Maximum Blur"), &s.CausticsMaxBlur, 0.0f, 10.0f, "%.1f",
		T(TKEY("caustics_max_blur_tooltip"), "Most mip levels of blur added away from the focal depth. 0 keeps caustics sharp at every depth. Default 6."));
}

void WaterEffects::LoadSettings(json& o_json)
{
	settings = o_json;
	// Malformed values must never reach the shaders (a zero divisor or a negative blur).
	const Settings d{};
	auto clampTo = [](float& v, float lo, float hi, float fallback) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
	clampTo(settings.CausticsProjection, 0.1f, 1000.0f, d.CausticsProjection);
	clampTo(settings.CausticsMinLightZ, 0.01f, 1.0f, d.CausticsMinLightZ);
	clampTo(settings.CausticsFocalDepth, 0.0f, 1000.0f, d.CausticsFocalDepth);
	clampTo(settings.CausticsDepthOfField, 0.01f, 1000.0f, d.CausticsDepthOfField);
	clampTo(settings.CausticsMaxBlur, 0.0f, 12.0f, d.CausticsMaxBlur);
}

void WaterEffects::SaveSettings(json& o_json)
{
	o_json = settings;
}

void WaterEffects::RestoreDefaultSettings()
{
	settings = {};
}

WaterEffects::SharedSettings WaterEffects::GetCommonBufferData() const
{
	SharedSettings data{};
	data.Caustics0 = { std::max(settings.CausticsProjection, 0.1f), std::clamp(settings.CausticsMinLightZ, 0.01f, 1.0f), std::max(settings.CausticsFocalDepth, 0.0f), std::max(settings.CausticsDepthOfField, 0.01f) };
	data.Caustics1 = { std::clamp(settings.CausticsMaxBlur, 0.0f, 12.0f), 0.0f, 0.0f, 0.0f };
	return data;
}
