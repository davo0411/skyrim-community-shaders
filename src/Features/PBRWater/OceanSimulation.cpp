#include "OceanSimulation.h"
#include "Profiling.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
	using namespace PBRWaterModel;

	constexpr uint32_t Cascades = NumCascades;

	uint32_t ValidResolution(uint32_t size)
	{
		return size <= 128 ? 128 : (size <= 256 ? 256 : 512);
	}

	uint32_t Log2(uint32_t size)
	{
		uint32_t log = 0;
		while ((1u << log) < size)
			++log;
		return log;
	}

	std::unique_ptr<Texture2D> CreateArray(uint32_t size, uint32_t slices, DXGI_FORMAT format, uint32_t mips, bool uav, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = size;
		desc.Height = size;
		desc.MipLevels = mips;
		desc.ArraySize = slices;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
		if (mips > 1) {
			desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
			desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		}
		auto texture = std::make_unique<Texture2D>(desc, name);

		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srv.Texture2DArray.MostDetailedMip = 0;
		srv.Texture2DArray.MipLevels = mips;
		srv.Texture2DArray.FirstArraySlice = 0;
		srv.Texture2DArray.ArraySize = slices;
		texture->CreateSRV(srv);

		if (uav) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
			u.Format = format;
			u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			u.Texture2DArray.MipSlice = 0;
			u.Texture2DArray.FirstArraySlice = 0;
			u.Texture2DArray.ArraySize = slices;
			texture->CreateUAV(u);
		}
		return texture;
	}
}

void OceanSimulation::SetupResources()
{
	constants = std::make_unique<ConstantBuffer>(ConstantBufferDesc<OceanCB>(), "PBRWater::OceanCB");
	CreateTextures(256);
	CompileShaders();
}

void OceanSimulation::CreateTextures(uint32_t size)
{
	size = ValidResolution(size);
	resolution = size;
	mipCount = Log2(size) + 1;
	ready = false;

	for (uint32_t i = 0; i < 2; ++i)
		spectrum[i] = CreateArray(size, Cascades * 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, true, i == 0 ? "PBRWater::OceanSpectrum0" : "PBRWater::OceanSpectrum1");
	displacement = CreateArray(size, Cascades, DXGI_FORMAT_R16G16B16A16_FLOAT, mipCount, true, "PBRWater::OceanDisplacement");
	derivatives = CreateArray(size, Cascades, DXGI_FORMAT_R16G16B16A16_FLOAT, mipCount, true, "PBRWater::OceanDerivatives");
	surface = CreateArray(size, Cascades, DXGI_FORMAT_R16G16_FLOAT, mipCount, true, "PBRWater::OceanSurface");
	// Same description as the displacement so the whole mip chain can be copied in one call.
	previousDisplacement = CreateArray(size, Cascades, DXGI_FORMAT_R16G16B16A16_FLOAT, mipCount, true, "PBRWater::OceanPreviousDisplacement");
	// Persistent, written and read by the assemble pass (typed UAV load: R32_FLOAT is the format every device supports).
	foam = CreateArray(size, Cascades, DXGI_FORMAT_R32_FLOAT, mipCount, true, "PBRWater::OceanFoam");
	foamClear = true;
	Reset();
}

void OceanSimulation::CompileShaders()
{
	const std::string size = std::to_string(resolution);
	const std::string log2 = std::to_string(Log2(resolution));
	auto compile = [](const wchar_t* path, const std::vector<std::pair<const char*, const char*>>& defines) {
		winrt::com_ptr<ID3D11ComputeShader> shader;
		shader.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, defines, "cs_5_0")));
		return shader;
	};
	spectrumCS = compile(L"Data\\Shaders\\PBRWater\\OceanSpectrumCS.hlsl", {});
	fftRowsCS = compile(L"Data\\Shaders\\PBRWater\\OceanFFTCS.hlsl", { { "FFT_SIZE", size.c_str() }, { "FFT_LOG2", log2.c_str() } });
	fftColumnsCS = compile(L"Data\\Shaders\\PBRWater\\OceanFFTCS.hlsl", { { "FFT_SIZE", size.c_str() }, { "FFT_LOG2", log2.c_str() }, { "VERTICAL", "" } });
	assembleCS = compile(L"Data\\Shaders\\PBRWater\\OceanAssembleCS.hlsl", {});
	ready = spectrumCS && fftRowsCS && fftColumnsCS && assembleCS && displacement && foam && constants;
	if (!ready)
		logger::warn("[PBR Water] FFT ocean shaders failed to compile; open water stays flat");
}

void OceanSimulation::ClearShaderCache()
{
	if (resolution)
		CompileShaders();
}

void OceanSimulation::Reset()
{
	hasHistory = false;
}

