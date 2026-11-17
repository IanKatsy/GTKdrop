#!/usr/bin/env python3
"""Converts Stardrop's Avalonia colour themes (.xaml) into GTKdrop JSON themes.

    tools/convert_stardrop_themes.py <Stardrop/Themes dir> <output dir>

Brushes a theme leaves out are filled from Stardrop's Defaults.xaml, the same way
Stardrop's ThemeManager backfills them, so every generated theme is complete.
Avalonia writes colours as #AARRGGBB; the JSON uses CSS order (#RRGGBB / #RRGGBBAA).
"""
import json
import re
import sys
from pathlib import Path

COLOR_RE = re.compile(r'<Color\s+x:Key="([^"]+)"\s*>\s*(#[0-9A-Fa-f]+)\s*</Color>')
BRUSH_RE = re.compile(r'<SolidColorBrush\s+x:Key="([^"]+)"\s+Color="([^"]+)"(?:\s+Opacity="([0-9.]+)")?')

# GTKdrop theme key -> Stardrop brush
MAPPING = {
    "window_bg": "ThemeBackgroundBrush",
    "window_fg": "ThemeForegroundBrush",
    "border": "HighlightBrush",
    "accent": "HighlightForegroundBrush",
    "list_header_bg": "DataGridHeaderBrush",
    "row_bg": "DataGridRowBackground",
    "row_alt_bg": "AlternativeDataGridRowBackground",
    "row_fg": "DataGridRowForeground",
    "popover_bg": "ComboBoxItemBackgroundBrush",
    "popover_selected_bg": "ComboBoxItemSelectedBackgroundBrush",
    "success": "UpdateAvailableBrush",
    "warning": "UpdateUnofficialBrush",
    "error": "UpdateBrokenBrush",
}


def parse_argb(text):
    digits = text.lstrip("#")
    if len(digits) in (3, 4):
        digits = "".join(c * 2 for c in digits)
    if len(digits) not in (6, 8):
        return None
    alpha = int(digits[:2], 16) / 255 if len(digits) == 8 else 1.0
    rgb = digits[-6:]
    return tuple(int(rgb[i:i + 2], 16) / 255 for i in (0, 2, 4)) + (alpha,)


def parse(path):
    text = path.read_text(encoding="utf-8-sig")
    colors = {key: parse_argb(value) for key, value in COLOR_RE.findall(text)}
    brushes = {}
    for key, value, opacity in BRUSH_RE.findall(text):
        if value.startswith("{DynamicResource "):
            color = colors.get(value[len("{DynamicResource "):].rstrip("}"))
        else:
            color = parse_argb(value)
        if color:
            r, g, b, a = color
            brushes[key] = (r, g, b, a * (float(opacity) if opacity else 1.0))
    return brushes


def to_css(color):
    r, g, b, a = (round(c * 255) for c in color)
    return f"#{r:02x}{g:02x}{b:02x}" + ("" if a == 255 else f"{a:02x}")


def luminance(color):
    r, g, b, _ = color
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def main(themes_dir, out_dir):
    themes_dir, out_dir = Path(themes_dir), Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    defaults = parse(themes_dir / "Defaults.xaml")

    for path in sorted(themes_dir.rglob("*.xaml")):
        if path.stem == "Defaults":
            continue
        brushes = {**defaults, **parse(path)}
        colors = {key: to_css(brushes[brush]) for key, brush in MAPPING.items() if brush in brushes}
        theme = {"name": path.stem}
        if path.parent.parent.name == "Contributors":
            theme["author"] = path.parent.name
        if "ThemeBackgroundBrush" in brushes:
            theme["dark"] = luminance(brushes["ThemeBackgroundBrush"]) < 0.5
        theme["colors"] = colors

        target = out_dir / (path.stem + ".json")
        target.write_text(json.dumps(theme, indent=2) + "\n", encoding="utf-8")
        print(f"{path.relative_to(themes_dir)} -> {target.name}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
