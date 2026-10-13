#pragma once

#include "Buffer.h"

/**
 * @brief Camera-centred history of the swash (GPU, ShoreSimCS.hlsl): the foam the backwash leaves on the sand and
 * how wet the sand is.
 *
 * The run-up itself is analytic (PBRWater.hlsli: Stockdon et al. 2006 run-up of each crest of the shore wave train,
 * ballistic backwash), so every shader evaluates it on its own. Only what depends on the past is kept here: since
 * when each texel has been dry, the line the last uprush reached, the foam it left and how far up the sand is still
 * wet. The lit terrain and objects read the Wet texture (t123 through SharedData's SwashGrid), and the grid scrolls
 * with the camera in whole texels so the state is never resampled.
 */
class ShoreSimulation
{
public:
	static constexpr uint32_t GridSize = 512;

	struct Settings
	{
		float extent = 32768.0f;     ///< world units covered by the grid
		float foamLifetime = 30.0f;  ///< s the backwash's foam lasts on the sand (three wave periods)
		float dryRate = 0.35f;       ///< units/s the wet level sinks as the sand dries
	};

	void SetupResources();
	void ClearShaderCache();
	void Reset();

	/**
	 * @brief Render thread: steps the swash to the camera. PBR Water's constants (b7), frame constants (b12), textures
	 * (t110-t120) and samplers (s12-s13) are bound for the compute shader by the caller.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(float cameraX, float cameraY, float dt, const Settings& settings);

	/** @brief x wet level, y foam left on the bed, z its deposit, w water surface height (units above the still water). */
	ID3D11ShaderResourceView* GetWetSRV() const { return ready ? wet[current]->srv.get() : nullptr; }
	/** @brief Absolute world position of the grid's corner. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	float GetExtent() const { return texelSize * GridSize; }
	bool IsReady() const { return ready && hasOrigin; }

private:
	struct alignas(16) SimCB
	{
		int32_t shiftX;
		int32_t shiftY;
		uint32_t gridSize;
		float dt;
		float originX;
		float originY;
		float texelSize;
		float foamLifetime;
		float dryRate;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);

	std::unique_ptr<Texture2D> state[2];  ///< per texel history of the swash (layout owned by ShoreSimCS)
	std::unique_ptr<Texture2D> wet[2];
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexelX = 0;
	int64_t originTexelY = 0;
	float texelSize = 35.0f;
	bool hasOrigin = false;
	bool ready = false;
};
