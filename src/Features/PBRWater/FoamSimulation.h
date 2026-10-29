#pragma once

#include "Buffer.h"

/**
 * @brief Camera-centred foam on the water (GPU, FoamSimCS.hlsl): the whitecap opacity map of Tessendorf, Reinhard & Gao
 * (2023), kept like Crest's foam simulation.
 *
 * Breaking waves write foam (the foam Jacobian of the summed waves past the wind's whitecap threshold, the rollers of
 * the breaking shore waves, the front of the swash) and everywhere it decays, so a crest leaves a trail and the surf
 * leaves foam at the water's edge. The grid is indexed by the undisplaced (Lagrangian) position, so the foam stays with
 * the water that carries it. Two levels (slices of one texture array) of GridSize^2 texels, the second LevelRatio
 * times coarser, reach as far as the waves are displaced; each scrolls with the camera in whole texels, so the state is
 * never resampled.
 */
class FoamSimulation
{
public:
	static constexpr uint32_t GridSize = 512;
	static constexpr uint32_t Levels = 2;
	static constexpr float LevelRatio = 4.0f;

	struct Settings
	{
		float extent = 65536.0f;     ///< world units covered by the coarse level
		float lifetime = 2.0f;       ///< s: foam thickness decays to 1/e (Callaghan, Deane & Stokes 2012 measured 1.4-4.8 s)
		float freshLifetime = 1.0f;  ///< s: a crest breaks actively for about a second (stage A)
		float waterLevel = 0.0f;     ///< absolute z of the water around the camera, where the shore field knows none
	};

	void SetupResources();
	void ClearShaderCache();
	void Reset();

	/**
	 * @brief Render thread: steps the foam. PBR Water's constants (b7), frame constants (b12), textures (t110-t124 but
	 * t121) and samplers (s12-s13) are bound for the compute shader by the caller.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(float cameraX, float cameraY, float dt, const Settings& settings);

	/** @brief x foam thickness, y freshness; slice 0 the fine level, 1 the coarse one. */
	ID3D11ShaderResourceView* GetSRV() const { return IsReady() ? state[current]->srv.get() : nullptr; }
	/** @brief A level's grid: absolute corner x, y and 1 / extent. */
	void GetLevel(uint32_t level, float& originX, float& originY, float& inverseExtent) const;
	bool IsReady() const { return ready && hasOrigin; }

private:
	struct alignas(16) SimCB
	{
		int32_t shift[4];
		float origin[4];
		float texel[4];
		float decay[4];
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);

	std::unique_ptr<Texture2D> state[2];  ///< ping-pong (layout owned by FoamSimCS)
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexel[Levels][2]{};
	float texelSize[Levels] = { 32.0f, 128.0f };
	bool hasOrigin = false;
	bool ready = false;
};
