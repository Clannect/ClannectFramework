"""Character categories for the syllabic shapers (USE, Indic, Khmer, Myanmar).

The categories are those of the OpenType shaping specifications (Microsoft's
Universal Shaping Engine and Indic/Khmer/Myanmar documents), derived from
Indic_Syllabic_Category, Indic_Positional_Category, Joining_Type,
Default_Ignorable_Code_Point, General_Category, Block and Script, with the
adjustments HarfBuzz makes, so both shape alike.
"""
import os
import re

import ppucd

# ---- Universal Shaping Engine ----

USE = ['O', 'B', 'N', 'GB', 'CGJ', 'SUB', 'CS', 'H', 'HVM', 'HN', 'IS', 'G', 'HM', 'HR', 'J', 'SB', 'SE', 'ZWNJ',
       'RK', 'R', 'Sk', 'WJ',
       'FAbv', 'FBlw', 'FPst', 'FMAbv', 'FMBlw', 'FMPst', 'MAbv', 'MBlw', 'MPst', 'MPre', 'CMAbv', 'CMBlw',
       'SMAbv', 'SMBlw', 'VAbv', 'VBlw', 'VPst', 'VPre', 'VMAbv', 'VMBlw', 'VMPst', 'VMPre']

USE_DISABLED_SCRIPTS = {'Arabic', 'Lao', 'Samaritan', 'Syriac', 'Thai'}

USE_POSITIONS = {
    'F': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Right']},
    'M': {'Abv': ['Top'], 'Blw': ['Bottom', 'Bottom_And_Left', 'Bottom_And_Right'], 'Pst': ['Right'],
          'Pre': ['Left', 'Top_And_Bottom_And_Left']},
    'CM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck']},
    'V': {'Abv': ['Top', 'Top_And_Bottom', 'Top_And_Bottom_And_Right', 'Top_And_Right'],
          'Blw': ['Bottom', 'Overstruck', 'Bottom_And_Right'], 'Pst': ['Right'],
          'Pre': ['Left', 'Top_And_Left', 'Top_And_Left_And_Right', 'Left_And_Right']},
    'VM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck'], 'Pst': ['Right'], 'Pre': ['Left']},
    'SM': {'Abv': ['Top'], 'Blw': ['Bottom']},
    'FM': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Not_Applicable']},
}


def use_category(u, isc, ipc, jt, di, gc):
    if 0x1CE2 <= u <= 0x1CE8:
        isc = 'Cantillation_Mark'
    if 0x0F18 <= u <= 0x0F19 or 0x0F3E <= u <= 0x0F3F:
        isc = 'Vowel_Dependent'
    if u == 0x1CED:
        isc = 'Tone_Mark'
    if u in (0x11302, 0x11303, 0x114C1):
        ipc = 'Top'

    base = (isc in ('Number', 'Consonant', 'Consonant_Head_Letter', 'Tone_Letter', 'Vowel_Independent')
            or (jt in ('C', 'D', 'L', 'R') and isc != 'Joiner')
            or (gc == 'Lo' and isc in ('Avagraha', 'Bindu', 'Consonant_Final', 'Consonant_Medial',
                                       'Consonant_Subjoined', 'Vowel', 'Vowel_Dependent')))
    base_other = isc == 'Consonant_Placeholder' or u in (0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE)
    cgj = isc == 'Joiner' or (di and gc in ('Mc', 'Me', 'Mn'))
    sym_mod = isc == 'Symbol_Modifier'
    word_joiner = (di and u not in (0x115F, 0x1160, 0x3164, 0xFFA0, 0x1BCA0, 0x1BCA1, 0x1BCA2, 0x1BCA3)
                   and isc == 'Other' and not cgj) or gc == 'Cn'
    sakot = u == 0x1A60
    tests = [
        ('B', base),
        ('N', isc == 'Brahmi_Joining_Number'),
        ('GB', base_other),
        ('CGJ', cgj),
        ('F', (isc == 'Consonant_Final' and gc != 'Lo') or isc == 'Consonant_Succeeding_Repha'),
        ('FM', isc == 'Syllable_Modifier'),
        ('M', (isc == 'Consonant_Medial' and gc != 'Lo') or isc == 'Consonant_Initial_Postfixed'),
        ('CM', isc in ('Nukta', 'Gemination_Mark', 'Consonant_Killer')),
        ('SUB', isc == 'Consonant_Subjoined' and gc != 'Lo'),
        ('CS', isc == 'Consonant_With_Stacker'),
        ('H', isc == 'Virama' and u != 0x0DCA),
        ('HVM', u == 0x0DCA),
        ('HN', isc == 'Number_Joiner'),
        ('IS', isc == 'Invisible_Stacker' and not sakot),
        ('G', isc == 'Hieroglyph'),
        ('HM', isc == 'Hieroglyph_Modifier'),
        ('HR', isc == 'Hieroglyph_Mirror'),
        ('J', isc == 'Hieroglyph_Joiner'),
        ('SB', isc in ('Hieroglyph_Mark_Begin', 'Hieroglyph_Segment_Begin')),
        ('SE', isc in ('Hieroglyph_Mark_End', 'Hieroglyph_Segment_End')),
        ('ZWNJ', isc == 'Non_Joiner'),
        ('O', (gc == 'Po' or isc in ('Consonant_Dead', 'Joiner', 'Modifying_Letter', 'Other'))
         and not base and not base_other and not cgj and not sym_mod and not word_joiner),
        ('RK', isc == 'Reordering_Killer'),
        ('R', isc in ('Consonant_Preceding_Repha', 'Consonant_Prefixed')),
        ('Sk', sakot),
        ('SM', sym_mod),
        ('V', isc == 'Pure_Killer' or (gc != 'Lo' and isc in ('Vowel', 'Vowel_Dependent'))),
        ('VM', isc in ('Tone_Mark', 'Cantillation_Mark', 'Register_Shifter', 'Visarga')
         or (gc != 'Lo' and isc == 'Bindu')),
        ('WJ', word_joiner),
    ]
    found = [k for k, t in tests if t]
    assert len(found) == 1, (hex(u), isc, ipc, jt, di, gc, found)
    cat = found[0]
    positions = USE_POSITIONS.get(cat)
    if positions:
        suffix = [k for k, v in positions.items() if ipc in v]
        assert len(suffix) == 1, (hex(u), cat, ipc)
        cat += suffix[0]
    assert cat in USE, (hex(u), cat, isc, ipc)
    return cat


