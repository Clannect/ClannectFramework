# 0014 — M3: paths, scan conversion and the CPU painter, checked against Qt

**Status:** accepted, 2026-09-27. M2's CFW-side items are done (0013), so M3 starts with the foundation that
text and painting share.

## Where the pieces live

- **`cfw::PainterPath` is in cfw-core**, next to `Transform2D`. Glyph outlines (cfw-text), fills, strokes and
  clips (cfw-gfx) are all paths, and neither of those modules may depend on the other.
- **`cfw::Rasterizer` is in cfw-image.** It turns paths into per-pixel coverage. cfw-text needs it for glyph
  masks and cfw-gfx for painting. Both already depend on cfw-image, and it needs no GPU or platform code.

The original plan called this type `Path`, but `cfw::Path` has meant a filesystem path since M1 (cfw-io),
and one namespace cannot hold both. It is therefore `PainterPath`, which is also the name engine code knows from
`QPainterPath`.

`PainterPath` follows `QPainterPath` where engine code will notice: drawing on an empty path starts at (0, 0);
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

## cfw-gfx: the Painter and the CPU backend

`Painter` keeps the state stack: transform, opacity, blend mode, and clips that nest by
intersection and are undone by `restore()` or the destructor. It turns strokes into fills with `Stroker`,
so every backend draws strokes identically. A `PaintBackend` receives only resolved primitives: fill a
path, draw an image, push or pop a clip. `RasterPaintBackend` implements it on a premultiplied `Image`. A
GPU backend needs a GL context, which cfw-platform provides in M4. When it arrives it implements the same
interface and is diffed against the CPU backend.

The CPU backend uses the arithmetic of Qt's raster engine:

- 8-bit premultiplied colour, with x·y/255 rounded exactly;
- opacity as Qt's 256-step `intOpacity`;
- blend modes as Qt's composition functions, mixed with the old pixel by coverage;
- pixels outside a shape are never touched, even for DestinationIn (Qt behaves the same; SVG does not);
- gradients through a 1024-entry premultiplied colour table, with Qt's index-space spreading (repeat is
  modulo 1024 on `int(t·1023 + 0.5)`, so t = 1 is the last colour);
- bilinear image sampling at pixel centres, clamped to the source rectangle.

### Qt behaviour measured, not guessed

Each of these was measured with Qt 6.11 (PySide6, offscreen), and `StrokerTest`/`PainterTest` pin them:

- **Pen defaults:** width 1, square caps, bevel joins, miter limit 2. A width of 0 is a cosmetic pixel.
- **Dashes:** lengths are in pen widths. The pattern restarts at each sub-path and runs on around corners,
  so a dash can contain a join. An odd pattern drops its last entry. Zero-length dashes and zero-length
  sub-paths draw nothing, whatever the cap.
- **Miter:** the point is kept up to `miterLimit × width` past the outer edges, then cut off at that
  distance. The cut-off is not a bevel; SvgMiter is the join that falls back to a bevel.
- **Other rules:** stroke outlines are filled non-zero. Axis-aligned rectangle clips round each edge to
  the nearest pixel, even with anti-aliasing on. Path clips are anti-aliased. Gradients interpolate in
  premultiplied colour (`ColorInterpolation`).

### Deliberate differences

- **Inner corners of strokes.** Qt passes through the corner point, which leaves overlapping loops at
  every vertex of a flattened curve. Area accumulation over-covers where those loops meet an edge.
  CFW ends the two edges where they cross, whenever the crossing lies within both segments, and uses Qt's
  way only for hairpins and very short segments. The covered region is the same.
- **Curve precision.** Curves are flattened to 0.1 device pixels, which is finer than Qt. The results:

  | Shape | True area | CFW | Qt |
  |---|---:|---:|---:|
  | Stroked ellipse, 70×50, width 7 | 1328.8 | 1330.3 (+0.1%) | 1281.3 (−3.6%) |
  | Stroked circle ring | — | within 0.2% | 3.8% short |
  | Filled circle, r = 20 | πr² | 0.44% short | 0.57% short |

- **Always anti-aliased.** There is no aliased mode. Fills default to the non-zero rule, where
  QPainterPath defaults to even-odd; the two agree on the shapes the engine draws.
- **`ImageOptions::tint`.** It multiplies the image in one pass. The interface system used to need a
  Multiply pass and a DestinationIn pass for this.
- **Tiling.** `drawImage` with `tileSize` covers `drawTiledPixmap`.
- **Hostile input.** Invalid coordinates are ignored, as QPainterPath does. Arcs clamp to a full turn.
  Non-finite clips clip everything.

## Checked against Qt: golden images

