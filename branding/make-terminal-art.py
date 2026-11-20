#!/usr/bin/env python3
"""Turn the Mt. Platitudes ASCII art into the terminal welcome screens.

Reads:
  branding/mountain.txt      the art: plain ASCII characters
  branding/mountain.colors   the same grid, one color letter per character
  files/settings/console-colors   the console's 16 colors
Writes:
  files/core/share/welcome.vt    the machine's own screens (palette colors)
  files/core/share/welcome.ansi  other terminals (24-bit color)
  files/core/share/welcome       no color at all

Run from the repository root:  python3 branding/make-terminal-art.py
"""
import re

# Color letter -> console palette slot (see files/settings/console-colors).
SLOTS = {
    "r": 3,   # rock: amber
    "s": 15,  # snow
    "f": 1,   # summit flag
    "p": 7,   # flag pole
    "y": 11,  # stars
    "g": 7,   # ground
}

# Welcome text, below the mountain.
TEXT = [
    "",
    "  \x1b[1mMTplatitudes\x1b[22m (Mt. Platitudes), terminal edition",
    "  The OS lives in /core, settings in /settings, your files in /users.",
    "  Try: wisdom, optimize, activity.  To leave: elevate poweroff",
    "",
]
PLAIN = [re.sub(r"\x1b\[[0-9;]*m", "", t) for t in TEXT]

palette = {}
for line in open("files/settings/console-colors"):
    m = re.match(r"\s*(\d+)\s+([0-9A-Fa-f]{6})", line)
    if m:
        palette[int(m.group(1))] = tuple(int(m.group(2)[i:i + 2], 16) for i in (0, 2, 4))

art = open("branding/mountain.txt").read().rstrip("\n").split("\n")
colors = open("branding/mountain.colors").read().rstrip("\n").split("\n")
assert len(art) == len(colors), "mountain.txt and mountain.colors need the same number of lines"


def vt(slot):
    return f"\x1b[0;{30 + slot if slot < 8 else 90 + slot - 8}m"


def rgb(slot):
    r, g, b = palette[slot]
    return f"\x1b[0;38;2;{r};{g};{b}m"


def render(color, text):
    out = []
    for i, line in enumerate(art):
        row, current = "  ", None
        for j, ch in enumerate(line):
            if ch == " ":
                row += ch
                continue
            letter = colors[i][j] if j < len(colors[i]) else " "
            assert letter in SLOTS, f"line {i + 1}, column {j + 1}: no color for {ch!r}"
            if color and SLOTS[letter] != current:
                current = SLOTS[letter]
                row += color(current)
            row += ch
        if color:
            row += "\x1b[0m"
        out.append(row if color else row.rstrip())
    out.extend(text)
    return "\n".join(out) + "\n"


open("files/core/share/welcome.vt", "w").write(render(vt, TEXT))
open("files/core/share/welcome.ansi", "w").write(render(rgb, TEXT))
open("files/core/share/welcome", "w").write(render(None, PLAIN))
print("wrote welcome.vt, welcome.ansi and welcome:", max(len(l) for l in art), "x", len(art))
