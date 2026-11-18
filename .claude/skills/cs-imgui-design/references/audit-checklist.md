# CS ImGui audit checklist

Use this during step 3 of `cs-imgui-design`. Each finding gets one line: sign, where (`file:line` or the widget), and the fix.

## A. Slop signs, translated to ImGui

These come from the unslop audit, adapted to a game overlay. Chrome should be quiet; the richness belongs to one or two content moments.

- **Glow everywhere.** Coloured outer glows or heavy shadows on every panel, button or card. Fix: flat fill, one-pixel border in a neutral tint, shadow only on floating popups.
- **Accent on every element.** More than one accent colour in a single view, or the accent used for plain text. Fix: one accent, reserved for the state that matters (enabled, active, warning).
- **Emoji or decorative glyphs in labels.** Fix: Tabler icon (`Icons::TI(ICON_TI_<NAME>)`) or plain text.
- **Shouty copy.** ALL CAPS labels, exclamation marks, "New!" badges, marketing verbs. Fix: short sentence case labels in the game's own voice.
- **Uppercase tracked labels.** Fix: normal case, or a section header with `ImGui::SeparatorText` or `TextDisabled`.
- **Uneven spacing.** Literal pixel values, different gaps between similar rows, a card padded differently from its neighbour. Fix: `ThemeManager::Constants` spacing for every gap, and `ItemSpacing` / `FramePadding` set once, not per widget.
- **Hardcoded colours.** `IM_COL32(...)` or `ImVec4(...)` literals in a renderer. Fix: a named constant in `ThemeManager::Constants`, or a colour taken from the active theme.
- **Every section boxed.** A card or framed child around every row. Fix: boxes only where grouping needs one; otherwise separators and indentation.
- **Stacked controls with no hierarchy.** Many same-weight widgets in one column with no grouping or title. Fix: group with `CollapsingHeader` or a tab bar, and put the most used setting first.
- **Pulsing or looping animation on idle UI.** Fix: animate only on state change, and keep it under the menu's existing easing (`Util::Button` already eases hover).
- **Text that does not fit.** Clipped or overlapping labels at the user's font scale. Fix: `TextWrapped`, or widen the column; check at a large font size, not only the default.

## D. Hallmark tells, translated to ImGui

From the Hallmark plugin's Floor and Reflex lists. Web-only rules (CSS tokens, `overflow-x`, focus rings in CSS) do not apply; the intent does.

- **Eyebrow above every heading.** A small label over each section title. Fix: drop it, or use a plain `SeparatorText`.
- **Tracked capitals on labels, captions or table heads.** Fix: body font, sentence case, no letter-spacing (ImGui has no tracking; just avoid uppercase strings).
- **Middle-dot or arrow-glued meta strings** (`Version 2 · Enabled · 4 presets`). Fix: one value per line or a small table.
- **Decorative status dots** next to every row. Fix: colour only where state matters, plus a text label.
- **Invented numbers.** Fake counts, "benchmarks", sample stats in a settings page. Fix: show only values the mod actually has.
- **Card inside a card**, or a frame drawn around a frame. Fix: one level of grouping.
- **Too many ideas on one screen.** Hallmark caps a screen at about 80 words of text. Fix: cut helper prose; move long explanations behind a tooltip or the `_tooltip` key.
- **Action not where the user expects it.** The primary action of the page sits far from the content it acts on. Fix: place it next to the thing it changes.
- **Hero text that does not fit the first view** (the header and first section run off the top of the menu at the default size). Fix: check at the default window size, the first screen is the one that matters.

## B. CS project conventions (non-negotiable)

- Spacing, padding and colours from `ThemeManager::Constants` (`src/Menu/ThemeManager.h`).
- `Util::Button`, not `ImGui::Button`.
- `BeginTable` / `EndTable` always paired.
- RAII guards for style and font changes (`Util::DisableGuard`, `MenuFonts::FontRoleGuard`, `MenuFonts::ImFontGuard`).
- Stable IDs for repeated widgets; `##suffix` for empty labels.
- All user strings through `T()` with a key in the right namespace. `en.json` regenerated with `tools/extract-i18n.py --write`.
- Doxygen comments on any new public method.
- Renderers receive state as parameters or callbacks. No new public members on `Menu`.
- No TODO or FIXME placeholders in shipped code.
- Do not modify backend, renderer or font-atlas setup code unless asked.

## C. Verify-after checklist

- [ ] Before and after captures taken at the same resolution and UI scale.
- [ ] Section A list re-checked against the after capture; nothing left unaddressed.
- [ ] Every string on the page goes through `T()`; `en.json` regenerated.
- [ ] No new literal colours or pixel values in the renderer.
- [ ] Hover, focus, disabled and active states all read clearly.
- [ ] Text fits at the largest font scale the menu allows.
- [ ] `BuildDevFast.bat` clean for touched files.
