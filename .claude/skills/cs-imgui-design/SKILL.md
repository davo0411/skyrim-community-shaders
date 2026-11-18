---
name: cs-imgui-design
description: Designs and builds any new or changed Community Shaders UI so it is cohesive and good-looking, not generic: a new feature, a new system such as the CS Editor, menu pages, settings, overlays, notifications, feature windows and HUD panels. Use automatically whenever UI is generated or edited for CS, and when asked to design, redesign, polish or add UI or to make a screen look less AI-made. Runs shape, audit, direction, themed canvas mockup, critique, implementation with shared widgets, build, in-game capture, scored review and a Pretty UI gate, calling the installed design skills along the way.
---

# CS ImGui design

Turns "add a settings page for X" or "make this window look right" into finished code in `src/` that looks like it belongs in Community Shaders and is not slop. It orchestrates local tools and the installed design skills (Impeccable, unslop, ponytail), and applies the distilled rules from those and Hallmark.

**Read first, every run:** `PRODUCT.md` and `DESIGN.md` (repo root: who it is for, mode, theme model, tokens, motion, components), `.claude/CLAUDE.md` (ImGui, i18n, build rules), and the nearest existing page to what you are building.

**References** (all in `references/`): `in-game-capture.md` (how to capture the UI; read before step 2), `design-principles.md` (the rules), `reuse-catalog.md` (shared widgets), `audit-checklist.md` (signs of slop), `critique-rubric.md` (scored review), `pretty-ui-checklist.md` (the gate), `skill-map.md` (exact external calls and briefs), `ATTRIBUTION.md`.

## Ground rules

- **The live game is the reference.** Capture it before judging; if no capture can be taken, stop and say so. Do not design from code alone.
- **Stay in scope.** Change only the requested surface and the constants it needs. Never restyle neighbours or re-apply settings values.
- **Reuse before building.** Check `reuse-catalog.md`. A new pattern becomes a shared `Util::` widget, not inline page code.
- **Roles, not values.** Colours from role accessors, sizes from `ThemeManager::Constants` times `Util::GetUIScale()`, fonts by role. A design that only works in one theme is broken.
- **Say only true things.** No invented numbers, claims or states.
- **No commit or push** unless asked.
- **Be terse in the report;** the work is in the code and the captures.

## Which surface

| Surface | Where | Capture |
|---|---|---|
| Menu pages and shared controls | `src/Menu/*Renderer.cpp`, `src/Menu/ThemeManager.h` | `mcp__devbench-se__menu` to open it, then `capture` |
| Overlays, status bar | `OverlayRenderer.cpp`, `StatusBarRenderer.cpp` | trigger it, then `capture` |
| Notifications | `src/Utils/Notifications.cpp`, `UISurface.cpp`, `UIAnim.cpp` | fire it with a devbench scenario, then `capture` quickly |
| Feature windows, HUD panels | `src/Features/*.cpp` calling `ImGui::Begin`, draw-list HUD code | open it from the menu or setting, then `capture` |
| Tool systems (CS Editor) | `src/CSEditor/` | open from its entry point; capture each sub-window |
| New feature or system | `src/Features/<Name>.cpp/h`, `features/<Name>/`, or a new system in `src/` | design the entry point, settings and every window first; start from `docs/new-feature-template/` |

A new feature or system gets a design pass for its whole UI (entry point, settings, every window, empty, loading and error states), planned in step 3.

## Workflow

Keep the task list current. Steps 1 and 5 are skipped for a small tweak to an existing view; say so when you skip.

### 1. Shape (new feature, new system, or a heavy rework)
Plan the UX before any visual work: the task path, the screen regions, the states, the entry point. Call `Skill` `impeccable` with args `shape <feature>` using the brief in `skill-map.md`. If it is not installed, write the plan yourself from `design-principles.md` §1, §2 and §7. Output: a short plan the user could correct.

### 2. Capture the live state
First confirm the surface can appear in the current build: read what triggers it (the Constraints table, for example, only shows when a feature overrides `GetActiveConstraints()`, and none does today). If it cannot appear, say so and pick another surface.

