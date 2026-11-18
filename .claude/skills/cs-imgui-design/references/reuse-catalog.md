# Reuse catalog

Check here before drawing anything. The shared widgets own these patterns (Impeccable polish: replace one-off implementations with the shared component the system already owns). Signatures are from `src/Utils/UI.h` unless noted; read the header for the exact current form.

If nothing here fits, say so in the report and add the new widget to `Util::` (with a Doxygen comment, theme-role colours, and `Util::Anim` motion) instead of building it inline in a page.

## Buttons and toggles

| Need | Use |
|---|---|
| Text button | `Util::Button(label, size)` (eased hover, hand cursor). Never `ImGui::Button`. |
| Icon button | `Util::IconButton(id, Icons::TI(ICON_TI_X), tooltip, active, color)` |
| Positive, caution, destructive | `SuccessButton`, `WarningButton`, `ErrorButton`, `ErrorTextButton`, `ErrorButtonWithFlash` |
| Feature on/off | `Util::FeatureToggle(label, &enabled, size)` |
| Checkbox with tooltip | `Util::CheckboxTip(label, &v, tooltip)` |
| Slider with tooltip | `Util::SliderFloatTip(label, &v, min, max, fmt, tooltip)` |

`ImGui::Checkbox`, `SliderFloat` and `Selectable` are hooked (`src/Utils/UIHooks.cpp`), so the eased check, glided grab and gliding highlight apply to the normal calls. Do not call the `Anim::Eased…` or `Anim::GlideSelectable` wrappers yourself.
| Fit slider and label in a column | `Util::FitNextItemWithLabel(label)` |

## Choosing between options

| Need | Use |
|---|---|
| 2 to 5 mutually exclusive modes | `Util::SegmentedControl(id, segments, selected, options)` |
| Pill tabs | `Util::BeginPillTabBar` / `BeginPillTabItem` / `EndPillTabItem` / `EndPillTabBar` |
| Pick one of a few options that open below | `Util::OptionCard(id, content, selected, size)`; size a row with `OptionCardHeight` |
| Selectable list row | `ImGui::Selectable` (glides automatically), `Util::TableRowSelectable`, `Util::Surface::DrawListHighlight` |

## Grouping and layout

| Need | Use |
|---|---|
| Group related settings on a page | `Util::SectionCard(id, ICON_TI_X, title)` (RAII; surface role `SectionCard`) |
| Side-by-side settings that stack when narrow | `Util::SettingColumns(id, columns, minColumnFontSizes)` then `.Next()` |
| Section title in a font role | `Util::SeparatorTextWithFont(text, Menu::FontRole::Heading)` |
| Lock a block while something holds it | `Util::LockedSection` |
| Show defaults while a mode is active | `Util::DefaultsWhileActive`, `AreSettingsShowingDefaults()` |
| Toolbar divider | `Util::ToolbarDivider` |

## Help, hints, feedback

| Need | Use |
|---|---|
| One line explaining the setting just drawn | `Util::SettingHint(text)` |
| Hover help on any item | `Util::AddTooltip`, `HoverTip`, `HoverTooltipWrapper` |
| `(?)` marker | `Util::HelpMarker(text)` |
| Longer explanatory block | `Util::Explainer(label, text)` |
| Inline status (info, warning, error) | `Util::StatusBanner(icon, message, color)` |
| Small label chip | `Util::Badge(label, tint, options)` |
| HDR/SDR capability | `Util::DrawHdrSdrCapabilityPill` |
| Transient message | `Util::Notifications::Show(message, color, duration)` or `ShowOnce` (toast cards, `src/Utils/Notifications.h`) |
| Lock state | `Util::LockStatusBadgeButton`, `DrawLockStatusBadge` |
| Value origin (inherited or overridden) | `Util::PushTintedFrameStyle(color)` / `PopTintedFrameStyle()` |

## Dialogs

| Need | Use |
|---|---|
| Yes/no on a destructive or heavy action | `Util::ConfirmationPopup` (`Request()`, `Draw()`) |
| Modal opened from a draw path | `Util::DeferredModal` |
| Centred popup | `Util::CenteredPopupModal` |

Prefer inline or undoable actions over a modal. Name the action on the confirm button, never "OK" or "Yes".

## Colour and text

| Need | Use |
|---|---|
| Role colours | `Util::Colors::GetDefault/GetSecondary/GetDisabled/GetAccent/GetSuccess/GetWarning/GetError/GetInfo/GetUnsaved` |
| Themed coloured text | `Util::Text::Warning/Error/Success/Info/Disabled/Secondary` and `Wrapped…` |
| Quiet icon glyph colour | `Util::QuietIconColor(hover, selected)`; tint `Util::GetIconTint()` |
| Disabled block | `Util::DisableGuard` |

## Surfaces and motion

| Need | Use |
|---|---|
| A flat card, toast, palette or highlight | `Util::Surface::Draw(drawList, min, max, Role, options)`; roles in `src/Utils/UISurface.h` |
| Accent frame (reveal, attention) | `Util::Surface::DrawAccentFrame` |
| Fade a block in | `Anim::BeginFadeIn` / `EndFadeIn`, `Anim::PageFade`, `Anim::EnterGroup` (staggered cards) |
| Smooth a value or toggle | `Anim::Smooth`, `Anim::Approach`, `Anim::Toggle` |
| Attention pulse | `Anim::Pulse` (attention only) |
| Respect user motion settings | `Anim::ReducedMotion()`, `Anim::Speed()` (every animation must honour them) |
| Popups and combos | `ImGui::BeginPopup` and combos fade automatically through the same hooks |

## Fonts and icons

`MenuFonts::FontRoleGuard`, `MenuFonts::ImFontGuard` (`src/Menu/Fonts.h`); `Icons::TI(ICON_TI_X)` for UI, `Icons::FA` and `Icons::GI` for game-world icons (`src/Menu/Icons/helpers/IconFonts.h`); `Util::GetUIScale()` for any size.

## Page-level pieces

Header, status bar, Home cards, go-to palette, setting reveal, undo toast and feature undo live in `src/Menu/` (`MenuHeaderRenderer`, `StatusBarRenderer`, `HomePageRenderer`, `GoToPalette`, `SettingReveal`, `FeatureUndo`). A new page registers with these rather than inventing parallel navigation, search or undo.
