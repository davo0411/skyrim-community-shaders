#include "SprayParticles.h"
#include "Profiling.h"
#include "WaveModel.h"

#include "Feature.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace
{
	constexpr float UnitsPerMetre = static_cast<float>(PBRWaterModel::UnitsPerMetre);
	constexpr float DegreesToRadians = std::numbers::pi_v<float> / 180.0f;
	/// Particle (Spray.hlsli): 64 bytes.
	constexpr UINT ParticleStride = 64;

	std::unique_ptr<Buffer> CreateStructured(UINT stride, UINT count, bool uav, bool append, const char* name)
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = stride * count;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = stride;
		auto buffer = std::make_unique<Buffer>(desc, nullptr, name);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = count;
		buffer->CreateSRV(srvDesc);
		if (uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = count;
			uavDesc.Buffer.Flags = append ? D3D11_BUFFER_UAV_FLAG_APPEND : 0;
			buffer->CreateUAV(uavDesc);
		}
		return buffer;
	}
}

void to_json(nlohmann::json& j, const SprayParticles::Emitter& e)
{
	j = nlohmann::json{
		{ "Enabled", e.Enabled }, { "Source", e.SourceKind }, { "Texture", e.Texture }, { "Animate", e.Animate },
		{ "Rate", e.Rate }, { "StrengthMin", e.StrengthMin }, { "StrengthFull", e.StrengthFull }, { "MaxDistance", e.MaxDistance },
		{ "Speed", e.Speed }, { "SpeedVariation", e.SpeedVariation }, { "Declination", e.Declination }, { "DeclinationVariation", e.DeclinationVariation },
		{ "PlanarSpread", e.PlanarSpread }, { "InheritWave", e.InheritWave }, { "InheritWind", e.InheritWind }, { "StartHeight", e.StartHeight },
		{ "LifeSpan", e.LifeSpan }, { "LifeVariation", e.LifeVariation }, { "Radius", e.Radius }, { "RadiusVariation", e.RadiusVariation },
		{ "EndSize", e.EndSize }, { "GrowTime", e.GrowTime }, { "FadeTime", e.FadeTime }, { "FadeIn", e.FadeIn },
		{ "Gravity", e.Gravity }, { "Drag", e.Drag }, { "RotationSpeed", e.RotationSpeed }, { "RotationVariation", e.RotationVariation },
		{ "Color", { e.Color.x, e.Color.y, e.Color.z } }, { "Alpha", e.Alpha }, { "LightingInfluence", e.LightingInfluence },
		{ "ForwardScattering", e.ForwardScattering }, { "Additive", e.Additive }, { "SoftDepth", e.SoftDepth }, { "MotionStretch", e.MotionStretch }
	};
}