# ---- Indic, Khmer, Myanmar ----

INDIC = ['X', 'C', 'V', 'N', 'H', 'ZWNJ', 'ZWJ', 'M', 'SM', 'A', 'VD', 'PLACEHOLDER', 'DOTTEDCIRCLE', 'RS', 'MPst',
         'Repha', 'Ra', 'CM', 'Symbol', 'CS', 'SMPst',
         'VAbv', 'VBlw', 'VPre', 'VPst', 'Robatic', 'Xgroup', 'Ygroup',
         'IV', 'As', 'DB', 'GB', 'MH', 'MR', 'MW', 'MY', 'PT', 'VS', 'ML']
POSITIONS = ['START', 'RA_TO_BECOME_REPH', 'PRE_M', 'PRE_C', 'BASE_C', 'AFTER_MAIN', 'ABOVE_C', 'BEFORE_SUB',
             'BELOW_C', 'AFTER_SUB', 'BEFORE_POST', 'POST_C', 'AFTER_POST', 'SMVD', 'END']

INDIC_SINGLES = (0x00A0, 0x25CC)
INDIC_BLOCKS = ['Basic Latin', 'Latin-1 Supplement', 'Devanagari', 'Bengali', 'Gurmukhi', 'Gujarati', 'Oriya',
                'Tamil', 'Telugu', 'Kannada', 'Malayalam', 'Myanmar', 'Khmer', 'Vedic Extensions',
                'General Punctuation', 'Superscripts and Subscripts', 'Devanagari Extended', 'Myanmar Extended-B',
                'Myanmar Extended-A', 'Myanmar Extended-C']

