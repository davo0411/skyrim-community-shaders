# Design principles

The rules the skill applies. Each is distilled from an installed design skill and translated to ImGui and `DESIGN.md`. Source tags: **[IMP]** Impeccable, **[UNS]** unslop, **[HM]** Hallmark. Credits and licences: `ATTRIBUTION.md`. When a source rule is web-only (CSS, `rem`, `:focus-visible`), the ImGui equivalent is given, or the rule is dropped.

The tiers (from Hallmark's gates): **Floor** never ships. **Reflex** is a default a model falls into; it may be waived on the record with a reason. **Finish** is craft detail: answer it, never block on it.

## 1. Stance: this is an Operate surface

- [IMP] The visitor is in a task. Scanability, consistency and familiar controls outrank expression. The test: can a user fluent in this kind of tool trust the screen immediately, or do they pause at every subtly off component?
- [IMP] The failure mode is strangeness without purpose: over-decorated buttons, mismatched controls, motion that does nothing, display fonts where labels belong, invented affordances for standard tasks.
- [IMP] The "world" (here: Skyrim and the active theme) lends four things only: type, palette, density and one signature move. It never supplies layout, navigation or controls. Never a costume of the world drawn onto controls.
- [IMP] Restrained colour is the floor: the accent marks primary action, current selection and state, nothing else. A second neutral layer for sidebars, toolbars and panels.
- [IMP] Modals are usually laziness. Exhaust inline and progressive alternatives first.
- [HM] One idea per screen. Fullness comes from scale and structure, never from more words.

## 2. Hierarchy and layout (Floor: items 2.1 to 2.3)

- 2.1 **One primary thing per view.** Squint test [IMP]: blurred, you can still name the primary element, the secondary element and the major groups, in order.
- 2.2 **Group by proximity before containers** [IMP, UNS]. Space inside a group is smaller than space between groups; more space above a header than below. If you need a box to show a grouping, the spacing is wrong.
- 2.3 **Rows in one list share one height and one gap** [UNS]. Same role, same padding.
- 2.4 **Rhythm** [IMP]: deliberate contrast between tight and generous intervals. One spacing value repeated everywhere gives everything equal weight.
- 2.5 **Density follows use** [IMP]: dense for settings people scan often, roomy for one-time setup. Operate surfaces may be dense; they may not be noisy.
- 2.6 **Name the spatial thesis before editing** [IMP]: the primary task path, what belongs together, what leads and what supports, the density, and what happens at extremes.
- 2.7 **Structure follows content** [IMP]: repeated cards or columns only when the items are truly equivalent, never as a default.
- 2.8 **Cognitive load** [IMP critique]: at any decision point, at most 4 visible options is comfortable, 5 to 7 is a push, 8 or more needs grouping or progressive disclosure. Group long lists, put the most used first, hide advanced settings behind a disclosure.
- 2.9 **Scale, not literals** [UNS]: spacing steps on a 4-unit grid at the 1080p baseline (4, 8, 12, 16, 20, 24, 32, 40, 48), multiplied by `Util::GetUIScale()`. Compact controls 4 to 12 inside, related rows 2 to 8 apart, groups 12 to 24, sections 32 to 64.
- 2.10 **Alignment**: icons in one column, labels start on one line, numbers align in one column. Nothing nudged by a stray offset.

## 3. Type (Floor: 3.1)

- 3.1 **Role by meaning** [IMP, DESIGN.md]: Body, Title, Heading, Subheading, Subtext. Never pick a size by hand.
- 3.2 **Fewest roles that make the hierarchy unmistakable** [IMP]. Combine role, space and colour step; do not ask size alone to do all the work.
- 3.3 **Sentence case; no tracked capitals** [UNS, HM Floor]. No eyebrow above a heading [IMP, HM Floor]: delete the label and let the heading carry it.
- 3.4 **Tight scale ratio** [IMP]: 1.125 to 1.2 between steps is typical for product UI. Exaggerated contrast is noise.
- 3.5 **Prose measure 45 to 75 characters** [IMP]. Tooltips wrap at 35 font sizes (`TOOLTIP_WRAP_FONT_SIZES`). Dense data may run wider.
- 3.6 **One number format per column** with consistent decimals and units [UNS].
- 3.7 **Stress test** [IMP]: long names, large font size, 30 to 40% longer translations. Wrap or truncate with a tooltip; never clip silently.
- 3.8 **No italic or bold for emphasis** where colour step or role would do [UNS].

## 4. Colour (Floor: 4.1, 4.2)

- 4.1 **Roles only** [DESIGN.md]. No `ImVec4`/`IM_COL32` literals in a renderer.
- 4.2 **Contrast** [IMP craft floor]: body and placeholder text at least 4.5:1, large text 3:1. Check in the lightest and darkest themes and in HighContrast. Text on an accent fill reaches 4.5:1 or uses dark text.
- 4.3 **Three text roles** [UNS]: default, secondary, disabled. Not ten tints.
- 4.4 **One accent, used rarely** [UNS, IMP]: selection, primary action, the one number that matters. Several hues only inside a chart that must tell series apart, and even then one leads.
- 4.5 **State colours carry meaning and always come with text or an icon** [IMP, HM]. Never colour alone.
- 4.6 **Quiet borders** [UNS]: hairlines at 6 to 10% of the text colour (`SURFACE_BORDER_ALPHA` 0.08).
- 4.7 **Depth from surface steps** [UNS, IMP]: a one-step lift plus hairline; a soft offset shadow only for floating things. A zero-offset coloured halo is decoration.
- 4.8 **No hue-to-hue gradients, gradient text, coloured glows, neon borders** [UNS, HM Floor]. A gradient is allowed only as a tone shift inside one neutral.
- 4.9 **Neutral chrome** [UNS, IMP]: people look at it for hours; content and the game image carry the colour.

## 5. Icons (Finish)

- [UNS] One set, one size per context, one stroke, text-role colour, fixed icon box so labels align. Drop icons that add nothing. Unicode glyphs and emoji are not icons [IMP, HM Floor]. Icons that act have a tooltip; navigation icons have labels.

## 6. Copy (Floor: 6.1; the rest Reflex)

- 6.1 **Say only true things** [HM Floor 46a, UNS]. No invented numbers, claims, benchmarks or states. Sample data looks real, sums, and is labelled as sample.
- 6.2 **Labels name the action or the thing; tooltips say what it does to the image** [IMP clarify]. Say each idea once; if the heading explains the state, the intro adds new information or goes.
- 6.3 **Buttons: specific verb and object** [IMP]. Destructive actions name the object and consequence; the confirm button repeats the action, never "OK", "Yes" or "Submit". Prefer undo to confirmation when recovery is safe.
- 6.4 **Errors answer three things** [IMP]: what failed, why (when known), how to recover. No raw codes as the primary message. No jokes on blocked work or data loss.
- 6.5 **No exclamation marks, hype words, greetings, trailing periods on labels** [UNS].
- 6.6 **A change says what it compares to** ("+12% vs last preset") [UNS].
- 6.7 **Complete translatable sentences** [IMP]: no concatenated fragments; keep placeholders structured so translators can reorder them. All strings through `T()`.
- 6.8 **Consistent terminology** [IMP]: same noun and verb for the same concept across the product. Keep a glossary when it drifts.

## 7. States and feedback (Floor: 7.1, 7.2)

- 7.1 **Every interactive element has** default, hover, focus (keyboard and gamepad), active, disabled, loading, error, success [IMP, HM]. Half the states is unfinished.
- 7.2 **Disabled** [HM]: looks disabled, is disabled, and says why where the reason is not obvious.
- 7.3 **Loading** [IMP]: names the real operation; determinate progress when known; never invented progress; skeleton where the shape is known, spinner only inside a button.
- 7.4 **Empty states teach** [IMP, HM]: one line on what is empty, one on why it matters, one action. Distinguish first use, no results, filtered, permission, failure. Never a bare "No results".
- 7.5 **Undo over confirm** [HM]: reversible actions run at once and offer undo; irreversible ones confirm, naming the thing.
- 7.6 **Visible feedback for every change** [IMP heuristics]: save, apply, failed setting, restart needed. Use the existing notification and banner paths.
- 7.7 **CS states** [DESIGN.md]: unsaved, restart needed, overridden or inherited, locked, feature not loaded, hotkey capture.
- 7.8 **Inputs never change size with state**: state moves colour, border colour and fill, not width, padding or height. Reserve space for a right-edge clear or error glyph so nothing reflows [HM].

## 8. Motion (Reflex: 8.1; Floor: 8.2)

- 8.1 **Motion states a change** [IMP, UNS]: hover, selection, appear, feedback. 150 to 250 ms for most transitions; users are in flow. No orchestrated load sequences; at most one short fade and rise with a small stagger.
- 8.2 **Honour `Util::Anim::ReducedMotion()` and `Speed()`** always.
- 8.3 **Nothing loops while idle** except a deliberate attention pulse [UNS]. No bounce, no overshoot, no parallax.
- 8.4 **Use the CS motion table** in `DESIGN.md` §6; add rates there, not inline.

## 9. Hardening: the extremes [IMP harden]

Test with: very long and very short text, many items (hundreds), none, large font size and UI scale, longer translations, missing or failed data, slow operations, and concurrent changes. In ImGui add: very small window, window moved to an edge, 1080p and 1440p, CJK and Cyrillic glyph coverage of the active font role.

## 10. Placement and input (Floor: 10.1)

- 10.1 **Never block the HUD or input you do not need** [DESIGN.md]. Keep overlays and toasts off the crosshair and dialogue.
- 10.2 **Keyboard and gamepad reach everything** the mouse does, in a logical order; focus is visible.
- 10.3 **First screen works without scrolling** at the default size [HM: the hero is the first screen].

## 11. Honesty in data [HM data-viz]

One value is a stat, not a one-bar chart. A series over time is a line. Parts of a whole are a stacked bar, never a pie. Bars start at zero. One accent series, the rest neutral. Text never wears the data colour. Fewer, truer figures beat a wall of gauges.

## 12. Cohesion [IMP polish]

Classify every drift before fixing it: **missing token** (the system needs a reusable value), **one-off implementation** (a shared widget should replace it), **conceptual mismatch** (flow or hierarchy differs from comparable pages), **local defect**. Fix at the narrowest correct level. Triage order: broken or blocked tasks and misleading state, then missing states, then flow and hierarchy drift, then visual and motion inconsistencies, then code cleanup. Do not perfect one corner while leaving the rest below the same bar.

## 13. Direction (for new features, systems and heavy redesigns)

Written before any mockup. Format = unslop's five lines + Hallmark's Picks block:

```
CS Picks
- Surface    · which page, window or widget; which row of DESIGN.md's surface table
- Job        · the one thing the user comes to do, and where its action sits
- Character  · two or three concrete words (not clean, modern, sleek, premium)
- Moment     · the one thing on this view that carries the craft (a chart, a preview, a state)
- Signature  · one small detail only this view has, used once
- Reuse      · shared widgets used, and any new one added to Util::
- Material   · real: facts and values the view shows · absent: anything we do not have
- Gates      · lint result and checklist result (filled at the end)
```

Mirror test [HM]: would a neighbouring page in the same product get this exact structure? Then change one decision and say which. For an existing page, polish mode: keep the page's identity and fix what the audit found.

## 14. Pre-build critique [HM, adapted]

Score the mockup 1 to 5 before writing C++. Anything under 3 is revised first.

| Axis | 3 | 5 |
|---|---|---|
| Hierarchy | needs a second look | survives the squint test |
| Specificity | fits any settings page | could only be this feature |
| Restraint | one decoration too many | lost something you liked |
| Cohesion | similar to neighbours | indistinguishable from the best existing page |
| Execution | a few loose details | would pass the lint and checklist first time |

Name the weakest axis and the fix taken. Never invent a weakness to fill a field, and never fabricate a pass.