void from_json(const nlohmann::json& j, SprayParticles::Emitter& e)
{
	// Missing or mistyped values keep what `e` held (its slot's default emitter).
	if (!j.is_object())
		return;
	auto read = [&](const char* key, auto& value) {
		const auto it = j.find(key);
		if (it == j.end())
			return;
		using T = std::remove_reference_t<decltype(value)>;
		if constexpr (std::is_same_v<T, bool>) {
			if (it->is_boolean())
				value = it->template get<bool>();
		} else {
			if (it->is_number())
				value = it->template get<T>();
		}
	};
	read("Enabled", e.Enabled);
	read("Source", e.SourceKind);
	read("Texture", e.Texture);
	read("Animate", e.Animate);
	read("Rate", e.Rate);
	read("StrengthMin", e.StrengthMin);
	read("StrengthFull", e.StrengthFull);
	read("MaxDistance", e.MaxDistance);
	read("Speed", e.Speed);
	read("SpeedVariation", e.SpeedVariation);
	read("Declination", e.Declination);
	read("DeclinationVariation", e.DeclinationVariation);
	read("PlanarSpread", e.PlanarSpread);
	read("InheritWave", e.InheritWave);
	read("InheritWind", e.InheritWind);
	read("StartHeight", e.StartHeight);
	read("LifeSpan", e.LifeSpan);
	read("LifeVariation", e.LifeVariation);
	read("Radius", e.Radius);
	read("RadiusVariation", e.RadiusVariation);
	read("EndSize", e.EndSize);
	read("GrowTime", e.GrowTime);
	read("FadeTime", e.FadeTime);
	read("FadeIn", e.FadeIn);
	read("Gravity", e.Gravity);
	read("Drag", e.Drag);
	read("RotationSpeed", e.RotationSpeed);
	read("RotationVariation", e.RotationVariation);
	if (const auto it = j.find("Color"); it != j.end() && it->is_array() && it->size() == 3 && (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
		e.Color = { (*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>() };
	read("Alpha", e.Alpha);
	read("LightingInfluence", e.LightingInfluence);
	read("ForwardScattering", e.ForwardScattering);
	read("Additive", e.Additive);
	read("SoftDepth", e.SoftDepth);
	read("MotionStretch", e.MotionStretch);
	SprayParticles::Sanitize(e);
}

// ============================================================================
// Emitters
// ============================================================================

SprayParticles::Emitters SprayParticles::DefaultEmitters()
{
	// Shore speeds are for a 1 m wave and grow with the square root of its height. Drag k: about 0.0002 / radius (m) for
	// one droplet, far more for mist, whose terminal speed sqrt(g / k) is then a few centimetres a second.
	Emitters e{};

	auto& splash = e[0];  // the splash-up's sheets and clumps of water
	splash.SourceKind = Plunge;
	splash.Texture = 0;
	splash.Animate = true;
	splash.Rate = 3.0f;
	splash.StrengthMin = 0.05f;
	splash.StrengthFull = 0.7f;
	splash.MaxDistance = 250.0f;
	splash.Speed = 4.5f;
	splash.SpeedVariation = 0.35f;
	splash.Declination = 30.0f;
	splash.DeclinationVariation = 20.0f;
	splash.PlanarSpread = 50.0f;
	splash.InheritWave = 0.5f;
	splash.StartHeight = 0.1f;
	splash.LifeSpan = 1.3f;
	splash.Radius = 0.7f;
	splash.RadiusVariation = 0.4f;
	splash.EndSize = 2.2f;
	splash.GrowTime = 0.15f;
	splash.FadeTime = 0.5f;
	splash.Drag = 0.05f;
	splash.RotationSpeed = 30.0f;
	splash.RotationVariation = 0.6f;
	splash.Alpha = 0.9f;
	splash.SoftDepth = 0.4f;

	auto& droplets = e[1];
	droplets.SourceKind = Plunge;
	droplets.Texture = 3;
	droplets.Rate = 8.0f;
	droplets.StrengthMin = 0.1f;
	droplets.StrengthFull = 0.8f;
	droplets.MaxDistance = 80.0f;
	droplets.Speed = 5.5f;
	droplets.SpeedVariation = 0.4f;
	droplets.Declination = 25.0f;
	droplets.DeclinationVariation = 20.0f;
	droplets.PlanarSpread = 60.0f;
	droplets.InheritWave = 0.5f;
	droplets.StartHeight = 0.2f;
	droplets.LifeSpan = 1.4f;
	droplets.Radius = 0.05f;
	droplets.RadiusVariation = 0.5f;
	droplets.GrowTime = 0.0f;
	droplets.FadeTime = 0.2f;
	droplets.Drag = 0.004f;
	droplets.Alpha = 0.85f;
	droplets.ForwardScattering = 0.7f;
	droplets.SoftDepth = 0.1f;
	droplets.MotionStretch = 0.03f;

	auto& mist = e[2];
	mist.SourceKind = Plunge;
	mist.Texture = 2;
	mist.Rate = 1.0f;
	mist.StrengthMin = 0.1f;
	mist.StrengthFull = 0.8f;
	mist.MaxDistance = 300.0f;
	mist.Speed = 2.5f;
	mist.SpeedVariation = 0.4f;
	mist.Declination = 20.0f;
	mist.DeclinationVariation = 20.0f;
	mist.PlanarSpread = 70.0f;
	mist.InheritWave = 0.4f;
	mist.InheritWind = 0.6f;
	mist.StartHeight = 0.3f;
	mist.LifeSpan = 3.0f;
	mist.Radius = 1.2f;
	mist.RadiusVariation = 0.4f;
	mist.EndSize = 3.0f;
	mist.GrowTime = 0.4f;
	mist.FadeTime = 1.8f;
	mist.FadeIn = 0.1f;
	mist.Gravity = 0.05f;
	mist.Drag = 2.0f;
	mist.RotationSpeed = 10.0f;
	mist.RotationVariation = 0.8f;
	mist.Color = { 0.95f, 0.97f, 1.0f };
	mist.Alpha = 0.22f;
	mist.ForwardScattering = 0.6f;
	mist.SoftDepth = 1.5f;

	auto& roller = e[3];
	roller.SourceKind = Roller;
	roller.Texture = 1;
	roller.Rate = 2.0f;
	roller.StrengthMin = 0.1f;
	roller.StrengthFull = 0.8f;
	roller.MaxDistance = 200.0f;
	roller.Speed = 1.8f;
	roller.SpeedVariation = 0.4f;
	roller.Declination = 35.0f;
	roller.DeclinationVariation = 20.0f;
	roller.PlanarSpread = 60.0f;
	roller.InheritWave = 0.8f;
	roller.StartHeight = 0.05f;
	roller.LifeSpan = 0.8f;
	roller.Radius = 0.4f;
	roller.RadiusVariation = 0.4f;
	roller.EndSize = 2.0f;
	roller.FadeTime = 0.4f;
	roller.Drag = 0.1f;
	roller.RotationSpeed = 40.0f;
	roller.RotationVariation = 0.6f;
	roller.Alpha = 0.6f;
	roller.SoftDepth = 0.3f;

	auto& rollerMist = e[4];
	rollerMist = mist;
	rollerMist.SourceKind = Roller;
	rollerMist.Rate = 0.4f;
	rollerMist.StrengthMin = 0.2f;
	rollerMist.StrengthFull = 1.0f;
	rollerMist.MaxDistance = 250.0f;
	rollerMist.Speed = 1.0f;
	rollerMist.Declination = 15.0f;
	rollerMist.DeclinationVariation = 15.0f;
	rollerMist.PlanarSpread = 90.0f;
	rollerMist.InheritWave = 0.6f;
	rollerMist.InheritWind = 0.7f;
	rollerMist.LifeSpan = 2.5f;
	rollerMist.Radius = 1.0f;
	rollerMist.EndSize = 2.5f;
	rollerMist.FadeTime = 1.5f;
	rollerMist.Gravity = 0.03f;
	rollerMist.Alpha = 0.15f;

	auto& surf = e[5];
	surf.SourceKind = Surf;
	surf.Texture = 1;
	surf.Rate = 0.6f;
	surf.StrengthMin = 0.2f;
	surf.StrengthFull = 1.0f;
	surf.MaxDistance = 120.0f;
	surf.Speed = 1.2f;
	surf.SpeedVariation = 0.4f;
	surf.Declination = 30.0f;
	surf.DeclinationVariation = 20.0f;
	surf.PlanarSpread = 70.0f;
	surf.InheritWave = 0.3f;
	surf.StartHeight = 0.02f;
	surf.LifeSpan = 0.6f;
	surf.Radius = 0.25f;
	surf.RadiusVariation = 0.4f;
	surf.EndSize = 1.8f;
	surf.FadeTime = 0.3f;
	surf.Drag = 0.1f;
	surf.Alpha = 0.5f;
	surf.SoftDepth = 0.2f;

	auto& whitecap = e[6];
	whitecap.SourceKind = Whitecap;
	whitecap.Texture = 1;
	whitecap.Animate = true;
	whitecap.Rate = 1.2f;
	whitecap.StrengthMin = 0.15f;
	whitecap.StrengthFull = 1.0f;
	whitecap.MaxDistance = 300.0f;
	whitecap.Speed = 1.5f;
	whitecap.SpeedVariation = 0.4f;
	whitecap.Declination = 45.0f;
	whitecap.DeclinationVariation = 20.0f;
	whitecap.PlanarSpread = 40.0f;
	whitecap.InheritWave = 0.4f;
	whitecap.InheritWind = 0.3f;
	whitecap.StartHeight = 0.05f;
	whitecap.LifeSpan = 1.0f;
	whitecap.Radius = 0.6f;
	whitecap.RadiusVariation = 0.4f;
	whitecap.EndSize = 2.0f;
	whitecap.GrowTime = 0.15f;
	whitecap.FadeTime = 0.5f;
	whitecap.Drag = 0.1f;
	whitecap.RotationSpeed = 20.0f;
	whitecap.RotationVariation = 0.6f;
	whitecap.Alpha = 0.5f;
	whitecap.SoftDepth = 0.4f;

	auto& spindrift = e[7];
	spindrift = mist;
	spindrift.SourceKind = Spindrift;
	spindrift.Rate = 1.5f;
	spindrift.StrengthFull = 1.0f;
	spindrift.MaxDistance = 400.0f;
	spindrift.Speed = 1.0f;
	spindrift.Declination = 70.0f;
	spindrift.DeclinationVariation = 15.0f;
	spindrift.PlanarSpread = 25.0f;
	spindrift.InheritWave = 0.3f;
	spindrift.InheritWind = 0.8f;
	spindrift.StartHeight = 0.2f;
	spindrift.LifeSpan = 2.5f;
	spindrift.Radius = 1.0f;
	spindrift.GrowTime = 0.3f;
	spindrift.FadeTime = 1.5f;
	spindrift.Drag = 1.5f;
	spindrift.Alpha = 0.2f;
	return e;
}

void SprayParticles::Sanitize(Emitter& e)
{
	const Emitter d{};
	auto clamp = [](float& v, float lo, float hi, float fallback) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
	e.SourceKind = std::clamp(e.SourceKind, 0, static_cast<int>(SourceCount) - 1);
	e.Texture = std::clamp(e.Texture, 0, static_cast<int>(TextureCount) - 1);
	clamp(e.Rate, 0.0f, 10000.0f, d.Rate);
	clamp(e.StrengthMin, 0.0f, 1.0f, d.StrengthMin);
	clamp(e.StrengthFull, 0.0f, 1.0f, d.StrengthFull);
	clamp(e.MaxDistance, 1.0f, 5000.0f, d.MaxDistance);
	clamp(e.Speed, 0.0f, 100.0f, d.Speed);
	clamp(e.SpeedVariation, 0.0f, 1.0f, d.SpeedVariation);
	clamp(e.Declination, 0.0f, 180.0f, d.Declination);
	clamp(e.DeclinationVariation, 0.0f, 180.0f, d.DeclinationVariation);
	clamp(e.PlanarSpread, 0.0f, 180.0f, d.PlanarSpread);
	clamp(e.InheritWave, -2.0f, 2.0f, d.InheritWave);
	clamp(e.InheritWind, -2.0f, 2.0f, d.InheritWind);
	clamp(e.StartHeight, -10.0f, 10.0f, d.StartHeight);
	clamp(e.LifeSpan, 0.05f, 60.0f, d.LifeSpan);
	clamp(e.LifeVariation, 0.0f, 1.0f, d.LifeVariation);
	clamp(e.Radius, 0.005f, 50.0f, d.Radius);
	clamp(e.RadiusVariation, 0.0f, 1.0f, d.RadiusVariation);
	clamp(e.EndSize, 0.0f, 50.0f, d.EndSize);
	clamp(e.GrowTime, 0.0f, 60.0f, d.GrowTime);
	clamp(e.FadeTime, 0.0f, 60.0f, d.FadeTime);
	clamp(e.FadeIn, 0.0f, 1.0f, d.FadeIn);
	clamp(e.Gravity, -10.0f, 10.0f, d.Gravity);
	clamp(e.Drag, 0.0f, 1000.0f, d.Drag);
	clamp(e.RotationSpeed, -3600.0f, 3600.0f, d.RotationSpeed);
	clamp(e.RotationVariation, 0.0f, 1.0f, d.RotationVariation);
	clamp(e.Color.x, 0.0f, 100.0f, d.Color.x);
	clamp(e.Color.y, 0.0f, 100.0f, d.Color.y);
	clamp(e.Color.z, 0.0f, 100.0f, d.Color.z);
	clamp(e.Alpha, 0.0f, 1.0f, d.Alpha);
	clamp(e.LightingInfluence, 0.0f, 1.0f, d.LightingInfluence);
	clamp(e.ForwardScattering, -0.95f, 0.95f, d.ForwardScattering);
	clamp(e.Additive, 0.0f, 1.0f, d.Additive);
	clamp(e.SoftDepth, 0.0f, 100.0f, d.SoftDepth);
	clamp(e.MotionStretch, 0.0f, 1.0f, d.MotionStretch);
}

SprayParticles::EmitterGpu SprayParticles::ToGpu(const Emitter& e, float amount)
{
	const auto& texture = Textures[std::clamp(e.Texture, 0, static_cast<int>(TextureCount) - 1)];
	EmitterGpu g{};
	g.source = { static_cast<float>(e.SourceKind), e.Enabled ? e.Rate * std::max(amount, 0.0f) : 0.0f, e.StrengthMin, std::max(e.StrengthFull, e.StrengthMin + 1e-3f) };
	g.launch = { e.Speed * UnitsPerMetre, e.SpeedVariation, e.InheritWave, e.InheritWind };
	g.angles = { e.Declination * DegreesToRadians, e.DeclinationVariation * DegreesToRadians, e.PlanarSpread * DegreesToRadians, e.StartHeight * UnitsPerMetre };
	g.life = { e.LifeSpan, e.LifeVariation, e.GrowTime, e.FadeTime };
	g.size = { e.Radius * UnitsPerMetre, e.RadiusVariation, e.EndSize, e.MaxDistance * UnitsPerMetre };
	// Drag: a = -k |v| v in metres is the same law with k / (units per metre) in units.
	g.motion = { e.Gravity, e.Drag / UnitsPerMetre, e.RotationSpeed * DegreesToRadians, e.RotationVariation };
	g.color = { e.Color.x, e.Color.y, e.Color.z, e.Alpha };
	g.shade = { e.FadeIn, e.LightingInfluence, e.ForwardScattering, e.Additive };
	g.atlas = { static_cast<float>(texture.columns), static_cast<float>(texture.rows), static_cast<float>(texture.columns * texture.rows), e.Animate ? 1.0f : 0.0f };
	g.render = { e.SoftDepth * UnitsPerMetre, e.MotionStretch, static_cast<float>(e.Texture), 0.0f };
	return g;
}

void SprayParticles::LoadTextures()
{
	if (texturesLoaded.load(std::memory_order_acquire))
		return;
	std::string report;
	for (uint32_t i = 0; i < TextureCount; ++i) {
		RE::NiPointer<RE::NiTexture> texture;
		RE::BSShaderManager::GetTexture(Textures[i].path, true, texture, false);
		if (texture && texture->GetRTTI() == globals::rtti::NiSourceTextureRTTI.get()) {
			auto* source = static_cast<RE::NiSourceTexture*>(texture.get());
			if (source->rendererTexture && source->rendererTexture->resourceView) {
				textures[i] = RE::NiPointer<RE::NiSourceTexture>(source);
				continue;
			}
		}
		report += std::format("Texture not found: {}\n", Textures[i].path);
	}
	if (!report.empty())
		logger::warn("[PBR Water] Spray: {}", report);
	status = std::move(report);
	texturesLoaded.store(true, std::memory_order_release);
}

std::string SprayParticles::GetStatus() const
{
	return texturesLoaded.load(std::memory_order_acquire) ? status : std::string();
}

// ============================================================================
// Resources
// ============================================================================

void SprayParticles::SetupResources()
{
	auto device = globals::d3d::device;

	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		counters = std::make_unique<Buffer>(desc, nullptr, "PBRWater::SprayCounters");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = 4;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		counters->CreateUAV(uavDesc);
	}
	{
		const UINT args[4] = { 4, 0, 0, 0 };
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(args);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
		D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
		drawArgs = std::make_unique<Buffer>(desc, &init, "PBRWater::SprayDrawArgs");
	}
	{
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(EmitterGpu) * EmitterCount;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(EmitterGpu);
		emitters = std::make_unique<Buffer>(desc, nullptr, "PBRWater::SprayEmitters");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = EmitterCount;
		emitters->CreateSRV(srvDesc);
	}
	emitCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<EmitCB>(), "PBRWater::SprayEmitCB");
	simCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SimCB>(), "PBRWater::SpraySimCB");

	// Premultiplied: each emitter sits anywhere between alpha blended and additive (the alpha it writes).
	D3D11_BLEND_DESC blendDesc{};
	auto& rt = blendDesc.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D11_BLEND_ONE;
	rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
	rt.DestBlendAlpha = D3D11_BLEND_ONE;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
	Util::SetResourceName(blendState.get(), "PBRWater::SprayBlend");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = FALSE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthState.put()));
	Util::SetResourceName(depthState.get(), "PBRWater::SprayDepth");

	D3D11_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
	Util::SetResourceName(rasterizerState.get(), "PBRWater::SprayRasterizer");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampler.put()));
	Util::SetResourceName(sampler.get(), "PBRWater::SpraySampler");

	CreatePool(Settings{}.poolSize);
	CompileShaders();
}

