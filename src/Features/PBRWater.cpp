#include "PBRWater.h"

#include "Features/UnifiedWater.h"
#include "Features/Upscaling.h"
#include "I18n/I18n.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/UI.h"
#include "Utils/VersionedRelocation.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#define I18N_KEY_PREFIX "feature.pbr_water."

// Two NLOHMANN_JSON_PASTE runs: the field count exceeds the single-macro limit of 63.
void to_json(nlohmann::json& nlohmann_json_j, const PBRWater::Settings& nlohmann_json_t)
{
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		EnableTessellation,
		TessellationTriangleSize,
		WindSpeedCalm,
		WindSpeedStorm,
		WaveHeight,
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		WaveResolution,
		EnableShoreWaves,
		Roughness,
		Visibility,
		Turbidity))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_TO,
		SedimentColor,
		FoamAmount,
		EnableRipples,
		GameplayWaves,
		EnableFloatingObjects,
		CarryActors,
		WetShores,
		WireframeMode,
		DebugView))
}

void from_json(const nlohmann::json& nlohmann_json_j, PBRWater::Settings& nlohmann_json_t)
{
	const PBRWater::Settings nlohmann_json_default_obj{};
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		EnableTessellation,
		TessellationTriangleSize,
		WindSpeedCalm,
		WindSpeedStorm,
		WaveHeight,
		SwellHeight,
		SwellPeriod,
		SwellDirection,
		WaveResolution,
		EnableShoreWaves,
		Roughness,
		Visibility,
		Turbidity))
	NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
		NLOHMANN_JSON_FROM_WITH_DEFAULT,
		SedimentColor,
		FoamAmount,
		EnableRipples,
		GameplayWaves,
		EnableFloatingObjects,
		CarryActors,
		WetShores,
		WireframeMode,
		DebugView))
}

namespace
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float CellSize = static_cast<float>(PBRWaterModel::CellSize);
	constexpr float InteriorFetchMetres = 50.0f;
	constexpr float TwoPi = static_cast<float>(PBRWaterModel::TwoPi);

	using SC = SIE::ShaderCache;

	/// Names the GPU work of a scope for RenderDoc, Nsight and the nvperf ranges.
	struct PerfScope
	{
		explicit PerfScope(std::string_view title) { globals::state->BeginPerfEvent(title); }
		~PerfScope() { globals::state->EndPerfEvent(); }
		PerfScope(const PerfScope&) = delete;
		PerfScope& operator=(const PerfScope&) = delete;
	};

	uint32_t WaterTechnique(uint32_t descriptor)
	{
		return (descriptor >> 11) & 0xF;
	}

	bool IsTessellatedTechnique(uint32_t technique)
	{
		return technique != static_cast<uint32_t>(SC::WaterShaderTechniques::Lod);
	}

	bool IsStencilTechnique(uint32_t technique)
	{
		return technique == static_cast<uint32_t>(SC::WaterShaderTechniques::Stencil);
	}

	float SmoothFactor(float dt, float timeConstant)
	{
		return timeConstant > 0.0f ? 1.0f - std::exp(-std::max(dt, 0.0f) / timeConstant) : 1.0f;
	}

	bool IsDynamicMotion(const RE::hkpRigidBody* body)
	{
		using Type = RE::hkpMotion::MotionType;
		const auto type = body->motion.type.get();
		return type == Type::kDynamic || type == Type::kSphereInertia || type == Type::kBoxInertia || type == Type::kThinBoxInertia;
	}
}

// ============================================================================
// Settings
// ============================================================================

void PBRWater::LoadSettings(json& o_json)
{
	settings = o_json;
	SanitizeSettings();
}

void PBRWater::SanitizeSettings()
{
	// Safety bounds only, not the sliders' ranges: Ctrl+click may type any value past a slider's end and it must stick
	// (this runs every frame). Only what would break the maths is held back: non-finite values, divisors and sizes that
	// must stay positive, factors that only mean something in 0..1, and the hardware's tessellation limit.
	auto& s = settings;
	constexpr float Huge = 1.0e6f;
	auto clamp = [](float& value, float lo, float hi, float fallback) {
		value = std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
	};
	const Settings d{};
	clamp(s.TessellationTriangleSize, 1.0f, Huge, d.TessellationTriangleSize);
	clamp(s.WindSpeedCalm, 0.0f, Huge, d.WindSpeedCalm);
	clamp(s.WindSpeedStorm, 0.1f, Huge, d.WindSpeedStorm);
	clamp(s.WaveHeight, 0.0f, Huge, d.WaveHeight);
	clamp(s.SwellHeight, 0.0f, Huge, d.SwellHeight);
	clamp(s.SwellPeriod, 0.5f, Huge, d.SwellPeriod);
	clamp(s.SwellDirection, -Huge, Huge, d.SwellDirection);
	s.WaveResolution = std::clamp(s.WaveResolution, 0, 2);
	clamp(s.Roughness, 0.001f, 1.0f, d.Roughness);
	clamp(s.Visibility, 0.001f, Huge, d.Visibility);
	clamp(s.Turbidity, 0.0f, Huge, d.Turbidity);
	clamp(s.SedimentColor.x, 0.0f, Huge, d.SedimentColor.x);
	clamp(s.SedimentColor.y, 0.0f, Huge, d.SedimentColor.y);
	clamp(s.SedimentColor.z, 0.0f, Huge, d.SedimentColor.z);
	clamp(s.FoamAmount, 0.0f, Huge, d.FoamAmount);
	s.WireframeMode = std::clamp(s.WireframeMode, 0, 2);
	s.DebugView = std::clamp(s.DebugView, 0, 13);
}

void PBRWater::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PBRWater::RestoreDefaultSettings()
{
	settings = {};
}

void PBRWater::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("waves"), "Waves"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("wave_height"), "Wave Height"), &settings.WaveHeight, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wave_height_tooltip"), "Scales the wind-driven wave spectrum. The waves themselves come from the weather's wind, the open-water distance upwind (fetch) and the water depth."));
		ImGui::SliderFloat(T(TKEY("swell_height"), "Swell Height"), &settings.SwellHeight, 0.0f, 4.0f, "%.2f m");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("swell_height_tooltip"), "Long, regular waves arriving from distant storms, on top of the local wind sea. Only on the open sea: sheltered water never sees them."));
		ImGui::SliderFloat(T(TKEY("swell_period"), "Swell Period"), &settings.SwellPeriod, 4.0f, 20.0f, "%.1f s");
		ImGui::SliderFloat(T(TKEY("swell_direction"), "Swell Direction"), &settings.SwellDirection, -180.0f, 180.0f, "%.0f deg");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("swell_direction_tooltip"), "Direction the swell travels, relative to the wind. Swell crossing the wind sea gives the most natural, irregular water."));
		{
			const char* sizes[] = { T(TKEY("wave_resolution_low"), "Low (128)"), T(TKEY("wave_resolution_medium"), "Medium (256)"), T(TKEY("wave_resolution_high"), "High (512)") };
			ImGui::Combo(T(TKEY("wave_resolution"), "Wave Detail"), &settings.WaveResolution, sizes, IM_ARRAYSIZE(sizes));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wave_resolution_tooltip"), "Resolution of each FFT wave cascade. Higher values add finer ripples close up at a small GPU cost; gameplay is unaffected."));
		}
		ImGui::SliderFloat(T(TKEY("wind_speed_calm"), "Calm Wind Speed"), &settings.WindSpeedCalm, 0.0f, 10.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_calm_tooltip"), "Wind speed used when the weather reports no wind."));
		ImGui::SliderFloat(T(TKEY("wind_speed_storm"), "Storm Wind Speed"), &settings.WindSpeedStorm, 1.0f, 40.0f, "%.1f m/s");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("wind_speed_storm_tooltip"), "Wind speed reached at the weather's maximum wind. Wave height grows with the square of the wind speed."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("shore"), "Shoreline"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_shore_waves"), "Shore Waves"), &settings.EnableShoreWaves);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_shore_waves_tooltip"), "Waves roll in towards the shore, slow down and close up over the shoaling bed, steepen, pitch forward and break into foaming bores, and each one runs up the beach and drains back, leaving wet sand. Only on beaches where waves break: they surge against rock faces and cliffs instead."));
		if (settings.EnableShoreWaves) {
			ImGui::Checkbox(T(TKEY("wet_shores"), "Wet Shores"), &settings.WetShores);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("wet_shores_tooltip"), "Sand, rocks and anything else the swash reaches turn wet and slowly dry out (Wetness Effects)."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("surface"), "Surface"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("roughness"), "Surface Roughness"), &settings.Roughness, 0.02f, 0.5f, "%.3f");
		ImGui::SliderFloat(T(TKEY("visibility"), "Water Visibility"), &settings.Visibility, 0.05f, 5.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("visibility_tooltip"), "Scales the water form's visibility distance. Absorption colour comes from the water form's shallow colour."));
		ImGui::SliderFloat(T(TKEY("turbidity"), "Turbidity"), &settings.Turbidity, 0.0f, 5.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("turbidity_tooltip"), "Sediment suspended everywhere in the water. 0 keeps the water form's own clarity."));
		ImGui::ColorEdit3(T(TKEY("sediment_color"), "Sediment Colour"), reinterpret_cast<float*>(&settings.SedimentColor));
		ImGui::SliderFloat(T(TKEY("foam_amount"), "Foam Amount"), &settings.FoamAmount, 0.0f, 3.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("foam_amount_tooltip"), "Foam is made only where waves break, in the open sea and in the surf. The wind sets how much of the sea whitecaps (Monahan 1980). 1 is as measured."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("interaction"), "Interaction"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable_ripples"), "Interactive Ripples"), &settings.EnableRipples);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_ripples_tooltip"), "Simulates ripples and wakes from every actor and object in the water. Replaces the vanilla wading mesh."));
		ImGui::Checkbox(T(TKEY("gameplay_waves"), "Swimming Follows Waves"), &settings.GameplayWaves);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("gameplay_waves_tooltip"), "The game's water height includes the rendered waves, so swimming actors ride them."));
		ImGui::Checkbox(T(TKEY("enable_floating_objects"), "Boats and Ice Ride the Waves"), &settings.EnableFloatingObjects);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_floating_objects_tooltip"), "Boats, ships, rafts and ice floes heave, pitch and roll on the rendered waves, with everything on board. Found automatically: anything placed afloat that does not rest on the bottom. Nothing is saved; objects are back in place whenever the game saves."));
		if (settings.EnableFloatingObjects) {
			ImGui::Checkbox(T(TKEY("carry_actors"), "Carry Actors on Board"), &settings.CarryActors);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(T(TKEY("carry_actors_tooltip"), "The player and NPCs standing on a floating hull move with its deck, and their weight sinks and tilts small boats."));
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("geometry"), "Geometry"), ImGuiTreeNodeFlags_None)) {
		ImGui::Checkbox(T(TKEY("enable_tessellation"), "Tessellation"), &settings.EnableTessellation);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("enable_tessellation_tooltip"), "Subdivides the water mesh on the GPU so waves are real geometry. Without it, waves are limited by the mesh resolution."));
		ImGui::SliderFloat(T(TKEY("tessellation_triangle_size"), "Target Triangle Size"), &settings.TessellationTriangleSize, 4.0f, 40.0f, "%.0f px");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("tessellation_triangle_size_tooltip"), "Smaller values give smoother waves at a higher GPU cost. Scales automatically with the render resolution."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"), ImGuiTreeNodeFlags_None)) {
		const char* wireModes[] = { T(TKEY("wireframe_off"), "Off"), T(TKEY("wireframe_overlay"), "Overlay"), T(TKEY("wireframe_only"), "Wireframe Only") };
		ImGui::Combo(T(TKEY("wireframe"), "Wireframe"), &settings.WireframeMode, wireModes, IM_ARRAYSIZE(wireModes));
		const char* views[] = {
			T(TKEY("debug_off"), "Off"),
			T(TKEY("debug_normal"), "Normal"),
			T(TKEY("debug_foam"), "Foam"),
			T(TKEY("debug_depth"), "Depth / Shore"),
			T(TKEY("debug_jacobian"), "Foam Jacobian"),
			T(TKEY("debug_roughness"), "Roughness"),
			T(TKEY("debug_height"), "Wave Height"),
			T(TKEY("debug_fetch"), "Fetch"),
			T(TKEY("debug_clarity"), "Water Clarity"),
			T(TKEY("debug_wind"), "Local Wind"),
			T(TKEY("debug_shore_field"), "Shore Field"),
			T(TKEY("debug_wave_direction"), "Wave Direction"),
			T(TKEY("debug_shore_level"), "Shore Field Level"),
			T(TKEY("debug_shore_wave"), "Shore Wave"),
		};
		ImGui::Combo(T(TKEY("debug_view"), "Debug View"), &settings.DebugView, views, IM_ARRAYSIZE(views));

		if (const auto snap = snapshot.load(std::memory_order_acquire); snap && snap->ocean) {
			const auto& o = *snap->ocean;
			ImGui::Text("Wind %.1f m/s, peak period %.1f s, Hs %.2f m, choppiness %.2f", o.windSpeed, TwoPi / o.peakOmega, o.significantHeight / UnitsPerMetre, o.choppiness);
			ImGui::Text("Whitecaps %.2f%% of the sea, breaking below eigenvalue %.2f; mean square slope %.3f", o.whitecapCoverage * 100.0f, o.foamThreshold, o.meanSquareSlope);
			ImGui::Text("FFT %u x %u cascades: %s, gameplay mirror: %s", o.resolution, PBRWaterModel::NumCascades, oceanReady.load() ? "running" : "unavailable", snap->mirror0 ? "running" : "idle");
		}
		ImGui::Text("Water draws last frame: %u, %u tessellated with %u patches", lastDrawStats.draws, lastDrawStats.tessellated, lastDrawStats.patches);
		ImGui::Text("Fetch: %s, bathymetry: %s%s", environment.GetFetch() ? "ready" : "none", environment.GetBathymetry() ? "ready" : "none", environment.IsBuilding() ? " (building)" : "");
		ImGui::Text("Shore field: %s%s, swash: %s", environment.GetShore() ? "ready" : "none", environment.IsBuildingShore() ? " (building)" : "",
			frameData.Swash0.w > 0.5f ? "running" : "idle");
		const auto floatingStats = floating.GetStats();
		ImGui::Text("Floating: %u hulls, %u attached, %u actors on board, %u small objects riding the water", floatingStats.floaters, floatingStats.parts, floatingStats.carried, floatingStats.riders);
		ImGui::TextUnformatted(floating.GetSelectionReport().c_str());
		ImGui::TreePop();
	}
}

