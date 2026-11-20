# Emti, the MTplatitudes mascot

<img src="emti.png" width="256" alt="Emti"> <img src="icon.png" width="128" alt="MTplatitudes icon">

**Emti** (from "MT", as in *empty*) is endlessly confident and says absolutely
nothing, so her speech bubble is always empty and her eyes are closed in quiet
satisfaction. Her amber hair is the color of an old amber-screen terminal, her
hair clip is a blinking cursor, her hoodie says `~$_`, and the key on her
lanyard is for `keeper` and `elevate`. She is an original character made for
this project.

| File | What it is |
|---|---|
| `emti.svg` / `emti.png` | Full mascot (vector, and a 512 px render) |
| `icon.svg` / `icon.png` | Round icon (vector, and a 256 px render) |
| `emti.txt` | Emti as ASCII art, as she appears on the console |
| `emti.colors` | The same grid with one color letter per character |
| `make-terminal-art.py` | Turns the two into the welcome screens in `files/core/share/` |
| `console.png` | How she looks on the machine's own screen after signing in |

The console's 16 colors come from `files/settings/console-colors`. After
editing `emti.txt`, `emti.colors` or the colors, run
`python3 branding/make-terminal-art.py` from the repository root.

![Emti on the console](console.png)
