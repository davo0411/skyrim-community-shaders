# Pretty UI checklist

The gate every new or changed CS UI passes before it is reported as done. Two layers:

1. **Automatic (code):** `tools/cs-ui-lint.py` runs as a Claude Code PostToolUse hook (`.claude/settings.json`) on every Edit or Write to a UI file. Any finding goes back to the agent as feedback. Run it by hand with `python tools/cs-ui-lint.py --changed --strict`.
2. **Manual (eyes):** the items below, answered from the in-game capture, not from the code.

Mark each item PASS, FAIL, or N/A with a reason. Every FAIL gets fixed or waived in writing. Put the table in the final report.

## 1. Automatic checks (must be clean)

- [ ] No `ImGui::Button`; buttons go through `Util::Button`. (ERROR)
- [ ] No `TODO` or `FIXME` in shipped code. (ERROR)
- [ ] No character above U+FFFF in UI strings. The game font cannot load it. (ERROR)
- [ ] `BeginTable`/`EndTable` and `PushID`/`PopID` pair in each file. (ERROR)
- [ ] Every user-visible literal goes through `T()`. (WARN)
- [ ] No colour literals outside `ThemeManager` and the icon/font files. (WARN)
- [ ] No all-caps labels, middle-dot meta strings, or arrows glued to labels. (WARN)
- [ ] No fixed pixel sizes in `ImVec2(...)` that should scale with the font. (WARN)

Warnings on legacy code in a touched file count against the change only when the change adds them.

## 2. Cohesion (does it belong with the rest of CS?)

- [ ] **Same family.** Spacing, rounding, border weight, and button shape match the nearest existing page or feature.
- [ ] **One accent.** The accent colour marks the active or changed state, not decoration.
- [ ] **Same vocabulary.** Labels and tooltips use the terms the menu already uses for the same settings.
- [ ] **Same icon style.** Tabler icons for UI; no mixing with FA or Game-Icons outside world-style icons.
- [ ] **Same motion.** Hover and open/close easing match the menu; nothing animates while idle.

## 3. Hierarchy and layout

- [ ] **One primary thing.** Each view has one element the eye lands on first.
- [ ] **Scannable grouping.** Related settings are grouped with a header, separator, or indent. No flat wall of same-weight controls.
- [ ] **Most-used first.** The settings people change most are at the top.
- [ ] **Actions near their content.** Apply, reset, and per-setting buttons sit next to what they change.
- [ ] **Empty, loading, and error states** exist and are designed, not left as blank panels.

## 4. Type and copy

- [ ] **Labels are short** and in sentence case.
- [ ] **Tooltips explain one thing** and say what the setting does to the image, not how the code works.
- [ ] **No invented numbers or claims** in any text.
- [ ] **Text fits** at the default font scale and at the largest scale the menu allows, without clipping or overlap.
- [ ] **Units and ranges are shown** on sliders and inputs.

## 5. Colour and contrast

- [ ] **Readable text.** Body text and labels pass the contrast the menu already uses in its darkest and lightest themes.
- [ ] **State is not colour alone.** Enabled, disabled, warning, and error also differ by label or icon.
- [ ] **Disabled controls** look disabled and say why, where the reason is not obvious.

## 6. States and feedback

- [ ] **Hover, focus, active, and disabled** each look distinct.
- [ ] **Keyboard and gamepad** reach every control (menu nav is enabled).
- [ ] **Changes are visible.** Saving, applying, or a failed setting gives feedback through the existing notification path.
- [ ] **Destructive actions** ask first.

## 7. Placement and scale

- [ ] **Clear of the game's HUD** at the smallest supported resolution.
- [ ] **Overlays and notifications** do not block input or cover the crosshair or dialogue.
- [ ] **Checked at 1080p and 1440p**, plus the user's UI scale if it differs.
- [ ] **Stays on screen** when the window is resized or moved to an edge.

## 8. Performance and polish

- [ ] **No per-frame allocation** of strings or vectors in the draw path, unless the existing code already does it.
- [ ] **No redundant redraws**: static content is not rebuilt every frame without a reason.
- [ ] **Nothing pulses or loops** on screen when the user is idle.

## Scoring

- **All sections PASS:** ship.
- **Any FAIL in sections 1, 3, or 6:** fix before reporting. These are the ones people feel first.
- **FAIL only in 2, 4, 5, 7, or 8:** fix, or waive with a reason in the report.