Seven scenes cover fills, strokes, gradients, transforms (including perspective), blend modes, clips and
images. Each is written once as JSON (`modules/cfw-gfx/testdata/scenes`). `testing/qt-oracle/render_scenes.py`
renders the scenes with QPainter and commits the PNGs. `PainterGoldenTest` renders them through
`Painter` and compares, and on a mismatch it writes the rendering and a difference image.

- **Images and colour arithmetic:** match Qt to within 3 levels.
- **Straight edges:** match to within rounding.
- **Curved edges:** differ by up to ~30 levels on under 1% of pixels, because Qt's curves are less precise.
  Each scene's tolerance says this explicitly.

## Performance budgets

Measured on the same machine:

- **10,000 anti-aliased, semi-transparent rounded rectangles at 1440p:** CFW takes 150 ms; Qt 6.11's
  QPainter takes 770 ms with `drawRoundedRect`. That is 0.2× Qt's time; the budget is ≤ 1.2×. (It was
  290 ms. Solid source-over now blends two channels at a time, with the same rounding.)
- **2,000 cached Latin text runs at 14 px** (DejaVu Sans): 19 ms. Qt draws the same runs' `QGlyphRun`s in
  19.9 ms, measured through PySide6, so Qt's figure includes some Python overhead. That is about 1.0×;
  the budget is ≤ 1.2×.
- **2,000 uncached runs shaped:** 34 ms. Qt's uncached `QTextLayout` takes 32 ms; the budget is ≤ 1.5×.
- **Allocations per repainted frame, steady state:** 0. `cfw-bench` gates this on every build with a
  UI-like frame (panels, outlines, dashes, a gradient, a clipped rotated image, 40 text labels), and also
  checks the fill and cached-text benchmarks.

What made the allocation count zero:

- The rasteriser streams flattened points through the transform instead of building polylines.
- `Rasterizer::sweep` takes its row function as a template, not a `std::function`.
- The stroker's dash buffers are never swapped.

## Fuzzing

The fuzz target `FuzzPaint` runs byte-driven programs of paths, pens, brushes, degenerate and perspective
transforms, clips, blend modes and images. Every pixel must remain valid premultiplied colour. Its first
minutes found four places where a non-finite value reached a float-to-int conversion (undefined
behaviour): a NaN arc sweep, a NaN clip rectangle, NaN stroke directions from overflowing coordinates, and
an infinite image source rectangle. All four are fixed, and the target joins the CI fuzz job.

## Text and icons

- **Text:** `Painter::drawText(TextLayout)` and `drawGlyphs` take cfw-text's layout (decision 0015).
  - Under a plain translation, glyphs are 8-bit masks from `GlyphCache`: unhinted, at quarter-pixel
    positions, on whole-pixel baselines, in 1024² atlas pages. The backend's `fillMasks` blends them with
    the same span compositor as fills, so brushes, opacity, blend modes and clips apply as they do to
    fills.
  - Under any other transform, and for glyphs too large for the atlas, glyphs are filled as outlines.
  - `TextPaintTest` requires masks and outlines to agree within 20/255 per pixel at exact positions. The
    masks themselves are checked against FreeType in `GlyphCacheTest`.
- **Icons:** `SvgImage` replaces QSvgRenderer for the editor's icons.
  - It supports the subset icons use:
    - elements `svg`, `g`, `path`, `rect`, `circle`, `ellipse`, `line`, `polyline` and `polygon`;
    - all path commands, elliptical arcs included;
    - fill and stroke, with colours, `currentColor`, widths, caps, joins, miter limit, fill rule and
      opacities;
    - transforms and simple `style` attributes.

    Other elements are skipped.
  - `SvgImageTest` renders ten icons at 16, 20, 24 and 32 px exactly as `EditorIcons.cpp` does, and
    compares them with QSvgRenderer. Coverage is within 5% of Qt's ink (6% at 16 px, where Qt's coarser
    flattening of curves shows), and colours away from edges are within 3 levels.
  - `FuzzSvg` ran 270,000 documents with no findings.
- **Qt's thin pens, not imitated:** QRasterPaintEngine draws any pen at most 1 px wide after scaling with
  its cosmetic line stroker (`fast_pen`). That stroker covers less than the pen's true area: a 0.8 px
  diagonal line has about 20% less ink than exact coverage gives. CFW strokes every pen geometrically, so
  sub-pixel diagonal strokes are darker than Qt's. The editor's icon pens are wider than 1 px at every
  size it uses. One-pixel axis-aligned lines, the common UI case, come out the same in both.

## Next in M3

- The Qt-oracle A/B of the engine's own interface and diagnostic renders. This needs the engine port.
