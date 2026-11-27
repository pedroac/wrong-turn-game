# Wrong Turn

A small neon game made with [raylib](https://www.raylib.com/). A fuse burns up a path that keeps forking.
Pick the branch that continues and avoid the dead ends. Choose wrong and the whole tree burns away.

## Play

- **Intro:** choose a difficulty by picking a side: left is easy, right is hard.
- **Keyboard:** Left/Right (or A/D) to select a branch. Space or Enter to retry. M mutes, F11 is fullscreen.
- **Mouse:** move left or right to select, or click a path.
- **Touch:** swipe left or right to step the selection, or tap a path.

Yellow shows where you will go if you do nothing. Red shows the alternatives.

## Build

Desktop (needs raylib and a C compiler):

```
make        # build
make run    # build and run
```

Web (needs [Emscripten](https://emscripten.org/); uses the raylib web build in `raylib-web/`):

```
make web      # web/index.html (+ .js .wasm .data)
make web-zip  # wrong-turn-web.zip, ready to upload to itch.io as an HTML5 game
```

## AI assistance

This game was built with the help of AI (Claude Code, by Anthropic). The code, sound effects and visuals
are generated in code; there are no sampled audio or image assets.

## Fonts

The game uses three external fonts, loaded from `assets/fonts/` at startup (and bundled into the web build):

| Font | Used for | Folder |
| --- | --- | --- |
| [Tilt Neon](https://fonts.google.com/specimen/Tilt+Neon) | all the text | `Tilt_Neon/` |
| Neoneon | the "Wrong Turn" title | `neoneon/` |
| [Beon](https://github.com/noirblancrouge/Beon) | the score numbers | `beon/` |

Tilt Neon and Beon are under the SIL Open Font License (license files are included in their folders).
Neoneon's license file is not included yet, so check its terms before distributing the game.
If a font file is missing, the game falls back to another font instead of failing.

## Credits

- [raylib](https://www.raylib.com/) (zlib license), in `raylib-web/` for the web build.
- Fonts: see above.