INDIC_CATEGORY = {
    'Other': 'X', 'Avagraha': 'Symbol', 'Bindu': 'SM', 'Brahmi_Joining_Number': 'PLACEHOLDER',
    'Cantillation_Mark': 'A', 'Consonant': 'C', 'Consonant_Dead': 'C', 'Consonant_Final': 'CM',
    'Consonant_Head_Letter': 'C', 'Consonant_Initial_Postfixed': 'C', 'Consonant_Killer': 'M',
    'Consonant_Medial': 'CM', 'Consonant_Placeholder': 'PLACEHOLDER', 'Consonant_Preceding_Repha': 'Repha',
    'Consonant_Prefixed': 'X', 'Consonant_Subjoined': 'CM', 'Consonant_Succeeding_Repha': 'CM',
    'Consonant_With_Stacker': 'CS', 'Gemination_Mark': 'SM', 'Invisible_Stacker': 'H', 'Joiner': 'ZWJ',
    'Modifying_Letter': 'X', 'Non_Joiner': 'ZWNJ', 'Nukta': 'N', 'Number': 'PLACEHOLDER',
    'Number_Joiner': 'PLACEHOLDER', 'Pure_Killer': 'M', 'Register_Shifter': 'RS', 'Syllable_Modifier': 'SM',
    'Tone_Letter': 'X', 'Tone_Mark': 'N', 'Virama': 'H', 'Visarga': 'SM', 'Vowel': 'V', 'Vowel_Dependent': 'M',
    'Vowel_Independent': 'V',
}
INDIC_POSITION = {
    'Not_Applicable': 'END', 'Left': 'PRE_C', 'Top': 'ABOVE_C', 'Bottom': 'BELOW_C', 'Right': 'POST_C',
    'Bottom_And_Right': 'POST_C', 'Left_And_Right': 'POST_C', 'Top_And_Bottom': 'BELOW_C',
    'Top_And_Bottom_And_Left': 'BELOW_C', 'Top_And_Bottom_And_Right': 'POST_C', 'Top_And_Left': 'ABOVE_C',
    'Top_And_Left_And_Right': 'POST_C', 'Top_And_Right': 'POST_C', 'Overstruck': 'AFTER_MAIN',
    'Visual_order_left': 'PRE_M', 'Visual_Order_Left': 'PRE_M',
}


def _overrides():
    o = {u: 'VS' for u in range(0xFE00, 0xFE10)}
    o.update({u: 'PLACEHOLDER' for u in (0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE)})
    o.update({u: 'Ra' for u in (0x0930, 0x09B0, 0x09F0, 0x0A30, 0x0AB0, 0x0B30, 0x0BB0, 0x0C30, 0x0CB0, 0x0D30)})
    o.update({0x0953: 'SM', 0x0954: 'SM', 0x0A40: 'MPst', 0x0A72: 'C', 0x0A73: 'C'})
    o.update({u: 'A' for u in (0x1CE2, 0x1CE3, 0x1CE4, 0x1CE5, 0x1CE6, 0x1CE7, 0x1CE8, 0x1CED)})
    o.update({u: 'Symbol' for u in (0xA8F2, 0xA8F3, 0xA8F4, 0xA8F5, 0xA8F6, 0xA8F7, 0x1CE9, 0x1CEA, 0x1CEB, 0x1CEC,
                                    0x1CEE, 0x1CEF, 0x1CF0, 0x1CF1)})
    o.update({0x0A51: 'M', 0x11301: 'SM', 0x11302: 'SM', 0x11303: 'SM', 0x1133B: 'N', 0x1133C: 'N', 0x0AFB: 'N',
              0x0B55: 'N', 0x09FC: 'PLACEHOLDER', 0x0C80: 'PLACEHOLDER', 0x0D04: 'PLACEHOLDER',
              0x25CC: 'DOTTEDCIRCLE'})
    # Khmer
    o.update({0x179A: 'Ra', 0x17CC: 'Robatic', 0x17C9: 'Robatic', 0x17CA: 'Robatic'})
    o.update({u: 'Xgroup' for u in (0x17C6, 0x17CB, 0x17CD, 0x17CE, 0x17CF, 0x17D0, 0x17D1)})
    o.update({u: 'Ygroup' for u in (0x17C7, 0x17C8, 0x17DD, 0x17D3)})
    o[0x17D9] = 'PLACEHOLDER'
    # Myanmar
    o.update({0x104E: 'C', 0x1004: 'Ra', 0x101B: 'Ra', 0x105A: 'Ra', 0x1032: 'A', 0x1036: 'A', 0x103A: 'As',
              0x103E: 'MH', 0x1060: 'ML', 0x103C: 'MR', 0x103D: 'MW', 0x1082: 'MW', 0x103B: 'MY', 0x105E: 'MY',
              0x105F: 'MY'})
    o.update({u: 'PT' for u in (0x1063, 0x1064, 0x1069, 0x106A, 0x106B, 0x106C, 0x106D, 0xAA7B)})
    o.update({u: 'SM' for u in (0x1038, 0x1087, 0x1088, 0x1089, 0x108A, 0x108B, 0x108C, 0x108D, 0x108F, 0x109A,
                                0x109B, 0x109C)})
    o[0x104A] = 'PLACEHOLDER'
    return o