// ============================================================================
// Resources
// ============================================================================

void PBRWater::SetupResources()
{
	gpuBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::Constants");
	drawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::DrawConstants");
	{
		// Half floats: the profile is smooth, and the bilinear filter does the rest.
		const auto& profile = PBRWaterModel::GetShoreProfile();
		using Table = PBRWaterModel::ShoreProfileTable;
		std::vector<uint16_t> texels(profile.slice0.size() + profile.slice1.size());
		for (size_t i = 0; i < profile.slice0.size(); ++i) {
			texels[i] = DirectX::PackedVector::XMConvertFloatToHalf(profile.slice0[i]);
			texels[profile.slice0.size() + i] = DirectX::PackedVector::XMConvertFloatToHalf(profile.slice1[i]);
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = Table::Phases;
		desc.Height = Table::Stages;
		desc.MipLevels = 1;
		desc.ArraySize = 2;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const UINT pitch = Table::Phases * 4 * sizeof(uint16_t);
		D3D11_SUBRESOURCE_DATA init[2] = { { texels.data(), pitch, 0 }, { texels.data() + profile.slice0.size(), pitch, 0 } };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (SUCCEEDED(globals::d3d::device->CreateTexture2D(&desc, init, texture.put()))) {
			shoreProfileTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::ShoreProfile");
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.ArraySize = 2;
			shoreProfileTexture->CreateSRV(srvDesc);
		}
	}
	flatDrawBuffer = std::make_unique<ConstantBuffer>(ConstantBufferDesc<GpuData>(), "PBRWater::FlatDrawConstants");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));
	Util::SetResourceName(linearClampSampler.get(), "PBRWater::LinearClampSampler");

	// The ocean tiles repeat (wrap) and are seen at grazing angles (anisotropic filtering).
	D3D11_SAMPLER_DESC oceanDesc = samplerDesc;
	oceanDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	oceanDesc.MaxAnisotropy = 8;
	oceanDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	oceanDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&oceanDesc, oceanSampler.put()));
	Util::SetResourceName(oceanSampler.get(), "PBRWater::OceanSampler");

	ripples.SetupResources();
	swash.SetupResources();

	// The Skylighting occluder: a camera-centred grid of (OccluderQuads + 1)^2 vertices made by the vertex shader.
	{
		constexpr uint32_t verts = OccluderQuads + 1;
		static_assert(verts * verts <= 65536, "16-bit indices");
		std::vector<uint16_t> indices;
		indices.reserve(static_cast<size_t>(OccluderQuads) * OccluderQuads * 6);
		for (uint32_t y = 0; y < OccluderQuads; ++y) {
			for (uint32_t x = 0; x < OccluderQuads; ++x) {
				const auto a = static_cast<uint16_t>(y * verts + x);
				const auto b = static_cast<uint16_t>(a + 1);
				const auto c = static_cast<uint16_t>(a + verts);
				const auto e = static_cast<uint16_t>(c + 1);
				indices.insert(indices.end(), { a, c, b, b, c, e });
			}
		}
		D3D11_BUFFER_DESC indexDesc{};
		indexDesc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
		indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
		indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		const D3D11_SUBRESOURCE_DATA indexData{ indices.data(), 0, 0 };
		DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&indexDesc, &indexData, occluderIndices.put()));
		Util::SetResourceName(occluderIndices.get(), "PBRWater::SkylightingOccluderIndices");

		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthDesc, occluderDepthState.put()));
		Util::SetResourceName(occluderDepthState.get(), "PBRWater::SkylightingOccluderDepth");

		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, occluderRasterizer.put()));
		Util::SetResourceName(occluderRasterizer.get(), "PBRWater::SkylightingOccluderRasterizer");

		occluderCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<OccluderCB>(), "PBRWater::SkylightingOccluderCB");
		CompileOccluder();
	}
	ocean.SetupResources();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = SurfaceNoiseSize;
		desc.Height = SurfaceNoiseSize;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		noiseTexture = std::make_unique<Texture2D>(desc, "PBRWater::SurfaceNoise");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		noiseTexture->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		noiseTexture->CreateUAV(uavDesc);
	}
	CompileSurfaceNoise();

	// The foam texture takes a few hundred milliseconds to bake: off the render thread, uploaded when done.
	if (!foamTexture && !foamBake.valid())
		foamBake = std::async(std::launch::async, [] { return PBRWaterFoam::Bake(); });
}

void PBRWater::ClearShaderCache()
{
	ripples.ClearShaderCache();
	swash.ClearShaderCache();
	CompileOccluder();
	CompileSurfaceNoise();
	ocean.ClearShaderCache();
	oceanReady.store(ocean.IsReady(), std::memory_order_release);
	tessellationFailureLogged = false;
}

void PBRWater::Reset()
{
	frameDataFrame = UINT32_MAX;
}

void PBRWater::UpdateFoamTexture()
{
	if (foamTexture || !foamBake.valid() || foamBake.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	std::vector<PBRWaterFoam::Level> levels;
	try {
		levels = foamBake.get();
	} catch (...) {
		logger::warn("[PBR Water] Failed to bake the foam texture; the water draws no foam pattern");
		return;
	}
	if (levels.empty())
		return;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = levels[0].size;
	desc.Height = levels[0].size;
	desc.MipLevels = static_cast<UINT>(levels.size());
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
	for (size_t i = 0; i < levels.size(); ++i) {
		init[i].pSysMem = levels[i].texels.data();
		init[i].SysMemPitch = levels[i].size;
	}
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, init.data(), texture.put()))) {
		logger::warn("[PBR Water] Failed to create the foam texture");
		return;
	}
	foamTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::FoamPattern");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = desc.MipLevels;
	foamTexture->CreateSRV(srvDesc);
}

void PBRWater::UpdateFetchTexture()
{
	const uint32_t generation = environment.GetFetchGeneration();
	if (generation == uploadedFetchGeneration)
		return;
	uploadedFetchGeneration = generation;

	auto field = environment.GetFetch();
	uploadedFetch = field;
	fetchTexture.reset();
	if (!field || !field->Valid())
		return;

	using PBRWaterModel::FetchField;
	std::vector<uint16_t> halfData(field->kilometres.size());
	for (size_t i = 0; i < halfData.size(); ++i)
		halfData[i] = DirectX::PackedVector::XMConvertFloatToHalf(field->kilometres[i]);

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = field->width;
	desc.Height = field->height;
	desc.MipLevels = 1;
	desc.ArraySize = FetchField::Directions;
	desc.Format = DXGI_FORMAT_R16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	std::array<D3D11_SUBRESOURCE_DATA, FetchField::Directions> init{};
	for (uint32_t d = 0; d < FetchField::Directions; ++d) {
		init[d].pSysMem = &halfData[static_cast<size_t>(d) * field->width * field->height];
		init[d].SysMemPitch = static_cast<UINT>(field->width * sizeof(uint16_t));
	}

	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, init.data(), texture.put()))) {
		logger::warn("[PBR Water] Failed to create the fetch texture");
		return;
	}
	fetchTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::Fetch");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.ArraySize = FetchField::Directions;
	fetchTexture->CreateSRV(srvDesc);
}

