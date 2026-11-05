#pragma once

#include "WaveModel.h"

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

/**
 * @brief Reads a worldspace's terrain straight from the LAND records of the plugin files.
 *
 * The game keeps LAND heights in memory only for the cells around the player, and the pre-generated heightmaps quantise
 * z too coarsely to know where a shore is: the depth waves feel has to come from the records themselves. Plugins are
 * read as files, so this runs on a worker thread and never touches the engine's own file state.
 */
namespace PBRWaterLand
{
	/// Everything Load needs from the game, gathered on the main thread.
	struct Request
	{
		std::vector<std::filesystem::path> plugins;  ///< active plugins in load order: a later plugin's LAND overrides an earlier one's
		std::string owner;                           ///< file name of the plugin that defines the worldspace
		uint32_t worldSpace = 0;                     ///< its form ID local to that plugin
	};

	/**
	 * @brief Every LAND of the worldspace as one grid of its terrain vertices (32 quads per cell, 128 units apart).
	 * Cells without a LAND read as deep water, below the lowest terrain.
	 * @return the grid, or null when no plugin has terrain for the worldspace or `stop` was requested
	 */
	std::shared_ptr<PBRWaterModel::Bathymetry> Load(const Request& request, std::stop_token stop);
}
