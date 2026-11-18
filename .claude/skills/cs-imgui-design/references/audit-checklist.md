# Audit checklist

Used in playbook step 3 (existing UI) and again in step 8. One line per finding: the sign, where it lives (`file:line` or the widget), and the fix. Sections A to C come from unslop's signs of slop, Impeccable's craft floor and Hallmark's gates, translated to ImGui. Section D is the CS convention list. Credits: `ATTRIBUTION.md`; rules in full: `design-principles.md`.

Classify the result: **heavy** (many signs across colour, type, icons, copy and spacing: full passes), **light** (mostly designed, a few signs: fix only those, keep the identity), **none** (say so; suggest at most three small refinements with exact values and stop).

## A. Signs of slop, in ImGui terms

**Surfaces and colour**
- Hue-to-hue gradients on cards, buttons or the active row → one neutral surface; accent only where it means something.
- Gradient text or a gradient wordmark → solid text colour.
- Coloured glows and accent-coloured shadow halos → remove; depth from the surface step and hairline.
- Blurred blobs or colour washes behind content → remove.
- Neon or high-contrast coloured borders → hairline at about 8%.
- Many near-identical tints of one hue for text → three text roles: default, secondary, disabled.
- Every element highlighted so nothing stands out → one accent, once or twice per view.
- Glass cards with an inner highlight and a coloured shadow → the plain surface role.
- A zero-offset coloured halo as "depth" → offset soft shadow, or none.

**Type**
- Uppercase labels with tracking (`NAVIGATION`, `PAID THIS MONTH`) → sentence case.
- An eyebrow label above every heading → delete it.
- Too many sizes and weights on one view → the five font roles only.
- Mixed number formats in one column → one format, one decimal count.
- Italic or bold used for emphasis → colour step or role.
- A display font in labels, buttons or data → Body role.

**Icons**
- Emoji or Unicode glyphs as icons or label decoration → `Icons::TI` or nothing.
- Icons in many sizes and colours → one size, text-role colour.
- Icons nudged off the column → one icon box width in every row (`ICON_LABEL_GAP`).
- A sparkle for anything "smart" → remove.

**Spacing**
- Literal pixel values (`ImVec2(13, 7)`) → scale steps via `Util::GetUIScale()` and `ThemeManager::Constants`.
- Rows in one list with different heights or gaps → one height, one gap.
- A group sitting closer to its neighbour than to its own items → tighter inside, looser between.
- Uneven padding in sibling cards → one padding (the surface role's).
- Large gap above a section, cramped below → balanced rhythm.

**Copy**
- Greeting headlines, exclamation marks, hype words (supercharge, seamless, unlock) → what the thing does.
- Changes without a reference (`+12%!!`) → `+12% vs last preset`.
- Wordy placeholders (`Search anything...`) → `Search`.
- Trailing periods on labels and buttons → none.
- Fake-looking data (`[X]`, `123,456`) → real or labelled sample.
- Invented numbers, claims or benchmarks → remove or source them.

**Components**
- Glowing pills for counts, plans or status → quiet text or a `Util::Badge`.
- `PRO`, `New`, `Beta` badges everywhere → one quiet badge, only if the stage is real.
- Everything a full pill → radius by role; pills only where the pattern is a pill.
- Charts with rainbow series, glowing bars, uppercase labels → one or two series, muted grid, one highlight.
- Sparklines or mini bars added to fill every stat → the number, its label and its change.
- Equal cards side by side each with the same content → vary widths; one main block, compact supporting stats.
- Card inside a card, or a frame around a frame → one level of grouping.
- A modal for a task that needs neither interruption nor protected focus → inline or popover.

**Motion**
- Pulsing or looping animation on things that are not live → static.
- Bounce, overshoot, scale-on-hover on cards → colour or surface step.
- Everything flying in from different directions → one short fade, small stagger.
- Animating layout size per frame so neighbours jump → fade or position only.
- Ignoring `Anim::ReducedMotion()` or the user's speed setting → honour both.

**Template look** (what a cleaned-up screen falls back to when nobody decided)
- Same flat grey list of toggles as every other page → give the view a job, a primary thing, a moment.
- A heading that would fit any feature → name what this feature does.
- A visual that is an empty box → real content, a labelled sample, or nothing.

## B. Craft floor [Impeccable], checked on the capture

- Contrast: body text at least 4.5:1, large text 3:1, in Default, Light and HighContrast.
- Depth: shadows carry an offset and soft blur.
- Spacing: tight groups, generous separation, more space above a header than below.
- Type: roles distinct at a glance; text fits at the largest font size.
- Motion: one authored moment, not scattered effects.
- States: hover, disabled, loading, error, empty, plus real content, working controls, keyboard and gamepad focus.
- Copy: controls name their action; errors name the problem and the recovery.
- Coverage: every requirement present and findable within seconds.
- Refuse: cards of icon plus heading plus text as page structure; a hero-metric template; an eyebrow; decorative blur; coloured side-borders above 1 px on cards or alerts; hard offset shadows; sparklines standing in for content; monospace as a costume; Unicode glyphs as icons.

## C. Gate tiers [Hallmark], how to settle a finding

- **Floor**: fix. Invented facts, literal colours, `ImGui::Button`, missing `T()`, missing states, illegible contrast, eyebrow or tracked capitals, colour-only state, HUD-blocking placement.
- **Reflex**: fix, or waive in the report with a real reason and the evidence. Examples: a modal where inline would work, an accent used twice, a pill, a non-shared widget.
- **Finish**: answer it in the report; never block on it.

A waiver that cannot state its evidence is usually a defect.

## D. CS conventions (non-negotiable)

- Spacing, padding and colours from `ThemeManager::Constants` and the role accessors.
- `Util::Button`, not `ImGui::Button`. Shared widgets before custom drawing (`reuse-catalog.md`).
- `BeginTable`/`EndTable` and `PushID`/`PopID` paired. Stable IDs for repeated widgets.
- RAII guards for style and font changes (`Util::DisableGuard`, `MenuFonts::FontRoleGuard`, `MenuFonts::ImFontGuard`).
- All user strings through `T()` under the right key prefix; `en.json` regenerated.
- Doxygen comments on new public methods.
- Renderers get state through parameters and callbacks; no new public members on `Menu`.
- No `TODO` or `FIXME` in shipped code.
- Do not modify backend, renderer or font-atlas setup unless asked.
