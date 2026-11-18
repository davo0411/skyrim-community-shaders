#!/usr/bin/env python3
"""Pretty-UI lint for Community Shaders ImGui code.

Deterministic checks behind the cs-imgui-design "Pretty UI checklist". It only
flags things a regex can see: convention breaks, leftover placeholders, and
common slop tells. Visual judgement stays in the checklist.

Usage:
  python tools/cs-ui-lint.py [paths...]     lint files or folders
  python tools/cs-ui-lint.py --changed      lint UI files changed vs HEAD (plus untracked)
  python tools/cs-ui-lint.py --hook         Claude Code PostToolUse hook: reads JSON on stdin

Exit codes: 0 clean, 1 warnings under --strict, 2 errors (or hook feedback to Claude).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# UI code this lint applies to. Anything outside these trees is ignored.
UI_DIRS = ("src/Menu", "src/Features", "src/Utils", "src/CSEditor")
# Files that define the palette and icon plumbing are allowed literals.
ALLOWED_LITERAL_FILES = ("src/Menu/ThemeManager", "src/Menu/Icons", "src/Menu/Fonts")
SOURCE_EXTS = (".h", ".hpp", ".cpp")

ERROR, WARN = "ERROR", "WARN"

# (severity, code, pattern, message)
LINE_RULES = [
    (ERROR, "button", re.compile(r"\bImGui::Button\s*\("),
     "use Util::Button (eased hover, hand cursor), not ImGui::Button"),
    (ERROR, "placeholder", re.compile(r"\b(TODO|FIXME)\b"),
     "no TODO/FIXME placeholders in shipped code (CLAUDE.md)"),
    (WARN, "literal-colour", re.compile(r"\b(IM_COL32\s*\(|ImVec4\s*\(|ImVec4\s+\w+\s*\()"),
     "colour literal; take it from ThemeManager::Constants or the active theme"),
    (WARN, "untranslated", re.compile(
        r'\bImGui::(Text|TextWrapped|TextDisabled|Button|SmallButton|Checkbox|SliderFloat|SliderInt|'
        r'Selectable|CollapsingHeader|SeparatorText|MenuItem|BeginTabItem|TextLink)\s*\(\s*"(?!##)'),
     "user-visible literal not wrapped in T(...)"),
    (WARN, "tracked-caps", re.compile(r'"[^"\n]*\b[A-Z]{3,} [A-Z]{3,}\b[^"\n]*"'),
     "all-caps label; use sentence case (tracked capitals read as template UI)"),
    (WARN, "middot-meta", re.compile(r'"[^"\n]*\s[·•]\s[^"\n]*"'),
     "middle-dot joined meta string; use one value per line or a small table"),
    (WARN, "arrow-link", re.compile(r'"[^"\n]*(→|»)[^"\n]*"'),
     "arrow glued to label text; drop it or use an icon"),
    (WARN, "pixel-literal", re.compile(r"\bImVec2\s*\(\s*-?\d+(\.\d+)?f?\s*,"),
     "fixed pixel size; scale by font size or ThemeManager::Constants"),
]


def is_ui_file(rel: str) -> bool:
    return rel.endswith(SOURCE_EXTS) and any(rel.startswith(d + "/") for d in UI_DIRS)


def literal_allowed(rel: str) -> bool:
    return any(rel.startswith(p) for p in ALLOWED_LITERAL_FILES)


def strip_line_comment(line: str) -> str:
    """Drop // comments so commented code does not trip the rules (strings are kept)."""
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '"' and (i == 0 or line[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str and line.startswith("//", i):
            return line[:i]
        i += 1
    return line


def lint_file(path: Path) -> list[tuple[str, int, str, str, str]]:
    rel = path.relative_to(ROOT).as_posix()
    text = path.read_text(encoding="utf-8", errors="replace")
    findings: list[tuple[str, int, str, str, str]] = []

    for lineno, raw in enumerate(text.splitlines(), 1):
        code = strip_line_comment(raw)
        if not code.strip():
            continue
        for sev, name, pat, msg in LINE_RULES:
            if name == "literal-colour" and literal_allowed(rel):
                continue
            if pat.search(code):
                findings.append((sev, rel, lineno, name, msg))
        if any(ord(ch) > 0xFFFF for ch in code):
            findings.append((ERROR, rel, lineno, "astral-glyph",
                             "character above U+FFFF will not load in the game's font; use Tabler or plain text"))

    # Whole-file pairing checks.
    pairs = [("BeginTable", "EndTable"), ("PushID", "PopID")]
    for open_tok, close_tok in pairs:
        n_open = len(re.findall(rf"\b(ImGui::)?{open_tok}\s*\(", text))
        n_close = len(re.findall(rf"\b(ImGui::)?{close_tok}\s*\(", text))
        if n_open != n_close:
            findings.append((ERROR, rel, 0, "pairing",
                             f"{open_tok} x{n_open} but {close_tok} x{n_close}; they must pair"))
    return findings


def collect(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        base = (ROOT / p) if not Path(p).is_absolute() else Path(p)
        if base.is_dir():
            out += [f for f in base.rglob("*") if f.suffix in SOURCE_EXTS]
        elif base.is_file():
            out.append(base)
    return [f for f in out if is_ui_file(f.relative_to(ROOT).as_posix())]


def changed_files() -> list[str]:
    tracked = subprocess.run(["git", "diff", "--name-only", "HEAD"], cwd=ROOT,
                             capture_output=True, text=True).stdout.split()
    untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT,
                               capture_output=True, text=True).stdout.split()
    return sorted(set(tracked + untracked))


def report(findings: list[tuple[str, int, str, str, str]]) -> None:
    for sev, rel, line, name, msg in findings:
        where = f"{rel}:{line}" if line else rel
        print(f"{where}: {sev} [{name}] {msg}")


def main(argv: list[str]) -> int:
    strict = "--strict" in argv
    args = [a for a in argv if a != "--strict"]

    if "--hook" in args:
        try:
            payload = json.load(sys.stdin)
        except json.JSONDecodeError:
            return 0
        file_path = (payload.get("tool_input") or {}).get("file_path", "")
        if not file_path:
            return 0
        try:
            rel = Path(file_path).resolve().relative_to(ROOT).as_posix()
        except ValueError:
            return 0
        if not is_ui_file(rel) or not Path(file_path).is_file():
            return 0
        findings = lint_file(Path(file_path))
        if not findings:
            return 0
        # Exit 2 sends stderr back to Claude as feedback, so every finding reaches the agent.
        out = sys.stderr
        for sev, rel, line, name, msg in findings:
            where = f"{rel}:{line}" if line else rel
            print(f"{where}: {sev} [{name}] {msg}", file=out)
        print("Pretty-UI checklist: resolve these findings (ERROR first) before the UI change is done. "
              "See .claude/skills/cs-imgui-design/references/pretty-ui-checklist.md.", file=out)
        return 2

    if "--changed" in args:
        files = [ROOT / f for f in changed_files() if is_ui_file(f)]
    else:
        files = collect([a for a in args if not a.startswith("--")] or list(UI_DIRS))

    findings: list[tuple[str, int, str, str, str]] = []
    for f in files:
        findings += lint_file(f)
    report(findings)

    n_err = sum(1 for f in findings if f[0] == ERROR)
    n_warn = sum(1 for f in findings if f[0] == WARN)
    print(f"{len(files)} file(s), {n_err} error(s), {n_warn} warning(s)")
    if n_err:
        return 2
    if strict and n_warn:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
