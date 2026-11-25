#include "KelvinWake.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

namespace PBRWaterWake
{
	winrt::com_ptr<ID3D11ShaderResourceView> CreatePattern()
	{
		const auto start = std::chrono::steady_clock::now();
		// X behind the source (0..Length), Y to one side (0..Width; the pattern is symmetric), both in units of 1 / k0.
		constexpr uint32_t Columns = 192;
		constexpr uint32_t Rows = 96;
		// The integral over the waves' directions theta: eta(X, Y) = Re integral of sec^3(theta) exp(-eps sec^2(theta))
		// exp(i sec^2(theta) (X cos(theta) + Y sin(theta))). eps is the body's size in 1 / k0 (a point would ring forever
		// at the wedge's edges): it damps the short divergent waves near +-90 degrees, which a body of finite size does not
		// make. The phase turns fastest near the ends; the steps keep it well under a radian there.
		constexpr double Eps = 0.6;
		constexpr double ThetaMax = 1.25;
		constexpr uint32_t Steps = 900;
		std::vector<double> secTheta(Steps), weight(Steps), cosT(Steps), sinT(Steps);
		for (uint32_t i = 0; i < Steps; ++i) {
			const double theta = -ThetaMax + 2.0 * ThetaMax * (i + 0.5) / Steps;
			const double c = std::cos(theta);
			const double sec2 = 1.0 / (c * c);
			secTheta[i] = sec2;
			weight[i] = sec2 / c * std::exp(-Eps * sec2) * (2.0 * ThetaMax / Steps);
			cosT[i] = c;
			sinT[i] = std::sin(theta);
		}
		std::vector<double> height(Columns * Rows);
		double peak = 1e-9;
		for (uint32_t r = 0; r < Rows; ++r) {
			const double y = Width * (r + 0.5) / Rows;
			for (uint32_t col = 0; col < Columns; ++col) {
				const double x = Length * (col + 0.5) / Columns;
				double sum = 0.0;
				for (uint32_t i = 0; i < Steps; ++i)
					sum += weight[i] * std::cos(secTheta[i] * (x * cosT[i] + y * sinT[i]));
				// It grows in over the first transverse wave (the near field is no part of this far-field form) and fades
				// out before the texture's end.
				const double grow = 1.0 - std::exp(-x / 2.0);
				const double fade = 1.0 - std::clamp((x - 0.6 * Length) / (0.4 * Length), 0.0, 1.0);
				const double side = 1.0 - std::clamp((y - 0.75 * Width) / (0.25 * Width), 0.0, 1.0);
				height[r * Columns + col] = sum * grow * fade * fade * side;
				peak = std::max(peak, std::abs(height[r * Columns + col]));
			}
		}

		// Normalised to a peak of 1 (each source scales it), with central-difference slopes per unit of X and Y.
		std::vector<float> texels(Columns * Rows * 4);
		const double dx = Length / Columns, dy = Width / Rows;
		auto at = [&](int col, int r) {
			col = std::clamp(col, 0, static_cast<int>(Columns) - 1);
			r = std::clamp(r, 0, static_cast<int>(Rows) - 1);
			return height[r * Columns + col] / peak;
		};
		for (int r = 0; r < static_cast<int>(Rows); ++r) {
			for (int col = 0; col < static_cast<int>(Columns); ++col) {
				float* t = &texels[(r * Columns + col) * 4];
				t[0] = static_cast<float>(at(col, r));
				t[1] = static_cast<float>((at(col + 1, r) - at(col - 1, r)) / (2.0 * dx));
				// Symmetric about Y = 0: the row below the first mirrors it.
				const double below = r == 0 ? at(col, 0) : at(col, r - 1);
				t[2] = static_cast<float>((at(col, r + 1) - below) / (r == 0 ? dy : 2.0 * dy));
				t[3] = 0.0f;
			}
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = Columns;
		desc.Height = Rows;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{ texels.data(), Columns * 4 * static_cast<UINT>(sizeof(float)), 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, &data, texture.put())) ||
			FAILED(globals::d3d::device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
			logger::warn("[PBR Water] The Kelvin wake pattern could not be created; wakes are off");
			return nullptr;
		}
		Util::SetResourceName(texture.get(), "PBRWater::KelvinWakePattern");
		Util::SetResourceName(srv.get(), "PBRWater::KelvinWakePattern SRV");
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		logger::info("[PBR Water] Kelvin wake pattern evaluated in {:.0f} ms", ms);
		return srv;
	}
}
