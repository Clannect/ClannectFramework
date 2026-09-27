# Golden scenes

Each `*.json` file is one scene: a canvas and a list of painter operations. `PainterGoldenTest` renders every
scene through `cfw::Painter` and the CPU backend, and compares the result with `../golden/<scene>.png`. That PNG
is the same scene rendered by Qt 6.11's QPainter (raster engine, anti-aliasing and smooth pixmap transforms on).
The script `testing/qt-oracle/render_scenes.py` renders it and is run outside the build (decisions 0007, 0014).
Straight edges and all colour arithmetic match Qt to within rounding. Curved edges differ by up to about 32 levels,
because Qt flattens curves more coarsely: its r = 20 circle covers 0.57% less than the true area, CFW's 0.44% less.
Its stroked curves are further off, 3.6% short for the stroked ellipse in 0014. Tolerances allow for that on at
most 1% of the pixels.

Each scene states how closely CFW must match Qt: `tolerance.max` is the largest allowed channel difference, and
`tolerance.over` (default 8) with `tolerance.fraction` bound how many pixels may differ by more than `over`.

## Format

```
{ "size": [w, h], "background": [r, g, b, a], "tolerance": {...}, "ops": [ ... ] }
```

Colours are straight 8-bit RGBA. Operations:

| op | fields |
|---|---|
| `save`, `restore` | |
| `translate` / `scale` / `rotate` | `d: [x, y]` / `s: [x, y]` / `deg` |
| `transform` | `m: [xx, xy, tx, yx, yy, ty]`, applied before the current transform |
| `quad` | `from`, `to`: four points each; the perspective map, applied before the current transform |
| `opacity` | `v` |
| `blend` | `mode`: `source-over`, `source`, `destination-in`, `destination-out`, `multiply`, `screen`, `plus` |
| `clipRect` / `clipPath` | `r: [x, y, w, h]` / `path`, `rule` |
| `fill` / `fillRect` | `path`, `rule` (`nonzero` or `evenodd`) / `r`; and `brush` |
| `stroke` | `path`, `pen` |
| `image` | `image` (a generated test image: `photo`, `alpha`, `checker`), `target`, optional `source`, `smooth`, `tile` (draws the whole image repeated at its natural size), `tint` |

Paths are lists of commands: `["M", x, y]`, `["L", x, y]`, `["Q", cx, cy, x, y]`,
`["C", c1x, c1y, c2x, c2y, x, y]`, `["Z"]`, `["rect", x, y, w, h]`, `["rrect", x, y, w, h, rx, ry]`,
`["ellipse", x, y, w, h]`, `["arc", x, y, w, h, start, sweep]`, `["poly", x1, y1, x2, y2, ...]` (closed).

Brushes: `{"color": [r, g, b, a]}`, `{"linear": [x1, y1, x2, y2], "stops": [[offset, r, g, b, a], ...],
"spread": "pad" | "repeat" | "reflect"}`, `{"radial": [cx, cy, radius], "focal": [fx, fy], "stops": ..., "spread": ...}`.

Pens: `{"width", "brush" | "color", "cap": "flat" | "square" | "round", "join": "bevel" | "miter" | "svgmiter" | "round",
"miter", "dashes", "offset", "cosmetic"}`, with Qt's defaults.
