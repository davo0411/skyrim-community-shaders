#pragma once

#include "Buffer.h"
#include "WaveModel.h"

#include <array>

/**
 * @brief GPU half of the FFT ocean.
 *
 * Every frame each cascade is synthesised from the spectrum (OceanSpectrumCS), transformed along rows and
 * columns (OceanFFTCS) and unpacked into mip-mapped texture arrays (OceanAssembleCS): displacement,
 * slopes and compression, and LEAN slope moments. The water shaders sample them for geometry,
 * normals, roughness, foam and tessellation; last frame's displacement is kept for
 * motion vectors.
 */
class OceanSimulation
{
public:
	struct Settings
	{
		uint32_t resolution = 256;  ///< FFT size per cascade (128, 256 or 512)
		float deltaTime = 0.0f;     ///< seconds since the last frame; 0 while the game is paused
		/// The foam accumulation of GarrettGunnell/Water (MIT), per 1/60 s: see OceanAssembleCS.hlsl.
		bool foamMips = true;  ///< false: the foam's mip chain is not rebuilt (Enable Foam off)
		float foamBias = 0.0f;
		float foamThreshold = 0.0f;
		float foamAdd = 0.0f;
		float foamDecayRate = 0.0f;
		uint32_t cascadeMask = 0xF;  ///< cascades to update this frame (bit c); the others keep last frame's waves
	};

	void SetupResources();
	void ClearShaderCache();
	/** @brief Forgets last frame's displacement. */
	void Reset();

	/** @brief Render thread, once per frame: synthesises the cascades at the snapshot's wave time. */
	void Update(const PBRWaterModel::WaveSnapshot& snapshot, const Settings& settings);

	bool IsReady() const { return ready; }
	uint32_t GetResolution() const { return resolution; }
	/** @brief Number of mip levels of the sampled textures. */
	uint32_t GetMipCount() const { return mipCount; }

	ID3D11ShaderResourceView* GetDisplacementSRV() const { return ready ? displacement->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetDerivativesSRV() const { return ready ? derivatives->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetSurfaceSRV() const { return ready ? surface->srv.get() : nullptr; }
	/** @brief The persistent per-cascade foam (R32F, mip-mapped). */
	ID3D11ShaderResourceView* GetFoamSRV() const { return ready ? foam->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetPreviousDisplacementSRV() const { return ready ? previousDisplacement->srv.get() : nullptr; }

private:
	/** @brief Matches OceanCB in OceanSpectrum.hlsli. */
	struct alignas(16) OceanCB
	{
		float4 Wind;
		float4 Spread;
		float4 Swell;
		float4 Time;
		float4 Band[PBRWaterModel::NumCascades];
		float4 Foam;
		float4 SwellShape;
	};
	STATIC_ASSERT_ALIGNAS_16(OceanCB);

	void CreateTextures(uint32_t size);
	void CompileShaders();

	std::unique_ptr<Texture2D> spectrum[2];  ///< packed complex fields, ping-pong through the FFT passes
	std::unique_ptr<Texture2D> initial;      ///< h0(k) and h0(-k) per cascade (OceanSpectrumCS with INITIAL): rebuilt when initialKey changes
	std::unique_ptr<Texture2D> displacement;
	std::unique_ptr<Texture2D> derivatives;
	std::unique_ptr<Texture2D> surface;
	std::unique_ptr<Texture2D> previousDisplacement;
	std::unique_ptr<Texture2D> foam;  ///< persistent foam, one slice per cascade, updated in place by the assemble pass
	/// One cascade's slice of the displacement, derivatives, surface and foam, all mips: the mip chains of the cascades a
	/// frame updated are rebuilt alone (GenerateMips works on the view's slices).
	winrt::com_ptr<ID3D11ShaderResourceView> sliceSRV[4][PBRWaterModel::NumCascades];
	/// The cascades last frame updated (Settings::cascadeMask): only their slices of last frame's displacement differ from
	/// the copy kept for motion vectors.
	uint32_t lastUpdateMask = 0xF;
	std::unique_ptr<ConstantBuffer> constants;

	winrt::com_ptr<ID3D11ComputeShader> spectrumCS;
	winrt::com_ptr<ID3D11ComputeShader> initialCS;
	winrt::com_ptr<ID3D11ComputeShader> fftRowsCS;
	winrt::com_ptr<ID3D11ComputeShader> fftColumnsCS;
	winrt::com_ptr<ID3D11ComputeShader> assembleCS;

	uint32_t resolution = 0;
	uint32_t mipCount = 1;
	bool hasHistory = false;
	bool foamClear = true;  ///< the foam texture has to be emptied before the next step
	/// What the spectrum's amplitudes read of OceanCB (Wind, Spread.xzw, Swell.xyz, Band, SwellShape.xy) when `initial`
	/// was baked; the time, the choppiness, the loop frequency, the foam and the mask only enter the per-frame pass.
	std::array<float, 30> initialKey{};
	bool initialValid = false;
	bool ready = false;
};
