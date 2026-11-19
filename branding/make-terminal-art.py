#!/usr/bin/env python3
"""Turn emti-pixels.txt into the terminal welcome screens.

Writes:
  files/core/share/welcome.vt    for the machine's screens (uses the 16-color
                                 palette from files/settings/console-colors)
  files/core/share/welcome.ansi  for other terminals (24-bit color)

Each text cell shows two pixels stacked, using the half-block character.
Run from the repository root:  python3 branding/make-terminal-art.py
"""
import re

PIXELS = "branding/emti-pixels.txt"
COLORS = "files/settings/console-colors"
# Pixel letter -> palette slot.
KEY = {'.': 0, 'h': 6, 'H': 14, 'S': 15, 'k': 8, 'w': 15, 'p': 4, 'i': 12,
       'b': 9, 'm': 1, 't': 13, 'c': 10, 'y': 11, 'o': 8, 'd': 14}

TEXT = [
    "",
    " ┌──────────────────┐",
    "─┤                  │",
    " └──────────────────┘",
    "",
    "\x1b[1mMTplatitudes\x1b[22m, terminal edition",
    "The OS lives in /core, settings in",
    "/settings, your files in /users.",
    "",
    "Try: wisdom, optimize, activity",
    "To leave: elevate poweroff",
    "",
    "",
]

palette = {}
for line in open(COLORS):
    m = re.match(r"\s*(\d+)\s+([0-9A-Fa-f]{6})", line)
    if m:
        palette[int(m.group(1))] = tuple(int(m.group(2)[i:i + 2], 16) for i in (0, 2, 4))

rows = [l.rstrip("\n") for l in open(PIXELS) if l.strip()]
width = max(len(r) for r in rows)
rows = [r.ljust(width, ".") for r in rows]
if len(rows) % 2:
    rows.append("." * width)


def fg_vt(c):
    return 30 + c if c < 8 else 90 + c - 8


def fg_rgb(c):
    r, g, b = palette[c]
    return f"38;2;{r};{g};{b}"


def bg_vt(c):
    # The Linux console ignores bright-background codes (100-107). Its own
    # way to get one is the "blink" attribute, which the graphical console
    # shows as a bright background instead of blinking.
    return str(40 + c) if c < 8 else f"5;{40 + c - 8}"


def bg_rgb(c):
    r, g, b = palette[c]
    return f"48;2;{r};{g};{b}"


def render(fg, bg):
    """Slot 0 is treated as transparent, so the art sits on any background."""
    out = []
    for y in range(0, len(rows), 2):
        line = "  "
        for x in range(width):
            top, bottom = KEY[rows[y][x]], KEY[rows[y + 1][x]]
            if top == 0 and bottom == 0:
                line += "\x1b[0m "
            elif top == 0:
                line += f"\x1b[0;{fg(bottom)}m▄"
            elif bottom == 0:
                line += f"\x1b[0;{fg(top)}m▀"
            else:
                line += f"\x1b[0;{fg(top)};{bg(bottom)}m▀"
        line += "\x1b[0m  "
        text = TEXT[y // 2] if y // 2 < len(TEXT) else ""
        out.append(line + text)
    return "\n".join(out) + "\n\x1b[0m\n"


open("files/core/share/welcome.vt", "w").write(render(lambda c: str(fg_vt(c)), bg_vt))
open("files/core/share/welcome.ansi", "w").write(render(fg_rgb, bg_rgb))
print("wrote welcome.vt and welcome.ansi:", width, "x", len(rows) // 2, "cells")
