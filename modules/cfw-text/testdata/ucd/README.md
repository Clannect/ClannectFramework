# Unicode conformance tests

The official test files of the Unicode Character Database (Unicode 18.0), zlib-compressed (`*.txt.z`). The
tests decompress them with cfw-core's own inflater. They are taken from ICU's copy
(`icu4c/source/test/testdata/` in https://github.com/unicode-org/icu):

| File | Algorithm | Test |
|---|---|---|
| `GraphemeBreakTest.txt` | UAX #29 grapheme clusters | `TextBreaksTest` |
| `LineBreakTest.txt` | UAX #14 line breaking | `TextBreaksTest` |
| `BidiTest.txt`, `BidiCharacterTest.txt` | UAX #9 bidirectional text | `BidiTest` |

Unicode data files are © Unicode, Inc. and licensed under the Unicode License v3
(https://www.unicode.org/license.txt), which permits copying and redistribution with this notice.
