# Emti, the MTplatitudes mascot

<img src="emti.png" width="256" alt="Emti"> <img src="icon.png" width="128" alt="MTplatitudes icon">

**Emti** (from "MT", as in *empty*) is endlessly confident and says absolutely
nothing, so her speech bubble is always empty. Her hair clip is a blinking
cursor, she wears a hoodie with `~$_` on it, and the key on her lanyard is for
`keeper` and `elevate`. She is an original character made for this project.

| File | What it is |
|---|---|
| `emti.svg` / `emti.png` | Full mascot (vector, and a 512 px render) |
| `icon.svg` / `icon.png` | Round icon (vector, and a 256 px render) |
| `emti-pixels.txt` | Hand-drawn 26x26 pixel version: one letter per pixel |
| `make-terminal-art.py` | Turns the pixel version into the welcome screens in `files/core/share/` |
| `console.png` | How she looks on the machine's own screen after signing in |

The console's 16 colors come from `files/settings/console-colors`. After
editing `emti-pixels.txt` or the colors, run
`python3 branding/make-terminal-art.py` from the repository root.

![Emti on the console](console.png)
