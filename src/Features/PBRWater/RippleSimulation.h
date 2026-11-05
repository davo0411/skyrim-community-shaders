#pragma once

#include "Buffer.h"

#include <array>
#include <mutex>
#include <vector>

/**
 * @brief Camera-centred interactive ripple simulation (GPU wave equation).
 *
 * Replaces the vanilla wading displacement mesh: instead of a separate mesh that follows the player,
 * every actor limb and loose Havok object touching the water pushes the surface of a scrolling
 * heightfield, which the water shaders sample for displacement and normals.
 */
class RippleSimulation
{
public:
	static constexpr uint32_t GridSize = 512;
	static constexpr uint32_t MaxSources = 128;

	/** @brief A body touching the water, in absolute world units. Matches RippleSource in RippleSimCS.hlsl after conversion. */
	struct Source
	{
		float x = 0.0f;
		float y = 0.0f;
		float radius = 0.0f;  ///< units
		float depth = 0.0f;   ///< how far the body pushes the surface down (simulation units)
		float silt = 0.0f;    ///< sediment kicked up from the bed per simulation step
	};

	struct Settings
	{
		float extent = 4096.0f;      ///< world units covered by the grid
		float halfLife = 1.5f;       ///< s, amplitude decay
		float siltHalfLife = 20.0f;  ///< s, settling time of stirred-up sediment
		float siltSpread = 0.3f;     ///< diffusion per step (0..1)
		float flowSpeed = 0.0f;      ///< units/s: river current at full flow strength
	};

	/** @brief What the simulation reads from the rest of the water system; all optional. */
	struct Environment
	{
		ID3D11ShaderResourceView* terrain = nullptr;  ///< PBR terrain texture (R16_UNORM)
		float4 terrain0{};                            ///< PBRWater::Terrain0
		float4 terrain1{};                            ///< PBRWater::Terrain1
		ID3D11ShaderResourceView* flowmap = nullptr;  ///< Unified Water's worldspace flowmap
		float4 flow0{};                               ///< PBRWater::Flow0 (x = 0: none)
		ID3D11SamplerState* sampler = nullptr;        ///< linear clamp
	};

	/** @brief Cells per side of the still-water-level table. */
	static constexpr int32_t LevelCells = 5;
	static constexpr float CellSize = 4096.0f;

	/** @brief Still water levels of the LevelCells^2 exterior cells around the eye. */
	struct WaterLevels
	{
		int32_t baseCellX = 0;  ///< absolute cell of entry 0
		int32_t baseCellY = 0;
		bool valid = false;
		std::array<float, LevelCells * LevelCells> level{};  ///< absolute z at i + 5 j; < -1e8: no water
	};

	void SetupResources();
	void ClearShaderCache();
	void Reset();

	/** @brief Main thread: replaces the pending source list. */
	void SubmitSources(std::vector<Source>&& sources);
	/** @brief Main thread: the still water the grid is for (the bed depth is measured against it). */
	void SubmitWaterLevels(const WaterLevels& levels);

	/**
	 * @brief Render thread: steps the simulation to the current camera position.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(float cameraX, float cameraY, float dt, const Settings& settings, const Environment& environment);

	ID3D11ShaderResourceView* GetSRV() const;
	/** @brief The state as it was displayed last frame, for motion vectors. */
	ID3D11ShaderResourceView* GetPreviousSRV() const;
	/** @brief Absolute world position of the grid's (0, 0) corner. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	/** @brief Grid origin of the previous frame's state. */
	float GetPreviousOriginX() const { return static_cast<float>(previousOriginTexelX) * texelSize; }
	float GetPreviousOriginY() const { return static_cast<float>(previousOriginTexelY) * texelSize; }
	/** @brief Interpolation between the last two simulation steps for the render time (and last frame's). */
	float GetStepAlpha() const { return stepAlpha; }
	float GetPreviousStepAlpha() const { return previousStepAlpha; }
	float GetExtent() const { return texelSize * GridSize; }
	bool IsReady() const { return ready; }

private:
	struct alignas(16) GpuSource
	{
		float2 position;
		float radius;
		float depth;
		float silt;
		float pad[3];
	};
	static_assert(sizeof(GpuSource) == 32);

	struct alignas(16) SimCB
	{
		int32_t shiftX;
		int32_t shiftY;
		uint32_t numSources;
		uint32_t gridSize;
		float damping;
		float waveSpeed2;
		float siltDecay;
		float siltDiffusion;
		float viscosity;
		float texelSize;
		float stepTime;
		float pad;
		int32_t originX;
		int32_t originY;
		int32_t cellBaseX;
		int32_t cellBaseY;
		float4 terrain0;
		float4 terrain1;
		float4 flow0;
		float4 flowParams;
		float cellLevels[28];  // 7 registers: the 25 levels, padded
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);
	static_assert(sizeof(SimCB) == 240);

	void Step(int32_t shiftX, int32_t shiftY, uint32_t numSources, const SimCB& base, const Environment& environment);

	std::unique_ptr<Texture2D> state[2];
	std::unique_ptr<Texture2D> previousFrame;
	std::unique_ptr<StructuredBuffer> sourceBuffer;
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexelX = 0;
	int64_t originTexelY = 0;
	int64_t previousOriginTexelX = 0;
	int64_t previousOriginTexelY = 0;
	float stepAlpha = 0.0f;
	float previousStepAlpha = 0.0f;
	float texelSize = 8.0f;
	float accumulator = 0.0f;
	bool hasOrigin = false;
	bool ready = false;

	std::mutex sourcesMutex;
	std::vector<Source> pendingSources;
	WaterLevels pendingLevels;
};