void OceanSimulation::Update(const WaveSnapshot& snapshot, const Settings& settings)
{
	if (ValidResolution(settings.resolution) != resolution) {
		CreateTextures(settings.resolution);
		CompileShaders();
	}
	auto skipped = [] {
		PBRWaterProfiling::Skipped("PBRWater::OceanSpectrum");
		PBRWaterProfiling::Skipped("PBRWater::OceanFFT");
		PBRWaterProfiling::Skipped("PBRWater::OceanAssemble");
		PBRWaterProfiling::Skipped("PBRWater::OceanMips");
	};
	if (!ready || !snapshot.ocean) {
		skipped();
		return;
	}
	const OceanSpectrum& ocean = *snapshot.ocean;
	if (ocean.calm) {
		// Nothing to displace (indoors, still air): the shaders skip the cascades, so skip building them.
		hasHistory = false;
		foamClear = true;
		skipped();
		return;
	}

	auto context = globals::d3d::context;

	OceanCB cb{};
	cb.Wind = { ocean.windDirX, ocean.windDirY, ocean.peakOmega, ocean.heightScale };
	cb.Spread = { ocean.spreadPeak, ocean.choppiness, ocean.swellAlpha, ocean.swellPeak };
	cb.Swell = { ocean.swellDirX, ocean.swellDirY, static_cast<float>(Gravity), static_cast<float>(TwoPi / LoopPeriod) };
	double loop = std::fmod(snapshot.time, LoopPeriod) / LoopPeriod;
	if (loop < 0.0)
		loop += 1.0;
	cb.Time = { static_cast<float>(loop), static_cast<float>(resolution), static_cast<float>(UnitsPerMetre), 0.0f };

	for (uint32_t c = 0; c < Cascades; ++c) {
		const CascadeBand& band = ocean.cascades[c];
		cb.Band[c] = { band.length, band.kLow, band.kHigh, band.kFadeStart };
	}
	cb.Time.w = std::clamp(settings.deltaTime, 0.0f, 0.25f) * 60.0f;
	cb.Foam = { settings.foamBias, settings.foamThreshold, settings.foamAdd, settings.foamDecayRate };
	cb.SwellShape = { ocean.swellGamma, ocean.swellSpread, 0.0f, 0.0f };
	constants->Update(cb);

	if (foamClear) {
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(foam->uav.get(), zero);
		foamClear = false;
	}

	// What was displayed last frame becomes the motion-vector reference for this frame.
	if (hasHistory)
		context->CopyResource(previousDisplacement->resource.get(), displacement->resource.get());

	ID3D11Buffer* cbs[1] = { constants->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	ID3D11ShaderResourceView* nullSrvs[1] = { nullptr };
	ID3D11UnorderedAccessView* nullUavs[4] = {};
	const uint32_t groups = (resolution + 7) / 8;

	// 1. Spectrum at the current time.
	{
		PBRWaterProfiling::Scope pass("PBRWater::OceanSpectrum");
		ID3D11UnorderedAccessView* uav = spectrum[0]->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(spectrumCS.get(), nullptr, 0);
		context->Dispatch(groups, groups, Cascades);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	}

	// 2. Inverse FFT: rows (0 -> 1), then columns (1 -> 0).
	auto fft = [&](ID3D11ComputeShader* shader, Texture2D& source, Texture2D& destination) {
		ID3D11ShaderResourceView* srv = source.srv.get();
		ID3D11UnorderedAccessView* uav = destination.uav.get();
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch(1, resolution, Cascades * 2);
		context->CSSetShaderResources(0, 1, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	};
	{
		PBRWaterProfiling::Scope pass("PBRWater::OceanFFT");
		fft(fftRowsCS.get(), *spectrum[0], *spectrum[1]);
		fft(fftColumnsCS.get(), *spectrum[1], *spectrum[0]);
	}

	// 3. Unpack.
	{
		PBRWaterProfiling::Scope pass("PBRWater::OceanAssemble");
		ID3D11ShaderResourceView* srv = spectrum[0]->srv.get();
		ID3D11UnorderedAccessView* uavs[4] = { displacement->uav.get(), derivatives->uav.get(), surface->uav.get(), foam->uav.get() };
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(assembleCS.get(), nullptr, 0);
		context->Dispatch(groups, groups, Cascades);
		context->CSSetShaderResources(0, 1, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
	}

	ID3D11Buffer* nullCb = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	{
		PBRWaterProfiling::Scope pass("PBRWater::OceanMips");
		context->GenerateMips(displacement->srv.get());
		context->GenerateMips(derivatives->srv.get());
		context->GenerateMips(surface->srv.get());
		context->GenerateMips(foam->srv.get());
	}

	if (!hasHistory) {
		context->CopyResource(previousDisplacement->resource.get(), displacement->resource.get());
		hasHistory = true;
	}
}
