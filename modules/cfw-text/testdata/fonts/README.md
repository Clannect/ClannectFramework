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

- The shaping test fonts are made by `testing/text-oracle/make_layout_font.py`:

  | Font | Covers |
  |---|---|
  | `CfwTestLayout.ttf` | Box outlines with GSUB, GPOS, GDEF and `kern` tables that use every lookup type and subtable format: single, multiple, alternate (with `rand`), ligature, contextual and chained contextual (formats 1–3), extension and reverse chaining substitution; single and pair (formats 1–2), cursive, mark-to-base, mark-to-ligature, mark-to-mark, contextual and chained contextual (formats 1–3) and extension positioning; anchors of all three formats, mark attachment classes, mark filtering sets, a required feature, a language system, and `kern` subtables of formats 0 and 2. The script checks that each type and format really is in the compiled font. |
  | `CfwTestPlain.ttf` | DejaVu Sans (Latin, marks, Greek, Hebrew, Arabic and presentation forms) without GSUB, GPOS and GDEF but with its `kern` table: marks placed by glyph boxes, Arabic forms from presentation forms, Hebrew presentation forms, legacy kerning |
  | `CfwTestPlain.otf` | A smaller CFF version, for glyph boxes computed from CFF outlines |

- `CfwTestIndic.ttf` and `CfwTestIndicOld.ttf` are made by `testing/text-oracle/make_indic_font.py` (box
  outlines, no third-party data). They hold Devanagari and Malayalam lookups for every Indic basic feature,
  under the second spec's tags (`dev2`, `mlm2`) and the first's (`deva`, `mlym`). The below-base Ra exists
  only before KA, a chained rule with lookahead, so the base consonant depends on whether the engine's
  "would substitute" tests may use context. The script also writes HarfBuzz's results for both fonts.

- `expected.json` holds, for each font, what the references report: names, metrics, a hash of the whole character
  map, and hashes of every glyph's outline in blocks of 64. fontTools is the reference, and FreeType is used where
  fontTools cannot interpret a glyph (Type 2 arithmetic, point-matched components). `FontFaceTest` compares
  against it.

DejaVu's licence (`LICENSE-DejaVu.txt`) allows modified copies, provided they do not use the names "Bitstream" or
"Vera"; the derived fonts are named "CfwTest".