def _matra_position(u, pos, block):
    table = {
        'PRE_C': {},
        'POST_C': {'Devanagari': 'AFTER_SUB', 'Bengali': 'AFTER_POST', 'Gurmukhi': 'AFTER_POST',
                   'Gujarati': 'AFTER_POST', 'Oriya': 'AFTER_POST', 'Tamil': 'AFTER_POST',
                   'Telugu': 'BEFORE_SUB' if u <= 0x0C42 else 'AFTER_SUB',
                   'Kannada': 'BEFORE_SUB' if u < 0x0CC3 or u > 0x0CD6 else 'AFTER_SUB',
                   'Malayalam': 'AFTER_POST'},
        'ABOVE_C': {'Devanagari': 'AFTER_SUB', 'Gurmukhi': 'AFTER_POST', 'Gujarati': 'AFTER_SUB',
                    'Oriya': 'AFTER_MAIN', 'Tamil': 'AFTER_SUB', 'Telugu': 'BEFORE_SUB', 'Kannada': 'BEFORE_SUB'},
        'BELOW_C': {'Devanagari': 'AFTER_SUB', 'Bengali': 'AFTER_SUB', 'Gurmukhi': 'AFTER_POST',
                    'Gujarati': 'AFTER_POST', 'Oriya': 'AFTER_SUB', 'Tamil': 'AFTER_POST', 'Telugu': 'BEFORE_SUB',
                    'Kannada': 'BEFORE_SUB', 'Malayalam': 'AFTER_POST'},
    }
    if pos == 'PRE_C':
        return 'PRE_M'
    return table[pos].get(block, 'AFTER_SUB')


def indic_categories(isc_of, ipc_of, block_of):
    """{code point: (category, position)} for the code points the Indic, Khmer and Myanmar
    shapers distinguish; everything else is (X, END)."""
    data = {}
    for u in set(isc_of) | set(ipc_of):
        block = block_of(u)
        if u not in INDIC_SINGLES and block not in INDIC_BLOCKS:
            continue
        cat = INDIC_CATEGORY[isc_of.get(u, 'Other')]
        pos = ipc_of.get(u, 'Not_Applicable')
        if cat == 'SM' and pos == 'Not_Applicable':
            cat = 'SMPst'
        data[u] = [cat, INDIC_POSITION[pos], block]
    for u, cat in _overrides().items():
        pos = data[u][1] if u in data else 'END'
        data[u] = [cat, pos, block_of(u)]
    for u, d in data.items():
        cat, pos, block = d
        if cat not in ('CM', 'SM', 'RS', 'H', 'M', 'MPst'):
            pos = 'END'
        if cat in ('C', 'CS', 'Ra', 'CM', 'V', 'PLACEHOLDER', 'DOTTEDCIRCLE'):
            pos = 'BASE_C'
        elif cat in ('M', 'MPst'):
            if block.startswith('Khmer') or block.startswith('Myanmar'):
                cat = {'PRE_C': 'VPre', 'ABOVE_C': 'VAbv', 'BELOW_C': 'VBlw', 'POST_C': 'VPst'}[pos]
            else:
                pos = _matra_position(u, pos, block)
        elif cat in ('SM', 'SMPst', 'VD', 'A', 'Symbol'):
            pos = 'SMVD'
        data[u] = (cat, pos)
    data[0x0A51] = (data[0x0A51][0], 'BELOW_C')
    data[0x0B01] = (data[0x0B01][0], 'BEFORE_SUB')
    return data


# ---- Output ----

