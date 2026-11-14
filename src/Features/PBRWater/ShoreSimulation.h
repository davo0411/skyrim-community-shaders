#pragma once

#include "Buffer.h"

/**
 * @brief The shore's camera-centred simulations (GPU): the surf and swash, the foam the surf leaves on the water, and
 * how wet the shore is.
 *
 * The shore waves come in as an analytic train (PBRWater.hlsli ShoreWaves). In the inner surf each broken wave is handed
 * to a shallow water simulation (SwashStepCS.hlsl), which carries it on as a bore and runs its water up the beach and
 * back; the draws pass from the train's surface to the simulation's over a fixed band of depths (PBRWater::ComposeSurf).
 * ShoreFoamCS.hlsl lays out what the waves bring each frame and carries the foam they leave on the water; SwashOutCS.hlsl
 * packs both for the draws. A coarser grid (ShoreSimCS.hlsl) keeps how far up the shore it is still wet, which the lit
 * terrain and objects read (t123 through SharedData's SwashGrid). All grids scroll with the camera in whole texels, so
 * no state is ever resampled.
 */
class ShoreSimulation
{
public:
	static constexpr uint32_t GridSize = 512;
	/// The surf simulation and the surf's foam: a finer grid around the camera.
	static constexpr uint32_t FoamGridSize = 1024;
	static constexpr float FoamTexelSize = 12.0f;  ///< units (17 cm): 175 m across

	struct Settings
	{
		float extent = 32768.0f;  ///< world units covered by the grid
		float dryRate = 0.35f;    ///< units/s the wet level sinks as the sand dries
	};

	void SetupResources();
	void ClearShaderCache();
	void Reset();

	/**
	 * @brief Render thread: moves the wet level to the camera and updates it. PBR Water's constants (b7), frame constants
	 * (b12), textures (t110-t127) and samplers (s12-s13) are bound for the compute shader by the caller.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(float cameraX, float cameraY, float dt, const Settings& settings);

	/**
	 * @brief Render thread: steps the surf's foam (made by the breakers, carried by the water, decaying) and the surf
	 * simulation (SwashStepCS: the shallow water equations through the inner surf and up the beach, fed by the train's
	 * waves). Bindings as for Update; run before it, which reads the simulation.
	 */
	void UpdateFoam(float cameraX, float cameraY, float dt);

	/** @brief x wet level, yz unused, w water surface height (units above the still water). */
	ID3D11ShaderResourceView* GetWetSRV() const { return ready ? wet[current]->srv.get() : nullptr; }
	/** @brief Absolute world position of the grid's corner. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	float GetExtent() const { return texelSize * GridSize; }
	bool IsReady() const { return ready && hasOrigin; }

	/** @brief x the surf's foam, y the surf simulation's surface (units above the still water), z its depth (units), w its foam. */
	ID3D11ShaderResourceView* GetFoamSRV() const { return foamReady ? foam[foamCurrent]->srv.get() : nullptr; }
	float GetFoamOriginX() const { return static_cast<float>(foamOriginX) * FoamTexelSize; }
	float GetFoamOriginY() const { return static_cast<float>(foamOriginY) * FoamTexelSize; }
	float GetFoamExtent() const { return FoamTexelSize * FoamGridSize; }
	bool IsFoamReady() const { return foamReady && foamHasOrigin; }
	/** @brief Surf simulation steps the last UpdateFoam took (Debug section). */
	uint32_t GetLastSwashSteps() const { return lastSwashSteps; }

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

	std::unique_ptr<Texture2D> foam[2];    ///< ping-pong, read by the draws (layout owned by SwashOutCS)
	std::unique_ptr<Texture2D> stage;      ///< ShoreFoamCS: its foam, the waves' direction and the white water they carry
	std::unique_ptr<Texture2D> prep;       ///< ShoreFoamCS: the bed, the hand-over's shares and the train's level
	std::unique_ptr<Texture2D> surf;       ///< the surf simulation's state (layout owned by SwashStepCS)
	std::unique_ptr<Texture2D> surfMoved;  ///< the state between a step's two passes: new velocities, old depths
	winrt::com_ptr<ID3D11ComputeShader> foamCS;
	winrt::com_ptr<ID3D11ComputeShader> momentumCS;    ///< SwashStepCS, first pass
	winrt::com_ptr<ID3D11ComputeShader> continuityCS;  ///< SwashStepCS with SWASH_CONTINUITY, second pass
	winrt::com_ptr<ID3D11ComputeShader> outCS;
	uint32_t foamCurrent = 0;
	uint32_t lastSwashSteps = 0;
	int64_t foamOriginX = 0;
	int64_t foamOriginY = 0;
	bool foamHasOrigin = false;
	bool foamReady = false;
};
