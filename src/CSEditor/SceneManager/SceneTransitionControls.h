#pragma once

/// The Scene Manager's global transition settings, drawn on the CS Editor's Scene Manager page.
namespace SceneTransitionControls
{
	/**
	 * @brief Draws the time of day, location, weather and easing sliders.
	 *
	 * Each slider shows a Reset while the user holds their own value, which hands the control back to the active
	 * preset or the default.
	 */
	void Draw();
}
