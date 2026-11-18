# Design system

The visual system of the Community Shaders in-game UI (Dear ImGui). It is written down from the code so designers, agents and design tools work from the same facts. When this file and the code disagree, the code wins; fix this file.

Companion: [PRODUCT.md](PRODUCT.md) (who it is for, mode, voice). Sources: `src/Menu/ThemeManager.h` (constants), `src/Menu.h` (`ThemeSettings`), `src/Utils/UI.h`, `src/Utils/UISurface.h`, `src/Utils/UIAnim.h`, `package/SKSE/Plugins/CommunityShaders/Themes/*.json`.

## 1. Theme model

Everything resolves from the active theme at runtime, so design in **roles**, never literal values. A screen is not finished until it reads correctly in **Default (dark)**, **Light** and **HighContrast**; DwemerBronze and DragonBlood are the warm checks.

| Theme | Character | Frame rounding | Font roles |
|---|---|---|---|
| Default | Cool dark neutral, soft rounding | 6 | Jost |
| Light | Default turned light: same layout and type on a bright palette, blue accent | 6 | Jost |
| HighContrast | Black and white, outlined controls (`FrameBorderSize 1`), larger heavier text | 3 | IBM Plex Sans + Inter |
| DwemerBronze | Warm bronze, square-ish | 2 | IBM Plex Mono, Roboto Slab, Roboto Condensed |
| DragonBlood | Dark red, red accent, display faces for Title and Subheading | 3 | Crimson Pro, Sanguis, Sovngarde |

A theme supplies: `Palette` (Background, Text, WindowBorder, FrameBorder, Separator, ResizeGrip), `StatusPalette` (Disable, Error, Warning, RestartNeeded, CurrentHotkey, SuccessColor, InfoColor), `FeatureHeading` (ColorDefault, ColorHovered, MinimizedFactor), a full `Colors` map, `Style` (rounding, padding, spacing), per-role `Surfaces`, scrollbar opacity, five font roles, cursor images.

## 2. Colour roles

Use the role accessors, never `ImVec4(...)` or `IM_COL32(...)` literals in a renderer.

