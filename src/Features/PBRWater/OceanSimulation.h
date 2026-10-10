#pragma once

#include "Buffer.h"
#include "WaveModel.h"

/**
 * @brief GPU half of the FFT ocean.
 *
 * Every frame each cascade is synthesised from the spectrum (OceanSpectrumCS), transformed along rows and
 * columns (OceanFFTCS) and unpacked into mip-mapped texture arrays (OceanAssembleCS): displacement,
 * slopes and compression, LEAN slope moments, crest foam trails and curvature. The water shaders sample
 * them for geometry, normals, roughness, foam and tessellation; last frame's displacement is kept for
 * motion vectors.
 */
class OceanSimulation
{
public:
	struct Settings
	{
		uint32_t resolution = 256;     ///< FFT size per cascade (128, 256 or 512)
		float crestThreshold = 0.55f;  ///< Jacobian below which the surface counts as folding
		float foamTrail = 1.5f;        ///< seconds a crest's foam lingers
	};

	void SetupResources();
	void ClearShaderCache();
	/** @brief Forgets the foam trails and last frame's displacement. */
	void Reset();

	/**
	 * @brief Render thread, once per frame: synthesises the cascades at the snapshot's wave time.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(const PBRWaterModel::WaveSnapshot& snapshot, float dt, const Settings& settings);

	bool IsReady() const { return ready; }
	uint32_t GetResolution() const { return resolution; }
	/** @brief Number of mip levels of the sampled textures. */
	uint32_t GetMipCount() const { return mipCount; }

	ID3D11ShaderResourceView* GetDisplacementSRV() const { return ready ? displacement->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetDerivativesSRV() const { return ready ? derivatives->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetSurfaceSRV() const { return ready ? surface->srv.get() : nullptr; }
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
		float4 Foam0;
		float4 FoamBand[PBRWaterModel::NumCascades];
	};
	STATIC_ASSERT_ALIGNAS_16(OceanCB);

	void CreateTextures(uint32_t size);
	void CompileShaders();

	std::unique_ptr<Texture2D> spectrum[2];  ///< packed complex fields, ping-pong through the FFT passes
	std::unique_ptr<Texture2D> displacement;
	std::unique_ptr<Texture2D> derivatives;
	std::unique_ptr<Texture2D> surface;
	std::unique_ptr<Texture2D> previousDisplacement;
	std::unique_ptr<Texture2D> foam[2];
	std::unique_ptr<ConstantBuffer> constants;
	winrt::com_ptr<ID3D11SamplerState> wrapSampler;

	winrt::com_ptr<ID3D11ComputeShader> spectrumCS;
	winrt::com_ptr<ID3D11ComputeShader> fftRowsCS;
	winrt::com_ptr<ID3D11ComputeShader> fftColumnsCS;
	winrt::com_ptr<ID3D11ComputeShader> assembleCS;

	uint32_t resolution = 0;
	uint32_t mipCount = 1;
	uint32_t foamIndex = 0;
	bool hasHistory = false;
	bool ready = false;
};
