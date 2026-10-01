# lucide-icons.ttf

The icons on VATs' buttons and menus: a subset of the [Lucide](https://lucide.dev) icon font, merged into
the UI font by `ui/theme.cpp` (`load_fonts`). Licence: ISC, with some icons under MIT (derived from Feather);
both texts are in `Lucide-ISC.txt`.

- Source: `https://cdn.jsdelivr.net/npm/lucide-static@1.48.0/font/lucide.ttf` (npm package `lucide-static`,
  version 1.48.0), licence from the same package's `LICENSE`.
- Kept: the 125 glyphs `ui/icons.h` names (Lucide names and codepoints in its comments), 49,768 bytes.
- Rebuild: `tools/subset-icons.sh [version]` (needs curl and fontTools' `pyftsubset`). To add an icon, add
  its constant to `ui/icons.h` with the `U+` codepoint from the package's `font/codepoints.json`, then run
  the script.

Glyphs: skip-back, rewind, play, pause, fast-forward, skip-forward, repeat, mouse-pointer-2, move, rotate-3d,
scale-3d, box, globe, axis-3d, bone, diamond-plus, maximize, focus, unfold-vertical, iteration-ccw,
flip-vertical-2, flip-horizontal-2, trash-2, file-plus, folder-open, save, undo-2, redo-2, file-output,
upload, import, bookmark-plus, pencil, radio, square, smartphone, circle-dot, map-pin, eye, eye-off, lock,
lock-open, between-horizontal-start, spline, blend, flip-horizontal, timer, notebook-pen, chevrons-right,
chart-spline, funnel, camera, arrow-left-right, eraser, ellipsis, diamond-minus, chart-gantt, route, shield-check,
circle-alert, wrench, crosshair, list-checks, refresh-cw, ghost, shrink, scissors, copy, file-text, search, check,
move-horizontal, footprints, wind, waves, plus, layers-plus, stamp, rotate-ccw, smile, scan-eye, gauge,
list-ordered, chevron-up, chevron-down, circle-play, folders, anchor, sparkles, frame, lamp, sun, lamp-desk,
sun-dim, sunset, moon, wallpaper, toy-brick, tags, pin, clapperboard, image, store, scale, rabbit, hand-grab,
package, audio-lines, folder-plus, ruler, earth, person-standing, webcam, armchair, locate-fixed, link, pin-off,
hand, atom, between-horizontal-end, trending-down, users, infinity, arrow-down-to-dot, rotate-cw.

The graph editor's tangent buttons are drawn with ImGui's draw list instead (`curve_icon_button` in
`ui/icon_button.cpp`): no icon font has those curve shapes.
