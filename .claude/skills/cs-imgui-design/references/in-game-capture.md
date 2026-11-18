# Capturing the CS UI in game

Learned in the first trial run (2026-11-18). **Neither devbench `capture` nor `communityshaders_capture kind=screenshot` shows the ImGui layer.** CS's screenshot feature deliberately saves a clean pre-UI scene (`src/Features/ScreenshotFeature.cpp`: "Pre-UI scene snapshot (no HUD, no menu blur)"), and devbench's native provider copies that same file. A capture taken with the menu open shows only the game.

What works: a RenderDoc frame capture, which records the final backbuffer including ImGui, then export that render target. About a minute and a 1.5 GB file, so batch: capture every page and state you need in one session, not one per tweak.

## Recipe

1. **Open the surface.** `mcp__devbench-se__input` `tap` keyboard, key `207` (End, the default menu toggle), `owner` any stable name. Navigate with further taps or by hand. Do not assume it opened: the next steps show the truth. If the menu was already open, the tap closes it.
2. **Capture.** `mcp__devbench-se__communityshaders_capture` `kind: renderdoc`, `frames: 1`. Needs `DeveloperTools` loaded (`communityshaders_feature get DeveloperTools` returns the RenderDoc settings when it is). The file appears in `<MO2 overwrite>\SKSE\Plugins\CommunityShaders\Captures\Skyrim_AE_*_frame<N>.rdc` within about 10 seconds; take the newest.
3. **Open it.** `mcp__renderdoc-mcp__open_capture` with that path.
4. **Find the last draw.** Event IDs have gaps and `totalEvents` is not the highest ID. Call `list_draws` with `limit` 2000: the result is saved to a file; parse it with Python for `"eventId":(\d+)` and take the largest (ImGui draws come last). `offset` is ignored.
5. **Export.** `goto_event` `eventId` = that ID, then `export_render_target` `index` 0. The PNG lands in `Captures\renderdoc-mcp-export\rt_<event>_0.png`.
6. **Make it viewable.** The PNG is RGBA with alpha 0: `Image.open(p).convert("RGB")`. Crop the region you need from the full-resolution image; a downscaled full frame hides detail.

## Measuring from the capture

Contrast: sample the text region, take the median luminance as background and the 98.5th percentile as text, and compute the WCAG ratio. It is a floor (antialiasing lowers the peak) and depends on the scene behind the menu, so note the scene. Layout: crop and measure in pixels; divide by `font size / 21` to get 1080p-baseline units.

## What this cannot show

Theme switching, hover and focus states, gamepad navigation and animation need input the devbench tools do not provide for the menu today. Say so in the report instead of claiming those states were checked.

## Do not deploy over the user's test build

The Bottlerim profile runs a build from whichever branch the user was testing. The menu title shows it (for example `[feat/pbr-water*][v1.9.0-…-dirty]`). Before deploying a build from another tree, compare that with the tree you built, and ask first; a different branch's DLL can break the user's in-progress work (a DLL and shader mismatch shows as garbage colours). A fresh worktree also has no `build/` or vcpkg tree, so the first configure is slow and network-sensitive; prefer building where `build/dfb` already exists.
