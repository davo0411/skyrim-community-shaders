#pragma once

#include "Buffer.h"

/**
 * @brief Camera-centred swash state (GPU, ShoreSimCS.hlsl): how far each breaking wave runs up the beach, the foam
 * the backwash leaves on the sand and how wet the sand is.
 *
 * Every texel steps the run-up of the wave ray through it, forced by the FFT sea arriving at the ray's waterline
 * (Stockdon et al. 2006 run-up, ballistic backwash). The water shaders raise the surface up the beach to it, the
 * lit terrain and objects read the foam and the wet level, and the grid scrolls with the camera in whole texels so
 * the state is never resampled.
 */
class ShoreSimulation
{
public:
	static constexpr uint32_t GridSize = 512;

	struct Settings
	{
		float extent = 32768.0f;     ///< world units covered by the grid
		float foamLifetime = 10.0f;  ///< s the backwash's foam lasts on the sand
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

	ID3D11ShaderResourceView* GetStateSRV() const { return ready ? state[current]->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetPreviousSRV() const { return ready ? previousFrame->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetWetSRV() const { return ready ? wet[current]->srv.get() : nullptr; }
	/** @brief Absolute world position of the grid's corner, now and last frame. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	float GetPreviousOriginX() const { return static_cast<float>(previousOriginTexelX) * texelSize; }
	float GetPreviousOriginY() const { return static_cast<float>(previousOriginTexelY) * texelSize; }
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

	std::unique_ptr<Texture2D> state[2];
	std::unique_ptr<Texture2D> wet[2];
	std::unique_ptr<Texture2D> previousFrame;
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexelX = 0;
	int64_t originTexelY = 0;
	int64_t previousOriginTexelX = 0;
	int64_t previousOriginTexelY = 0;
	float texelSize = 35.0f;
	bool hasOrigin = false;
	bool ready = false;
};