void SprayParticles::CreatePool(uint32_t size)
{
	particles = CreateStructured(ParticleStride, size, true, false, "PBRWater::SprayParticles");
	alive = CreateStructured(sizeof(uint32_t), size, true, true, "PBRWater::SprayAlive");
	poolSize = size;
	Reset();
}

void SprayParticles::ClearShaderCache()
{
	CompileShaders();
}

void SprayParticles::CompileShaders()
{
	emitCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SprayEmitCS.hlsl", {}, "cs_5_0")));
	simCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SpraySimCS.hlsl", {}, "cs_5_0")));

	// The draw shades as the vanilla particles do: the same features' shadows, IBL and Linear Lighting.
	std::vector<std::pair<const char*, const char*>> defines;
	std::vector<std::pair<std::string, std::string>> storage;
	for (auto* feature : Feature::GetFeatureList()) {
		if (!feature->loaded || !feature->HasShaderDefine(RE::BSShader::Type::Particle))
			continue;
		storage.emplace_back(std::string(feature->GetShaderDefineName()), std::string());
		for (const auto& [name, value] : feature->GetShaderDefineOptions())
			storage.emplace_back(std::string(name), std::string(value));
	}
	for (const auto& [name, value] : storage) {
		if (!name.empty())
			defines.emplace_back(name.c_str(), value.empty() ? nullptr : value.c_str());
	}
	drawVS.attach(reinterpret_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SprayDraw.hlsl", defines, "vs_5_0", "VSMain")));
	drawPS.attach(reinterpret_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\PBRWater\\SprayDraw.hlsl", defines, "ps_5_0", "PSMain")));

	ready = emitCS && simCS && drawVS && drawPS && particles && alive && counters && drawArgs && emitters && emitCB && simCB;
	if (!ready)
		logger::warn("[PBR Water] Spray shaders failed to compile; the waves throw no spray");
}

void SprayParticles::Reset()
{
	auto context = globals::d3d::context;
	const UINT zero[4] = { 0, 0, 0, 0 };
	if (counters)
		context->ClearUnorderedAccessViewUint(counters->uav.get(), zero);
	// Dead: age (word 3) at or past life (word 7), both zero.
	if (particles)
		context->ClearUnorderedAccessViewUint(particles->uav.get(), zero);
	hasParticles = false;
}

// ============================================================================
// Simulation
// ============================================================================

void SprayParticles::Update(const FrameInput& input, const Settings& settings)
{
	if (!ready)
		return;
	auto context = globals::d3d::context;

	const uint32_t wantedPool = std::clamp(settings.poolSize, 4096u, 262144u);
	if (wantedPool != poolSize)
		CreatePool(wantedPool);

	// The emitters are live settings: uploaded every frame (8 of 160 bytes).
	bool anyEmitter = false;
	const bool texturesReady = texturesLoaded.load(std::memory_order_acquire);
	if (settings.emitters && texturesReady) {
		std::array<EmitterGpu, EmitterCount> data{};
		for (uint32_t i = 0; i < EmitterCount; ++i) {
			data[i] = ToGpu((*settings.emitters)[i], settings.amount);
			// No texture, no particles.
			if (!textures[std::clamp((*settings.emitters)[i].Texture, 0, static_cast<int>(TextureCount) - 1)])
				data[i].source.y = 0.0f;
			anyEmitter = anyEmitter || data[i].source.y > 0.0f;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(emitters->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, data.data(), sizeof(data));
			context->Unmap(emitters->resource.get(), 0);
		}
	}

	const bool emitting = anyEmitter && input.dt > 0.0f;
	if (!emitting && !hasParticles) {
		PBRWaterProfiling::Skipped("PBRWater::SprayEmit");
		PBRWaterProfiling::Skipped("PBRWater::SpraySim");
		return;
	}

	const float cellSize = std::max(2.0f * settings.range * UnitsPerMetre / EmitGridSize, 8.0f);
	const float half = 0.5f * cellSize * EmitGridSize;
	const uint32_t budget = std::clamp(settings.frameBudget, 64u, poolSize / 4);

	ID3D11ShaderResourceView* emitterSrv = emitters->srv.get();
	if (emitting) {
		PBRWaterProfiling::Scope pass("PBRWater::SprayEmit");
		EmitCB cb{};
		cb.grid = { std::floor((input.cameraX - half) / cellSize) * cellSize, std::floor((input.cameraY - half) / cellSize) * cellSize, cellSize, static_cast<float>(EmitGridSize) };
		cb.frame = { std::min(input.dt, 0.1f), static_cast<float>(frameIndex & 0xFFFFFF), static_cast<float>(poolSize), static_cast<float>(EmitterCount) };
		cb.wind = { input.windVelocity.x, input.windVelocity.y, input.windSpeed, static_cast<float>(budget) };
		cb.levelGrid = { input.cellX, input.cellY, input.cellSize, 0.0f };
		for (size_t i = 0; i < 25; ++i)
			cb.cellLevels[i] = { input.cellLevels[i], 0.0f, 0.0f, 0.0f };
		emitCB->Update(cb);

		ID3D11UnorderedAccessView* uavs[2] = { particles->uav.get(), counters->uav.get() };
		ID3D11Buffer* cbs[1] = { emitCB->CB() };
		context->CSSetShader(emitCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &emitterSrv);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->Dispatch(EmitGridSize / 8, EmitGridSize / 8, 1);
		hasParticles = true;
	} else {
		PBRWaterProfiling::Skipped("PBRWater::SprayEmit");
	}
	++frameIndex;

	{
		PBRWaterProfiling::Scope pass("PBRWater::SpraySim");
		SimCB cb{};
		cb.frame = { std::min(input.dt, 0.1f), input.gravity, static_cast<float>(poolSize), static_cast<float>(budget) };
		cb.wind = { input.windVelocity.x, input.windVelocity.y, 0.0f, 0.0f };
		simCB->Update(cb);

		ID3D11UnorderedAccessView* uavs[3] = { particles->uav.get(), counters->uav.get(), alive->uav.get() };
		const UINT initialCounts[3] = { UINT(-1), UINT(-1), 0 };
		ID3D11Buffer* cbs[1] = { simCB->CB() };
		context->CSSetShader(simCS.get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &emitterSrv);
		context->CSSetUnorderedAccessViews(0, 3, uavs, initialCounts);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->Dispatch((poolSize + 63) / 64, 1, 1);
	}

	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUavs[3] = {};
	ID3D11Buffer* nullCb = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	// The living, for the draw's instance count.
	context->CopyStructureCount(drawArgs->resource.get(), 4, alive->uav.get());
}

// ============================================================================
// Draw
// ============================================================================

void SprayParticles::Draw(ID3D11ShaderResourceView* sceneDepth)
{
	if (!ready || !hasParticles || !sceneDepth || !texturesLoaded.load(std::memory_order_acquire))
		return;
	auto context = globals::d3d::context;
	PBRWaterProfiling::Scope pass("PBRWater::SprayDraw");

	// Save what the draw changes.
	winrt::com_ptr<ID3D11InputLayout> savedLayout;
	D3D11_PRIMITIVE_TOPOLOGY savedTopology{};
	winrt::com_ptr<ID3D11VertexShader> savedVS;
	winrt::com_ptr<ID3D11PixelShader> savedPS;
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	winrt::com_ptr<ID3D11HullShader> savedHS;
	winrt::com_ptr<ID3D11DomainShader> savedDS;
	ID3D11ShaderResourceView* savedVSSrvs[3] = {};
	ID3D11ShaderResourceView* savedPSSrvs[10] = {};
	ID3D11SamplerState* savedSampler = nullptr;
	ID3D11Buffer* savedVSCb = nullptr;
	ID3D11Buffer* savedPSCbs[3] = {};
	winrt::com_ptr<ID3D11BlendState> savedBlend;
	FLOAT savedBlendFactor[4]{};
	UINT savedSampleMask = 0;
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	winrt::com_ptr<ID3D11RasterizerState> savedRasterizer;
	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->IAGetInputLayout(savedLayout.put());
	context->IAGetPrimitiveTopology(&savedTopology);
	context->VSGetShader(savedVS.put(), nullptr, nullptr);
	context->PSGetShader(savedPS.put(), nullptr, nullptr);
	context->GSGetShader(savedGS.put(), nullptr, nullptr);
	context->HSGetShader(savedHS.put(), nullptr, nullptr);
	context->DSGetShader(savedDS.put(), nullptr, nullptr);
	context->VSGetShaderResources(0, 3, savedVSSrvs);
	context->PSGetShaderResources(0, 10, savedPSSrvs);
	context->PSGetSamplers(0, 1, &savedSampler);
	context->VSGetConstantBuffers(12, 1, &savedVSCb);
	context->PSGetConstantBuffers(4, 3, savedPSCbs);
	context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
	context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
	context->RSGetState(savedRasterizer.put());
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);

	if (savedRTVs[0]) {
		auto* state = globals::state;
		ID3D11Buffer* perFrame = *globals::game::perFrame.get();
		ID3D11Buffer* psCbs[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
		ID3D11ShaderResourceView* vsSrvs[3] = { emitters->srv.get(), particles->srv.get(), alive->srv.get() };
		ID3D11ShaderResourceView* psSrvs[10] = { emitters->srv.get(), nullptr, nullptr };
		for (uint32_t i = 0; i < TextureCount; ++i)
			psSrvs[3 + i] = textures[i] ? textures[i]->rendererTexture->resourceView : nullptr;
		psSrvs[7] = sceneDepth;
		ID3D11SamplerState* samplerState = sampler.get();

		// Only the colour target: the depth is read, not tested.
		context->OMSetRenderTargets(1, savedRTVs, nullptr);
		context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(depthState.get(), 0);
		context->RSSetState(rasterizerState.get());
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->VSSetShader(drawVS.get(), nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->PSSetShader(drawPS.get(), nullptr, 0);
		context->VSSetShaderResources(0, 3, vsSrvs);
		context->PSSetShaderResources(0, 10, psSrvs);
		context->PSSetSamplers(0, 1, &samplerState);
		context->VSSetConstantBuffers(12, 1, &perFrame);
		context->PSSetConstantBuffers(12, 1, &perFrame);
		context->PSSetConstantBuffers(4, 3, psCbs);
		context->DrawInstancedIndirect(drawArgs->resource.get(), 0);
	}

	// Restore.
	ID3D11ShaderResourceView* nullSrvs[10] = {};
	context->VSSetShaderResources(0, 3, nullSrvs);
	context->PSSetShaderResources(0, 10, nullSrvs);
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	context->IASetInputLayout(savedLayout.get());
	context->IASetPrimitiveTopology(savedTopology);
	context->VSSetShader(savedVS.get(), nullptr, 0);
	context->PSSetShader(savedPS.get(), nullptr, 0);
	context->GSSetShader(savedGS.get(), nullptr, 0);
	context->HSSetShader(savedHS.get(), nullptr, 0);
	context->DSSetShader(savedDS.get(), nullptr, 0);
	context->VSSetShaderResources(0, 3, savedVSSrvs);
	context->PSSetShaderResources(0, 10, savedPSSrvs);
	context->PSSetSamplers(0, 1, &savedSampler);
	context->VSSetConstantBuffers(12, 1, &savedVSCb);
	context->PSSetConstantBuffers(4, 3, savedPSCbs);
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->RSSetState(savedRasterizer.get());

	for (auto*& v : savedVSSrvs)
		if (v)
			v->Release();
	for (auto*& v : savedPSSrvs)
		if (v)
			v->Release();
	if (savedSampler)
		savedSampler->Release();
	if (savedVSCb)
		savedVSCb->Release();
	for (auto*& b : savedPSCbs)
		if (b)
			b->Release();
	for (auto*& r : savedRTVs)
		if (r)
			r->Release();
	if (savedDSV)
		savedDSV->Release();
}
