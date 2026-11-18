---
name: cs-imgui-design
description: End-to-end design and implementation of any new UI brought into Community Shaders: a new feature, a new system such as the CS Editor, menu pages, settings, feature cards, overlays, notifications, feature windows and HUD panels. Use whenever new or changed UI is generated for CS, or when the user asks to design, redesign, polish, or add UI, or to make a screen look less generic or AI-made. Chains audit, direction, canvas mockup, implementation, build, in-game capture via devbench, and review.
---

# CS ImGui design

This skill turns a request like "make the Presets page look good" into a finished change in `src/Menu/`. It is a playbook, not a template generator: every screen is judged against the live game and the project's own conventions before anything is written.

Read `CLAUDE.md` (project and user) first. Its ImGui, i18n, D3D11 and Features rules apply in full. The rules below add the workflow on top.

## Tools and what each is for

| Step | Tool | Purpose |
|---|---|---|
| Ground truth | `mcp__devbench-se__capture`, `mcp__devbench-se__menu` | Open the real page in the running game and capture it. Use this before judging anything. |
| Audit and direction | `unslop` plugin (`/unslop`), `references/audit-checklist.md` here | Find slop signs and set a direction. Its signs and checklist are web-oriented, so apply the ImGui translations in the checklist. |
| Anti-slop principles | `hallmark` plugin (Nutlope/hallmark, v2.0.0) | Its tell list and gate tiers (Floor, Reflex, Finish) feed the audit. Its build path and CSS floor are web-only: do not run `/hallmark` to build a menu page, and do not run its `sloplint.mjs` on C++. Read its references for the tells, then apply them through section D of the audit checklist. |
| Critique (optional) | `impeccable` / `design-taste-frontend` skills | Second opinion on hierarchy and rhythm. Use only if the user asks for a deeper critique. |
| Mockup | `mcp__imgui__imgui_*` (standalone canvas) | Composition, spacing and hierarchy on a throwaway layout. It is not Skyrim and not the CS ImGui build. |
| Implement | Edit under `src/Menu/`, `src/Menu/ThemeManager.h` | The shipped code. Mockups are never copied verbatim. |
| Build | `./BuildDevFast.bat` (compile check), `./BuildDev.bat` when a full AIO is needed | Compile and package. |
| Verify | `mcp__devbench-se__capture` with `golden` once a golden exists, else plain capture | Compare the shipped page to the mockup and the before shot. |
| Pretty UI gate | `tools/cs-ui-lint.py` (automatic via `.claude/settings.json` hook) and `references/pretty-ui-checklist.md` | Deterministic convention and slop checks on every UI edit, plus the visual checklist for the final review. |
| Simplify | `ponytail:ponytail-review` | Cut what the change does not need. |
| i18n | `python tools/extract-i18n.py --write`, `python tools/sort-i18n.py --write --prune-extra` | Keep `en.json` in sync. |

Known limit: the imgui MCP canvas is ImGui 1.92.8 with its own fonts and theme presets. The game pins 1.92.6 and uses its own theme. Use the canvas for layout and rhythm only. Copy colours and sizes from `ThemeManager::Constants` and the live game, never from the canvas theme presets (`dark`, `scifi`, and so on).

## Surfaces

Every in-game UI surface falls under this skill. Find which one the change touches first; it sets where the code goes and how to capture it.

| Surface | Where it lives | How to capture it |
|---|---|---|
| Main menu pages and shared controls | `src/Menu/*Renderer.cpp`, `src/Menu/ThemeManager.h` | `mcp__devbench-se__menu` to open the page, then `capture` |
| Overlays and status bar | `src/Menu/OverlayRenderer.cpp`, `src/Menu/StatusBarRenderer.cpp` | Trigger the overlay (its settings or a scenario), then `capture` |
| Notifications and toasts | `src/Utils/Notifications.cpp`, `src/Utils/UISurface.cpp`, `src/Utils/UIAnim.cpp` | Fire the notification with a devbench scenario, then `capture` quickly |
| Feature-owned windows and HUD panels | `src/Features/*.cpp` that call `ImGui::Begin` (for example `LightLimitFix`, `UnifiedWater`, `PerformanceOverlay/ABTesting`) | Open the feature's window (menu or setting), then `capture` |
| Draw-list-only HUD elements | Code using `GetBackgroundDrawList` / `GetForegroundDrawList` / `ImDrawList` | Trigger the element in game; judge at 1080p and 1440p |
| Tool systems such as the CS Editor | `src/CSEditor/` (`EditorWindow`, browser, palette, weather, light, scene manager windows) | Open the editor from its in-game entry point, then `capture`. Each sub-window needs its own capture. |
| New features and new systems | A new `src/Features/<Name>.cpp/h` plus its `features/<Name>/` folder, or a new top-level system in `src/` | Design the surface first (menu entry, settings window, overlay, or system window), then capture it using the matching row above. Start the feature from `docs/new-feature-template/`. |

Rules that apply to every surface:
- Route strings through `T()` and colours through `ThemeManager::Constants`, even outside `src/Menu/`. If a feature's constants live in its own file, move them under `ThemeManager::Constants` in the same change.
- Overlays and notifications must not block input or the game's own HUD. Check that they stay out of the way at the smallest supported resolution.
- Any surface with a setting gets the same stage and settings treatment as the rest of the feature (`.ini` version bumps when the settings structure changes, per `CLAUDE.md`).
- A new feature or system gets a design pass for its whole UI, not just one window: entry point, settings, every window, and its states (empty, loading, error). Plan all of them in the direction step.

## Workflow

Run these in order. Keep the task list updated while you work.

