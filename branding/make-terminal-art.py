#!/usr/bin/env python3
"""Turn Emti's ASCII art into the terminal welcome screens.

Reads:
  branding/emti.txt      the art: plain ASCII characters
  branding/emti.colors   the same grid, one color letter per character
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
    "h": 3,   # hair: amber
    "f": 15,  # face outline
    "e": 15,  # closed eyes
    "p": 9,   # blush
    "m": 1,   # "w" smile
    "c": 10,  # cursor hair clip
    "k": 11,  # key
    "s": 7,   # hoodie
    "B": 15,  # the (empty) speech bubble
}

# Welcome text, placed to the right of her head, under the bubble.
TEXT_COLUMN = 30
TEXT = {
    6: "\x1b[1mMTplatitudes\x1b[22m, terminal edition",
    7: "The OS lives in /core, settings in /settings,",
    8: "your files in /users.",
    10: "Try: wisdom, optimize, activity",
    11: "To leave: elevate poweroff",
}
PLAIN = {k: re.sub(r"\x1b\[[0-9;]*m", "", v) for k, v in TEXT.items()}

palette = {}
for line in open("files/settings/console-colors"):
    m = re.match(r"\s*(\d+)\s+([0-9A-Fa-f]{6})", line)
    if m:
        palette[int(m.group(1))] = tuple(int(m.group(2)[i:i + 2], 16) for i in (0, 2, 4))

art = open("branding/emti.txt").read().rstrip("\n").split("\n")
colors = open("branding/emti.colors").read().rstrip("\n").split("\n")
assert len(art) == len(colors), "emti.txt and emti.colors need the same number of lines"


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
        if i in text:
            row += " " * max(1, TEXT_COLUMN - len(line)) + text[i]
        out.append(row.rstrip() if not color else row)
    return "\n".join(out) + "\n\n"


open("files/core/share/welcome.vt", "w").write(render(vt, TEXT))
open("files/core/share/welcome.ansi", "w").write(render(rgb, TEXT))
open("files/core/share/welcome", "w").write(render(None, PLAIN))
print("wrote welcome.vt, welcome.ansi and welcome:", max(len(l) for l in art), "x", len(art))