Then capture it **with the UI**: devbench `capture` and the CS screenshot feature exclude ImGui, so use the RenderDoc route in `references/in-game-capture.md` (menu open via the End key, RenderDoc frame, export the last draw's render target, crop). Read the capture. Note resolution, font size, theme and the scene behind it. Also read the nearest page's code for the patterns to match.

### 3. Audit (existing UI) and direction
- **Audit:** go through `audit-checklist.md` A to D against the capture and the code. Run `python tools/cs-ui-lint.py <files>`. For a second opinion call `Skill` `unslop:unslop` as an audit only (brief in `skill-map.md`). Classify heavy, light or none.
- **Direction:** write the **CS Picks** block (`design-principles.md` §13): surface, job, character, moment, signature, reuse, material, gates. Apply the mirror test. For a heavy restyle of an existing page, show the audit and the Picks block and wait for the user's answer before editing; for new UI and light polish, proceed.
- Light polish keeps the page's identity. Never change a good screen just to make it different.

### 4. Mockup in the canvas, in the real theme
1. `python tools/cs-theme-to-mockup.py Default --height 1080` prints the arguments for the canvas. Apply them: `mcp__imgui__imgui_set_viewport_size`, `imgui_load_font` + `imgui_set_font` (the Body font from the output), `imgui_set_style` with the printed `set_style`, then one `imgui_set_style_var` per `style_vars` entry. Repeat with `Light` and `HighContrast` for the theme check.
2. Build the view from the Picks block with `imgui_create_window` and `imgui_add_widget` (tables, tab bars, collapsing headers, checkboxes, sliders, combos), using the same widget kinds the real renderer will use. Put real labels and plausible values in it, and one state worth seeing (empty, error, unsaved).
3. `imgui_screenshot` to a file in the scratchpad. Iterate at most three times against the live capture from step 2.
4. The canvas is ImGui 1.92.8 with no `Util::` widgets: judge composition and rhythm, not exact widget looks. The game pins 1.92.6.

### 5. Pre-build critique
Score the mockup on the five axes in `design-principles.md` §14 (anything under 3 is revised first) and with `critique-rubric.md`. When installed, also call `Skill` `impeccable` with args `critique <mockup screenshot path>` (brief in `skill-map.md`). Fix the P0 and P1 findings in the mockup before writing C++.

### 6. Implement
Follow `design-principles.md` and `DESIGN.md`. Apply unslop's passes in order across the whole view, finishing each before the next: spacing, colour, icons, copy, type, layout, details, motion. Spacing comes first because everything reads differently once the rhythm is right; copy comes before type and layout because its length decides sizes.
- Shared widgets from `reuse-catalog.md`; constants in `ThemeManager::Constants`; strings through `T()`; `Util::Button`; paired `BeginTable`/`EndTable`; stable IDs; RAII guards; callbacks and parameters, not new public `Menu` members. Doxygen on new public methods.
- Check ImGui APIs against the pinned 1.92.6 headers in vcpkg, not memory or the canvas.
- The PostToolUse hook runs `tools/cs-ui-lint.py` after each edit; resolve its findings as they appear.
- Do not touch backend, renderer or font-atlas setup unless asked. A settings structure change bumps the feature `.ini` version.

### 7. Build and strings
`./BuildDevFast.bat`; fix every error and warning in touched files. Then `python tools/extract-i18n.py --write` and `python tools/sort-i18n.py --write --prune-extra`, then formatting.

### 8. Verify in game
**Do not deploy over the user's test build.** Check which branch the deployed DLL came from (the menu title shows it) and ask before replacing it. Deploy as the checkout that owns `build/dfb` normally does, relaunch, open the surface, and capture it with the RenderDoc route (`references/in-game-capture.md`), saved as `cs-ui-<name>-after`. Check Default; theme switching, hover, focus and gamepad cannot be driven through devbench today, so do them by hand with the user or mark them unverified. Check the extremes from `design-principles.md` §9: largest font size, longer strings, 1080p and 1440p, empty and error states.

### 9. Review and gate
1. `python tools/cs-ui-lint.py --changed --strict`: fix every ERROR and every WARN the change added.
2. Score the after capture with `critique-rubric.md`; when installed, call `Skill` `impeccable` with args `critique <after capture path>`. Deeper single passes on request: `layout`, `typeset`, `clarify`, `harden`, `polish` (briefs in `skill-map.md`).
3. Walk `pretty-ui-checklist.md`. Fix every FAIL in sections 1, 4 and 7 and every open P0 or P1; fix or waive the rest with a reason and the evidence.
4. `Skill` `ponytail:ponytail-review` on the diff; cut what it flags.
5. Re-run the audit (`audit-checklist.md` A) on the after capture. Anything still listed gets fixed.

### 10. Report
Terse. The CS Picks block (with the gates row filled), the before and after capture paths, the critique score and top findings, the lint result, the checklist table (PASS, FAIL, N/A, waivers with evidence), what each external skill contributed and what was rejected, and everything unverified (themes, resolutions, gamepad). Then stop.

## Stop conditions

- No capture possible (game not running, menu will not open): report and stop before editing.
- Build fails in code you did not touch: report with the error; do not fix unrelated code.
- The request conflicts with a direction the user already approved: ask once, then proceed.
