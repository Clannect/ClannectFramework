# Shaping expectations

HarfBuzz's output for each test font, made by `testing/text-oracle/shape.py` (uharfbuzz 0.56.2, HarfBuzz
13) and zlib-compressed:

| File | Font | Cases |
|---|---|---:|
| `DejaVuSans.json.z` | `DejaVuSans.ttf` | 2,117 |
| `CfwTestLayout.json.z` | `CfwTestLayout.ttf` | 3,079 |
| `CfwTestPlain.ttf.json.z` | `CfwTestPlain.ttf` | 1,617 |
| `CfwTestPlain.otf.json.z` | `CfwTestPlain.otf` | 1,117 |

Each case is a string (fixed corpus, feature settings, forced directions and languages, and random strings
from per-script character pools) with the script and direction HarfBuzz resolved, and the glyphs as
`[glyph, cluster, x_advance, y_advance, x_offset, y_offset]` in font units. `ShaperTest` shapes each case
and requires every value to be equal. To regenerate:

    python3 testing/text-oracle/shape.py modules/cfw-text/testdata/fonts/DejaVuSans.ttf \
        modules/cfw-text/testdata/shaping/DejaVuSans.json.z --random 2000 --seed 1
    python3 testing/text-oracle/shape.py modules/cfw-text/testdata/fonts/CfwTestLayout.ttf \
        modules/cfw-text/testdata/shaping/CfwTestLayout.json.z --layout --random 3000 --seed 2
    python3 testing/text-oracle/shape.py modules/cfw-text/testdata/fonts/CfwTestPlain.ttf \
        modules/cfw-text/testdata/shaping/CfwTestPlain.ttf.json.z --random 1500 --seed 3
    python3 testing/text-oracle/shape.py modules/cfw-text/testdata/fonts/CfwTestPlain.otf \
        modules/cfw-text/testdata/shaping/CfwTestPlain.otf.json.z --random 1000 --seed 4

`ShaperTest <expected.json[.z]> <font>` compares any other font the same way.