| Role | Accessor | Use |
|---|---|---|
| Text | `Util::Colors::GetDefault()` | Body text and labels |
| Secondary text | `Util::Colors::GetSecondary()` | Muted but legible text for live controls, meta, captions |
| Disabled | `Util::Colors::GetDisabled()` | Inactive controls and their labels |
| Accent | `Util::Colors::GetAccent()` (the theme's `ImGuiCol_Header`, alpha 1) | Selected and active rows, segments, tabs. The one accent. |
| Success, Warning, Error, Info | `Util::Colors::GetSuccess/GetWarning/GetError/GetInfo()` | State meaning only, never decoration |
| Unsaved | `Util::Colors::GetUnsaved()` | Save button whose target has unsaved changes |
| Themed text lines | `Util::Text::Warning/Error/Success/Info/Disabled/Secondary` (and `Wrapped…`) | One-call coloured text |

How a theme reaches ImGui (`ThemeManager::SetupImGuiStyle`): the full palette (`Colors`, or a legacy 55-entry `FullPalette`) loads first, then the simple `Palette` overrides `WindowBg` (Background), `Text`, `Border` (WindowBorder), `Separator`, `ResizeGrip`, and `FrameBg` (**`Palette.FrameBorder` fills frames**, despite its name). The accent is not in `Palette`: it is the full palette's `Header` (with `CheckMark` and `SliderGrab`), so a theme gives it an accent colour there. Scrollbar alpha comes from `ScrollbarOpacity`.

Discipline (Operate mode): the chrome is neutral and quiet. The accent marks selection and the active state. State colours carry meaning and always come with text or an icon, never colour alone. Never one-off tints.

Shared alpha steps (`ThemeManager::Constants`): hover fill 0.06 of text colour · selected fill 0.20, outline 0.70 · list highlight fill 0.14 · surface border 0.08 · destructive icon 0.80 · unsaved 0.75 · version text 0.60 · feature description 0.70 · palette rows of features that are not loaded 0.50.

## 3. Typography

Five font roles on `Menu::FontRole`, each with its own family, style and `SizeScale` per theme:

| Role | Use | Default theme scale |
|---|---|---|
| Body | Default text, setting labels | 1.0 |
| Title | The large "Community Shaders" header | 1.3 (Light weight) |
| Heading | Page and section headers, option card titles (scale 1.25) | 1.0 |
| Subheading | Feature names, separators | 1.0 |
| Subtext | Descriptions, secondary content | 0.9 |

Rules:
- Pick the role by meaning, not by size. Use `MenuFonts::FontRoleGuard` / `Util::SeparatorTextWithFont`.
- Base size comes from the screen (21 px at 1080p, 28 at 1440p, 42 at 4K) or the user setting (16 to 108 px). Never hardcode a size. Feature titles scale 1.5 (`DEFAULT_FEATURE_TITLE_SCALE`).
- Hierarchy comes from role and colour step, not from extra weights. Sentence case. No tracked capitals.
- Shipped families: Jost, Inter, IBM Plex (Sans, Serif, Mono), Roboto, Roboto Slab, Rubik, Crimson Pro, OpenDyslexic. A new face needs a theme reason.
- Numbers in tables and sliders share one format and decimal count per column.

## 4. Spacing and layout

Sizes are authored at the **1080p baseline** and multiplied by `Util::GetUIScale()` (`font size / 21`). Fixed pixel values without that scale are bugs.

Theme `Style` defaults: window padding 10 · frame padding 8×5 · item spacing 8×6 · item inner spacing 6×4 · cell padding 8×4 · indent 14 · separator text padding 16×3.

Shared constants: icon to label gap 8 · button spacing 8 · Home card padding 12, gap 8 · toast padding 16×12, stack gap 8, bottom margin 32 · overlay window offset 10 · selected-row accent bar 3.

Rules:
- Group by proximity first: tight inside a group, looser between groups, more space above a header than below.
- Rows in one list share one height and one gap. Same role, same padding.
- Every view has one thing the eye lands on first. Squint test: primary, secondary and groups still read when blurred.
- The first screen of a page must work without scrolling at the default size.
- Two columns only when the content width allows (`HOME_TWO_COLUMN_MIN_CHARS`).

## 5. Shape and surfaces

Flat. A surface is the window colour lifted 4% toward white, a hairline border (8% white), and a single soft shadow only when it floats. Roles (`Util::Surface::Role`): `HomeCard`, `Toast`, `OverlayCard`, `Palette`, `ListHighlight`, `OptionCard`, `SectionCard`. Each has per-theme settings; draw through `Util::Surface::Draw`, not by hand.

Rounding comes from the theme (`FrameRounding`, `ChildRounding`, `PopupRounding`, `WindowRounding`). Nested shapes use outer radius minus padding. Pills only where a pill is the pattern (`PILL_ROUNDING`).

No gradients across hues, no coloured glows, no texture or ornament on controls.

## 6. Motion

`Util::Anim` owns motion: exponential approach (`1 - exp(-rate * dt)`), scaled by the user's animation speed (0.5 to 2.0) and cut by `Anim::ReducedMotion()`.

| Use | Value |
|---|---|
| Hover and press blend | rate 18 /s |
| Selection blend | rate 14 /s |
| Selection glide to a new row | rate 26 /s |
| Slider grab (idle / held) | rate 22 / 50 /s |
| Toggle knob | rate 14 /s |
| Popup and palette fade | 0.14 s (scale in from 0.97) |
| Page and card enter | 0.18 s, card stagger 0.04 s, from alpha 0.4 |
| Menu fade | 0.15 s |
| Toast fade in / out | 0.18 s / 0.55 s |
| Attention pulse | 2.6 s period, 0.25 amplitude, attention only |

Rules: motion states a change (hover, selection, appear, feedback), nothing else. Nothing loops while idle except a deliberate attention pulse. Never animate layout size per frame in a way that jumps neighbours.

## 7. Icons

Tabler (`Icons::TI(ICON_TI_<NAME>)`) for UI. Font Awesome (`Icons::FA`) and Game-Icons (`Icons::GI`) for game-world icons only (places, creatures, weather). One size per context, text-role colour, fixed icon box so labels align (`ICON_LABEL_GAP` 8). Glyphs above U+FFFF cannot load. No emoji.

## 8. Components (reuse first)

The shared widgets own these patterns. Use them before drawing anything custom. Full list with signatures: `.claude/skills/cs-imgui-design/references/reuse-catalog.md`.

Buttons and toggles: `Util::Button`, `Util::IconButton`, `SuccessButton/WarningButton/ErrorButton`, `FeatureToggle`, eased `Checkbox`/`Slider`. Selection: `SegmentedControl`, `BeginPillTabBar`, `GlideSelectable`, `OptionCard`. Grouping: `SectionCard`, `SettingColumns`, `LockedSection`. Feedback: `StatusBanner`, `Badge`, `Notifications::Show`, `ConfirmationPopup`, `DeferredModal`, `CenteredPopupModal`. Help: `HelpMarker`, `SettingHint`, `Explainer`, `HoverTip`, `CheckboxTip`, `SliderFloatTip`.

## 9. States

Every interactive thing defines: default, hover, focus (keyboard and gamepad), active, disabled (with a reason where it is not obvious), loading, error, success. CS adds: **unsaved** (`GetUnsaved`), **restart needed** (`StatusPalette.RestartNeeded`), **overridden or inherited** (`PushTintedFrameStyle`), **locked** (`LockStatusBadgeButton`), **feature not loaded** (rows at 0.5 alpha), **empty** (one line saying what is empty and the next action), **hotkey capture** (`CurrentHotkey`).

## 10. Placement and input

- The menu draws over a live scene: keep overlays and toasts off the crosshair, dialogue and the game's own HUD, and never block input they do not need.
- Toasts stack from the bottom (margin 32). Overlay cards sit at a 10 px offset. Overlapping windows raise their background alpha to 0.85.
- Every control is reachable by keyboard and gamepad navigation. Tooltips appear after the theme's hover delay (default 0.5 s).
- Text fits at the user's largest supported font size and in longer translations (allow 30 to 40% expansion). Wrap or truncate with a tooltip; never clip silently.

## 11. Do and don't

Do: reuse shared widgets · take colour from roles · test three themes · scale by `GetUIScale()` · route every string through `T()` · design empty, error and loading states · keep chrome quiet.

Don't: literal colours or pixel sizes · `ImGui::Button` · emoji or Unicode glyphs as icons · all-caps labels · middle-dot meta strings · card in card · modals where inline works · idle animation · invented numbers or claims.
