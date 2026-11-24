#pragma once

/**
 * @brief Stops the engine's own water ripple work while PBR Water is loaded.
 *
 * PBR Water's ripple simulation has its own sources, so the engine's ripple list (TESWaterSystem::AddRipple)
 * and the rain ripple meshes (TESWaterObject::waterRippleObject) are dead weight.
 */
namespace PBRWaterEngine
{
	/// Installs the hooks. Call once, from PBRWater::PostPostLoad.
	void Install();
}
