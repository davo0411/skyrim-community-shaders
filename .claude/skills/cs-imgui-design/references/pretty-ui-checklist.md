# Pretty UI checklist

The gate every new or changed CS UI passes before it is reported done. Three layers:

1. **Automatic (code):** `tools/cs-ui-lint.py` runs as a PostToolUse hook (`.claude/settings.json`) on every edit to a UI file, and by hand with `python tools/cs-ui-lint.py --changed --strict`. Findings go back to the agent.
2. **Scored review:** `critique-rubric.md` applied to the in-game capture, by this skill and, when installed, by a live `impeccable critique` call (`skill-map.md`).
3. **Manual checklist:** the items below, answered from the capture, not from the code.

Mark each item PASS, FAIL or N/A with a reason. Every FAIL is fixed or waived in writing (tier model: `audit-checklist.md` §C). Put the table in the report.

## 1. Automatic (must be clean)

- [ ] No `ImGui::Button`; buttons go through `Util::Button`. (ERROR)
- [ ] No `TODO` or `FIXME`. (ERROR)
- [ ] No character above U+FFFF in UI strings. (ERROR)
- [ ] `BeginTable`/`EndTable` and `PushID`/`PopID` pair. (ERROR)
- [ ] Every user-visible literal goes through `T()`. (WARN)
- [ ] No colour literals outside `ThemeManager` and the icon and font files; no `ImGui::TextColored` with a literal. (WARN)
- [ ] No all-caps labels, middle-dot meta strings, arrows glued to labels. (WARN)
- [ ] No fixed pixel sizes that should scale. (WARN)
- [ ] No `ImGui::PushFont` outside the font helpers. (WARN)

Warnings on legacy code in a touched file count against the change only when the change adds them.

## 2. Scored review (from `critique-rubric.md`)

- [ ] Heuristics scored out of the applicable maximum; **no heuristic below 2**, total in the "good" band or better for new UI.
- [ ] Cognitive load: at most 1 failed checklist item for a new view.
- [ ] Pre-build critique axes (`design-principles.md` §14) all at 3 or above; the weakest axis named.
- [ ] No open P0 or P1 findings.

## 3. Cohesion: does it belong in CS?

- [ ] **Same family.** Spacing, rounding, border weight and button shape match the nearest existing page; it reads as part of the same product with the name covered.
- [ ] **Reuse.** Shared `Util::` widgets used; any new pattern became a shared widget, not inline code (`reuse-catalog.md`).
- [ ] **One accent**, marking selection and the active state only.
- [ ] **Same vocabulary.** Labels and tooltips reuse the menu's terms.
- [ ] **Same icon style.** Tabler for UI.
- [ ] **Same motion.** Rates and durations come from `DESIGN.md` §6; nothing animates at idle.
- [ ] **Three themes.** Reads correctly in Default, Light and HighContrast (and DwemerBronze or DragonBlood for warm and display-font checks), or the report says which could not be checked.

## 4. Hierarchy and layout

- [ ] **One primary thing**; the squint test names primary, secondary and groups in order.
- [ ] **Grouped by proximity**; no boxes compensating for weak spacing; no card in a card.
- [ ] **Most used first**; advanced settings behind a disclosure.
- [ ] **Actions beside their content.**
- [ ] **First screen works without scrolling** at the default size.
- [ ] **Rhythm**: tight inside groups, generous between, one gap per role.

## 5. Type and copy

- [ ] **Roles by meaning**, sentence case, no eyebrow.
- [ ] **Labels short; tooltips say what it does to the image.**
- [ ] **No invented numbers or claims.** Sample data labelled.
- [ ] **Text fits** at the default and the largest font size and with 30 to 40% longer strings; wraps or truncates with a tooltip.
- [ ] **Units and ranges** shown on sliders and inputs; one number format per column.

## 6. Colour and contrast

- [ ] Body text at least 4.5:1, large text 3:1, in all three themes.
- [ ] State is never colour alone; every state colour comes with text or an icon.
- [ ] Disabled controls look disabled and say why where it is not obvious.

## 7. States and feedback

- [ ] Hover, focus, active, disabled, loading, error, success each look distinct; CS states handled where they apply (unsaved, restart needed, overridden, locked, not loaded).
- [ ] Empty state: what is empty, why it matters, the next action.
- [ ] Keyboard and gamepad reach every control with visible focus.
- [ ] Changes give feedback through the existing notification or banner path.
- [ ] Destructive actions are undoable, or confirm by naming the action and object.
- [ ] Inputs do not change size with state.

## 8. Placement and scale

- [ ] Clear of the HUD, crosshair and dialogue; never blocks input it does not need.
- [ ] Checked at 1080p and 1440p and the user's UI scale if different.
- [ ] Stays on screen when resized or moved to an edge.

## 9. Performance and polish

- [ ] No per-frame string or vector allocation in the draw path unless existing code already does it.
- [ ] No redundant rebuilds of static content every frame.
- [ ] `./BuildDevFast.bat` clean for touched files; `en.json` regenerated; formatting run.

## Scoring

- **Every section PASS:** ship.
- **Any FAIL in sections 1, 4 or 7, or an open P0 or P1:** fix before reporting.
- **FAIL only in 2, 3, 5, 6, 8 or 9:** fix, or waive with a reason and the evidence.
