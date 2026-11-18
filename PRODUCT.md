# Product

> Draft written from the codebase (`README.md`, `src/Menu/`, `.claude/CLAUDE.md`) so design tools have context. Edit anything that is wrong. Read by the `cs-imgui-design` skill and by Impeccable and Hallmark when they are installed.

## What it is

Community Shaders is an SKSE plugin that adds advanced graphics features to Skyrim SE/AE. Its in-game menu is a Dear ImGui overlay drawn over the running game: a feature list with per-feature settings, presets, themes, a Home page, a go-to palette, profiling, an issues page, notifications, and the CS Editor for in-game objects.

Platform: desktop, Windows, Direct3D 11, drawn into the game's frame. Input is mouse and keyboard, with keyboard and gamepad navigation enabled. It is not a web page and has no browser, so web-only tooling (HTML detectors, CSS rules) does not apply.

## Surface mode

**Operate.** Users are in a task: change a setting, watch the image behind the menu, decide. Scanability, consistency and familiar controls outrank expression. Brand shows in precise details and in the themes, never in decoration.

## Users (assumptions to confirm)

- **Players** tuning the look of their game. Many are not graphics experts. They want to know what a setting does to the image, change it, and close the menu.
- **Preset and mod authors, testers** who open the menu many times per session, compare settings, and need density, search and undo.
- **Players who need accessibility options:** large text (UI scale and font size), the HighContrast theme, a dyslexia-friendly font, reduced motion, keyboard or gamepad navigation.

## Use context

- The menu sits on top of a live 3D scene of unknown brightness. Legibility over any background matters more than looking good on an empty one.
- People judge changes by the image behind the menu, so the menu should hide as little as possible and fade in and out quickly.
- Sessions are short and repeated. The first screen of every page has to work without scrolling.
- Resolutions run from 1080p to 4K, with a user-set font size and UI scale. Layout is built on a 1080p baseline and scaled.

## Voice

Plain, direct, short. The vocabulary of Skyrim modding ("preset", "weather", "ENB", "load order") is fine; jargon from shader code is explained in a tooltip. Sentence case. No hype, no exclamation marks. Labels name the thing, tooltips say what it does to the image.

## Design principles

1. **The image is the point.** The menu serves the picture behind it.
2. **Familiar controls, quietly styled.** Standard widgets, one accent, the active theme's colours.
3. **Everything is a theme role.** A design that only works in one theme is broken.
4. **Say only true things.** No invented numbers, claims or states.
5. **Every state exists.** Hover, focus, disabled, loading, error, empty, unsaved, overridden.
6. **Reuse before building.** The shared `Util::` widgets own the common patterns.

## Anti-references

- Marketing-style dashboards: gradient text, glowing pills, KPI walls, decorative sparklines.
- Costume UI: faux-parchment, rune frames or "fantasy" ornament drawn onto controls. The Skyrim feel comes from the themes (DragonBlood, DwemerBronze) and type, not from textured chrome.
- Anything that animates while idle, or blocks input and the HUD.