### 1. Scope the page
- Name the exact surface from the Surfaces table and the file or widget within it, for example `HomePageRenderer`, `OverlayRenderer`, `Notifications.cpp`, or a feature window in `src/Features/`.
- Stay in scope. Change only what the request covers. Do not re-skin neighbouring pages (see the stay-in-scope memory).
- If the request is vague ("make it nicer"), pick the page the user has open or last mentioned, and say which one you took.

### 2. Capture the live state
- Start from the running game. Use `mcp__devbench-se__menu` to open the page, then `mcp__devbench-se__capture` with a `checkpointId` such as `cs-imgui-<page>-before`.
- Read the captured frame. Do not work from the code alone; the live page is the reference.
- Capture at the user's resolution and UI scale. Note both in the report.

### 3. Audit
- Go through `references/audit-checklist.md` section A (slop signs), section D (Hallmark tells) and section B (CS conventions). Write one line per finding with `file:line` where possible.
- Classify the work:
  - **Heavy**: many signs across colour, type, icons, copy and spacing. Full passes.
  - **Light**: mostly fine, a few signs. Fix only the findings.
  - **None**: say so and propose at most three small refinements with exact values. Stop there unless the user agrees.

### 4. Direction
Write five lines, derived from what the feature does and who uses it. Never from trends.
- **Product**: what this screen controls, for whom.
- **Character**: two or three concrete words. Not "clean", "modern", "sleek" or "premium".
- **World**: the material or place the mod belongs to (Skyrim's world: parchment, iron, weather, lantern light). Use it to pick the neutral tint and accent, not as decoration.
- **One bright element**: the single thing that carries colour and craft on this page. Everything else stays quiet.
- **Signature detail**: one small, specific thing only this page has.

If this changes the look of an existing page (heavy mode), show the audit and direction and wait for the user's answer before editing. Light polish and fixes can proceed without a wait.

### 5. Mockup in the canvas
- Build the page's layout with `mcp__imgui__imgui_create_window` / `imgui_add_widget` / `imgui_create_scene`. Use the same widget kinds the real renderer uses (tables, tab bars, collapsing headers, checkboxes, sliders, combos).
- Set colours to the `ThemeManager::Constants` values you intend to use. Do not rely on the canvas theme presets.
- Render with `mcp__imgui__imgui_screenshot`. Iterate at most three times. Compare against the live capture from step 2.
- Export with `mcp__imgui__imgui_export_cpp` if the layout is worth keeping as a reference for step 6. Treat the exported code as a sketch, not a patch.

### 6. Implement
Follow the project rules exactly:
- Spacing, padding and colours come from `ThemeManager::Constants` (`src/Menu/ThemeManager.h`). Add new constants there if needed. No hardcoded pixels or colour literals in renderers.
- `Util::Button`, not `ImGui::Button`, for buttons. `Util::IconButton` for icon buttons.
- Icons: Tabler for UI, written `Icons::TI(ICON_TI_<NAME>)` (see `src/Menu/Icons/helpers/IconFonts.h`). Never emoji in labels. FA and Game-Icons only for world-style icons.
- `BeginTable` / `EndTable` paired. RAII guards for style changes: `Util::DisableGuard` (`src/Utils/UI.h`), `MenuFonts::FontRoleGuard` and `MenuFonts::ImFontGuard` (`src/Menu/Fonts.h`). `CLAUDE.md` also names `MenuFonts::TabBarPaddingGuard`, but no such class exists in `src/` today, so do not use it until it is found or added.
- Stable IDs: `PushID` with a stable key, or `##suffix`, for repeated widgets.
- Every user-visible string through `T()` with a key under the right prefix (`menu.<page>.<item>`, `feature.<short>.<setting>`, and so on).
- Access menu state through callbacks and parameters, not by making `Menu` members public (`src/Menu/*Renderer` pattern).
- Use the APIs of the pinned ImGui 1.92.6 headers in vcpkg, not snippets from the canvas or from memory.
- Keep edits to the page's own renderer and its shared constants. Do not touch backend, font-atlas or DX11 setup code.

### 7. Build and i18n
1. `./BuildDevFast.bat`. Fix every error and warning in touched files.
2. If strings changed: `python tools/extract-i18n.py --write`, then `python tools/sort-i18n.py --write --prune-extra`. Translate only the English source; Weblate handles other locales.
3. `pre-commit run --all-files` or `cmake --build ./build/ALL --target FORMAT_CODE` for formatting.

### 8. Verify in game
- Deploy the build the way the team already does (dev-cycle / mux tools for this checkout) and relaunch.
- Open the page with `mcp__devbench-se__menu`, then `mcp__devbench-se__capture` with `checkpointId` `cs-imgui-<page>-after`.
- Compare with the before capture and the mockup. Check: spacing rhythm, alignment of columns, text overflow at the chosen font scale, hover and focus states, disabled states, and a second resolution if the layout uses fixed widths.
- Run the audit checklist again (section A). Anything still listed gets fixed before reporting.

### 9. Review
- Run `python tools/cs-ui-lint.py --changed --strict`. Fix every ERROR and WARN the change added.
- Run the Pretty UI checklist (`references/pretty-ui-checklist.md`) against the after capture. Every FAIL in sections 1, 3, or 6 is fixed before reporting; the rest are fixed or waived with a reason.
- Run `ponytail:ponytail-review` on the diff. Remove anything it flags as unneeded.
- Report: direction (five lines), what changed per pass, before and after captures, the build result, and any open items. Do not commit unless the user asks. Follow the commit rules in `CLAUDE.md` if they do.

## Stop conditions
- The live capture cannot be taken (game not running, menu will not open): report it and stop before editing. Do not design from the code alone.
- The build fails in code you did not touch: report it with the error, do not fix unrelated code.
- The request conflicts with the direction the user already approved: ask once, then proceed.