def _read_additional(path, rename):
    out = {}
    for line in open(path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        rng, value = (x.strip() for x in line.split(';')[:2])
        lo, hi = ppucd.parse_range(rng)
        for u in range(lo, hi + 1):
            out[u] = rename.get(value, value)
    return out


def _long_names(source, prop):
    names = {}
    for line in open(source, encoding='utf-8'):
        f = line.rstrip('\n').split(';')
        if f[0] == 'value' and f[1] == prop:
            names[f[2]] = f[3]
    return names


def _block_key(name):
    return re.sub('[^a-z0-9]', '', name.lower())


def write(source, ms_use, stamp, root, shift):
    v, _ = ppucd.load(source, ['InSC', 'InPC', 'jt', 'DI', 'gc', 'blk', 'sc'])
    isc_names = _long_names(source, 'InSC')
    ipc_names = _long_names(source, 'InPC')
    blk_names = _long_names(source, 'blk')
    sc_names = _long_names(source, 'sc')
    known_blocks = {_block_key(b): b for b in INDIC_BLOCKS}

    isc_of = {u: isc_names.get(x, x) for u, x in enumerate(v['InSC']) if x and isc_names.get(x, x) != 'Other'}
    ipc_of = {u: ipc_names.get(x, x) for u, x in enumerate(v['InPC'])
              if x and ipc_names.get(x, x) != 'Not_Applicable'}
    use_isc = dict(isc_of)
    use_isc.update(_read_additional(os.path.join(ms_use, 'IndicSyllabicCategory-Additional.txt'),
                                    {'Consonant_Final_Modifier': 'Syllable_Modifier'}))
    use_ipc = dict(ipc_of)
    use_ipc.update(_read_additional(os.path.join(ms_use, 'IndicPositionalCategory-Additional.txt'),
                                    {'NA': 'Not_Applicable'}))

    def block_of(u):
        b = v['blk'][u]
        b = blk_names.get(b, b) if b else 'No_Block'
        return known_blocks.get(_block_key(b), b)

    use = {}
    members = set(use_isc) | set(use_ipc) | {u for u in range(ppucd.N) if v['DI'][u] or v['jt'][u] in ('C', 'D', 'L', 'R')}
    for u in members:
        sc = v['sc'][u]
        if sc_names.get(sc, sc) in USE_DISABLED_SCRIPTS:
            continue
        use[u] = use_category(u, use_isc.get(u, 'Other'), use_ipc.get(u, 'Not_Applicable'), v['jt'][u],
                              bool(v['DI'][u]), v['gc'][u] or 'Cn')
    indic = indic_categories(isc_of, ipc_of, block_of)

    records = {}
    record_of = []
    for u in range(ppucd.N):
        cat, pos = indic.get(u, ('X', 'END'))
        rec = (USE.index(use.get(u, 'O')), INDIC.index(cat), POSITIONS.index(pos))
        record_of.append(records.setdefault(rec, len(records)))
    size = 1 << shift
    blocks = {}
    stage1 = []
    for b in range(ppucd.N >> shift):
        blocks_key = tuple(record_of[b * size:(b + 1) * size])
        stage1.append(blocks.setdefault(blocks_key, len(blocks)))
    assert len(records) < 256
    stage2 = [r for blk, _ in sorted(blocks.items(), key=lambda kv: kv[1]) for r in blk]
    ordered = [r for r, _ in sorted(records.items(), key=lambda kv: kv[1])]

    def rows(values, per_line):
        items = [str(x) for x in values]
        return '\n'.join('    ' + ', '.join(items[i:i + per_line]) + ',' for i in range(0, len(items), per_line))

    out = f'''// Generated by tools/unicode/generate.py (syllabic.py) from ICU's preparsed UCD
// ({stamp}) and the Universal Shaping Engine's additional categories. Do not edit.
//
// The character categories of the syllabic shapers: USE for the scripts the Universal Shaping Engine
// handles, and the Indic, Khmer and Myanmar categories with the Indic positions.

namespace cfw::syllabic {{

enum class UseCat : std::uint8_t {{ {", ".join(USE)} }};
inline constexpr int kUseCatCount = {len(USE)};

enum class IndicCat : std::uint8_t {{ {", ".join(INDIC)} }};
inline constexpr int kIndicCatCount = {len(INDIC)};

enum class IndicPos : std::uint8_t {{ {", ".join(POSITIONS)} }};

namespace data {{

inline constexpr int kShift = {shift};

// {{UseCat, IndicCat, IndicPos}}
inline constexpr std::uint8_t kRecords[{len(ordered)}][3] = {{
{rows(["{%d, %d, %d}" % r for r in ordered], 8)}
}};

inline constexpr std::uint16_t kStage1[{len(stage1)}] = {{
{rows(stage1, 16)}
}};

inline constexpr std::uint8_t kStage2[{len(stage2)}] = {{
{rows(stage2, 24)}
}};

}} // namespace data

}} // namespace cfw::syllabic
'''
    with open(os.path.join(root, 'modules/cfw-text/src/SyllabicTables.inc'), 'w') as f:
        f.write(out)
    print(f'syllabic: {len(records)} records, {len(blocks)} blocks, {len(use)} USE entries, {len(indic)} Indic')
