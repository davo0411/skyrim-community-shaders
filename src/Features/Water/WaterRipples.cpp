#include "WaterRipples.h"

#include "Util.h"
#include <imgui.h>

#include "RE/P/PlayerCharacter.h"
#include "RE/P/ProcessLists.h"

namespace UnifiedWaterRipples
{
	static PlayerMovementState playerMovementState;

	PlayerMovementState& GetPlayerMovementState()
	{
		return playerMovementState;
	}

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
		float currentRealTime)
	{
		outPlayerPosX = 0.0f;
		outPlayerPosY = 0.0f;
		outPlayerPosZ = 0.0f;
		outPlayerSpeed = 0.0f;
		outPlayerInWater = 0.0f;
		outPlayerVelocityX = 0.0f;
		outPlayerVelocityY = 0.0f;
		outPlayerWaterDepth = 0.0f;
		outWaterSurfaceHeight = 0.0f;

		bool hasWaterHeight = false;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return false;

		auto pos = player->GetPosition();
		outPlayerPosX = pos.x;
		outPlayerPosY = pos.y;
		outPlayerPosZ = pos.z;

		// Get player movement speed from ActorState
		outPlayerSpeed = player->AsActorState()->DoGetMovementSpeed();

		// Calculate actual velocity from position change
		if (playerMovementState.hasPlayerMovementData && currentRealTime > playerMovementState.lastPlayerUpdateTime) {
			float deltaTime = currentRealTime - playerMovementState.lastPlayerUpdateTime;
			if (deltaTime > 0.001f && deltaTime < 1.0f) {  // Sanity check
				playerMovementState.playerVelocity.x = (pos.x - playerMovementState.lastPlayerPos.x) / deltaTime;
				playerMovementState.playerVelocity.y = (pos.y - playerMovementState.lastPlayerPos.y) / deltaTime;
			}
		}
		playerMovementState.lastPlayerPos = pos;
		playerMovementState.lastPlayerUpdateTime = currentRealTime;
		playerMovementState.hasPlayerMovementData = true;

		outPlayerVelocityX = playerMovementState.playerVelocity.x;
		outPlayerVelocityY = playerMovementState.playerVelocity.y;

		// Get the relevant water height for the player
		float playerWaterHeight = player->GetWaterHeight();
		if (playerWaterHeight > -1000000.0f) {
			hasWaterHeight = true;
			outWaterSurfaceHeight = playerWaterHeight;

			// Calculate depth below water surface (positive = underwater)
			outPlayerWaterDepth = playerWaterHeight - pos.z;

			// Player head height estimate: ~120 units above feet (player Z position)
			float estimatedHeadHeight = pos.z + 120.0f;

			// Only create ripples if wading (feet wet but head above water)
			// If head is below water surface, disable ripples (fully submerged)
			if (pos.z < playerWaterHeight + 64.0f && estimatedHeadHeight > playerWaterHeight) {
				outPlayerInWater = 1.0f;
			}
		}

		return hasWaterHeight;
	}

	void UpdateActorRipples(
		ActorRippleBuffer& buffer,
		[[maybe_unused]] float waterSurfaceHeight,
		bool hasWaterHeight)
	{
		buffer.numActors = 0;

		if (!hasWaterHeight)
			return;

		RE::NiPoint3 cameraPos;
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			cameraPos = player->GetPosition();
		}

		const auto processLists = RE::ProcessLists::GetSingleton();
		if (!processLists)
			return;

		for (auto& actorHandle : processLists->highActorHandles) {
			if (buffer.numActors >= MAX_ACTOR_RIPPLES)
				break;

			auto actorPtr = actorHandle.get();
			if (!actorPtr || !actorPtr.get() || !actorPtr.get()->Is3DLoaded())
				continue;

			auto actor = actorPtr.get();
			auto pos = actor->GetPosition();

			// Skip actors too far from camera
			float distFromCamera = cameraPos.GetDistance(pos);
			if (distFromCamera > 4096.0f)
				continue;

			// Get the actor's relevant water height
			float actorWaterHeight = actor->GetWaterHeight();
			if (actorWaterHeight <= -1000000.0f)
				continue;

			// Check if actor is near water surface
			float heightAboveWater = pos.z - actorWaterHeight;
			bool nearWater = (heightAboveWater > -256.0f && heightAboveWater < 128.0f);

			if (!nearWater)
				continue;

			ActorRippleData& ripple = buffer.actors[buffer.numActors];
			ripple.PosX = pos.x;
			ripple.PosY = pos.y;

			// Estimate head height for actor (roughly 100-120 units above feet)
			float estimatedHeadHeight = pos.z + 100.0f;

			// Only enable ripples if wading (not fully submerged)
			ripple.InWater = (heightAboveWater < 64.0f && estimatedHeadHeight > actorWaterHeight) ? 1.0f : 0.0f;
			ripple.WaterDepth = actorWaterHeight - pos.z;  // Positive = below surface

			// Get actor movement speed from ActorState
			ripple.Speed = actor->AsActorState()->DoGetMovementSpeed();

			// Get velocity direction from actor's angle
			// Note: We don't have frame-to-frame position tracking for NPCs,
			// so we use their rotation angle as an approximation
			float angleZ = actor->GetAngleZ();  // Actor's facing direction in radians
			float speed = ripple.Speed;
			ripple.VelocityX = sin(angleZ) * speed;
			ripple.VelocityY = cos(angleZ) * speed;

			buffer.numActors++;
		}
	}

	void DrawRippleSettings(RippleSettings& settings)
	{
		ImGui::Checkbox("Enable Actor Ripples", &settings.EnableActorRipples);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Creates ripple effects when actors (player and NPCs) wade through water.");
		}

		if (settings.EnableActorRipples) {
			ImGui::Spacing();
			ImGui::Text("Ripple Appearance");
			ImGui::SliderFloat("Ripple Strength", &settings.RippleStrength, 0.0f, 3.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Overall intensity of ripple effects.");
			}
			ImGui::SliderFloat("Ripple Radius", &settings.RippleRadius, 128.0f, 1024.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Maximum distance ripples spread from actor.");
			}
			ImGui::SliderFloat("Normal Strength", &settings.RippleNormalStrength, 0.0f, 5.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How much ripples affect water surface normals.");
			}

			ImGui::Spacing();
			ImGui::Text("Wave Animation");
			ImGui::SliderFloat("Wave Speed", &settings.RippleWaveSpeed, 1.0f, 10.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Speed of ripple wave animation.");
			}

			ImGui::Spacing();
			ImGui::Text("Wave Frequencies");
			ImGui::SliderFloat("Primary Freq", &settings.RippleWaveFreq1, 0.02f, 0.2f, "%.3f");
			ImGui::SliderFloat("Secondary Freq", &settings.RippleWaveFreq2, 0.04f, 0.3f, "%.3f");
			ImGui::SliderFloat("Tertiary Freq", &settings.RippleWaveFreq3, 0.06f, 0.4f, "%.3f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Higher values = more ripple rings per unit distance.");
			}
		}
	}
}
