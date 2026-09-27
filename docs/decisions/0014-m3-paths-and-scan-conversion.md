# 0014 — M3 begins: paths in cfw-core, scan conversion in cfw-image, Qt as an outside oracle

**Status:** accepted, 2026-09-27. M2's CFW-side items are done (0013), so M3 starts with the foundation that
text and painting share.

## Where the pieces live

- **`cfw::Path` is in cfw-core**, next to `Transform2D`. Glyph outlines (cfw-text), fills, strokes and
  clips (cfw-gfx) are all paths, and neither of those modules may depend on the other.
- **`cfw::Rasterizer` is in cfw-image.** It turns paths into per-pixel coverage. cfw-text needs it for glyph
  masks and cfw-gfx for painting. Both already depend on cfw-image, and it needs no GPU or platform code.

`Path` follows `QPainterPath` where engine code will notice: drawing on an empty path starts at (0, 0);
after `close()`, drawing continues from the sub-path's start; `arcTo` angles are in degrees, with positive
angles counter-clockwise on screen; `addEllipse` goes clockwise from 3 o'clock. Unlike `QPainterPath`,
fills say their `FillRule` at the point of use instead of storing it in the path.

## How scan conversion works

This is the signed-area cell technique of FreeType's "gray" rasteriser, from which Qt's raster engine
also derives:

- 24.8 fixed point;
- exact area coverage per pixel;
- non-zero and even-odd rules.

Segments are clipped geometrically before scan conversion:

- parts above or below the target are dropped;
- parts right of it are dropped;
- parts left of it become vertical edges at x = 0, so winding stays correct.

A path with coordinates of 1e6 or with NaN therefore costs only its visible part. Curves are flattened
uniformly with a proven chord-error bound, capped at 1024 segments per curve. The default tolerance is
0.1 px.

## Checked against Qt, from outside the repository

Qt 6.11 (PySide6, offscreen) rendered the same shapes with anti-aliasing, and the coverage grids were
compared pixel for pixel (96×96):

| Shape                       | Max difference | Mean difference | Pixels > 8 apart |
|-----------------------------|---------------:|----------------:|-----------------:|
| slanted triangle            | 3              | 0.022           | 0                |
| star, non-zero              | 1              | 0.009           | 0                |
| star, even-odd              | 1              | 0.011           | 0                |
| rounded rectangle           | 10             | 0.029           | 4                |
| ellipse                     | 31             | 0.167           | 74               |
| cubic + quadratic curves    | 31             | 0.225           | 94               |

Polygons agree within rounding. The differences on curves come from Qt flattening more coarsely: its
circle of radius 20 covers 0.57% less than πr², and ours covers 0.44% less. Curves are therefore not
held to Qt's pixels. They are held to geometry: `RasterizerTest` checks areas against exact formulas,
along with exact results for aligned and half-pixel edges, both fill rules, clipping on every side, and
perspective transforms. As in decision 0007, Qt is used only as an outside reference and is not a
dependency.

## Next in M3

cfw-gfx: a `Painter` with a backend seam and a CPU backend onto premultiplied `Image`s, covering:

- fills, transforms and save/restore;
- a stroker (Qt defaults: square caps, bevel joins, dashes);
- gradients, blend modes and opacity;
- nested rectangle and path clips;
- `drawImage` with affine and perspective transforms.

Then cfw-text, which parses TrueType/OpenType itself, rasterises glyphs with this `Rasterizer`, shapes
text and builds a glyph atlas. Decision 0012 rules out FreeType and HarfBuzz.
