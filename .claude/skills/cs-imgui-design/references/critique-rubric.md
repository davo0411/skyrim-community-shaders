# Critique rubric

How to review a screen honestly. Adapted from Impeccable's critique command (Nielsen heuristics, cognitive load, personas, P0 to P3 severity) for the CS in-game UI. Credits: `ATTRIBUTION.md`.

Use it two ways: as the scoring sheet when the skill reviews a mockup or a capture itself, and as the brief for a live `impeccable critique` call (`skill-map.md`). Review from the capture, not from the code. Judge the design before reading any lint output so the lint does not anchor you.

## 1. Specificity verdict (write this first)

Does the screen feel authored for this feature and for Community Shaders, or could an unrelated settings page use it unchanged? Cover coherence, structural sameness with neighbouring pages, category-default choices, and missed chances for product character. One short paragraph.

## 2. Heuristics, scored 0 to 4

A 4 means genuinely excellent. Most real screens score 20 to 32 out of 40. Mark a heuristic `n/a` with a one-line reason only when it truly cannot apply (for example a toast has little to say about flexibility), and renormalise the total to the applicable maximum.

| # | Heuristic | What to check in CS |
|---|---|---|
| 1 | Visibility of system status | Is it clear what is saved, unsaved, loading, compiling, restart-needed, locked, inherited or overridden? Does every action give feedback through the notification or banner path? |
| 2 | Match with the real world | Skyrim and modding vocabulary where it helps; shader jargon explained in a tooltip; labels say what the setting does to the image. |
| 3 | User control and freedom | Undo and redo (`FeatureUndo`), reset to defaults, clear search, Esc closes, a way out of every popup and mode. |
| 4 | Consistency and standards | Same widgets, spacing and wording as the neighbouring pages; shared `Util::` widgets used; same theme roles. |
| 5 | Error prevention | Ranges clamped, destructive actions confirmed by name, values validated, settings that cannot be combined are disabled with a reason. |
| 6 | Recognition over recall | Options visible, labels on icons, tooltips, recent items, the go-to palette finds it. |
| 7 | Flexibility and efficiency | Keyboard and gamepad paths, hotkeys, presets, search, scene overrides for power users, sensible defaults for newcomers. |
| 8 | Aesthetic and minimalist design | Every element earns its place; one primary thing; no decoration doing no job. |
| 9 | Error recovery | Errors say what failed, why and how to recover; failures leave settings usable. |
| 10 | Help and documentation | Hints, `HelpMarker`, `Explainer` and tooltips answer the question the control raises, once. |

Scoring bands (applicable maximum): 90% and above excellent, 70 to 89% good, 50 to 69% acceptable (significant work), below 50% poor.

## 3. Cognitive load checklist (count failures)

- [ ] **Single focus**: the primary task completes without competing elements.
- [ ] **Chunking**: information in groups of about four.
- [ ] **Grouping**: related items grouped by proximity or a shared surface.
- [ ] **Visual hierarchy**: what is most important is obvious.
- [ ] **One thing at a time**: one decision before the next.
- [ ] **Minimal choices**: at most four visible options at a decision point (5 to 7 is a push, 8 or more is overload).
- [ ] **Working memory**: nothing to remember from another page.
- [ ] **Progressive disclosure**: advanced detail appears when needed.

0 to 1 failures: low load. 2 to 3: moderate, address soon. 4 or more: high, fix first.

Common violations: the wall of options, the memory bridge, hidden navigation, jargon, equal visual weight everywhere, inconsistent patterns, multi-task demand, context switching between pages for one decision.

## 4. Personas (pick two or three; write what broke for them)

| Persona | Walk this | Typical red flags |
|---|---|---|
| **Power tester** | Change many settings, compare, undo, save a preset, search | No keyboard path, no undo, long scrolling, repeated clicks for one change |
| **First-time player** | Enable a feature and find out what it does | Jargon, no tooltip, unclear on/off state, scary defaults, no empty-state guidance |
| **Low-vision player** | Same flow at 4K or with a large font size, in HighContrast | Text under 4.5:1, clipped labels at large size, colour-only state, tiny hit targets |
| **Gamepad player** | Same flow with keyboard and gamepad navigation only | Unreachable controls, no visible focus, hover-only affordances, dead ends |
| **Mod author** | Wire a new feature in, read errors, reproduce a bug | Unhelpful error text, hidden state, settings that silently reset |

Write specific failures ("Setting X has no tooltip and sits 9 rows below its group"), not generic persona descriptions.

## 5. Severity

- **P0 Blocking**: prevents the task, loses data, or misleads about state. Fix now.
- **P1 Major**: significant difficulty, or breaks a project rule (literal colours, `ImGui::Button`, untranslated strings, missing state). Fix before merge.
- **P2 Minor**: an annoyance with a workaround. Next pass.
- **P3 Polish**: nice to fix. Keep these few; a long tail buries the important ones.

## 6. Report

```
Method: design review only (detector not applicable: ImGui)   [or: dual: this skill + impeccable critique]
Specificity verdict: …
Score: NN/40 (rating)   table of 10 heuristics, one key issue each
Cognitive load: N failures (list)
What works: 2 to 3 specific things
Priority issues (3 to 5), each: [P?] what · why it matters · fix · which principle (design-principles.md §)
Persona red flags: 2 to 3 personas, specific
Minor observations
```

Rules: be direct and specific; say what is wrong and why it matters to users; give concrete fixes; prioritise ruthlessly; do not soften; do not report a finding you have not seen in the capture. If fewer than three priority issues, say so.