void PBRWater::UpdateTerrainTexture()
{
	const uint32_t generation = environment.GetBathymetryGeneration();
	if (generation == uploadedTerrainGeneration)
		return;
	uploadedTerrainGeneration = generation;

	auto grid = environment.GetBathymetry();
	uploadedTerrain = grid;
	terrainTexture.reset();
	if (!grid || !grid->Valid())
		return;

	// 16 bits over the land's range: about 2 cm, and half the memory of floats for a worldspace of 12 million vertices.
	std::vector<uint16_t> texels(grid->heights.size());
	const float scale = 65535.0f / std::max(grid->maxZ - grid->minZ, 1.0f);
	std::transform(grid->heights.begin(), grid->heights.end(), texels.begin(), [&](float z) { return static_cast<uint16_t>((z - grid->minZ) * scale + 0.5f); });

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = grid->width;
	desc.Height = grid->height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init{ texels.data(), static_cast<UINT>(grid->width * sizeof(uint16_t)), 0 };
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
		logger::warn("[PBR Water] Failed to create the terrain texture");
		uploadedTerrain.reset();
		return;
	}
	terrainTexture = std::make_unique<Texture2D>(texture.detach(), "PBRWater::Terrain");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	terrainTexture->CreateSRV(srvDesc);
}

// ============================================================================
// Per-frame (render thread)
// ============================================================================

uint32_t PBRWater::WaveResolutionSize() const
{
	return 128u << static_cast<uint32_t>(std::clamp(settings.WaveResolution, 0, 2));
}

void PBRWater::Prepass()
{
	PerfScope perf("PBR Water - Simulation");
	UpdateFoamTexture();
	UpdateFetchTexture();
	UpdateTerrainTexture();

	// Last frame's water draws leave our textures bound; the simulations below write them.
	{
		auto context = globals::d3d::context;
		ID3D11ShaderResourceView* nullSrvs[WaterSRVCount] = {};
		context->VSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->HSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->DSSetShaderResources(110, WaterSRVCount, nullSrvs);
		context->PSSetShaderResources(110, WaterSRVCount, nullSrvs);
	}
	drawResourcesFrame = UINT32_MAX;
	UpdateShoreTextures();

	const float dt = renderDelta.load(std::memory_order_acquire);
	if (const auto snap = snapshot.load(std::memory_order_acquire)) {
		OceanSimulation::Settings oceanSettings;
		oceanSettings.resolution = WaveResolutionSize();
		PerfScope oceanPerf("PBR Water - Ocean FFT");
		ocean.Update(*snap, oceanSettings);
	}
	oceanReady.store(ocean.IsReady(), std::memory_order_release);

	if (settings.EnableRipples) {
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		RippleSimulation::Settings rippleSettings;
		rippleSettings.extent = Tuning::RippleExtent * UnitsPerMetre;
		rippleSettings.waveSpeed = Tuning::RippleSpeed;
		rippleSettings.halfLife = Tuning::RippleHalfLife;
		rippleSettings.siltHalfLife = 20.0f;
		PerfScope ripplePerf("PBR Water - Ripples");
		ripples.Update(cameraPos.x, cameraPos.y, dt, rippleSettings);
	}

	UpdateFrameConstants();
	UpdateSurfaceNoise();
	UpdateSwash(dt);
	ApplyCameraWater();
}

void PBRWater::UpdateShoreTextures()
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	const auto current = snap ? snap->shoreField : nullptr;
	const auto previous = snap ? snap->shorePrevious : nullptr;
	if (current != shoreTextures.field) {
		if (previous && previous == shoreTextures.field)
			previousShoreTextures = std::move(shoreTextures);
		shoreTextures = UploadShoreField(current);
	}
	if (previous != previousShoreTextures.field)
		previousShoreTextures = UploadShoreField(previous);
}

PBRWater::ShoreTextures PBRWater::UploadShoreField(std::shared_ptr<const PBRWaterModel::ShoreField> field)
{
	ShoreTextures result;
	if (!field || !field->Valid())
		return result;

	const size_t count = static_cast<size_t>(field->width) * field->height;
	// Full precision for the travel times and distances: they become the waves' phase (omega tau, tens of radians) and the
	// nearest point of the coast, where half floats would step them visibly. The slownesses (0.05-1 s/m) and the rest
	// are fine as halves.
	std::vector<float> times(count * 4);
	std::vector<uint16_t> rays(count * 4), room(count);
	auto half = [](float v) { return DirectX::PackedVector::XMConvertFloatToHalf(v); };
	for (size_t i = 0; i < count; ++i) {
		times[i * 4 + 0] = field->travelTime[i];
		times[i * 4 + 1] = field->distance[i];
		times[i * 4 + 2] = field->along[i];
		// No water in reach: a sentinel the shaders test for (NaN would spread through the bilinear filter).
		times[i * 4 + 3] = std::isfinite(field->level[i]) ? field->level[i] : -1.0e9f;
		rays[i * 4 + 0] = half(field->slownessX[i]);
		rays[i * 4 + 1] = half(field->slownessY[i]);
		rays[i * 4 + 2] = half(field->slope[i]);
		rays[i * 4 + 3] = half(field->exposure[i]);
		room[i] = half(field->room[i]);
	}

	auto create = [&](DXGI_FORMAT format, const void* data, UINT pitch, const char* name) -> std::unique_ptr<Texture2D> {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = field->width;
		desc.Height = field->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init{ data, pitch, 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &init, texture.put()))) {
			logger::warn("[PBR Water] Failed to create the {} texture", name);
			return nullptr;
		}
		auto texture2D = std::make_unique<Texture2D>(texture.detach(), name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		texture2D->CreateSRV(srvDesc);
		return texture2D;
	};
	result.times = create(DXGI_FORMAT_R32G32B32A32_FLOAT, times.data(), static_cast<UINT>(field->width * 4 * sizeof(float)), "PBRWater::ShoreTimes");
	result.rays = create(DXGI_FORMAT_R16G16B16A16_FLOAT, rays.data(), static_cast<UINT>(field->width * 4 * sizeof(uint16_t)), "PBRWater::ShoreRays");
	result.room = create(DXGI_FORMAT_R16_FLOAT, room.data(), static_cast<UINT>(field->width * sizeof(uint16_t)), "PBRWater::ShoreRoom");
	if (result.times && result.rays && result.room)
		result.field = std::move(field);
	else
		result = {};
	return result;
}

void PBRWater::UpdateSwash(float dt)
{
	if (!gpuBuffer || frameData.Shore0.x < 0.5f) {
		frameData.Swash0 = {};
		return;
	}

	// The swash steps with this frame's wave constants and textures.
	PerfScope perf("PBR Water - Swash");
	auto context = globals::d3d::context;
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	context->CSSetConstantBuffers(7, 1, cbs);
	ID3D11Buffer* frameBuffers[1] = { *globals::game::perFrame.get() };
	context->CSSetConstantBuffers(12, 1, frameBuffers);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->CSSetShaderResources(110, 13, srvs);  // t110-t122: the swash's own history is bound by the simulation
	context->CSSetShaderResources(124, 1, &srvs[14]);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->CSSetSamplers(12, 2, samplers);

	const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
	ShoreSimulation::Settings swashSettings;
	// The swash runs wherever the waves are still at full height (about a metre per texel at the default fade).
	swashSettings.extent = std::clamp(2.0f * Tuning::DisplacementFadeStart, 8960.0f, 32768.0f);
	swash.Update(cameraPos.x, cameraPos.y, dt, swashSettings);

	ID3D11ShaderResourceView* nullSrvs[13] = {};
	context->CSSetShaderResources(110, 13, nullSrvs);
	context->CSSetShaderResources(124, 1, nullSrvs);
	ID3D11Buffer* nullCbs[1] = { nullptr };
	context->CSSetConstantBuffers(7, 1, nullCbs);
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	context->CSSetSamplers(12, 2, nullSamplers);

	// The draws read the wet state the simulation just wrote.
	frameData.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), swash.IsReady() ? 1.0f : 0.0f };

	// Lit terrain and objects read the wet sand, the foam on it and the water height from it.
	ID3D11ShaderResourceView* wetSrv = swash.IsReady() ? swash.GetWetSRV() : nullptr;
	context->PSSetShaderResources(123, 1, &wetSrv);
}

