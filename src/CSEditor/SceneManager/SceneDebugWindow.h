#pragma once

/// Floating CS Editor window showing what the scene resolver is doing right now, for diagnosing why a
/// setting does or does not apply. Developer-facing, so it is opened from the editor's Window menu.
namespace SceneDebugWindow
{
	/**
	 * @brief Draws the window while it is open; the resolver is only sampled while it is.
	 * @param open Pointer to the open/close state owned by the caller.
	 */
	void Draw(bool* open);
}
