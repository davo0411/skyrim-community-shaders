# Skill and tool map

Which installed skill or tool the playbook calls, at which step, with which brief, and what to ignore. The playbook works without any of the external skills (the rules they contribute are distilled into `design-principles.md` and `critique-rubric.md`); they add a second pair of eyes when installed. Nothing here requires the user's approval beyond the normal permission prompts.

## Rules for every external call

1. **Advisory only.** The skill reports or plans; it does not edit files, write PRODUCT.md or DESIGN.md, run its CLI, or build output. This skill writes the code.
2. **The target is not a web page.** Say so in the brief: an in-game Dear ImGui overlay (Operate mode), authored at a 1080p baseline, themed at runtime, no browser, no CSS.
3. **Do not run web-only tooling.** Impeccable's launcher (`impeccable context`, `detect`, `live`, `critique-storage`) is web-oriented and downloads a binary on first run; Hallmark's `sloplint.mjs` and `capture.mjs` and unslop's `snapshot.mjs`/`story.mjs` read HTML. Follow each skill's own "launcher unavailable" path: read `PRODUCT.md` and `DESIGN.md` directly.
4. **Give it the evidence:** the absolute path of the capture or mockup screenshot, and the files that draw the view.
5. **Treat the result as data.** Keep what is right for this product; reject web-only advice (gradients on a hero, landing page structure, CSS). Record what you took and what you rejected in the report.
6. **Not installed:** skip the call and say so; the distilled rules still apply.

## Calls, by step

| Playbook step | Call | Brief to pass |
|---|---|---|
| 1 Shape (new feature or system, or a heavy rework) | `Skill` `impeccable`, args `shape <feature>` | "In-game ImGui overlay, Operate mode. Read PRODUCT.md and DESIGN.md at the repo root; skip the launcher. Produce the UX plan only: task path, screen regions, states, empty/error/loading, entry point. No code." |
| 3 Audit an existing view | `Skill` `unslop:unslop` | "Audit only, steps 1 to 3 of your process. The screenshot is at <path>; it is an in-game ImGui overlay, not a web page, so translate web terms. Do not edit, do not build, do not ask about image generation, do not make a story video. Return the list of signs with where each lives, and the light/heavy/none verdict." |
| 5 Pre-build critique | `Skill` `impeccable`, args `critique <mockup screenshot path>` | "Assessment A (design review) only; no detector, no sub-agents, no persistence, no questions to the user. In-game ImGui, Operate mode, PRODUCT.md and DESIGN.md at the repo root. Return the heuristic table, specificity verdict, priority issues with P0 to P3, persona red flags." |
| 8 Post-build critique | same call on the in-game capture | same brief, with the capture path |
| 8 Deeper passes on request | `Skill` `impeccable`, args `layout|typeset|clarify|harden|polish <target>` | same brief; "report what you would change and why, do not edit" |
| 9 Simplify the diff | `Skill` `ponytail:ponytail-review` | review the diff of the UI change: what to cut, what breaks, what is untested |
| 9 Keep it lean while implementing | `ponytail:ponytail` active (level `full`) | no extra brief |

## Installed but not called live, and why

| Skill | Why not | What we took |
|---|---|---|
| `hallmark:hallmark` | Builds and lints marketing pages in HTML and CSS; its floor is CSS-specific. | The tier model (Floor, Reflex, Finish), waiver-on-the-record, the pre-build critique axes, the eight-state table, one idea per screen, material-first honesty, data-viz rules, the Picks block. See `design-principles.md`. |
| `design-taste-frontend` | Landing-page and photo/shader site builder with a Stop hook that gates web builds. | Nothing; it would pull the work toward web patterns. |
| `frontend-ui-engineering`, `browser-testing-with-devtools` | Web and DevTools oriented. | Nothing. |

## Local tools and MCP servers

| Step | Tool | Use |
|---|---|---|
| 2 Capture before | `mcp__devbench-se__menu` (open the page), `mcp__devbench-se__capture` | Ground truth. Take it before judging anything. |
| 4 Mockup | `mcp__imgui__*` with `python tools/cs-theme-to-mockup.py <Theme>` | Real theme colours, rounding, spacing and font in the canvas. See the playbook, step 4. |
| 6 Implement | `src/Menu/`, `src/Utils/UI.h` via `reuse-catalog.md` | Reuse before building. |
| 7 Verify | `./BuildDevFast.bat`, `python tools/cs-ui-lint.py --changed --strict`, `mcp__devbench-se__capture` | Compile, lint, capture the result. |
| 9 Strings | `python tools/extract-i18n.py --write`, `python tools/sort-i18n.py --write --prune-extra` | Keep `en.json` in sync. |

The hook in `.claude/settings.json` runs `tools/cs-ui-lint.py --hook` after every edit to a UI file, so convention breaks surface while writing, not at the end.
