# lucide-icons.ttf

The icons on VATs' buttons and menus: a subset of the [Lucide](https://lucide.dev) icon font, merged into
the UI font by `ui/theme.cpp` (`load_fonts`). Licence: ISC, with some icons under MIT (derived from Feather);
both texts are in `Lucide-ISC.txt`.

- Source: `https://cdn.jsdelivr.net/npm/lucide-static@1.48.0/font/lucide.ttf` (npm package `lucide-static`,
  version 1.48.0), licence from the same package's `LICENSE`.
- Kept: the 42 glyphs `ui/icons.h` names (Lucide names and codepoints in its comments), 15,916 bytes.
- Rebuild: `tools/subset-icons.sh [version]` (needs curl and fontTools' `pyftsubset`). To add an icon, add
  its constant to `ui/icons.h` with the `U+` codepoint from the package's `font/codepoints.json`, then run
  the script.

Glyphs: skip-back, rewind, play, pause, fast-forward, skip-forward, repeat, mouse-pointer-2, move, rotate-3d,
scale-3d, box, globe, axis-3d, bone, diamond-plus, maximize, focus, unfold-vertical, iteration-ccw,
flip-vertical-2, flip-horizontal-2, trash-2, file-plus, folder-open, save, undo-2, redo-2, file-output,
upload, import, bookmark-plus, pencil, radio, square, smartphone, circle-dot, map-pin, eye, eye-off, lock,
lock-open.

The graph editor's tangent buttons are drawn with ImGui's draw list instead (`curve_icon_button` in
`ui/icon_button.cpp`): no icon font has those curve shapes.
