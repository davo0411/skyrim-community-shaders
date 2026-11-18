#!/usr/bin/env python3
"""Turn a Community Shaders theme into settings for the imgui MCP mockup canvas.

The canvas is a standalone ImGui app. Without this, mockups render in a generic
theme and tell you nothing about how the menu will look. This reads a theme JSON
(package/SKSE/Plugins/CommunityShaders/Themes) and prints a JSON object holding
the arguments for the imgui MCP calls that reproduce it:

  set_style        -> mcp__imgui__imgui_set_style {theme, colors}
  style_vars       -> one mcp__imgui__imgui_set_style_var per entry
  font             -> mcp__imgui__imgui_load_font, then imgui_set_font
  viewport         -> mcp__imgui__imgui_set_viewport_size
  background       -> mcp__imgui__imgui_set_background

Colour mapping mirrors ThemeManager::SetupImGuiStyle: the full palette first, then
Palette.Background -> WindowBg, Text, WindowBorder -> Border, Separator, ResizeGrip,
and Palette.FrameBorder -> FrameBg (the name is misleading; it fills frames).

Usage:
  python tools/cs-theme-to-mockup.py [Default|Light|HighContrast|DwemerBronze|DragonBlood|path.json]
                                     [--height 1080]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THEMES = ROOT / "package/SKSE/Plugins/CommunityShaders/Themes"
FONTS = ROOT / "package/Interface/CommunityShaders/Fonts"

# Legacy positional palette (ImGui 1.90, 55 entries) -> ImGui 1.92 colour names.
LEGACY_NAMES = [
    "Text", "TextDisabled", "WindowBg", "ChildBg", "PopupBg", "Border", "BorderShadow",
    "FrameBg", "FrameBgHovered", "FrameBgActive", "TitleBg", "TitleBgActive", "TitleBgCollapsed",
    "MenuBarBg", "ScrollbarBg", "ScrollbarGrab", "ScrollbarGrabHovered", "ScrollbarGrabActive",
    "CheckMark", "SliderGrab", "SliderGrabActive", "Button", "ButtonHovered", "ButtonActive",
    "Header", "HeaderHovered", "HeaderActive", "Separator", "SeparatorHovered", "SeparatorActive",
    "ResizeGrip", "ResizeGripHovered", "ResizeGripActive",
    "Tab", "TabHovered", "TabSelected", "TabDimmed", "TabDimmedSelected",
    "DockingPreview", "DockingEmptyBg", "PlotLines", "PlotLinesHovered", "PlotHistogram",
    "PlotHistogramHovered", "TableHeaderBg", "TableBorderStrong", "TableBorderLight",
    "TableRowBg", "TableRowBgAlt", "TextSelectedBg", "DragDropTarget", "NavCursor",
    "NavWindowingHighlight", "NavWindowingDimBg", "ModalWindowDimBg",
]

FLOAT_VARS = [
    "WindowRounding", "ChildRounding", "PopupRounding", "FrameRounding", "ScrollbarRounding",
    "GrabRounding", "TabRounding", "WindowBorderSize", "ChildBorderSize", "PopupBorderSize",
    "FrameBorderSize", "IndentSpacing", "ScrollbarSize", "GrabMinSize",
]
VEC2_VARS = ["WindowPadding", "FramePadding", "ItemSpacing", "ItemInnerSpacing", "CellPadding"]


def load_theme(arg: str) -> tuple[str, dict]:
    path = Path(arg)
    if not path.suffix:
        path = THEMES / f"{arg}.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    return path.stem, data["Theme"]


def colours(theme: dict) -> dict[str, list[float]]:
    out: dict[str, list[float]] = {}
    if isinstance(theme.get("Colors"), dict):
        out.update({k: v for k, v in theme["Colors"].items() if isinstance(v, list)})
    elif isinstance(theme.get("FullPalette"), list):
        for name, value in zip(LEGACY_NAMES, theme["FullPalette"]):
            out[name] = value
    pal = theme.get("Palette", {})
    for key, name in [("Background", "WindowBg"), ("Text", "Text"), ("WindowBorder", "Border"),
                      ("Separator", "Separator"), ("ResizeGrip", "ResizeGrip"), ("FrameBorder", "FrameBg")]:
        if key in pal:
            out[name] = pal[key]
    if "ResizeGrip" in pal:
        grip = list(pal["ResizeGrip"])
        grip[3] = 0.67  # ThemeManager's RESIZE_GRIP_HOVER_ALPHA is applied to hovered and active
        out["ResizeGripHovered"] = grip
        out["ResizeGripActive"] = grip
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("theme", nargs="?", default="Default")
    ap.add_argument("--height", type=int, default=1080, help="viewport height; width is 16:9")
    args = ap.parse_args()

    try:
        name, theme = load_theme(args.theme)
    except (OSError, KeyError, json.JSONDecodeError) as exc:
        print(f"cannot read theme '{args.theme}': {exc}", file=sys.stderr)
        return 2

    style = theme.get("Style", {})
    # Theme sizes target the 1080p baseline; scale them for other viewport heights.
    scale = args.height / 1080.0
    def num(x: float) -> float | int:
        # The imgui MCP rejects whole numbers sent as "4.0" (arrives as a string); send 4 instead.
        x = round(x, 3)
        return int(x) if float(x).is_integer() else x

    style_vars = [{"var_name": v, "value": num(style[v] * scale)} for v in FLOAT_VARS if v in style]
    style_vars += [{"var_name": v, "value": [num(c * scale) for c in style[v]]} for v in VEC2_VARS if v in style]

    body = theme.get("FontRoles", [{}])[0]
    font_file = body.get("File", theme.get("FontName", "Jost/Jost-Regular.ttf"))
    font_path = FONTS / font_file
    base = 21.0 * scale  # CS base font size: 21 px at 1080p (ThemeManager DEFAULT_FONT_RATIO)

    result = {
        "theme": name,
        "viewport": {"width": round(args.height * 16 / 9), "height": args.height},
        "background": [0.12, 0.13, 0.15],
        "font": {
            "id": f"cs-{name.lower()}-body",
            "path": str(font_path),
            "size_pixels": round(base),
            "exists": font_path.exists(),
            "note": "Body role. Other roles: " + ", ".join(
                f"{r.get('Family')} {r.get('Style')} x{r.get('SizeScale')}" for r in theme.get("FontRoles", [])[1:]),
        },
        "set_style": {"theme": "dark", "colors": colours(theme)},
        "style_vars": style_vars,
        "status_palette": theme.get("StatusPalette", {}),
        "accent": (colours(theme).get("Header") or colours(theme).get("CheckMark")),
        "tip": "Mock up on a busy background, not an empty one: the menu always draws over a live scene.",
    }
    json.dump(result, sys.stdout, indent=2)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
