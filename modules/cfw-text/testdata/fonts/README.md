# Test fonts

- `DejaVuSans.ttf` is DejaVu Sans 2.37, unmodified. It is TrueType, with GSUB/GPOS (Latin ligatures, Arabic,
  Hebrew, marks) and a `kern` table.
- The `CfwTest*` fonts are made from DejaVu Sans by `testing/text-oracle/make_test_fonts.py` (fontTools), and
  cover what DejaVu does not:

  | Font | Covers |
  |---|---|
  | `CfwTestCff.otf` | Name-keyed CFF: outlines as cubics called through local and global subroutines, and glyphs hand-written to use every Type 2 operator (hints and masks, all curve forms, all flex forms, arithmetic and storage) |
  | `CfwTestCid.otf` | The same, CID-keyed, with two font DICTs and FDSelect format 3 |
  | `CfwTestComposite.ttf` | TrueType composites: offsets, uniform, x/y and 2×2 scales, point matching, nesting (and `SCALED_COMPONENT_OFFSET`, which is ignored as FreeType ignores it) |
  | `CfwTestCollection.ttc` | A collection of two faces |

- `expected.json` holds, for each font, what the references report: names, metrics, a hash of the whole character
  map, and hashes of every glyph's outline in blocks of 64. fontTools is the reference, and FreeType is used where
  fontTools cannot interpret a glyph (Type 2 arithmetic, point-matched components). `FontFaceTest` compares
  against it.

DejaVu's licence (`LICENSE-DejaVu.txt`) allows modified copies, provided they do not use the names "Bitstream" or
"Vera"; the derived fonts are named "CfwTest".
