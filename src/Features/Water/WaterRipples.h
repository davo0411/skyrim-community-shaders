#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include "RE/N/NiPoint2.h"
#include "RE/N/NiPoint3.h"

class ConstantBuffer;

namespace UnifiedWaterRipples
{
	static constexpr uint32_t MAX_ACTOR_RIPPLES = 32;

	struct RippleSettings
	{
		bool EnableActorRipples = true;
		float RippleStrength = 1.0f;
		float RippleRadius = 512.0f;
		float RippleWaveSpeed = 4.0f;
		float RippleWaveFreq1 = 0.08f;
		float RippleWaveFreq2 = 0.12f;
		float RippleWaveFreq3 = 0.18f;
		float RippleNormalStrength = 2.0f;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		RippleSettings,
		EnableActorRipples,
		RippleStrength,
		RippleRadius,
		RippleWaveSpeed,
		RippleWaveFreq1,
		RippleWaveFreq2,
		RippleWaveFreq3,
		RippleNormalStrength)

	struct alignas(16) ActorRippleData
	{
		float PosX;
		float PosY;
		float Speed;
		float InWater;  // 1.0 if actor is in water, 0.0 otherwise
		float VelocityX;  // Actual velocity for wake direction
		float VelocityY;
		float WaterDepth;  // Depth below water surface (negative = above)
		float pad0;
	};

	struct alignas(16) ActorRippleBuffer
	{
		ActorRippleData actors[MAX_ACTOR_RIPPLES];
		uint32_t numActors;
		uint32_t pad0[3];
	};

	// Player movement tracking state
	struct PlayerMovementState
	{
		RE::NiPoint3 lastPlayerPos{ 0.0f, 0.0f, 0.0f };
		RE::NiPoint2 playerVelocity{ 0.0f, 0.0f };
		float lastPlayerUpdateTime = 0.0f;
		bool hasPlayerMovementData = false;
	};

	// Get the singleton player movement state
	PlayerMovementState& GetPlayerMovementState();

	// Update player ripple data and return whether player is in water
	bool UpdatePlayerRippleData(
		float& outPlayerPosX,
		float& outPlayerPosY,
		float& outPlayerPosZ,
		float& outPlayerSpeed,
		float& outPlayerInWater,
		float& outPlayerVelocityX,
		float& outPlayerVelocityY,
		float& outPlayerWaterDepth,
		float& outWaterSurfaceHeight,
		float currentRealTime);

	// Update actor ripple buffer with all actors near water
	void UpdateActorRipples(
		ActorRippleBuffer& buffer,
		float waterSurfaceHeight,
		bool hasWaterHeight);

	// Draw ImGui settings UI for ripples
	void DrawRippleSettings(RippleSettings& settings);
}
