#pragma once

#include "Buffer.h"

/**
 * @brief Camera-centred history of the swash (GPU, ShoreSimCS.hlsl): how wet the shore is.
 *
 * The run-up itself is analytic (PBRWater.hlsli: Stockdon et al. 2006 run-up of each crest of the shore wave train,
 * ballistic backwash), so every shader evaluates it on its own. Only what depends on the past is kept here: how far
 * up the shore it is still wet. The lit terrain and objects read the Wet texture (t123 through SharedData's SwashGrid), and the grid scrolls
 * with the camera in whole texels so the state is never resampled.
 */
class ShoreSimulation
{
public:
	static constexpr uint32_t GridSize = 512;
	/// The surf's foam (ShoreFoamCS): a finer grid around the camera, where the foam's pattern shows.
	static constexpr uint32_t FoamGridSize = 512;
	static constexpr float FoamTexelSize = 24.0f;  ///< units (35 cm): 175 m across

	struct Settings
	{
		float extent = 32768.0f;  ///< world units covered by the grid
		float dryRate = 0.35f;    ///< units/s the wet level sinks as the sand dries
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

	/**
	 * @brief Render thread: steps the surf's foam (made by the breakers, carried by the water and the swash, decaying)
	 * and the swash (SwashStepCS: the shallow water equations up the beach, fed by the breaking bores). Bindings as for
	 * Update; run before it, which reads the swash.
	 */
	void UpdateFoam(float cameraX, float cameraY, float dt);

	/** @brief x wet level, yz unused, w water surface height (units above the still water). */
	ID3D11ShaderResourceView* GetWetSRV() const { return ready ? wet[current]->srv.get() : nullptr; }
	/** @brief Absolute world position of the grid's corner. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	float GetExtent() const { return texelSize * GridSize; }
	bool IsReady() const { return ready && hasOrigin; }

	/** @brief x the surf's foam, y the swash's surface, z its depth (units above the still water), w its foam. */
	ID3D11ShaderResourceView* GetFoamSRV() const { return foamReady ? foam[foamCurrent]->srv.get() : nullptr; }
	float GetFoamOriginX() const { return static_cast<float>(foamOriginX) * FoamTexelSize; }
	float GetFoamOriginY() const { return static_cast<float>(foamOriginY) * FoamTexelSize; }
	float GetFoamExtent() const { return FoamTexelSize * FoamGridSize; }
	bool IsFoamReady() const { return foamReady && foamHasOrigin; }

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
		float dryRate;
		float pad[4];
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);

	std::unique_ptr<Texture2D> wet[2];  ///< ping-pong (layout owned by ShoreSimCS)
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexelX = 0;
	int64_t originTexelY = 0;
	float texelSize = 35.0f;
	bool hasOrigin = false;
	bool ready = false;

	std::unique_ptr<Texture2D> foam[2];  ///< ping-pong, read by the draws (layout owned by SwashOutCS)
	std::unique_ptr<Texture2D> stage;    ///< ShoreFoamCS's foam and the waves' level and foam for the swash
	std::unique_ptr<Texture2D> prep;     ///< ShoreFoamCS: the bed and the waves' hand-over for the swash
	std::unique_ptr<Texture2D> swe[2];   ///< ping-pong swash state (layout owned by SwashStepCS)
	winrt::com_ptr<ID3D11ComputeShader> foamCS;
	winrt::com_ptr<ID3D11ComputeShader> stepCS;
	winrt::com_ptr<ID3D11ComputeShader> outCS;
	uint32_t sweCurrent = 0;
	uint32_t foamCurrent = 0;
	int64_t foamOriginX = 0;
	int64_t foamOriginY = 0;
	bool foamHasOrigin = false;
	bool foamReady = false;
};
