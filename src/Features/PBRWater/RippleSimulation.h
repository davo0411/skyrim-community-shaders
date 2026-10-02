#pragma once

#include "Buffer.h"

#include <mutex>
#include <vector>

/**
 * @brief Camera-centred interactive ripple simulation (GPU wave equation).
 *
 * Replaces the vanilla wading displacement mesh: instead of a separate mesh that follows the player,
 * every actor limb and loose Havok object touching the water pushes the surface of a scrolling
 * heightfield, which the water shaders sample for displacement, normals and foam.
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
		float foam = 0.0f;
	};

	struct Settings
	{
		float extent = 4096.0f;     ///< world units covered by the grid
		float waveSpeed = 1.2f;     ///< m/s
		float halfLife = 1.5f;      ///< s, amplitude decay
		float foamHalfLife = 3.0f;  ///< s
		float foamFromMotion = 6.0f;
	};

	void SetupResources();
	void ClearShaderCache();
	void Reset();

	/** @brief Main thread: replaces the pending source list. */
	void SubmitSources(std::vector<Source>&& sources);

	/**
	 * @brief Render thread: steps the simulation to the current camera position.
	 * @param dt game-time seconds since the last update (0 while paused)
	 */
	void Update(float cameraX, float cameraY, float dt, const Settings& settings);

	ID3D11ShaderResourceView* GetSRV() const;
	/** @brief Absolute world position of the grid's (0, 0) corner. */
	float GetOriginX() const { return static_cast<float>(originTexelX) * texelSize; }
	float GetOriginY() const { return static_cast<float>(originTexelY) * texelSize; }
	float GetExtent() const { return texelSize * GridSize; }
	bool IsReady() const { return ready; }

private:
	struct alignas(16) GpuSource
	{
		float2 position;
		float radius;
		float depth;
		float foam;
		float3 pad;
	};
	static_assert(sizeof(GpuSource) == 32);

	struct alignas(16) SimCB
	{
		int32_t shiftX;
		int32_t shiftY;
		uint32_t numSources;
		uint32_t gridSize;
		float damping;
		float foamDecay;
		float waveSpeed2;
		float foamFromMotion;
	};
	STATIC_ASSERT_ALIGNAS_16(SimCB);

	void Step(int32_t shiftX, int32_t shiftY, uint32_t numSources, const SimCB& base);

	std::unique_ptr<Texture2D> state[2];
	std::unique_ptr<StructuredBuffer> sourceBuffer;
	std::unique_ptr<ConstantBuffer> simCB;
	winrt::com_ptr<ID3D11ComputeShader> simCS;
	uint32_t current = 0;

	int64_t originTexelX = 0;
	int64_t originTexelY = 0;
	float texelSize = 8.0f;
	float accumulator = 0.0f;
	bool hasOrigin = false;
	bool ready = false;

	std::mutex sourcesMutex;
	std::vector<Source> pendingSources;
};
