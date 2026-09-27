#!/usr/bin/env python3
"""Generates cfw-text's Unicode property tables from ICU's preparsed UCD.

    python3 tools/unicode/generate.py path/to/ppucd.txt path/to/ms-use

ms-use is the directory holding Microsoft's Universal Shaping Engine additions to the UCD,
IndicSyllabicCategory-Additional.txt and IndicPositionalCategory-Additional.txt
(https://github.com/microsoft/font-tools, MIT). They are not committed either.

ppucd.txt is https://raw.githubusercontent.com/unicode-org/icu/main/icu4c/source/data/unidata/ppucd.txt
(Unicode data, Unicode License v3). It is not committed; the version and SHA-256 of the input go into the
generated files, so a regeneration is reproducible. Writes:

    modules/cfw-text/include/cfw/text/UnicodeEnums.h   the property value enums
    modules/cfw-text/src/UnicodeTables.inc              a two-stage trie of 8-byte records, bracket pairs,
                                                        mirroring pairs
    modules/cfw-text/src/ArabicFallback.inc             presentation forms for fonts without Arabic GSUB
    modules/cfw-text/src/SyllabicTables.inc             the syllabic shapers' character categories
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import ppucd  # noqa: E402
import syllabic  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
SHIFT = 7

# Property value lists in enum order. Short codes are what the algorithms (UAX #9, #14, #29) are written in.
GC = ['Cc', 'Cf', 'Cn', 'Co', 'Cs', 'Ll', 'Lm', 'Lo', 'Lt', 'Lu', 'Mc', 'Me', 'Mn', 'Nd', 'Nl', 'No', 'Pc', 'Pd',
      'Pe', 'Pf', 'Pi', 'Po', 'Ps', 'Sc', 'Sk', 'Sm', 'So', 'Zl', 'Zp', 'Zs']
BC = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON', 'LRE', 'LRO', 'RLE', 'RLO',
      'PDF', 'LRI', 'RLI', 'FSI', 'PDI']
GCB = ['XX', 'CR', 'LF', 'CN', 'EX', 'ZWJ', 'RI', 'PP', 'SM', 'L', 'V', 'T', 'LV', 'LVT', 'EB', 'EBG', 'EM', 'GAZ']
INCB = ['None', 'Consonant', 'Extend', 'Linker']
JT = ['U', 'C', 'D', 'L', 'R', 'T']
EA = ['N', 'A', 'H', 'F', 'Na', 'W']
BPT = ['n', 'o', 'c']


def camel(name):
    return ''.join(part[:1].upper() + part[1:] for part in name.split('_'))


def main():
    source = sys.argv[1]
    digest = hashlib.sha256(open(source, 'rb').read()).hexdigest()
    props = ['gc', 'sc', 'bc', 'lb', 'GCB', 'InCB', 'jt', 'ea', 'ExtPict', 'DI', 'EPres', 'bpt', 'bpb', 'bmg', 'ccc',
             'dm', 'dt', 'Comp_Ex']
    v, version = ppucd.load(source, props)

    # Value lists that come from the data: line break classes and scripts.
    lb_values = []
    scripts = []  # (short, long)
    for line in open(source, encoding='utf-8'):
        f = line.rstrip('\n').split(';')
        if f[0] == 'value' and f[1] == 'lb':
            lb_values.append(f[2])
        if f[0] == 'value' and f[1] == 'sc':
            scripts.append((f[2], f[3]))
    special = ['Zyyy', 'Zinh', 'Zzzz']  # Common, Inherited, Unknown first
    scripts.sort(key=lambda s: (special.index(s[0]) if s[0] in special else 3, s[1]))
    sc_index = {s[0]: i for i, s in enumerate(scripts)}
    assert len(scripts) < 256 and len(lb_values) < 64

    def index(values, x, default):
        return values.index(x if x is not None else default)

    records = {}
    record_of = []
    for cp in range(ppucd.N):
        rec = (
            index(GC, v['gc'][cp], 'Cn'),
            sc_index[v['sc'][cp] or 'Zzzz'],
            index(BC, v['bc'][cp], 'L'),
            index(lb_values, v['lb'][cp], 'XX'),
            index(GCB, v['GCB'][cp], 'XX') | index(INCB, v['InCB'][cp], 'None') << 5,
            index(JT, v['jt'][cp], 'U') | index(EA, v['ea'][cp], 'N') << 3,
            (1 if v['ExtPict'][cp] else 0) | (2 if v['DI'][cp] else 0) | (4 if v['EPres'][cp] else 0)
            | index(BPT, v['bpt'][cp], 'n') << 3,
            int(v['ccc'][cp] or 0),
        )
        assert rec[4] < 256 and rec[5] < 256 and rec[6] < 256
        record_of.append(records.setdefault(rec, len(records)))
    size = 1 << SHIFT
    blocks = {}
    stage1 = []
    for b in range(ppucd.N >> SHIFT):
        blk = tuple(record_of[b * size:(b + 1) * size])
        stage1.append(blocks.setdefault(blk, len(blocks)))
    assert len(records) < 65536 and len(blocks) * size < 65536 * size

    brackets = []
    for cp in range(ppucd.N):
        if v['bpt'][cp] in ('o', 'c'):
            brackets.append((cp, int(v['bpb'][cp], 16)))
    # Canonical decompositions (one step) and the primary composites made from them.
    decompositions = []
    compositions = []
    for cp in range(ppucd.N):
        dm = v['dm'][cp]
        if v['dt'][cp] != 'Can' or not dm or dm.startswith('<'):
            continue
        parts = [int(x, 16) for x in dm.split()]
        assert 1 <= len(parts) <= 2, (hex(cp), dm)
        decompositions.append((cp, parts[0], parts[1] if len(parts) == 2 else 0))
        if len(parts) == 2 and not v['Comp_Ex'][cp]:
            compositions.append((parts[0], parts[1], cp))
    compositions.sort()
    mirrors = [(cp, int(v['bmg'][cp], 16)) for cp in range(ppucd.N)
               if v['bmg'][cp] and v['bmg'][cp] != '<code point>' and v['bmg'][cp] != '<none>']

    stamp = f'Unicode {version}, ppucd.txt sha256 {digest}'
    header = f'''#pragma once

// Generated by tools/unicode/generate.py from ICU's preparsed UCD
// ({stamp}). Do not edit.
//
// Unicode property values, in the short codes the Unicode algorithms use.

#include <cstdint>

namespace cfw::unicode {{

inline constexpr const char *kUnicodeVersion = "{version}";

enum class GeneralCategory : std::uint8_t {{ {", ".join(GC)} }};

enum class BidiClass : std::uint8_t {{ {", ".join(BC)} }};

// UAX #14 line break classes.
enum class LineBreakClass : std::uint8_t {{ {", ".join(lb_values)} }};

// UAX #29 grapheme cluster break values (XX = Other, CN = Control, EX = Extend, PP = Prepend,
// SM = SpacingMark; EB, EBG, EM, GAZ are unused since Unicode 11).
enum class GraphemeBreak : std::uint8_t {{ {", ".join(GCB)} }};

// Indic_Conjunct_Break (UAX #29 rule GB9c).
enum class IndicConjunctBreak : std::uint8_t {{ {", ".join(INCB)} }};

// Joining_Type (Arabic, Syriac and other cursive scripts): U non-joining, C join-causing, D dual,
// L left, R right, T transparent.
enum class JoiningType : std::uint8_t {{ {", ".join(JT)} }};

enum class EastAsianWidth : std::uint8_t {{ {", ".join(EA)} }};

// Bidi_Paired_Bracket_Type.
enum class BracketType : std::uint8_t {{ None, Open, Close }};

// Scripts, Common, Inherited and Unknown first, then by name. iso15924() gives the four-letter code.
enum class Script : std::uint8_t {{
{chr(10).join(f"    {camel(long)}, // {short}" for short, long in scripts)}
}};

inline constexpr int kScriptCount = {len(scripts)};

}} // namespace cfw::unicode
'''
    with open(os.path.join(ROOT, 'modules/cfw-text/include/cfw/text/UnicodeEnums.h'), 'w') as out:
        out.write(header)

    def rows(values, per_line, fmt=str):
        items = [fmt(x) for x in values]
        return '\n'.join('    ' + ', '.join(items[i:i + per_line]) + ',' for i in range(0, len(items), per_line))

    ordered = sorted(records.items(), key=lambda kv: kv[1])
    stage2 = [r for blk, _ in sorted(blocks.items(), key=lambda kv: kv[1]) for r in blk]
    inc = f'''// Generated by tools/unicode/generate.py from ICU's preparsed UCD
// ({stamp}). Do not edit.
// {len(records)} distinct property records, {len(blocks)} blocks of {size} code points.

namespace cfw::unicode::data {{

inline constexpr int kShift = {SHIFT};

// {{general category, script, bidi class, line break, grapheme break | InCB << 5,
//  joining type | east asian width << 3, flags, combining class}}
// flags: 1 Extended_Pictographic, 2 Default_Ignorable_Code_Point, 4 Emoji_Presentation,
//        bits 3-4 Bidi_Paired_Bracket_Type.
inline constexpr Record kRecords[{len(records)}] = {{
{rows(["{%s}" % ", ".join(str(x) for x in r) for r, _ in ordered], 4)}
}};

inline constexpr std::uint16_t kStage1[{len(stage1)}] = {{
{rows(stage1, 16)}
}};

inline constexpr std::uint16_t kStage2[{len(stage2)}] = {{
{rows(stage2, 16)}
}};

// Bidi_Paired_Bracket: code point, its pair (sorted by code point).
inline constexpr Pair kBrackets[{len(brackets)}] = {{
{rows(["{0x%04X, 0x%04X}" % b for b in brackets], 6)}
}};

// Bidi_Mirroring_Glyph: code point, its mirror (sorted by code point).
inline constexpr Pair kMirrors[{len(mirrors)}] = {{
{rows(["{0x%04X, 0x%04X}" % m for m in mirrors], 6)}
}};

// Canonical decompositions, one step: code point, first, second (0 for a singleton). Sorted.
inline constexpr Triple kDecompositions[{len(decompositions)}] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X}" % d for d in decompositions], 4)}
}};

// Primary composites: first, second, composite. Sorted by (first, second).
inline constexpr Triple kCompositions[{len(compositions)}] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X}" % c for c in compositions], 4)}
}};

// ISO 15924 codes, in Script order.
inline constexpr const char kScriptCodes[{len(scripts)}][5] = {{
{rows(['"%s"' % s for s, _ in scripts], 12)}
}};

}} // namespace cfw::unicode::data
'''
    with open(os.path.join(ROOT, 'modules/cfw-text/src/UnicodeTables.inc'), 'w') as out:
        out.write(inc)
    write_arabic_fallback(v, stamp)
    syllabic.write(source, sys.argv[2], stamp, ROOT, SHIFT)
    print(f'{stamp}: {len(records)} records, {len(blocks)} blocks, {len(brackets)} brackets, {len(mirrors)} mirrors, '
          f'{len(scripts)} scripts, {len(lb_values)} line break classes, {len(decompositions)} decompositions, '
          f'{len(compositions)} compositions')


# The Arabic presentation-form ligatures that fonts without Arabic GSUB
# commonly have (the ones HarfBuzz's fallback shaping forms), plus three
# Private Use ones some fonts use.
ARABIC_LIGATURES = {
    0xF2EE, 0xFC08, 0xFC0E, 0xFC12, 0xFC32, 0xFC3F, 0xFC40, 0xFC41, 0xFC42, 0xFC43, 0xFC44, 0xFC4E, 0xFC5E, 0xFC60,
    0xFC61, 0xFC62, 0xFC6A, 0xFC6D, 0xFC6F, 0xFC70, 0xFC73, 0xFC75, 0xFC86, 0xFC8F, 0xFC91, 0xFC94, 0xFC9C, 0xFC9D,
    0xFC9E, 0xFC9F, 0xFCA1, 0xFCA2, 0xFCA3, 0xFCA4, 0xFCA8, 0xFCAA, 0xFCAC, 0xFCB0, 0xFCC9, 0xFCCA, 0xFCCB, 0xFCCC,
    0xFCCD, 0xFCCE, 0xFCCF, 0xFCD0, 0xFCD1, 0xFCD2, 0xFCD3, 0xFCD5, 0xFCDA, 0xFCDB, 0xFCDC, 0xFCDD, 0xFD30, 0xFD88,
    0xFEF5, 0xFEF6, 0xFEF7, 0xFEF8, 0xFEF9, 0xFEFA, 0xFEFB, 0xFEFC, 0xF201, 0xF211,
}
PUA_LIGATURES = [(0xF201, 'Iso', '0644 0644 0647'), (0xF211, 'Init', '0644 0645 062C'), (0xF2EE, 'Iso', '0020 064B 0651')]


def write_arabic_fallback(v, stamp):
    """Arabic presentation forms for fonts without Arabic GSUB: per letter its
    initial, medial, final and isolated form, and ligatures of those forms."""
    forms = {'Init': 0, 'Med': 1, 'Fin': 2, 'Iso': 3}
    shapes = {}
    ligatures = {}  # components -> {form or None: ligature}
    entries = [(cp, v['dt'][cp], v['dm'][cp]) for cp in range(ppucd.N) if v['dt'][cp] in forms and v['dm'][cp]]
    entries += PUA_LIGATURES
    for cp, dt, dm in entries:
        items = tuple(int(x, 16) for x in dm.split())
        form = forms[dt]
        if len(items) == 1:
            shapes.setdefault(items[0], {})[form] = cp
            continue
        if items[0] == 0x20:  # mark ligatures: a space, then the marks in visual order
            items = items[:0:-1]
            form = None
        if cp in ARABIC_LIGATURES:
            ligatures.setdefault(items, {})[form] = cp
    first, last = min(shapes), max(shapes)
    table = [[shapes.get(u, {}).get(f, 0) for f in range(4)] for u in range(first, last + 1)]
    lig2, lig3, mark2 = {}, {}, {}
    for key, by_form in ligatures.items():
        for form, cp in by_form.items():
            if form is None:
                mark2.setdefault(key[0], []).append((key[1], cp))
                continue
            shape = lambda i, f: shapes[key[i]][forms[f]]
            if len(key) == 3:
                seq = {3: ('Init', 'Med', 'Fin'), 2: ('Med', 'Med', 'Fin'), 0: ('Init', 'Med', 'Med')}[form]
                liga = tuple(shape(i, f) for i, f in enumerate(seq))
                lig3.setdefault(liga[0], []).append((liga[1], liga[2], cp))
            else:
                seq = {3: ('Init', 'Fin'), 2: ('Med', 'Fin'), 0: ('Init', 'Med')}[form]
                liga = tuple(shape(i, f) for i, f in enumerate(seq))
                lig2.setdefault(liga[0], []).append((liga[1], cp))

    def rows(items, per_line):
        return '\n'.join('    ' + ', '.join(items[i:i + per_line]) + ',' for i in range(0, len(items), per_line))

    out = f'''// Generated by tools/unicode/generate.py from {stamp}. Do not edit.
// Arabic fallback shaping (Shaper.cpp): presentation forms per letter and
// ligatures of presentation forms, for fonts without Arabic GSUB.

namespace cfw::arabic_fallback {{

inline constexpr char32_t kFirst = 0x{first:04X};
inline constexpr char32_t kLast = 0x{last:04X};
// Initial, medial, final, isolated form of each letter kFirst..kLast (0: none).
inline constexpr std::uint16_t kForms[{len(table)}][4] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X, 0x%04X}" % tuple(r) for r in table], 3)}
}};

struct Ligature2 {{
    std::uint16_t first;
    std::uint16_t second;
    std::uint16_t ligature;
}};
struct Ligature3 {{
    std::uint16_t first;
    std::uint16_t second;
    std::uint16_t third;
    std::uint16_t ligature;
}};
// Letter ligatures (of presentation forms), by first form; within a first
// form in the order to try.
inline constexpr Ligature2 kLigatures[] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X}" % (f, a, l) for f in sorted(lig2) for a, l in lig2[f]], 3)}
}};
// Three-letter ligatures.
inline constexpr Ligature3 kLigatures3[] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X, 0x%04X}" % (f, a, b, l) for f in sorted(lig3) for a, b, l in lig3[f]], 2)}
}};
// Mark ligatures (shadda with a vowel mark), isolated forms.
inline constexpr Ligature2 kMarkLigatures[] = {{
{rows(["{0x%04X, 0x%04X, 0x%04X}" % (f, a, l) for f in sorted(mark2) for a, l in mark2[f]], 3)}
}};

}} // namespace cfw::arabic_fallback
'''
    with open(os.path.join(ROOT, 'modules/cfw-text/src/ArabicFallback.inc'), 'w') as f:
        f.write(out)


if __name__ == '__main__':
    main()