PBRWater::SharedSettings PBRWater::GetCommonBufferData() const
{
	SharedSettings data{};
	if (!loaded)
		return data;
	if (frameData.Swash0.w > 0.5f) {
		data.SwashGrid = frameData.Swash0;
		data.Wetness = { settings.WetShores ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	}
	// Drawn this frame or the last (the reflection passes update the shared data before Skylighting's probes).
	const bool occluding = occluderFrame != UINT32_MAX && globals::state->frameCount - occluderFrame <= 1;
	data.Skylighting = { occluding ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	return data;
}

void PBRWater::CompileSurfaceNoise()
{
	noiseCS = nullptr;
	if (auto* cs = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SurfaceNoiseCS.hlsl", {}, "cs_5_0")))
		noiseCS.attach(cs);
	else
		logger::warn("[PBR Water] Surface noise shader failed to compile; the vertices evaluate the noise themselves");
}

void PBRWater::UpdateSurfaceNoise()
{
	if (!gpuBuffer || !noiseCS || !noiseTexture || frameData.Noise0.z <= 0.0f) {
		frameData.Noise0.w = 0.0f;
		return;
	}
	PerfScope perf("PBR Water - Surface Noise");
	auto context = globals::d3d::context;
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);
	ID3D11Buffer* cbs[1] = { gpuBuffer->CB() };
	ID3D11UnorderedAccessView* uav = noiseTexture->uav.get();
	context->CSSetConstantBuffers(7, 1, cbs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(noiseCS.get(), nullptr, 0);
	context->Dispatch(SurfaceNoiseSize / 8, SurfaceNoiseSize / 8, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(7, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
	// The draws read what was just baked.
	frameData.Noise0.w = 1.0f;
}

void PBRWater::CompileOccluder()
{
	occluderVS = nullptr;
	if (auto* vs = reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SkylightingOccluderVS.hlsl", {}, "vs_5_0")))
		occluderVS.attach(vs);
	else
		logger::warn("[PBR Water] Skylighting occluder failed to compile; the waves do not occlude the sky");
}

void PBRWater::DrawSkylightingOccluder(Texture2D& occlusion, const REX::W32::XMFLOAT4X4& occlusionViewProj)
{
	ID3D11DepthStencilView* dsv = occlusion.dsv.get();
	if (!loaded || !dsv || !occluderVS || !occluderIndices || !occluderCB || !occluderDepthState || !occluderRasterizer || !gpuBuffer || !globals::shaderCache->IsEnabled())
		return;
	// Calm water is the flat plane the probes are lifted above anyway: only waves need drawing.
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap || !snap->exterior || (frameData.Params0.x < 0.5f && frameData.Shore0.x < 0.5f))
		return;
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;
	PerfScope perf("PBR Water - Skylighting Occluder");

	// The still water of the cells around the eye, as SharedData::GetWaterData sees them.
	OccluderCB cb{};
	cb.occlusionViewProj = occlusionViewProj;
	const auto eye = Util::GetEyePosition();
	const float eyeCellX = std::floor(eye.x / CellSize);
	const float eyeCellY = std::floor(eye.y / CellSize);
	cb.eye = { eye.x, eye.y, eye.z, OccluderExtent };
	cb.grid = { static_cast<float>(OccluderQuads), OccluderExtent / OccluderQuads, eyeCellX, eyeCellY };
	bool anyWater = false;
	for (int32_t j = -2; j <= 2; ++j) {
		for (int32_t i = -2; i <= 2; ++i) {
			float level = -1.0e9f;
			const RE::NiPoint3 centre{ (eyeCellX + static_cast<float>(i) + 0.5f) * CellSize, (eyeCellY + static_cast<float>(j) + 0.5f) * CellSize, eye.z };
			if (auto* cell = tes->GetCell(centre)) {
				if (const float height = cell->GetExteriorWaterHeight(); height > -1.0e6f) {
					level = height;
					anyWater = true;
				}
			}
			cb.cellLevels[(i + 2) + 5 * (j + 2)] = { level, 0.0f, 0.0f, 0.0f };
		}
	}
	if (!anyWater)
		return;
	occluderCB->Update(cb);
	GpuData d = frameData;
	d.Tess0.x = 0.0f;
	gpuBuffer->Update(d);

	auto context = globals::d3d::context;

	// Save what this draw changes.
	winrt::com_ptr<ID3D11InputLayout> savedLayout;
	D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
	winrt::com_ptr<ID3D11Buffer> savedIndices;
	DXGI_FORMAT savedIndexFormat{};
	UINT savedIndexOffset = 0;
	context->IAGetInputLayout(savedLayout.put());
	context->IAGetPrimitiveTopology(&savedTopology);
	context->IAGetIndexBuffer(savedIndices.put(), &savedIndexFormat, &savedIndexOffset);
	winrt::com_ptr<ID3D11VertexShader> savedVS;
	winrt::com_ptr<ID3D11HullShader> savedHS;
	winrt::com_ptr<ID3D11DomainShader> savedDS;
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	winrt::com_ptr<ID3D11PixelShader> savedPS;
	context->VSGetShader(savedVS.put(), nullptr, nullptr);
	context->HSGetShader(savedHS.put(), nullptr, nullptr);
	context->DSGetShader(savedDS.put(), nullptr, nullptr);
	context->GSGetShader(savedGS.put(), nullptr, nullptr);
	context->PSGetShader(savedPS.put(), nullptr, nullptr);
	ID3D11Buffer* savedCB0 = nullptr;
	ID3D11Buffer* savedCB7 = nullptr;
	ID3D11Buffer* savedCB12 = nullptr;
	context->VSGetConstantBuffers(0, 1, &savedCB0);
	context->VSGetConstantBuffers(7, 1, &savedCB7);
	context->VSGetConstantBuffers(12, 1, &savedCB12);
	ID3D11ShaderResourceView* savedFlowmap = nullptr;
	ID3D11ShaderResourceView* savedSRVs[WaterSRVCount] = {};
	context->VSGetShaderResources(8, 1, &savedFlowmap);
	context->VSGetShaderResources(110, WaterSRVCount, savedSRVs);
	ID3D11SamplerState* savedSamplers[2] = {};
	context->VSGetSamplers(12, 2, savedSamplers);
	winrt::com_ptr<ID3D11RasterizerState> savedRasterizer;
	context->RSGetState(savedRasterizer.put());
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	context->RSGetViewports(&viewportCount, savedViewports);
	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepthState;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepthState.put(), &savedStencilRef);

	// Depth only, with Skylighting's own transform.
	context->OMSetRenderTargets(0, nullptr, dsv);
	context->OMSetDepthStencilState(occluderDepthState.get(), 0);
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(occlusion.desc.Width), static_cast<float>(occlusion.desc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(occluderRasterizer.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetIndexBuffer(occluderIndices.get(), DXGI_FORMAT_R16_UINT, 0);
	context->VSSetShader(occluderVS.get(), nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* cb0 = occluderCB->CB();
	ID3D11Buffer* cb7 = gpuBuffer->CB();
	ID3D11Buffer* cb12 = *globals::game::perFrame.get();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->VSSetConstantBuffers(7, 1, &cb7);
	context->VSSetConstantBuffers(12, 1, &cb12);
	ID3D11ShaderResourceView* srvs[WaterSRVCount];
	GatherWaterSRVs(d, srvs);
	context->VSSetShaderResources(110, WaterSRVCount, srvs);
	ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
	context->VSSetShaderResources(8, 1, &flowmap);
	ID3D11SamplerState* samplers[2] = { linearClampSampler.get(), oceanSampler.get() };
	context->VSSetSamplers(12, 2, samplers);

	context->DrawIndexed(OccluderQuads * OccluderQuads * 6, 0, 0);
	drawResourcesFrame = UINT32_MAX;

	// Restore.
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	context->OMSetDepthStencilState(savedDepthState.get(), savedStencilRef);
	context->RSSetViewports(viewportCount, savedViewports);
	context->RSSetState(savedRasterizer.get());
	context->IASetInputLayout(savedLayout.get());
	context->IASetPrimitiveTopology(savedTopology);
	context->IASetIndexBuffer(savedIndices.get(), savedIndexFormat, savedIndexOffset);
	context->VSSetShader(savedVS.get(), nullptr, 0);
	context->HSSetShader(savedHS.get(), nullptr, 0);
	context->DSSetShader(savedDS.get(), nullptr, 0);
	context->GSSetShader(savedGS.get(), nullptr, 0);
	context->PSSetShader(savedPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &savedCB0);
	context->VSSetConstantBuffers(7, 1, &savedCB7);
	context->VSSetConstantBuffers(12, 1, &savedCB12);
	context->VSSetShaderResources(8, 1, &savedFlowmap);
	context->VSSetShaderResources(110, WaterSRVCount, savedSRVs);
	context->VSSetSamplers(12, 2, savedSamplers);
	auto release = [](auto*& p) {
		if (p)
			p->Release();
		p = nullptr;
	};
	release(savedCB0);
	release(savedCB7);
	release(savedCB12);
	release(savedFlowmap);
	for (auto*& srv : savedSRVs)
		release(srv);
	for (auto*& sampler : savedSamplers)
		release(sampler);
	for (auto*& rtv : savedRTVs)
		release(rtv);
	release(savedDSV);

	occluderFrame = globals::state->frameCount;
}

void PBRWater::UpdateFrameConstants()
{
	const uint32_t frame = globals::state->frameCount;
	if (frame == frameDataFrame)
		return;
	const uint32_t lastFrame = frameDataFrame;  // frameData still holds that frame's constants
	frameDataFrame = frame;

	GpuData d{};
	const auto snap = snapshot.load(std::memory_order_acquire);

	if (snap && snap->ocean) {
		const auto& o = *snap->ocean;
		const uint32_t size = ocean.GetResolution() ? ocean.GetResolution() : WaveResolutionSize();
		for (uint32_t c = 0; c < PBRWaterModel::NumCascades; ++c) {
			// Tile coordinate of the reference camera, in double precision: the GPU only adds camera-relative offsets.
			const double tile = PBRWaterModel::CascadeLength[c] * PBRWaterModel::UnitsPerMetre;
			double u = snap->refX / tile;
			double v = snap->refY / tile;
			u -= std::floor(u);
			v -= std::floor(v);
			// FFT sample j sits at j * tile / size, i.e. at texel centres shifted by half a texel.
			const double halfTexel = 0.5 / size;
			const auto& band = o.cascades[c];
			d.Cascade0[c] = { static_cast<float>(1.0 / tile), static_cast<float>(u + halfTexel), static_cast<float>(v + halfTexel), static_cast<float>(tile / size) };
			d.Cascade1[c] = { band.meanOmega, 1.0f / std::max(band.pmWeightOpenSea, 1e-4f), band.meanK / UnitsPerMetre, o.choppiness * o.choppiness * band.meanSquareSlope };
			// Height spread (depth-limited breaking) and deep-water phase speed of the band.
			d.Cascade2[c] = { std::sqrt(std::max(band.variance, 0.0f)) * UnitsPerMetre, static_cast<float>(PBRWaterModel::Gravity) / std::max(band.meanOmega, 1e-3f) * UnitsPerMetre, 0.0f, 0.0f };
		}
		const bool active = ocean.IsReady() && !o.calm;
		d.Params0 = { active ? 1.0f : 0.0f, o.windSpeed, o.peakOmega, static_cast<float>(PBRWaterModel::Gravity) * UnitsPerMetre };
		d.Params1 = { std::cos(snap->windDirection), std::sin(snap->windDirection), Tuning::DisplacementFadeStart, Tuning::DisplacementFadeEnd };
		d.Params2 = { o.choppiness, 0.0f, active ? o.displacementBound : 0.0f, static_cast<float>(std::max(ocean.GetMipCount(), 1u) - 1) };
		d.RefCamPos = { static_cast<float>(snap->refX), static_cast<float>(snap->refY), static_cast<float>(snap->refZ), 0.0f };
		// Shore waves need the coastline maps the snapshot blends between on the GPU. The clock runs at this frame's
		// (eased) frequency, but each map's lag omega tau uses the frequency it was traced for: with the eased one, tau
		// (up to a minute far out) turned every change of the sea into crests racing or stalling until the frequency
		// settled. A map is rebuilt when they differ by more than a few percent.
		const auto& shore = snap->shore;
		const bool shoreReady = shore.enabled && snap->shoreField && shoreTextures.field == snap->shoreField;
		const bool blending = shoreReady && snap->shoreBlend < 1.0f && snap->shorePrevious && previousShoreTextures.field == snap->shorePrevious;
		d.Shore0 = { shoreReady ? 1.0f : 0.0f, blending ? snap->shoreBlend : 1.0f, 0.0f, shore.enabled ? shore.maxRunup : 0.0f };
		d.Shore1 = { 0.0f, 0.0f, shore.swellHeight, shore.windSeaHeight };
		// The clock now and as last frame drew it (motion vectors); the first frame has no motion.
		const float shoreClock = static_cast<float>(shore.phase);
		const bool consecutive = lastFrame != UINT32_MAX && frame == lastFrame + 1 && frameData.Shore2.w > 0.0f;
		d.Shore2 = { shoreClock, consecutive ? frameData.Shore2.x : shoreClock, shore.rate, shore.period };
		d.Shore3 = { 0.0f, 0.0f, 0.0f, shore.deepLength };
		d.Shore4 = { shoreReady ? snap->shoreField->omega : shore.omega, blending ? snap->shorePrevious->omega : shore.omega, 0.0f, 0.0f };
		auto fieldMap = [](const PBRWaterModel::ShoreField& f) {
			return float4{ f.minX - 0.5f * f.step, f.minY - 0.5f * f.step, 1.0f / (static_cast<float>(f.width) * f.step), 1.0f / (static_cast<float>(f.height) * f.step) };
		};
		if (shoreReady) {
			d.ShoreMap = fieldMap(*snap->shoreField);
			if (blending)
				d.Shore6 = fieldMap(*snap->shorePrevious);
			// Last frame's wet grid, until this frame's step (UpdateSwash) replaces it.
			d.Swash0 = { swash.GetOriginX(), swash.GetOriginY(), 1.0f / swash.GetExtent(), swash.IsReady() ? 1.0f : 0.0f };
		}

		// Fetch
		if (uploadedFetch && fetchTexture && snap->exterior) {
			const float originX = uploadedFetch->minCellX * CellSize;
			const float originY = uploadedFetch->minCellY * CellSize;
			d.Fetch0 = { originX, originY, 1.0f / (uploadedFetch->width * CellSize), 1.0f / (uploadedFetch->height * CellSize) };
			float slice = snap->windDirection / TwoPi;
			slice = (slice - std::floor(slice)) * PBRWaterModel::FetchField::Directions;
			d.Fetch1 = { 1.0f, static_cast<float>(PBRWaterModel::FetchField::Directions), slice, uploadedFetch->openSeaKm * 1000.0f };
		} else {
			d.Fetch1 = { 0.0f, 0.0f, 0.0f, snap->exterior ? 500000.0f : InteriorFetchMetres };
		}
	}

	// The worldspace's terrain: a texel per vertex, so texel (0, 0)'s centre lies on the grid's first vertex.
	if (snap && snap->exterior && uploadedTerrain && terrainTexture) {
		const auto& t = *uploadedTerrain;
		const float scaleX = 1.0f / (static_cast<float>(t.width) * t.stepX);
		const float scaleY = 1.0f / (static_cast<float>(t.height) * t.stepY);
		d.Terrain0 = { scaleX, scaleY, 0.5f / static_cast<float>(t.width) - t.minX * scaleX, 0.5f / static_cast<float>(t.height) - t.minY * scaleY };
		d.Terrain1 = { t.minZ, t.maxZ, 1.0f, t.stepX };
	}

	// Tessellation: on-screen size uses the internal render resolution, so it scales with upscalers.
	float renderHeight = static_cast<float>(globals::game::graphicsState->screenHeight);
	if (globals::features::upscaling.loaded)
		renderHeight *= globals::features::upscaling.resolutionScale.y;
	const float projY = std::abs(globals::game::frameBufferCached.GetCameraProj()._22);
	d.Tess0 = { 0.0f, settings.TessellationTriangleSize, Tuning::TessellationMaxFactor, 0.5f * renderHeight * projY };
	d.Draw0 = { 0.0f, static_cast<float>(settings.WireframeMode), static_cast<float>(settings.DebugView), 0.0f };
	// The loaded cells: their water meets the flat LOD water at the grid's border, so the waves fade out before it.
	// Read from the cells the grid holds: TES::currentGridX/Y lag the grid (seen two cells off its centre with the
	// player on its edge), which faded the waves out over loaded water along straight cell lines.
	d.Grid0 = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (const auto tes = RE::TES::GetSingleton(); tes && tes->gridCells && snap && snap->exterior) {
		const auto* grid = tes->gridCells;
		int32_t lo[2] = { INT32_MAX, INT32_MAX };
		int32_t hi[2] = { INT32_MIN, INT32_MIN };
		auto scan = [&]() {
			for (uint32_t x = 0; x < grid->length; ++x) {
				for (uint32_t y = 0; y < grid->length; ++y) {
					auto* cell = grid->GetCell(x, y);
					const auto* coords = cell && cell->IsExteriorCell() ? cell->GetCoordinates() : nullptr;
					if (!coords)
						continue;
					lo[0] = std::min(lo[0], coords->cellX);
					lo[1] = std::min(lo[1], coords->cellY);
					hi[0] = std::max(hi[0], coords->cellX);
					hi[1] = std::max(hi[1], coords->cellY);
				}
			}
		};
		if (REL::Module::IsAE() && REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
			// Skyrim 1.7 guards the grid's cell lookups with this lock (as UnifiedWater does).
			static REL::Relocation<RE::BSReadWriteLock*> terrainCellMapLock{ REL::ID(564236) };
			const RE::BSReadLockGuard lock(*terrainCellMapLock);
			scan();
		} else {
			scan();
		}
		if (lo[0] <= hi[0])
			d.Grid0 = { static_cast<float>(lo[0]) * CellSize, static_cast<float>(lo[1]) * CellSize,
				static_cast<float>(hi[0] + 1) * CellSize, static_cast<float>(hi[1] + 1) * CellSize };
	}

	if (settings.EnableRipples && ripples.IsReady()) {
		d.Ripple0 = { ripples.GetOriginX(), ripples.GetOriginY(), 1.0f / ripples.GetExtent(), 1.0f };
		d.Ripple1 = { Tuning::RippleHeight * UnitsPerMetre, Tuning::RippleNormalStrength, 0.0f, 1.0f / RippleSimulation::GridSize };
		d.Ripple2 = { ripples.GetPreviousOriginX(), ripples.GetPreviousOriginY(), ripples.GetStepAlpha(), ripples.GetPreviousStepAlpha() };
	}

	d.Light0 = { settings.Roughness, Tuning::Subsurface, Tuning::VanillaFresnel, Tuning::SunSpecular };
	d.Light1 = { settings.Visibility, 0.0f, Tuning::RefractionDistortion, Tuning::PointLightSpecular };
	// Where the waves break.
	d.Foam1 = { snap && snap->ocean ? snap->ocean->foamDeviations : -100.0f, 0.0f, 0.0f, Tuning::RiverWaveDamping };

	// Water clarity and the light in the water.
	d.Clarity0 = { Tuning::SedimentDensity / UnitsPerMetre, settings.Turbidity, Tuning::TurbidityPatchiness, Tuning::TurbidityPatchSize * UnitsPerMetre };
	d.Clarity1 = { settings.SedimentColor.x, settings.SedimentColor.y, settings.SedimentColor.z, Tuning::ShoreResuspension };
	d.Clarity2 = { Tuning::RiverTurbidity, weatherTurbidity.load(std::memory_order_acquire), settings.EnableRipples ? Tuning::WadingSilt : 0.0f, Tuning::SedimentLayerHeight * UnitsPerMetre };
	// Sediment plumes drift and reshape over hours; a daily wrap keeps their coordinates precise.
	d.Optics0 = { Tuning::ScatteringAnisotropy, Tuning::DownwellingAttenuation, std::fmod(globals::state->timer, 86400.0f), (snap && snap->ocean) ? snap->ocean->significantHeight : 0.0f };

	// Wind and rain on the surface.
	d.Surface0 = { Tuning::WindRoughness, Tuning::Gusts, Tuning::GustSize * UnitsPerMetre, rainIntensity.load(std::memory_order_acquire) };
	{
		// The surface noise bake around the camera (UpdateSurfaceNoise): four texels across a cell of the noise's finest
		// octave (the gusts' second, half their size; the sediment patches' third, a quarter of theirs), between 0.5 m and
		// 16 m. 128^2 texels cost what as many vertices did, and cover the tessellated water (640 m at the defaults). In
		// whole texels, so the samples stay put as the camera moves.
		const float finest = std::min(Tuning::GustSize / 2.03f, Tuning::TurbidityPatchSize / 4.12f) * UnitsPerMetre;
		const float texel = std::clamp(finest / 4.0f, 0.5f * UnitsPerMetre, 16.0f * UnitsPerMetre);
		const auto& cameraPos = globals::game::frameBufferCached.GetCameraPosAdjust();
		const float half = 0.5f * static_cast<float>(SurfaceNoiseSize);
		const float originX = (std::floor(cameraPos.x / texel) - half) * texel;
		const float originY = (std::floor(cameraPos.y / texel) - half) * texel;
		// w is set once this frame's bake has run.
		d.Noise0 = { originX, originY, 1.0f / (texel * static_cast<float>(SurfaceNoiseSize)), 0.0f };
	}
	d.Surface1 = { Tuning::WindStreaks, 0.0f, Tuning::FetchScale, 0.0f };

	// The worldspace flowmap calms waves on rivers, looked up by position in every water pass.
	worldFlowmap = nullptr;
	if (globals::features::unifiedWater.GetWorldFlowmap(worldFlowmap, d.Flow0) && !worldFlowmap)
		d.Flow0 = {};

	frameData = d;
}

// ============================================================================
// Per-draw (render thread)
// ============================================================================

void PBRWater::SetupDraw(RE::BSShader* waterShader, RE::BSRenderPass* pass)
{
	if (!gpuBuffer || !waterShader || !pass || !pass->geometry)
		return;

	auto context = globals::d3d::context;
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;

	UpdateFrameConstants();

	const uint32_t vertexDescriptor = state->modifiedVertexDescriptor;
	const uint32_t technique = WaterTechnique(vertexDescriptor);
	const bool waves = WavesReach(*pass->geometry);

	const auto snap = snapshot.load(std::memory_order_acquire);
	const bool hasWaves = waves && snap && ((snap->ocean && !snap->ocean->calm && ocean.IsReady()) || snap->shore.enabled || settings.EnableRipples);

	// Our hull/domain shaders only match our own vertex and pixel shaders of the same descriptor. While
	// those are still compiling the vanilla shaders are bound, and tessellating them would break the
	// stage signatures, so only tessellate once both are live.
	auto customShadersLive = [&] {
		return shaderCache->IsEnabled() && shaderCache->GetVertexShader(*waterShader, vertexDescriptor) &&
		       shaderCache->GetPixelShader(*waterShader, state->modifiedPixelDescriptor);
	};

	ID3D11HullShader* hullShader = nullptr;
	ID3D11DomainShader* domainShader = nullptr;
	if (settings.EnableTessellation && hasWaves && IsTessellatedTechnique(technique) && customShadersLive()) {
		hullShader = shaderCache->GetHullShader(*waterShader, vertexDescriptor);
		domainShader = shaderCache->GetDomainShader(*waterShader, vertexDescriptor);
		// Async compiles are queued on first request, so only a shader still missing well after the
		// compile queue drained is a real failure.
		if ((hullShader && domainShader) || shaderCache->IsCompiling()) {
			tessellationMissingSince = UINT32_MAX;
		} else if (tessellationMissingSince == UINT32_MAX) {
			tessellationMissingSince = state->frameCount;
		} else if (state->frameCount - tessellationMissingSince > 300 && !tessellationFailureLogged) {
			tessellationFailureLogged = true;
			logger::warn("[PBR Water] Hull/domain shader unavailable for water descriptor {:X}; drawing without tessellation", vertexDescriptor);
		}
	}
	const bool tessellate = hullShader && domainShader;

	ID3D11GeometryShader* geometryShader = nullptr;
	if (settings.WireframeMode > 0 && !IsStencilTechnique(technique) && customShadersLive())
		geometryShader = shaderCache->GetGeometryShader(*waterShader, vertexDescriptor);

	// Vertex spacing of this mesh, for filtering waves the untessellated grid cannot carry.
	float spacing = 0.0f;
	uint32_t triangleCount = 0;
	if (const auto triShape = pass->geometry->AsTriShape()) {
		triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
		if (hasWaves && !tessellate && triangleCount)
			spacing = pass->geometry->worldBound.radius * std::numbers::sqrt2_v<float> / std::max(std::sqrt(triangleCount * 0.5f), 1.0f);
	}

	// What the tessellator is given (Debug section): its patches are the meshes' own triangles.
	if (state->frameCount != drawStatsFrame) {
		lastDrawStats = drawStats;
		drawStats = {};
		drawStatsFrame = state->frameCount;
	}
	++drawStats.draws;
	if (tessellate) {
		++drawStats.tessellated;
		drawStats.patches += triangleCount;
	}

	// Most water is flat distant tiles, hundreds of them: anything done per draw is paid hundreds of times per pass. The
	// constants differ between draws only in whether they tessellate and, without tessellation, the spacing: both kinds
	// are uploaded once and swapped by binding the other buffer.
	GpuData& d = frameData;
	if (std::memcmp(&uploadedDrawData, &d, sizeof(GpuData)) != 0) {
		uploadedDrawData = d;
		GpuData drawData = d;
		drawData.Tess0.x = 1.0f;
		drawBuffer->Update(drawData);
		drawData.Tess0.x = 0.0f;
		flatDrawBuffer->Update(drawData);
		GatherWaterSRVs(d, drawSRVs);
		drawResourcesFrame = UINT32_MAX;
	}
	ID3D11Buffer* cb = tessellate ? drawBuffer->CB() : flatDrawBuffer->CB();
	if (spacing > 0.0f) {
		GpuData drawData = d;
		drawData.Tess0.x = 0.0f;
		drawData.Draw0.x = spacing;
		gpuBuffer->Update(drawData);
		cb = gpuBuffer->CB();
	}
	context->VSSetConstantBuffers(7, 1, &cb);
	context->PSSetConstantBuffers(7, 1, &cb);

	// Nothing else binds these slots outside the pixel stage, where Landscape Seams shares t110 and t111.
	ID3D11SamplerState* linearClamp = linearClampSampler.get();
	ID3D11SamplerState* samplers[2] = { linearClamp, oceanSampler.get() };
	if (drawResourcesFrame != state->frameCount) {
		drawResourcesFrame = state->frameCount;
		ID3D11ShaderResourceView* flowmap = d.Flow0.x != 0.0f ? worldFlowmap : nullptr;
		context->VSSetShaderResources(110, WaterSRVCount, drawSRVs);
		context->HSSetShaderResources(110, WaterSRVCount, drawSRVs);
		context->DSSetShaderResources(110, WaterSRVCount, drawSRVs);
		context->PSSetShaderResources(110, WaterSRVCount, drawSRVs);
		context->VSSetSamplers(12, 2, samplers);
		context->HSSetSamplers(12, 2, samplers);
		context->DSSetSamplers(12, 2, samplers);
		// The vertex program reads the worldspace flowmap to calm waves on rivers.
		context->VSSetShaderResources(8, 1, &flowmap);
		context->DSSetShaderResources(8, 1, &flowmap);
		context->VSSetSamplers(8, 1, &linearClamp);
		context->DSSetSamplers(8, 1, &linearClamp);
	} else {
		ID3D11ShaderResourceView* bound[2] = {};
		context->PSGetShaderResources(110, 2, bound);
		if (bound[0] != drawSRVs[0] || bound[1] != drawSRVs[1])
			context->PSSetShaderResources(110, WaterSRVCount, drawSRVs);
		for (auto* view : bound) {
			if (view)
				view->Release();
		}
	}
	context->PSSetSamplers(12, 2, samplers);

	// The renderer binds textures and constant buffers lazily, right before the draw call, so the
	// device still holds the previous draw's state here: read this draw's from the shadow state.
	auto& shadow = globals::game::shadowState->GetRuntimeData();
	// That flush also re-binds every sampler slot the renderer has marked modified (Lighting uses 12-13),
	// which would replace ours with a point or wrap sampler depending on what was drawn before: drop those
	// pending updates for this draw. RestoreDraw() marks them again so the next draw gets the game's.
	shadow.PSSamplerModifiedBits &= ~(3u << 12);

	if (tessellate) {
		// The domain shader runs the full vertex program: give it everything the vertex shader has.
		ID3D11Buffer* vsBuffers[3] = {};
		if (const auto* vertexShader = shadow.currentVertexShader) {
			for (uint32_t i = 0; i < 3; ++i)
				vsBuffers[i] = reinterpret_cast<ID3D11Buffer*>(vertexShader->constantBuffers[i].buffer);
		}
		ID3D11Buffer* frameBuffer = nullptr;
		context->VSGetConstantBuffers(12, 1, &frameBuffer);
		if (!frameBuffer)
			context->PSGetConstantBuffers(12, 1, &frameBuffer);

		context->HSSetConstantBuffers(0, 3, vsBuffers);
		context->HSSetConstantBuffers(7, 1, &cb);
		context->HSSetConstantBuffers(12, 1, &frameBuffer);
		context->DSSetConstantBuffers(0, 3, vsBuffers);
		context->DSSetConstantBuffers(7, 1, &cb);
		context->DSSetConstantBuffers(12, 1, &frameBuffer);

		if (frameBuffer)
			frameBuffer->Release();

		// The renderer only re-applies its cached topology when it changes, so make sure its cache
		// says "triangle list" with nothing pending, then switch the device to patches behind it.
		// RestoreDraw() puts the device back in sync before anything else is drawn.
		shadow.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		shadow.stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_PRIMITIVE_TOPO);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

		context->HSSetShader(hullShader, nullptr, 0);
		context->DSSetShader(domainShader, nullptr, 0);
		tessellationBound = true;
	}

	if (geometryShader) {
		context->GSSetConstantBuffers(7, 1, &cb);
		context->GSSetShader(geometryShader, nullptr, 0);
		geometryShaderBound = true;
	}
}

bool PBRWater::WavesReach(const RE::NiAVObject& geometry) const
{
	// The waves fade out inside the loaded cells (LoadedGridFade) and with the distance (DisplacementDistanceFade).
	const auto& bound = geometry.worldBound;
	const float4& grid = frameData.Grid0;
	if (grid.z > grid.x && (bound.center.x + bound.radius < grid.x || bound.center.x - bound.radius > grid.z || bound.center.y + bound.radius < grid.y || bound.center.y - bound.radius > grid.w))
		return false;
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float dx = bound.center.x - eye.x;
	const float dy = bound.center.y - eye.y;
	const float reach = frameData.Params1.w + bound.radius;
	return dx * dx + dy * dy < reach * reach;
}

void PBRWater::GatherWaterSRVs(const GpuData& d, ID3D11ShaderResourceView* (&srvs)[WaterSRVCount]) const
{
	srvs[0] = settings.EnableRipples ? ripples.GetSRV() : nullptr;
	srvs[1] = fetchTexture ? fetchTexture->srv.get() : nullptr;
	srvs[2] = (d.Terrain1.z > 0.5f && terrainTexture) ? terrainTexture->srv.get() : nullptr;
	srvs[3] = settings.EnableRipples ? ripples.GetPreviousSRV() : nullptr;
	srvs[4] = shoreProfileTexture ? shoreProfileTexture->srv.get() : nullptr;
	srvs[5] = ocean.GetDisplacementSRV();
	srvs[6] = ocean.GetDerivativesSRV();
	srvs[7] = ocean.GetSurfaceSRV();
	srvs[8] = ocean.GetPreviousDisplacementSRV();
	const bool shore = d.Shore0.x > 0.5f && shoreTextures.field;
	srvs[9] = shore ? shoreTextures.times->srv.get() : nullptr;
	srvs[10] = shore ? shoreTextures.rays->srv.get() : nullptr;
	srvs[11] = nullptr;  // t121: unused
	srvs[12] = shore && d.Shore0.y < 1.0f && previousShoreTextures.field ? previousShoreTextures.times->srv.get() : nullptr;
	srvs[13] = shore && d.Swash0.w > 0.5f ? swash.GetWetSRV() : nullptr;
	srvs[14] = shore ? shoreTextures.room->srv.get() : nullptr;
	srvs[15] = foamTexture ? foamTexture->srv.get() : nullptr;
	srvs[16] = d.Noise0.w > 0.5f && noiseTexture ? noiseTexture->srv.get() : nullptr;
}

void PBRWater::RestoreDraw()
{
	auto context = globals::d3d::context;
	// Rebound by the renderer's next flush: Lighting uses these slots.
	globals::game::shadowState->GetRuntimeData().PSSamplerModifiedBits |= (3u << 12);
	if (tessellationBound) {
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		tessellationBound = false;
	}
	if (geometryShaderBound) {
		context->GSSetShader(nullptr, nullptr, 0);
		geometryShaderBound = false;
	}
}

// ============================================================================
// Main thread
// ============================================================================

PBRWaterModel::SpectrumParams PBRWater::CurrentSpectrumParams(float windSpeed, bool exterior) const
{
	PBRWaterModel::SpectrumParams params;
	params.windSpeed = windSpeed;
	params.windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	params.heightScale = settings.WaveHeight;
	params.choppiness = Tuning::Choppiness;
	params.spread = Tuning::DirectionalSpread;
	// Swell comes in from the open sea; there is none indoors.
	params.swellHeight = exterior ? settings.SwellHeight : 0.0f;
	params.swellPeriod = settings.SwellPeriod;
	params.swellDirection = params.windDirection + settings.SwellDirection * (TwoPi / 360.0f);
	params.resolution = WaveResolutionSize();
	params.foamAmount = settings.FoamAmount;
	return params;
}

void PBRWater::MainThreadUpdate()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto tes = globals::game::tes;
	if (!player || !tes)
		return;

	// Weather blends and typed-in values write straight into the settings; keep them safe (not within the sliders).
	SanitizeSettings();

	const auto ui = RE::UI::GetSingleton();
	const float dt = (ui && ui->GameIsPaused()) ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.25f);

	const auto cell = player->GetParentCell();
	const bool exterior = cell && cell->IsExteriorCell();

	// Wind from the weather system (already blended across weather transitions); still air indoors.
	float windNormalised = 0.0f;
	float windAngle = std::atan2(smoothedWindDirY, smoothedWindDirX);
	if (exterior) {
		if (const auto sky = globals::game::sky) {
			windNormalised = std::clamp(sky->windSpeed, 0.0f, 1.0f);
			windAngle = sky->windAngle;
		}
	}
	const float targetWind = exterior ? settings.WindSpeedCalm + (settings.WindSpeedStorm - settings.WindSpeedCalm) * windNormalised : 0.5f;

	// A sea takes time to respond to the wind; this also hides abrupt weather data changes.
	const float response = SmoothFactor(dt, 8.0f);
	if (smoothedWindSpeed < 0.0f) {
		smoothedWindSpeed = targetWind;
		smoothedWindDirX = std::cos(windAngle);
		smoothedWindDirY = std::sin(windAngle);
	} else {
		smoothedWindSpeed += (targetWind - smoothedWindSpeed) * response;
		smoothedWindDirX += (std::cos(windAngle) - smoothedWindDirX) * response;
		smoothedWindDirY += (std::sin(windAngle) - smoothedWindDirY) * response;
		const float len = std::max(std::sqrt(smoothedWindDirX * smoothedWindDirX + smoothedWindDirY * smoothedWindDirY), 1e-4f);
		smoothedWindDirX /= len;
		smoothedWindDirY /= len;
	}

	auto next = std::make_shared<PBRWaterModel::WaveSnapshot>();
	const auto spectrum = std::make_shared<const PBRWaterModel::OceanSpectrum>(PBRWaterModel::GenerateSpectrum(CurrentSpectrumParams(smoothedWindSpeed, exterior)));
	next->ocean = spectrum;
	next->windDirection = std::atan2(smoothedWindDirY, smoothedWindDirX);
	next->exterior = exterior;

	// Shoreline waves: the sea's dominant waves as a wave train, its clock carried on from last frame (it keeps running
	// while shore waves are off, so turning them on never starts every beach in step).
	shoreState = PBRWaterModel::UpdateShoreParams(*spectrum, hasShoreState ? &shoreState : nullptr, dt);
	hasShoreState = true;
	const bool shoreEnabled = exterior && settings.EnableShoreWaves;

	const auto eye = Util::GetMainThreadEyePosition();
	clock.Advance(dt, eye.x, eye.y, eye.z);
	clock.Fill(*next);

	// Gameplay follows the long-wave cascades through the CPU mirror, but only while the GPU renders them.
	if (oceanReady.load(std::memory_order_acquire)) {
		mirror.Update(spectrum, clock.Time());
		next->mirror0 = mirror.frame0;
		next->mirror1 = mirror.frame1;
		next->mirrorAlpha = mirror.alpha;
	} else {
		mirror.Reset();
	}

	environment.Update(exterior ? tes->GetRuntimeData2().worldSpace : nullptr);
	next->fetch = environment.GetFetch();
	next->fetchScale = Tuning::FetchScale;
	if (exterior)
		next->bathymetry = environment.GetBathymetry();
	environment.UpdateShore(eye.x, eye.y, shoreState, environment.GetFetch(), Tuning::FetchScale, shoreEnabled);
	UpdateShoreField(dt, shoreEnabled);
	next->shore = shoreState;
	next->shore.enabled = shoreEnabled;
	next->shoreField = currentShore;
	next->shorePrevious = previousShore;
	next->shoreBlend = shoreBlend;

	std::shared_ptr<const PBRWaterModel::WaveSnapshot> published = next;
	snapshot.store(published, std::memory_order_release);
	renderDelta.store(dt, std::memory_order_release);
	UpdateCameraWater(*published, exterior);

	// Storms and rain cloud the water over minutes, and it takes as long to clear.
	smoothedWeatherTurbidity += (WeatherTurbidityTarget() - smoothedWeatherTurbidity) * SmoothFactor(dt, 90.0f);
	weatherTurbidity.store(exterior ? smoothedWeatherTurbidity : 0.0f, std::memory_order_release);
	// Rain on the surface follows the precipitation within seconds.
	smoothedRain += ((exterior ? RainFraction() : 0.0f) - smoothedRain) * SmoothFactor(dt, 4.0f);
	rainIntensity.store(smoothedRain, std::memory_order_release);

	GatherInteractions(*published, dt);

	FloatingObjects::Settings floatingSettings;
	floatingSettings.enabled = settings.EnableFloatingObjects && globals::shaderCache->IsEnabled();
	floatingSettings.range = Tuning::FloatingRange * UnitsPerMetre;
	floatingSettings.maxSize = Tuning::MaxFloatingSize * UnitsPerMetre;
	floatingSettings.response = Tuning::FloatingResponse;
	floatingSettings.carryActors = settings.CarryActors;
	floatingSettings.fadeStart = Tuning::DisplacementFadeStart;
	floatingSettings.fadeEnd = Tuning::DisplacementFadeEnd;
	floating.Update(*published, floatingSettings, dt);
}

void PBRWater::UpdateShoreField(float dt, bool enabled)
{
	// Seconds a new coastline map takes to take over: long enough that a map traced for a new period or moving with the
	// player only makes the crests drift a little for a moment, never jump.
	constexpr float BlendTime = 4.0f;
	const auto latest = enabled ? environment.GetShore() : nullptr;
	if (latest != currentShore) {
		previousShore = latest && currentShore ? currentShore : nullptr;
		shoreBlend = previousShore ? 0.0f : 1.0f;
		currentShore = latest;
	}
	shoreBlend = std::min(shoreBlend + dt / BlendTime, 1.0f);
	if (shoreBlend >= 1.0f)
		previousShore = nullptr;
}

float PBRWater::WeatherTurbidityTarget() const
{
	const auto sky = globals::game::sky;
	if (!sky)
		return 0.0f;
	// Strong wind lifts sediment in the shallows and mixes it through; rain washes silt in from the land.
	const float wind = std::clamp((smoothedWindSpeed - 6.0f) / 14.0f, 0.0f, 1.0f);
	return Tuning::StormTurbidity * (wind + 0.5f * RainFraction());
}

float PBRWater::RainFraction() const
{
	const auto sky = globals::game::sky;
	if (!sky)
		return 0.0f;
	auto rainy = [](const RE::TESWeather* weather) {
		return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy) ? 1.0f : 0.0f;
	};
	const float blend = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	return rainy(sky->currentWeather) * blend + rainy(sky->lastWeather) * (1.0f - blend);
}

void PBRWater::UpdateCameraWater(const PBRWaterModel::WaveSnapshot& snap, bool exterior)
{
	cameraSurfaceValid.store(false, std::memory_order_release);
	if (!exterior || !globals::shaderCache->IsEnabled())
		return;
	auto* tes = globals::game::tes;
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!tes || !waterSystem || !waterSystem->currentWaterType || waterSystem->waterObjects.empty())
		return;
	const auto eye = Util::GetMainThreadEyePosition();
	auto* cell = tes->GetCell(eye);
	float flat = 0.0f;
	if (!cell || !TESObjectCELL_GetWaterHeight::func(cell, eye, flat) || flat <= -1e6f)
		return;
	// The water level the camera is under is the displaced surface above it, not the flat plane: the underwater
	// view and the waterline across the lens follow the waves. Out of their reach the flat plane, but still decided
	// here, by the camera (TESWaterSystem_SetUnderwater).
	float surface = flat;
	if (std::abs(eye.z - flat) <= snap.VerticalBound() + 32.0f)
		surface += snap.SampleAt(eye.x, eye.y, flat).height;
	cameraSurface.store(surface, std::memory_order_release);
	cameraSurfaceFrame.store(globals::state->frameCount, std::memory_order_release);
	cameraSurfaceValid.store(true, std::memory_order_release);
	// Through the engine's setter (the original): it also updates the global copy the water shaders' technique follows.
	TESWaterSystem_SetUnderwater::func(waterSystem, eye.z < surface, surface);
}

void PBRWater::TESWaterSystem_SetUnderwater::thunk(RE::TESWaterSystem* waterSystem, bool underwater, float height)
{
	// Out of the engine's own calls, the camera decides, against the displaced surface over it.
	auto& feature = globals::features::pbrWater;
	if (feature.cameraSurfaceValid.load(std::memory_order_acquire) &&
		globals::state->frameCount - feature.cameraSurfaceFrame.load(std::memory_order_acquire) <= 1) {
		height = feature.cameraSurface.load(std::memory_order_acquire);
		underwater = Util::GetMainThreadEyePosition().z < height;
	}
	func(waterSystem, underwater, height);
}

void PBRWater::ApplyCameraWater()
{
	if (!cameraSurfaceValid.load(std::memory_order_acquire))
		return;
	// Only this frame's state, and only while the game has water to be under: during a load the main thread stops
	// updating while the world still renders, and the engine's underwater effects then expect a water object that a
	// stale "under water" would make them look for.
	if (globals::state->frameCount - cameraSurfaceFrame.load(std::memory_order_acquire) > 1)
		return;
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!waterSystem || !waterSystem->currentWaterType || waterSystem->waterObjects.empty())
		return;
	// Again right before the world is drawn, in case the game refreshed it after the main update.
	const float surface = cameraSurface.load(std::memory_order_acquire);
	const auto eye = Util::GetEyePosition();
	waterSystem->underwaterHeight = surface;
	waterSystem->playerUnderwater = eye.z < surface;
}

bool PBRWater::GetWaveHeight(const RE::NiPoint3& position, float flatWaterZ, float& height) const
{
	const auto snap = snapshot.load(std::memory_order_acquire);
	if (!snap)
		return false;
	height = snap->SampleAt(position.x, position.y, flatWaterZ).height;
	return true;
}

void PBRWater::GatherInteractions(const PBRWaterModel::WaveSnapshot& snap, float dt)
{
	const bool wantRipples = settings.EnableRipples;

	auto player = RE::PlayerCharacter::GetSingleton();
	const auto eye = Util::GetMainThreadEyePosition();
	const float range = Tuning::RippleExtent * UnitsPerMetre * 0.5f;
	const float rangeSq = range * range;

	std::vector<RippleSimulation::Source> sources;
	sources.reserve(RippleSimulation::MaxSources);
	std::unordered_map<const void*, RE::NiPoint3> waveVelocities;

	// The water at a position: flat plane height and the displaced surface above it, or false when there is none.
	auto surfaceAt = [&](RE::TESObjectREFR* ref, const RE::NiPoint3& pos, float& flatZ, PBRWaterModel::WaveSnapshot::Sample& wave) {
		auto parentCell = ref->GetParentCell();
		if (!parentCell)
			return false;
		float h = 0.0f;
		if (!TESObjectCELL_GetWaterHeight::func(parentCell, pos, h) || h <= -1e6f)
			return false;
		flatZ = h;
		wave = snap.SampleAt(pos.x, pos.y, h);
		return true;
	};

	// The ripple simulation lives in the water's rest (Lagrangian) coordinates and the shaders sample it at
	// undisplaced positions, so a body is placed where the water under it rests: its rings then ride the
	// waves with the water instead of sliding across them. Its motion is measured relative to the
	// water too, so a body bobbing with the waves makes no ripples, while one moving through them does.
	std::unordered_map<const void*, SourceTrack> tracks;
	auto addSource = [&](const void* key, const RE::NiPoint3& center, float radius, float flatZ, const PBRWaterModel::WaveSnapshot::Sample& wave, const RE::NiPoint3& anchor) {
		if (!wantRipples || sources.size() >= RippleSimulation::MaxSources)
			return;
		const float surfaceZ = flatZ + wave.height;
		// The waves move the water sideways by (anchor - origin); a limb is offset from the anchor by far less
		// than a wavelength, so it shares that displacement.
		const double originX = wave.originX + (center.x - anchor.x);
		const double originY = wave.originY + (center.y - anchor.y);
		float offset = center.z - surfaceZ;
		tracks[key] = { originX, originY, offset };
		const auto previous = lastSources.find(key);
		if (previous == lastSources.end() || dt <= 0.0f)
			return;
		// A fast fall can carry a body through the surface between two frames: that still hits the water.
		const bool crossed = previous->second.offset > 0.0f && offset < 0.0f;
		if (std::abs(offset) >= radius && !crossed)
			return;
		const float moveX = static_cast<float>(originX - previous->second.originX);
		const float moveY = static_cast<float>(originY - previous->second.originY);
		const float sink = previous->second.offset - offset;  // > 0: moving down into the water
		const float speed = std::sqrt(moveX * moveX + moveY * moveY + sink * sink) / dt;
		const float motion = std::clamp(speed / (1.5f * UnitsPerMetre), 0.0f, 1.0f);
		// A splash: hitting the water fast from above digs a crater.
		const float entry = std::clamp(sink / dt / (3.0f * UnitsPerMetre), 0.0f, 1.0f);
		if (motion <= 0.01f && entry <= 0.0f)
			return;
		// Footprint of the body where it cuts the surface, and how much of it is under water.
		offset = std::clamp(offset, -radius, radius);
		const float footprint = std::max(std::sqrt(std::max(radius * radius - offset * offset, 0.0f)), crossed ? 0.7f * radius : 0.0f);
		const float submerged = std::clamp(0.5f - offset / (2.0f * radius), 0.0f, 1.0f);
		const float size = std::clamp(footprint / 24.0f, 0.25f, 1.0f);
		RippleSimulation::Source s;
		s.x = static_cast<float>(originX);
		s.y = static_cast<float>(originY);
		s.radius = footprint;
		s.depth = submerged * size * motion + entry * size;
		sources.push_back(s);
	};

	// Silt kicked up by feet moving over the bed, released at the start of the next simulation steps.
	constexpr float siltRate = 0.06f;
	auto addSilt = [&](double x, double y, float amount) {
		if (sources.size() >= RippleSimulation::MaxSources || amount <= 0.0f)
			return;
		RippleSimulation::Source s;
		s.x = static_cast<float>(x);
		s.y = static_cast<float>(y);
		s.radius = 0.5f * UnitsPerMetre;
		s.depth = 0.0f;
		s.silt = amount;
		sources.push_back(s);
	};

	// Actors: every collision shape (legs, torso, ...) that crosses the surface makes its own ripple.
	if (wantRipples) {
		auto handleActor = [&](RE::Actor* actor) {
			if (!actor || !actor->Is3DLoaded())
				return;
			const auto pos = actor->GetPosition();
			if (pos.GetSquaredDistance(eye) > rangeSq)
				return;
			float flatZ;
			PBRWaterModel::WaveSnapshot::Sample wave;
			if (!surfaceAt(actor, pos, flatZ, wave))
				return;
			// Wading (feet on the bed, under water): moving feet stir the bottom up.
			if (siltRate > 0.0f && pos.z < flatZ + wave.height - 2.0f) {
				const auto state = actor->AsActorState();
				if (state && !state->IsSwimming()) {
					RE::NiPoint3 velocity;
					actor->GetLinearVelocity(velocity);
					const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y) / UnitsPerMetre;
					addSilt(wave.originX, wave.originY, siltRate * std::clamp(speed, 0.0f, 1.5f));
				}
			}
			auto root = actor->Get3D(false);
			if (!root)
				return;
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 center;
				float radius;
				if (Util::GetShapeBound(object, center, radius))
					addSource(object, center, radius, flatZ, wave, pos);
				return sources.size() < RippleSimulation::MaxSources ? RE::BSVisit::BSVisitControl::kContinue : RE::BSVisit::BSVisitControl::kStop;
			});
		};

		if (player)
			handleActor(player);
		if (const auto processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles) {
				if (auto actor = handle.get())
					handleActor(actor.get());
			}
		}
	}

	// Loose physics objects: ripples, plus buoyancy that follows the wave surface.
	if (player) {
		globals::game::tes->ForEachReferenceInRange(player, range, [&](RE::TESObjectREFR* ref) {
			if (!ref || ref->IsDisabled() || ref->IsDeleted() || ref->As<RE::Actor>() || !ref->Is3DLoaded())
				return RE::BSContainer::ForEachResult::kContinue;
			auto root = ref->Get3D();
			RE::bhkNiCollisionObject* niCollision = root ? root->GetCollisionObject() : nullptr;
			if (!niCollision)
				return RE::BSContainer::ForEachResult::kContinue;

			auto bhkBody = niCollision->body.get() ? niCollision->body.get()->AsBhkRigidBody() : nullptr;
			auto body = bhkBody ? bhkBody->GetRigidBody() : nullptr;
			if (!body || !IsDynamicMotion(body))
				return RE::BSContainer::ForEachResult::kContinue;

			RE::NiPoint3 center;
			float radius;
			if (!Util::GetShapeBound(niCollision, center, radius))
				return RE::BSContainer::ForEachResult::kContinue;

			float flatZ;
			PBRWaterModel::WaveSnapshot::Sample wave;
			if (!surfaceAt(ref, center, flatZ, wave))
				return RE::BSContainer::ForEachResult::kContinue;

			// Only bodies floating at the surface; sunken or airborne ones are left alone.
			if (std::abs(center.z - (flatZ + wave.height)) > radius * 1.5f)
				return RE::BSContainer::ForEachResult::kContinue;

			float velocity[4];
			_mm_storeu_ps(velocity, body->motion.linearVelocity.quad);

			if (wantRipples)
				addSource(niCollision, center, radius, flatZ, wave, center);

			if (dt > 0.0f) {
				// The engine's own buoyancy floats objects on the flat plane (and lets heavy ones sink).
				// Add only the water's orbital motion, as a change in velocity relative to what was added
				// last frame: floating objects ride the waves, sinking ones keep sinking, and nothing is
				// pinned to the surface.
				const float strength = std::clamp(Tuning::BuoyancyStrength, 0.0f, 3.0f) / 3.0f;
				// Props are denser than the water parcels around them, so they lag its rise (0.75), and nothing is lifted once it
				// rides above the surface: following a fast-rising crest flung them into the air as it fell away.
				const float surface = flatZ + wave.height;
				const float lift = center.z > surface ? std::min(wave.verticalVelocity, 0.0f) : wave.verticalVelocity * 0.75f;
				const RE::NiPoint3 target{ wave.velocityX * strength, wave.velocityY * strength, lift * strength };
				RE::NiPoint3 previous{};
				if (const auto it = lastWaveVelocities.find(bhkBody); it != lastWaveVelocities.end())
					previous = it->second;
				waveVelocities[bhkBody] = target;
				const float worldScale = RE::bhkWorld::GetWorldScale();
				velocity[0] += (target.x - previous.x) * worldScale;
				velocity[1] += (target.y - previous.y) * worldScale;
				velocity[2] += (target.z - previous.z) * worldScale;
				bhkBody->SetLinearVelocity(RE::hkVector4(velocity[0], velocity[1], velocity[2], 0.0f));
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	lastSources = std::move(tracks);
	lastWaveVelocities = std::move(waveVelocities);
	if (wantRipples)
		ripples.SubmitSources(std::move(sources));
}

// ============================================================================
// Hooks
// ============================================================================

void PBRWater::PostPostLoad()
{
	// Without the vanilla displacement (wading) mesh: it is a second water surface around the player
	// that cannot follow the waves. Its job is done by the ripple simulation on the real surface.
	stl::detour_thunk<TESWaterSystem_InitializeWater>(REL::RelocationID(31388, 32179));

	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
	stl::write_vfunc<0x7, BSWaterShader_RestoreGeometry>(RE::VTABLE_BSWaterShader[0]);

	stl::detour_thunk<TESObjectCELL_GetWaterHeight>(REL::RelocationID(18543, 19002));
	// SE 31409 sits with its neighbours AddWater (31406 / 32213) and AddRipple (31410 / 32217).
	stl::detour_thunk<TESWaterSystem_SetUnderwater>(REL::RelocationID(31409, 32216));

	// Same Main::Update call site Grass Collision uses; write_thunk_call chains with it.
	stl::write_thunk_call<MainUpdate>(REL::RelocationID(35565, 36564).address() + Util::VersionedRelocation::Select(0x748, 0xC26, 0xC38));

	logger::info("[PBR Water] Installed hooks");
}

void PBRWater::TESWaterSystem_InitializeWater::thunk(RE::TESWaterSystem* waterSystem, RE::BSTriShape* waterTri, RE::TESWaterForm* form, float waterHeight, void* unk4, bool, bool isProcedural)
{
	func(waterSystem, waterTri, form, waterHeight, unk4, true, isProcedural);
}

void PBRWater::BSWaterShader_SetupGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	// Closed in RestoreGeometry, which the engine always pairs with this call.
	globals::state->BeginPerfEvent("PBR Water - Draw");
	func(waterShader, pass, renderFlags);
	globals::features::pbrWater.SetupDraw(waterShader, pass);
}

void PBRWater::BSWaterShader_RestoreGeometry::thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags)
{
	globals::features::pbrWater.RestoreDraw();
	func(waterShader, pass, renderFlags);
	globals::state->EndPerfEvent();
}

bool PBRWater::TESObjectCELL_GetWaterHeight::thunk(RE::TESObjectCELL* cell, const RE::NiPoint3& position, float& waterHeight)
{
	const bool result = func(cell, position, waterHeight);
	auto& feature = globals::features::pbrWater;
	// Only when the waves are actually rendered, so physics never follows an invisible surface.
	if (result && feature.settings.GameplayWaves && waterHeight > -1e6f && globals::shaderCache->IsEnabled()) {
		float wave;
		if (feature.GetWaveHeight(position, waterHeight, wave))
			waterHeight += wave;
	}
	return result;
}

void PBRWater::GameLoaded()
{
	// The loaded game's references have new 3D; whatever floated before belongs to the old one.
	floating.Reset();
	cameraSurfaceValid.store(false, std::memory_order_release);
}

void PBRWater::SavingGame()
{
	floating.RestorePlacements();
}

void PBRWater::MainUpdate::thunk()
{
	func();
	globals::features::pbrWater.MainThreadUpdate();
}

#undef I18N_KEY_PREFIX
