#include "cfw/text/Shaper.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "OtLayout.h"
#include "Syllabic.h"
#include "ArabicFallback.inc"
#include "cfw/text/Unicode.h"

namespace cfw {

using unicode::GeneralCategory;
using unicode::Script;

namespace {

constexpr std::uint32_t tag(const char (&t)[5]) { return FontFace::tag(t); }
constexpr std::uint32_t tag4(const char *t) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(t[0])) << 24 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(t[1])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(t[2])) << 8 | static_cast<unsigned char>(t[3]);
}
constexpr std::uint32_t kGlobalBit = 1u << 31;
constexpr unsigned kMaxCombiningMarks = 32;

enum class Engine : std::uint8_t { Default, Arabic, Hebrew, Thai, Hangul, Use, Indic, Khmer, Myanmar };

// What runs between two GSUB stages.
enum Pause : std::uint8_t {
    PauseNone,
    PauseClearSubstitution, // forget which glyphs were substituted
    PauseClearSyllables,
    PauseUseSyllables, // USE: find syllables, set the rphf and joining masks
    PauseUseRphf,      // USE: a substituted glyph under rphf is a repha
    PauseUsePref,      // USE: a substituted glyph under pref is a pre-base vowel
    PauseUseReorder,   // USE: dotted circles, then reordering
    PauseIndicSyllables,
    PauseIndicInitial, // Indic: consonant positions, dotted circles, reordering, masks
    PauseIndicFinal,   // Indic: matras, reph and pre-base forms to their places
    PauseKhmerSyllables,
    PauseKhmerReorder,
    PauseMyanmarSyllables,
    PauseMyanmarReorder,
};

// Arabic joining forms, in the order of kArabicFeatures.
enum ArabicAction : std::uint8_t { Isol, Fina, Fin2, Fin3, Medi, Med2, Init, None };
constexpr std::uint32_t kArabicFeatures[7] = {tag("isol"), tag("fina"), tag("fin2"), tag("fin3"),
                                              tag("medi"), tag("med2"), tag("init")};

// HarfBuzz's modified combining classes: Hebrew points in SBL order, Arabic
// shadda first, and a few others, so marks reorder the way fonts expect.
std::uint8_t modifiedCombiningClass(char32_t u, std::uint8_t ccc) {
    // Reorder SAKOT to ensure it comes after any tone marks (HarfBuzz).
    if (u == 0x1A60) {
        return 254;
    }
    // Reorder PADMA to ensure it comes after any vowel marks (HarfBuzz).
    if (u == 0x0FC6) {
        return 254;
    }
    // Reorder TSA -PHRU to reorder before U+0F74 (HarfBuzz).
    if (u == 0x0F39) {
        return 127;
    }
    switch (ccc) {
    case 10: return 22;  // sheva
    case 11: return 15;  // hataf segol
    case 12: return 16;  // hataf patah
    case 13: return 17;  // hataf qamats
    case 14: return 23;  // hiriq
    case 15: return 18;  // tsere
    case 16: return 19;  // segol
    case 17: return 20;  // patah
    case 18: return 21;  // qamats
    case 19: return 14;  // holam
    case 20: return 24;  // qubuts
    case 21: return 12;  // dagesh
    case 22: return 25;  // meteg
    case 23: return 13;  // rafe
    case 24: return 10;  // shin dot
    case 25: return 11;  // sin dot
    case 26: return 26;  // point varika
    case 27: return 28;  // fathatan
    case 28: return 29;  // dammatan
    case 29: return 30;  // kasratan
    case 30: return 31;  // fatha
    case 31: return 32;  // damma
    case 32: return 33;  // kasra
    case 33: return 27;  // shadda
    case 34: return 34;  // sukun
    case 35: return 35;  // superscript alef
    case 36: return 36;  // superscript alaph
    case 84: return 4;   // Telugu length mark
    case 91: return 5;   // Telugu ai length mark
    case 103: return 3;  // Thai sara u / sara uu
    case 130: return 132; // Tibetan sign i
    case 132: return 131; // Tibetan sign u
    default: return ccc;
    }
}

// Default ignorables, as HarfBuzz lists them (the UCD set without the
// Hangul fillers and a few others that fonts draw).
bool isDefaultIgnorable(char32_t u) {
    switch (u >> 8) {
    case 0x00: return u == 0x00AD;
    case 0x03: return u == 0x034F;
    case 0x06: return u == 0x061C;
    case 0x17: return u >= 0x17B4 && u <= 0x17B5;
    case 0x18: return u >= 0x180B && u <= 0x180F;
    case 0x20: return (u >= 0x200B && u <= 0x200F) || (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x206F);
    case 0xFE: return (u >= 0xFE00 && u <= 0xFE0F) || u == 0xFEFF;
    case 0xFF: return u >= 0xFFF0 && u <= 0xFFF8;
    case 0x1D1: return u >= 0x1D173 && u <= 0x1D17A;
    default: return u >= 0xE0000 && u <= 0xE0FFF;
    }
}

bool isRegionalIndicator(char32_t u) { return u >= 0x1F1E6 && u <= 0x1F1FF; }

// Variation selectors for cmap variation sequences (the Mongolian free
// variation selectors are not among them for normalisation).
bool isVariationSelector(char32_t u) { return (u >= 0xFE00 && u <= 0xFE0F) || (u >= 0xE0100 && u <= 0xE01EF); }

bool isMarkCategory(std::uint8_t gc) {
    return gc == static_cast<std::uint8_t>(GeneralCategory::Mn) || gc == static_cast<std::uint8_t>(GeneralCategory::Mc) ||
           gc == static_cast<std::uint8_t>(GeneralCategory::Me);
}

// A character's Unicode properties as the shaper keeps them (HarfBuzz's
// unicode props): category, default-ignorable flags, and for marks the
// modified combining class and the continuation flag.
void setUnicodeProps(ot::GlyphInfo &g) {
    const char32_t u = g.codepoint;
    const unicode::Properties props = unicode::properties(u);
    g.generalCategory = static_cast<std::uint8_t>(props.generalCategory);
    g.flags = 0;
    g.combiningClass = 0;
    if (u < 0x80) {
        return;
    }
    if (isDefaultIgnorable(u)) {
        g.flags |= ot::kIgnorable;
        if (u == 0x200C) {
            g.flags |= ot::kZwnj;
        } else if (u == 0x200D) {
            g.flags |= ot::kZwj;
        } else if ((u >= 0x180B && u <= 0x180D) || u == 0x180F || (u >= 0xE0020 && u <= 0xE007F) || u == 0x034F) {
            g.flags |= ot::kHidden; // shown to GSUB, hidden from GPOS
        }
    }
    if (isMarkCategory(g.generalCategory)) {
        g.flags |= ot::kContinuation;
        g.combiningClass = modifiedCombiningClass(u, props.combiningClass);
    }
}

// Space fallback widths for spaces the font has no glyph for.
enum SpaceType : std::uint8_t {
    NotSpace = 0,
    SpaceEm = 1,
    SpaceEm2 = 2,
    SpaceEm3 = 3,
    SpaceEm4 = 4,
    SpaceEm5 = 5,
    SpaceEm6 = 6,
    SpaceEm16 = 16,
    Space4Em18,
    SpacePlain,
    SpaceFigure,
    SpacePunctuation,
    SpaceNarrow,
};

SpaceType spaceType(char32_t u) {
    switch (u) {
    case 0x0020:
    case 0x00A0: return SpacePlain;
    case 0x2000:
    case 0x2002: return SpaceEm2;
    case 0x2001:
    case 0x2003:
    case 0x3000: return SpaceEm;
    case 0x2004: return SpaceEm3;
    case 0x2005: return SpaceEm4;
    case 0x2006: return SpaceEm6;
    case 0x2007: return SpaceFigure;
    case 0x2008: return SpacePunctuation;
    case 0x2009: return SpaceEm5;
    case 0x200A: return SpaceEm16;
    case 0x202F: return SpaceNarrow;
    case 0x205F: return Space4Em18;
    default: return NotSpace;
    }
}

// OpenType script tags to try, most preferred first.
void scriptTags(Script script, std::vector<std::uint32_t> &out) {
    out.clear();
    const auto add = [&](const char (&t)[5]) { out.push_back(tag(t)); };
    // Indic scripts: the Universal Shaping Engine's tag, then the second
    // Indic spec's, then the first's.
    switch (script) {
    case Script::Bengali: add("bng3"); add("bng2"); break;
    case Script::Devanagari: add("dev3"); add("dev2"); break;
    case Script::Gujarati: add("gjr3"); add("gjr2"); break;
    case Script::Gurmukhi: add("gur3"); add("gur2"); break;
    case Script::Kannada: add("knd3"); add("knd2"); break;
    case Script::Malayalam: add("mlm3"); add("mlm2"); break;
    case Script::Oriya: add("ory3"); add("ory2"); break;
    case Script::Tamil: add("tml3"); add("tml2"); break;
    case Script::Telugu: add("tel3"); add("tel2"); break;
    case Script::Myanmar: add("mym2"); break;
    default: break;
    }
    switch (script) {
    case Script::Hiragana:
    case Script::Katakana:
    case Script::KatakanaOrHiragana: add("kana"); return;
    case Script::Lao: add("lao "); return;
    case Script::Yi: add("yi  "); return;
    case Script::Nko: add("nko "); return;
    case Script::Vai: add("vai "); return;
    default: break;
    }
    const char *code = unicode::iso15924(script);
    out.push_back(static_cast<std::uint32_t>(code[0] | 0x20) << 24 | static_cast<std::uint32_t>(code[1]) << 16 |
                  static_cast<std::uint32_t>(code[2]) << 8 | static_cast<std::uint32_t>(code[3]));
}

// The Arabic joining state machine (HarfBuzz's): columns are joining types
// U, L, R, D (C counts as D) and the Syriac groups Alaph and Dalath-Rish;
// each entry is {previous action, current action, next state}.
struct JoinEntry {
    std::uint8_t prevAction;
    std::uint8_t currAction;
    std::uint8_t next;
};
constexpr JoinEntry kJoining[7][6] = {
    // State 0: prev was U, not willing to join.
    {{None, None, 0}, {None, Isol, 2}, {None, Isol, 1}, {None, Isol, 2}, {None, Isol, 1}, {None, Isol, 6}},
    // State 1: prev was R or ISOL/ALAPH, not willing to join.
    {{None, None, 0}, {None, Isol, 2}, {None, Isol, 1}, {None, Isol, 2}, {None, Fin2, 5}, {None, Isol, 6}},
    // State 2: prev was D/L in ISOL form, willing to join.
    {{None, None, 0}, {None, Isol, 2}, {Init, Fina, 1}, {Init, Fina, 3}, {Init, Fina, 4}, {Init, Fina, 6}},
    // State 3: prev was D in FINA form, willing to join.
    {{None, None, 0}, {None, Isol, 2}, {Medi, Fina, 1}, {Medi, Fina, 3}, {Medi, Fina, 4}, {Medi, Fina, 6}},
    // State 4: prev was FINA ALAPH, not willing to join.
    {{None, None, 0}, {None, Isol, 2}, {Med2, Isol, 1}, {Med2, Isol, 2}, {Med2, Fin2, 5}, {Med2, Isol, 6}},
    // State 5: prev was FIN2/FIN3 ALAPH, not willing to join.
    {{None, None, 0}, {None, Isol, 2}, {Isol, Isol, 1}, {Isol, Isol, 2}, {Isol, Fin2, 5}, {Isol, Isol, 6}},
    // State 6: prev was DALATH/RISH, not willing to join.
    {{None, None, 0}, {None, Isol, 2}, {None, Isol, 1}, {None, Isol, 2}, {None, Fin3, 5}, {None, Isol, 6}},
};

// The joining column of a character, or -1 for transparent ones.
int joiningColumn(char32_t u) {
    if (u == 0x0710) {
        return 4; // Alaph
    }
    if (u == 0x0715 || u == 0x0716 || u == 0x072A || u == 0x072F) {
        return 5; // Dalath-Rish
    }
    switch (unicode::joiningType(u)) {
    case unicode::JoiningType::T: return -1;
    case unicode::JoiningType::L: return 1;
    case unicode::JoiningType::R: return 2;
    case unicode::JoiningType::D:
    case unicode::JoiningType::C: return 3;
    default: return 0;
    }
}

// The scripts HarfBuzz shapes with the Universal Shaping Engine (given a
// font with the script's own lookups).
bool isUseScript(Script s) {
    switch (s) {
    case Script::Tibetan: case Script::Mongolian: case Script::Sinhala: case Script::Buhid: case Script::Hanunoo:
    case Script::Tagalog: case Script::Tagbanwa: case Script::Limbu: case Script::TaiLe: case Script::Buginese:
    case Script::Kharoshthi: case Script::SylotiNagri: case Script::Tifinagh: case Script::Balinese: case Script::Nko:
    case Script::PhagsPa: case Script::Cham: case Script::KayahLi: case Script::Lepcha: case Script::Rejang:
    case Script::Saurashtra: case Script::Sundanese: case Script::EgyptianHieroglyphs: case Script::Javanese:
    case Script::Kaithi: case Script::MeeteiMayek: case Script::TaiTham: case Script::TaiViet: case Script::Batak:
    case Script::Brahmi: case Script::Mandaic: case Script::Chakma: case Script::Miao: case Script::Sharada:
    case Script::Takri: case Script::Duployan: case Script::Grantha: case Script::Khojki: case Script::Khudawadi:
    case Script::Mahajani: case Script::Manichaean: case Script::Modi: case Script::PahawhHmong:
    case Script::PsalterPahlavi: case Script::Siddham: case Script::Tirhuta: case Script::Ahom: case Script::Multani:
    case Script::Adlam: case Script::Bhaiksuki: case Script::Marchen: case Script::Newa: case Script::MasaramGondi:
    case Script::Soyombo: case Script::ZanabazarSquare: case Script::Dogra: case Script::GunjalaGondi:
    case Script::HanifiRohingya: case Script::Makasar: case Script::Medefaidrin: case Script::OldSogdian:
    case Script::Sogdian: case Script::Elymaic: case Script::Nandinagari: case Script::NyiakengPuachueHmong:
    case Script::Wancho: case Script::Chorasmian: case Script::DivesAkuru: case Script::KhitanSmallScript:
    case Script::Yezidi: case Script::CyproMinoan: case Script::OldUyghur: case Script::Tangsa: case Script::Toto:
    case Script::Vithkuqi: case Script::Kawi: case Script::NagMundari: case Script::Garay: case Script::GurungKhema:
    case Script::KiratRai: case Script::OlOnal: case Script::Sunuwar: case Script::Todhri: case Script::TuluTigalari:
    case Script::BeriaErfe: case Script::Sidetic: case Script::TaiYo: case Script::TolongSiki: case Script::Jurchen:
    case Script::ProtoCuneiform: case Script::Seal:
        return true;
    default: return false;
    }
}

bool isIndicScript(Script s) {
    switch (s) {
    case Script::Bengali: case Script::Devanagari: case Script::Gujarati: case Script::Gurmukhi: case Script::Kannada:
    case Script::Malayalam: case Script::Oriya: case Script::Tamil: case Script::Telugu:
        return true;
    default: return false;
    }
}

// Scripts that join like Arabic (in the Arabic engine or the USE).
bool hasArabicJoining(Script s) {
    switch (s) {
    case Script::Adlam: case Script::Arabic: case Script::Chorasmian: case Script::HanifiRohingya: case Script::Mandaic:
    case Script::Manichaean: case Script::Mongolian: case Script::Nko: case Script::OldUyghur: case Script::PhagsPa:
    case Script::PsalterPahlavi: case Script::Sogdian: case Script::Syriac:
        return true;
    default: return false;
    }
}

void zeroMarkWidths(ot::Buffer &b, bool adjust) {
    for (std::size_t i = 0; i < b.len(); ++i) {
        if (ot::isMark(b.info[i])) {
            if (adjust) {
                b.pos[i].xOffset -= b.pos[i].xAdvance;
                b.pos[i].yOffset -= b.pos[i].yAdvance;
            }
            b.pos[i].xAdvance = 0;
            b.pos[i].yAdvance = 0;
        }
    }
}

// Scripts whose native direction HarfBuzz leaves open (it never reverses them).
bool hasNativeDirection(Script s) {
    return s != Script::OldHungarian && s != Script::OldItalic && s != Script::Runic && s != Script::Tifinagh;
}

// Hebrew presentation forms, composed when the font cannot position marks.
bool composeHebrew(char32_t a, char32_t b, char32_t &ab) {
    static constexpr char32_t kDagesh[0x05EA - 0x05D0 + 1] = {
        0xFB30, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB36, 0, 0xFB38, 0xFB39, 0xFB3A, 0xFB3B, 0xFB3C, 0,
        0xFB3E, 0,      0xFB40, 0xFB41, 0,      0xFB43, 0xFB44, 0, 0xFB46, 0xFB47, 0xFB48, 0xFB49, 0xFB4A};
    switch (b) {
    case 0x05B4: if (a == 0x05D9) { ab = 0xFB1D; return true; } break;
    case 0x05B7:
        if (a == 0x05F2) { ab = 0xFB1F; return true; }
        if (a == 0x05D0) { ab = 0xFB2E; return true; }
        break;
    case 0x05B8: if (a == 0x05D0) { ab = 0xFB2F; return true; } break;
    case 0x05B9: if (a == 0x05D5) { ab = 0xFB4B; return true; } break;
    case 0x05BC:
        if (a >= 0x05D0 && a <= 0x05EA) {
            ab = kDagesh[a - 0x05D0];
            return ab != 0;
        }
        if (a == 0xFB2A) { ab = 0xFB2C; return true; }
        if (a == 0xFB2B) { ab = 0xFB2D; return true; }
        break;
    case 0x05BF:
        if (a == 0x05D1) { ab = 0xFB4C; return true; }
        if (a == 0x05DB) { ab = 0xFB4D; return true; }
        if (a == 0x05E4) { ab = 0xFB4E; return true; }
        break;
    case 0x05C1:
        if (a == 0x05E9) { ab = 0xFB2A; return true; }
        if (a == 0xFB49) { ab = 0xFB2C; return true; }
        break;
    case 0x05C2:
        if (a == 0x05E9) { ab = 0xFB2B; return true; }
        if (a == 0xFB49) { ab = 0xFB2D; return true; }
        break;
    default: break;
    }
    return false;
}

// Modifier combining marks (Arabic): they go first among marks of their class.
bool isArabicModifierMark(char32_t u) {
    switch (u) {
    case 0x0654: case 0x0655: case 0x0658: case 0x06DC: case 0x06E3: case 0x06E7: case 0x06E8: case 0x08CA:
    case 0x08CB: case 0x08CD: case 0x08CE: case 0x08CF: case 0x08D3: case 0x08F3: return true;
    default: return false;
    }
}

} // namespace

// ---- Arabic fallback shaping ----

// Builds a GSUB table (no scripts or features, just lookups) from Arabic
// presentation forms the font maps, as HarfBuzz synthesises it for fonts
// without Arabic GSUB: initial, medial, final and isolated forms, then
// three-letter, two-letter and mark ligatures. `masks` gives each of the
// seven lookups' feature mask (0: the feature is off); lookups the font has
// no glyphs for are left out.
void synthesiseArabicFallback(const FontFace &face, const std::uint32_t (&masks)[7], std::vector<std::byte> &table,
                              std::vector<ot::PlannedLookup> &lookups) {
    namespace af = arabic_fallback;
    std::vector<std::vector<std::uint16_t>> built; // each lookup as 16-bit words
    const auto glyph = [&](char32_t u) { return face.glyphIndex(u); };
    const auto coverage = [](std::vector<std::uint16_t> &w, const std::vector<GlyphId> &glyphs) {
        w.push_back(1);
        w.push_back(static_cast<std::uint16_t>(glyphs.size()));
        w.insert(w.end(), glyphs.begin(), glyphs.end());
    };
    const auto single = [&](unsigned form) {
        std::vector<std::pair<GlyphId, GlyphId>> pairs;
        for (char32_t u = af::kFirst; u <= af::kLast; ++u) {
            const char32_t shaped = af::kForms[u - af::kFirst][form];
            const GlyphId ug = glyph(u);
            const GlyphId sg = shaped ? glyph(shaped) : 0;
            if (shaped && ug && sg && ug != sg) {
                pairs.push_back({ug, sg});
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
        std::vector<std::uint16_t> w;
        if (!pairs.empty()) {
            // Lookup: type 1, IgnoreMarks, one subtable (format 2) at word 4.
            w = {1, 0x08, 1, 8, 2, static_cast<std::uint16_t>(6 + 2 * pairs.size()), static_cast<std::uint16_t>(pairs.size())};
            for (const auto &pr : pairs) {
                w.push_back(pr.second);
            }
            std::vector<GlyphId> cov;
            for (const auto &pr : pairs) {
                cov.push_back(pr.first);
            }
            coverage(w, cov);
        }
        return w;
    };
    struct Lig {
        GlyphId first;
        std::vector<std::vector<GlyphId>> ligs; // ligature glyph, then the other components
    };
    const auto ligature = [&](auto entries, std::uint16_t flags, auto components) {
        std::vector<Lig> sets;
        for (std::size_t i = 0; i < entries.size();) {
            std::size_t j = i;
            while (j < entries.size() && entries[j].first == entries[i].first) {
                ++j;
            }
            if (const GlyphId first = glyph(entries[i].first)) {
                Lig set{first, {}};
                for (std::size_t k = i; k < j; ++k) {
                    const GlyphId lig = glyph(entries[k].ligature);
                    std::vector<GlyphId> item{lig};
                    bool ok = lig != 0;
                    for (const char32_t c : components(entries[k])) {
                        const GlyphId g = glyph(c);
                        ok = ok && g != 0;
                        item.push_back(g);
                    }
                    if (ok) {
                        set.ligs.push_back(std::move(item));
                    }
                }
                sets.push_back(std::move(set));
            }
            i = j;
        }
        std::stable_sort(sets.begin(), sets.end(), [](const Lig &x, const Lig &y) { return x.first < y.first; });
        std::vector<std::uint16_t> w;
        std::size_t total = 0;
        for (const Lig &set : sets) {
            total += set.ligs.size();
        }
        if (total == 0) {
            return w;
        }
        // Lookup header (4 words), subtable: format, coverage, set count, set offsets.
        w = {4, flags, 1, 8, 1, 0, static_cast<std::uint16_t>(sets.size())};
        const std::size_t subtable = 4;
        const std::size_t setOffsets = w.size();
        w.resize(w.size() + sets.size());
        for (std::size_t si = 0; si < sets.size(); ++si) {
            const std::size_t set = w.size();
            w[setOffsets + si] = static_cast<std::uint16_t>((set - subtable) * 2);
            w.push_back(static_cast<std::uint16_t>(sets[si].ligs.size()));
            const std::size_t ligOffsets = w.size();
            w.resize(w.size() + sets[si].ligs.size());
            for (std::size_t li = 0; li < sets[si].ligs.size(); ++li) {
                const std::vector<GlyphId> &item = sets[si].ligs[li];
                w[ligOffsets + li] = static_cast<std::uint16_t>((w.size() - set) * 2);
                w.push_back(item[0]);
                w.push_back(static_cast<std::uint16_t>(item.size()));
                w.insert(w.end(), item.begin() + 1, item.end());
            }
        }
        w[subtable + 1] = static_cast<std::uint16_t>((w.size() - subtable) * 2);
        std::vector<GlyphId> cov;
        for (const Lig &set : sets) {
            cov.push_back(set.first);
        }
        coverage(w, cov);
        return w;
    };
    lookups.clear();
    for (unsigned i = 0; i < 7; ++i) {
        if (masks[i] == 0) {
            continue;
        }
        std::vector<std::uint16_t> w;
        if (i < 4) {
            w = single(i);
        } else if (i == 4) {
            w = ligature(Span<const af::Ligature3>(af::kLigatures3), std::uint16_t{0x08},
                         [](const af::Ligature3 &l) { return std::array<char32_t, 2>{l.second, l.third}; });
        } else {
            const bool marks = i == 6;
            w = ligature(Span<const af::Ligature2>(marks ? Span<const af::Ligature2>(af::kMarkLigatures)
                                                         : Span<const af::Ligature2>(af::kLigatures)),
                         static_cast<std::uint16_t>(marks ? 0 : 0x08),
                         [](const af::Ligature2 &l) { return std::array<char32_t, 1>{l.second}; });
        }
        if (!w.empty()) {
            lookups.push_back({static_cast<std::uint16_t>(built.size()), masks[i], true, true, false});
            built.push_back(std::move(w));
        }
    }
    // The table: header, empty script and feature lists, the lookup list.
    std::vector<std::uint16_t> t = {1, 0, 10, 12, 14, 0, 0, static_cast<std::uint16_t>(built.size())};
    const std::size_t listStart = 7;
    const std::size_t offsets = t.size();
    t.resize(t.size() + built.size());
    for (std::size_t i = 0; i < built.size(); ++i) {
        t[offsets + i] = static_cast<std::uint16_t>((t.size() - listStart) * 2);
        t.insert(t.end(), built[i].begin(), built[i].end());
    }
    table.resize(t.size() * 2);
    for (std::size_t i = 0; i < t.size(); ++i) {
        table[i * 2] = static_cast<std::byte>(t[i] >> 8);
        table[i * 2 + 1] = static_cast<std::byte>(t[i] & 0xFF);
    }
}

// ---- Plans ----

struct Shaper::Plan {
    std::uint64_t faceId = 0;
    Script script = Script::Common;
    TextDirection direction = TextDirection::LeftToRight;
    std::uint32_t language = 0;
    std::vector<FontFeature> features;

    Engine engine = Engine::Default;
    ot::Gdef gdef;
    ot::LayoutTable gsub;
    ot::LayoutTable gpos;
    std::vector<std::vector<ot::PlannedLookup>> gsubStages;
    std::vector<ot::PlannedLookup> gposLookups;
    std::uint32_t globalMask = kGlobalBit;
    std::uint32_t arabicMasks[8] = {};
    std::uint32_t rtlmMask = 0;
    std::uint32_t kernMask = 0;
    struct RangedFeature {
        std::uint32_t mask;
        std::uint32_t shift;
        std::uint32_t value;
        std::uint32_t start;
        std::uint32_t end;
    };
    std::vector<RangedFeature> ranged;
    bool applyGpos = false;
    bool applyKern = false;
    bool hasGposMark = false;
    bool fallbackGlyphClasses = false;
    bool adjustMarkOffsets = false;
    bool hasGposKern = false;
    bool fallbackMarkPositioning = false;
    bool thaiPua = false;
    bool zeroMarks = true;
    bool composeCharacters = true;
    std::uint32_t hangulMasks[4] = {};
    // The syllabic engines: what runs after each GSUB stage, and their masks.
    std::vector<std::uint8_t> pauses;
    std::uint32_t rphfMask = 0;
    std::uint32_t topographicalMasks[4] = {}; // isol, init, medi, fina
    bool arabicJoining = false;
    bool zeroMarksEarly = false;
    bool shortCircuit = true; // normalisation may keep precomposed characters the font has
    std::uint32_t caltMask = 0;
    // Arabic fallback shaping: lookups made from presentation forms, run
    // after GSUB stage fallbackStage.
    std::vector<std::byte> fallbackGsub;
    ot::LayoutTable fallbackTable;
    std::vector<ot::PlannedLookup> fallbackLookups;
    unsigned fallbackStage = 0;
    std::uint32_t fracMask = 0;
    std::uint32_t numrMask = 0;
    std::uint32_t dnomMask = 0;
    syllabic::IndicPlan indic;
    syllabic::KhmerPlan khmer;
};

struct Shaper::Scratch {
    ot::Buffer buffer;
    std::vector<std::uint32_t> tags;
    std::vector<std::uint16_t> lookups;
};

Shaper::Shaper() : m_scratch(std::make_unique<Scratch>()) {}
Shaper::~Shaper() = default;

TextDirection Shaper::scriptDirection(Script s) noexcept {
    switch (s) {
    case Script::Arabic: case Script::Hebrew: case Script::Syriac: case Script::Thaana: case Script::Nko:
    case Script::Samaritan: case Script::Mandaic: case Script::ImperialAramaic: case Script::Phoenician:
    case Script::Lydian: case Script::Cypriot: case Script::Kharoshthi: case Script::OldSouthArabian:
    case Script::Avestan: case Script::InscriptionalPahlavi: case Script::InscriptionalParthian:
    case Script::PsalterPahlavi: case Script::OldTurkic: case Script::OldNorthArabian: case Script::Nabataean:
    case Script::Palmyrene: case Script::Manichaean: case Script::MendeKikakui: case Script::Hatran:
    case Script::MeroiticCursive: case Script::MeroiticHieroglyphs: case Script::Adlam: case Script::HanifiRohingya: case Script::OldSogdian:
    case Script::Sogdian: case Script::Elymaic: case Script::Chorasmian: case Script::Yezidi:
    case Script::OldUyghur: case Script::Garay:
        return TextDirection::RightToLeft;
    default: return TextDirection::LeftToRight;
    }
}

const Shaper::Plan &Shaper::plan(const FontFace &face, Script script, TextDirection direction, std::uint32_t language,
                                 Span<const FontFeature> features) {
    for (const std::unique_ptr<Plan> &p : m_plans) {
        if (p->faceId == face.uniqueId() && p->script == script && p->direction == direction && p->language == language &&
            p->features.size() == features.size() &&
            std::equal(features.begin(), features.end(), p->features.begin(), [](const FontFeature &a, const FontFeature &b) {
                return a.tag == b.tag && a.value == b.value && a.start == b.start && a.end == b.end;
            })) {
            return *p;
        }
    }
    auto p = std::make_unique<Plan>();
    p->faceId = face.uniqueId();
    p->script = script;
    p->direction = direction;
    p->language = language;
    p->features.assign(features.begin(), features.end());
    p->gdef.init(face);
    p->gsub.init(face, ot::TableKind::Gsub);
    p->gpos.init(face, ot::TableKind::Gpos);
    scriptTags(script, m_scratch->tags);
    p->gsub.selectScript(m_scratch->tags, language);
    p->gpos.selectScript(m_scratch->tags, language);

    // The engine, as HarfBuzz categorises scripts. Syriac joins only with a
    // font made for it; Arabic always (it has fallback forms).
    switch (script) {
    case Script::Arabic: p->engine = Engine::Arabic; break;
    case Script::Syriac:
        if (p->gsub.chosenScript() != tag("DFLT")) {
            p->engine = Engine::Arabic;
        }
        break;
    case Script::Hebrew: p->engine = Engine::Hebrew; break;
    case Script::Thai:
    case Script::Lao: p->engine = Engine::Thai; break;
    case Script::Hangul: p->engine = Engine::Hangul; break;
    default: break;
    }
    const std::uint32_t gsubScript = p->gsub.chosenScript();
    const bool genericTag = gsubScript == tag("DFLT") || gsubScript == tag("latn");
    if ((isUseScript(script) || (isIndicScript(script) && (gsubScript & 0xFF) == '3')) && !genericTag) {
        p->engine = Engine::Use;
    } else if (isIndicScript(script) && !genericTag) {
        p->engine = Engine::Indic;
    } else if (script == Script::Khmer) {
        p->engine = Engine::Khmer;
    } else if (script == Script::Myanmar && !genericTag && gsubScript != tag("mymr")) {
        p->engine = Engine::Myanmar; // fonts for the old 'mymr' tag get the default engine
    }

    // The features, in HarfBuzz's order and GSUB stages (a stage ends at
    // each pause; lookups within a stage run in lookup-list order).
    enum : std::uint8_t { Global = 1, ManualZwj = 2, ManualZwnj = 4, HasFallback = 8, Random = 16, PerSyllable = 32 };
    struct Feature {
        std::uint32_t tag;
        std::uint8_t flags;
        std::uint32_t maxValue;
        std::uint32_t defaultValue;
        unsigned stage;
    };
    std::vector<Feature> list;
    unsigned stage = 0;
    std::vector<std::pair<unsigned, std::uint8_t>> pauses;
    const auto add = [&](std::uint32_t t, std::uint8_t flags, std::uint32_t value = 1) {
        list.push_back({t, flags, value, (flags & Global) ? value : 0u, stage});
    };
    const auto pause = [&](std::uint8_t action) {
        pauses.emplace_back(stage, action);
        ++stage;
    };
    add(tag("rvrn"), Global);
    ++stage;
    if (direction == TextDirection::LeftToRight) {
        add(tag("ltra"), Global);
        add(tag("ltrm"), Global);
    } else {
        add(tag("rtla"), Global);
        add(tag("rtlm"), 0);
    }
    add(tag("frac"), 0);
    add(tag("numr"), 0);
    add(tag("dnom"), 0);
    add(tag("rand"), Global | Random, 255);
    if (p->engine == Engine::Hangul) {
        add(tag("ljmo"), 0);
        add(tag("vjmo"), 0);
        add(tag("tjmo"), 0);
    }
    if (p->engine == Engine::Use) {
        constexpr std::uint8_t Zs = ManualZwj | PerSyllable;
        pause(PauseUseSyllables);
        add(tag("locl"), Global | PerSyllable);
        add(tag("ccmp"), Global | PerSyllable);
        add(tag("nukt"), Global | PerSyllable);
        add(tag("akhn"), Global | Zs);
        pause(PauseClearSubstitution);
        add(tag("rphf"), Zs);
        pause(PauseUseRphf);
        pause(PauseClearSubstitution);
        add(tag("pref"), Global | Zs);
        pause(PauseUsePref);
        for (const char *f : {"rkrf", "abvf", "blwf", "half", "pstf", "vatu", "cjct"}) {
            add(tag4(f), Global | Zs);
        }
        pause(PauseUseReorder);
        pause(PauseClearSyllables);
        add(tag("isol"), 0);
        add(tag("init"), 0);
        add(tag("medi"), 0);
        add(tag("fina"), 0);
        pause(PauseNone);
        for (const char *f : {"abvs", "blws", "haln", "pres", "psts"}) {
            add(tag4(f), Global | ManualZwj);
        }
    }
    constexpr std::uint8_t ManualJoiners = ManualZwj | ManualZwnj;
    if (p->engine == Engine::Indic) {
        pause(PauseIndicSyllables);
        add(tag("locl"), Global | PerSyllable);
        add(tag("ccmp"), Global | PerSyllable);
        pause(PauseIndicInitial);
        // The basic features, one stage each.
        for (const auto &[f, global] : {std::pair{"nukt", true}, std::pair{"akhn", true}, std::pair{"rphf", false},
                                        std::pair{"rkrf", true}, std::pair{"pref", false}, std::pair{"blwf", false},
                                        std::pair{"abvf", false}, std::pair{"half", false}, std::pair{"pstf", false},
                                        std::pair{"vatu", true}, std::pair{"cjct", true}}) {
            add(tag4(f), static_cast<std::uint8_t>((global ? Global : 0) | ManualJoiners | PerSyllable));
            pause(PauseNone);
        }
        pause(PauseIndicFinal);
        add(tag("init"), ManualJoiners | PerSyllable);
        for (const char *f : {"pres", "abvs", "blws", "psts", "haln"}) {
            add(tag4(f), Global | ManualJoiners | PerSyllable);
        }
    }
    if (p->engine == Engine::Khmer) {
        pause(PauseKhmerSyllables);
        pause(PauseKhmerReorder);
        add(tag("locl"), Global | PerSyllable);
        add(tag("ccmp"), Global | PerSyllable);
        for (const char *f : {"pref", "blwf", "abvf", "pstf", "cfar"}) {
            add(tag4(f), ManualJoiners | PerSyllable);
        }
        pause(PauseClearSyllables);
        for (const char *f : {"pres", "abvs", "blws", "psts"}) {
            add(tag4(f), Global | ManualJoiners);
        }
    }
    if (p->engine == Engine::Myanmar) {
        pause(PauseMyanmarSyllables);
        add(tag("locl"), Global | PerSyllable);
        add(tag("ccmp"), Global | PerSyllable);
        pause(PauseMyanmarReorder);
        for (const char *f : {"rphf", "pref", "blwf", "pstf"}) {
            add(tag4(f), Global | ManualZwj | PerSyllable);
            pause(PauseNone);
        }
        pause(PauseClearSyllables);
        for (const char *f : {"pres", "abvs", "blws", "psts"}) {
            add(tag4(f), Global | ManualZwj);
        }
    }
    if (p->engine == Engine::Arabic) {
        add(tag("stch"), Global);
        ++stage;
        add(tag("ccmp"), Global | ManualZwj);
        add(tag("locl"), Global | ManualZwj);
        ++stage;
        for (std::size_t k = 0; k < 7; ++k) {
            const bool syriacOnly = k == Fin2 || k == Fin3 || k == Med2;
            add(kArabicFeatures[k], ManualZwj | (script == Script::Arabic && !syriacOnly ? HasFallback : 0));
            ++stage;
        }
        ++stage;
        p->fallbackStage = stage;
        add(tag("rlig"), Global | ManualZwj | HasFallback);
        if (script == Script::Arabic) {
            ++stage;
        }
        add(tag("calt"), Global | ManualZwj);
        std::vector<std::uint16_t> &unused = m_scratch->lookups;
        if (!p->gsub.featureLookups(tag("rclt"), unused) && !p->gpos.featureLookups(tag("rclt"), unused)) {
            ++stage;
        }
        add(tag("liga"), Global | ManualZwj);
        add(tag("clig"), Global | ManualZwj);
        add(tag("mset"), Global | ManualZwj);
    }
    add(tag("abvm"), Global);
    add(tag("blwm"), Global);
    add(tag("ccmp"), Global);
    add(tag("locl"), Global);
    add(tag("mark"), Global | ManualZwj | ManualZwnj);
    add(tag("mkmk"), Global | ManualZwj | ManualZwnj);
    add(tag("rlig"), Global);
    add(tag("calt"), Global);
    add(tag("clig"), Global);
    add(tag("curs"), Global);
    add(tag("dist"), Global);
    add(tag("kern"), Global | HasFallback);
    add(tag("liga"), Global);
    add(tag("rclt"), Global);
    for (const FontFeature &f : features) {
        add(f.tag, (f.start == 0 && f.end == 0xFFFFFFFFu) ? Global : 0, f.value);
    }
    if (p->engine == Engine::Hangul) {
        add(tag("calt"), 0); // its own mask bit, so jamo can be kept out of it
    }
    // The engines' overrides: Indic and Khmer turn liga off (disabling is
    // adding it again, global with value 0); the Khmer spec requires clig.
    if (p->engine == Engine::Khmer) {
        add(tag("clig"), Global);
    }
    if (p->engine == Engine::Indic || p->engine == Engine::Khmer) {
        add(tag("liga"), Global, 0);
    }
    if (p->engine == Engine::Indic) {
        pause(PauseClearSyllables);
    }
    const unsigned stages = stage + 1;
    p->pauses.assign(stages, PauseNone);
    for (const auto &[at, action] : pauses) {
        p->pauses[at] = action;
    }

    // Merge duplicates: the first keeps its flags (but global-ness and the
    // fallback flag merge in) and the earliest stage.
    std::stable_sort(list.begin(), list.end(), [](const Feature &x, const Feature &y) { return x.tag < y.tag; });
    std::vector<Feature> merged;
    for (const Feature &f : list) {
        if (merged.empty() || merged.back().tag != f.tag) {
            merged.push_back(f);
            continue;
        }
        Feature &m = merged.back();
        if (f.flags & Global) {
            m.flags |= Global;
            m.maxValue = f.maxValue;
            m.defaultValue = f.defaultValue;
        } else {
            m.flags &= static_cast<std::uint8_t>(~Global);
            m.maxValue = std::max(m.maxValue, f.maxValue);
        }
        m.flags |= f.flags & HasFallback;
        m.stage = std::min(m.stage, f.stage);
    }

    // Mask bits (bit 31 is shared by the on/off global features) and lookups.
    struct Mapped {
        std::uint32_t tag;
        std::uint32_t mask;
        unsigned shift;
    };
    std::vector<Mapped> mapped;
    std::vector<std::pair<std::uint32_t, unsigned>> mappedStages; // tag, GSUB stage
    const std::uint32_t requiredGsub = p->gsub.requiredFeatureTag();
    const std::uint32_t requiredGpos = p->gpos.requiredFeatureTag();
    unsigned requiredGsubStage = 0;
    p->gsubStages.assign(stages, {});
    std::vector<ot::PlannedLookup> gpos;
    std::vector<std::uint16_t> gsubLookups;
    std::vector<std::uint16_t> &gposLookups = m_scratch->lookups;
    unsigned nextBit = 4;
    bool arabicFound[7] = {};
    bool arabicMapped[7] = {};
    std::uint32_t rligMask = 0;
    for (const Feature &f : merged) {
        const bool usesGlobalBit = (f.flags & Global) && f.maxValue == 1;
        const unsigned bits = usesGlobalBit ? 0 : std::min(8u, static_cast<unsigned>(std::bit_width(f.maxValue)));
        if (f.maxValue == 0 || nextBit + bits >= 31) {
            continue;
        }
        if (f.tag == requiredGsub) {
            requiredGsubStage = f.stage;
        }
        const bool inGsub = p->gsub.featureLookups(f.tag, gsubLookups);
        const bool inGpos = p->gpos.featureLookups(f.tag, gposLookups);
        if (!inGsub && !inGpos && !(f.flags & HasFallback)) {
            continue;
        }
        std::uint32_t mask = kGlobalBit;
        unsigned shift = 31;
        if (!usesGlobalBit) {
            shift = nextBit;
            mask = (1u << (nextBit + bits)) - (1u << nextBit);
            nextBit += bits;
            p->globalMask |= (f.defaultValue << shift) & mask;
        }
        mapped.push_back({f.tag, mask, shift});
        mappedStages.emplace_back(f.tag, f.stage);
        const std::uint32_t oneMask = (1u << shift) & mask;
        const bool autoZwj = !(f.flags & ManualZwj);
        const bool autoZwnj = !(f.flags & ManualZwnj);
        const bool random = (f.flags & Random) != 0;
        const bool perSyllable = (f.flags & PerSyllable) != 0;
        for (const std::uint16_t l : gsubLookups) {
            p->gsubStages[f.stage].push_back({l, mask, autoZwnj, autoZwj, random, perSyllable});
        }
        for (const std::uint16_t l : gposLookups) {
            gpos.push_back({l, mask, autoZwnj, autoZwj, random});
        }
        for (std::size_t k = 0; k < 7; ++k) {
            if (f.tag == kArabicFeatures[k]) {
                p->arabicMasks[k] = oneMask;
                arabicMapped[k] = true;
                arabicFound[k] = inGsub || inGpos;
            }
        }
        if (f.tag == tag("rlig")) {
            rligMask = oneMask;
        } else if (f.tag == tag("rphf")) {
            p->rphfMask = oneMask;
        }
        if (f.tag == tag("ljmo")) {
            p->hangulMasks[1] = oneMask;
        } else if (f.tag == tag("vjmo")) {
            p->hangulMasks[2] = oneMask;
        } else if (f.tag == tag("tjmo")) {
            p->hangulMasks[3] = oneMask;
        } else if (f.tag == tag("calt")) {
            p->caltMask = oneMask;
        }
        if (f.tag == tag("rtlm")) {
            p->rtlmMask = oneMask;
        } else if (f.tag == tag("kern")) {
            p->kernMask = mask;
            p->hasGposKern = inGpos;
        } else if (f.tag == tag("mark")) {
            p->hasGposMark = true;
        } else if (f.tag == tag("frac")) {
            p->fracMask = oneMask;
        } else if (f.tag == tag("numr")) {
            p->numrMask = oneMask;
        } else if (f.tag == tag("dnom")) {
            p->dnomMask = oneMask;
        }
    }
    // The required features, at the stage of their tag (else the first).
    if (requiredGsub != 0) {
        p->gsub.requiredFeatureLookups(gsubLookups);
        for (const std::uint16_t l : gsubLookups) {
            p->gsubStages[requiredGsubStage].push_back({l, kGlobalBit, true, true, false});
        }
    }
    if (requiredGpos != 0) {
        p->gpos.requiredFeatureLookups(gposLookups);
        for (const std::uint16_t l : gposLookups) {
            gpos.push_back({l, kGlobalBit, true, true, false});
        }
    }
    const auto sortMerge = [](std::vector<ot::PlannedLookup> &v) {
        std::stable_sort(v.begin(), v.end(), [](const ot::PlannedLookup &x, const ot::PlannedLookup &y) { return x.index < y.index; });
        std::size_t w = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (w > 0 && v[w - 1].index == v[i].index) {
                v[w - 1].mask |= v[i].mask;
                v[w - 1].autoZwj = v[w - 1].autoZwj && v[i].autoZwj;
                v[w - 1].autoZwnj = v[w - 1].autoZwnj && v[i].autoZwnj;
            } else {
                v[w++] = v[i];
            }
        }
        v.resize(w);
    };
    for (std::vector<ot::PlannedLookup> &st : p->gsubStages) {
        sortMerge(st);
    }
    sortMerge(gpos);
    p->gposLookups = std::move(gpos);
    // Features on part of the text, in the order given.
    for (const FontFeature &f : features) {
        if (f.start == 0 && f.end == 0xFFFFFFFFu) {
            continue;
        }
        for (const Mapped &m : mapped) {
            if (m.tag == f.tag) {
                p->ranged.push_back({m.mask, m.shift, f.value, f.start, f.end});
            }
        }
    }

    // The syllabic engines' masks (0 for global features, as HarfBuzz's
    // engines keep them) and the lookups of each feature's stage.
    const auto oneMask = [&](const char *t) {
        for (const Mapped &m : mapped) {
            if (m.tag == tag4(t)) {
                return (1u << m.shift) & m.mask;
            }
        }
        return 0u;
    };
    const auto stageLookups = [&](const char *t, std::vector<std::uint16_t> &out) {
        out.clear();
        for (const auto &[ft, st] : mappedStages) {
            if (ft == tag4(t) && st < p->gsubStages.size()) {
                for (const ot::PlannedLookup &l : p->gsubStages[st]) {
                    out.push_back(l.index);
                }
            }
        }
    };
    if (p->engine == Engine::Indic) {
        syllabic::IndicPlan &ip = p->indic;
        ip.configure(script, (gsubScript & 0xFF) != '2');
        ip.gsub = &p->gsub;
        ip.rphfMask = oneMask("rphf");
        ip.prefMask = oneMask("pref");
        ip.blwfMask = oneMask("blwf");
        ip.abvfMask = oneMask("abvf");
        ip.halfMask = oneMask("half");
        ip.pstfMask = oneMask("pstf");
        ip.initMask = oneMask("init");
        stageLookups("rphf", ip.rphf);
        stageLookups("pref", ip.pref);
        stageLookups("blwf", ip.blwf);
        stageLookups("pstf", ip.pstf);
        stageLookups("vatu", ip.vatu);
        const char32_t virama = [&]() -> char32_t {
            switch (script) {
            case Script::Devanagari: return 0x094D;
            case Script::Bengali: return 0x09CD;
            case Script::Gurmukhi: return 0x0A4D;
            case Script::Gujarati: return 0x0ACD;
            case Script::Oriya: return 0x0B4D;
            case Script::Tamil: return 0x0BCD;
            case Script::Telugu: return 0x0C4D;
            case Script::Kannada: return 0x0CCD;
            case Script::Malayalam: return 0x0D4D;
            default: return 0;
            }
        }();
        ip.viramaGlyph = virama ? face.glyphIndex(virama) : 0;
    }
    if (p->engine == Engine::Khmer) {
        p->khmer.prefMask = oneMask("pref");
        p->khmer.blwfMask = oneMask("blwf");
        p->khmer.abvfMask = oneMask("abvf");
        p->khmer.pstfMask = oneMask("pstf");
        p->khmer.cfarMask = oneMask("cfar");
    }

    // As HarfBuzz decides: GPOS if the font has it (the Hebrew engine wants
    // Hebrew in it); the kern table unless GPOS kerns this script (not for
    // Thai and Lao); without GPOS, marks are placed by their bounding boxes
    // (except in Thai and Lao).
    const bool fallbackPosition = p->engine != Engine::Thai && p->engine != Engine::Use && p->engine != Engine::Indic &&
                                  p->engine != Engine::Khmer && p->engine != Engine::Myanmar;
    const bool disableGpos = p->engine == Engine::Hebrew && p->gpos.chosenScript() != tag("hebr");
    p->applyGpos = !disableGpos && p->gpos.present();
    p->applyKern = (!p->hasGposKern || !p->applyGpos) && ot::hasKernTable(face) && fallbackPosition;
    p->fallbackGlyphClasses = !p->gdef.hasGlyphClasses();
    p->adjustMarkOffsets = !p->applyGpos && (!p->applyKern || !ot::hasCrossStreamKerning(face));
    p->fallbackMarkPositioning = p->adjustMarkOffsets && fallbackPosition;
    p->zeroMarks = p->engine != Engine::Hangul && p->engine != Engine::Indic && p->engine != Engine::Khmer &&
                   (!p->applyKern || !ot::hasMachineKerning(face));
    p->composeCharacters = p->engine != Engine::Hangul; // HarfBuzz's normalisation mode "none" for Hangul
    p->zeroMarksEarly = p->engine == Engine::Use || p->engine == Engine::Myanmar;
    p->shortCircuit = p->engine != Engine::Use && p->engine != Engine::Indic && p->engine != Engine::Khmer &&
                      p->engine != Engine::Myanmar;
    p->arabicJoining = p->engine == Engine::Use && hasArabicJoining(script);
    if (p->engine == Engine::Use && !p->arabicJoining) {
        for (std::size_t k = 0; k < 4; ++k) {
            const std::size_t form = k == 0 ? Isol : k == 1 ? Init : k == 2 ? Medi : Fina;
            p->topographicalMasks[k] = p->arabicMasks[form];
        }
    }
    p->thaiPua = script == Script::Thai && !p->gsub.foundScript();
    // Arabic fonts without the joining forms: forms and ligatures from the
    // presentation forms the font maps.
    bool arabicFallback = script == Script::Arabic;
    for (const std::size_t k : {std::size_t{Isol}, std::size_t{Fina}, std::size_t{Medi}, std::size_t{Init}}) {
        arabicFallback = arabicFallback && arabicMapped[k] && !arabicFound[k];
    }
    if (arabicFallback) {
        const std::uint32_t masks[7] = {p->arabicMasks[Init], p->arabicMasks[Medi], p->arabicMasks[Fina], p->arabicMasks[Isol],
                                        rligMask, rligMask, rligMask};
        synthesiseArabicFallback(face, masks, p->fallbackGsub, p->fallbackLookups);
        p->fallbackTable.init(Span<const std::byte>(p->fallbackGsub.data(), p->fallbackGsub.size()), ot::TableKind::Gsub);
    }

    // Plans of faces since destroyed are never matched again; keep the cache bounded.
    if (m_plans.size() >= 64) {
        m_plans.erase(m_plans.begin());
    }
    m_plans.push_back(std::move(p));
    return *m_plans.back();
}

// ---- Shaping ----

namespace {

struct Normalizer {
    const FontFace &face;
    Engine engine;
    bool hasGposMark;
    ot::Buffer &b;
    bool recompose = true; // false: decompose only what the font lacks, never recompose
    bool shortCircuit = true; // false: decompose characters without marks too, if the font has the parts

    // A character replacing the current one (a decomposition part): its
    // own Unicode properties.
    void output(char32_t u, GlyphId glyph) {
        ot::GlyphInfo g = b.cur();
        g.codepoint = u;
        g.glyph = glyph;
        setUnicodeProps(g);
        b.out.push_back(g);
    }
    // The current character, as it is.
    void pass(GlyphId glyph) {
        b.cur().glyph = glyph;
        b.nextGlyph();
    }
    bool compose(char32_t a, char32_t c, char32_t &ab) const {
        if ((engine == Engine::Use || engine == Engine::Indic || engine == Engine::Khmer) &&
            unicode::isMark(unicode::generalCategory(a))) {
            return false; // never a mark with a mark (split matras stay split)
        }
        if (engine == Engine::Indic && a == 0x09AF && c == 0x09BC) {
            ab = 0x09DF; // a composition exclusion the Indic engine recomposes
            return true;
        }
        if (unicode::compose(a, c, ab)) {
            return true;
        }
        return engine == Engine::Hebrew && !hasGposMark && composeHebrew(a, c, ab);
    }
    // Outputs the decomposition of `ab` if the font has glyphs for it: the
    // shortest one the font covers, or (not shortest) the full one.
    // The engine's decompositions: Unicode's, less a few the Indic engine
    // keeps whole, plus Khmer's split matras (which Unicode does not split).
    bool decomposeChar(char32_t ab, char32_t &a, char32_t &c) const {
        if (engine == Engine::Indic && (ab == 0x0931 || ab == 0x09DC || ab == 0x09DD || ab == 0x0B94)) {
            return false;
        }
        if (engine == Engine::Khmer && (ab == 0x17BE || ab == 0x17BF || ab == 0x17C0 || ab == 0x17C4 || ab == 0x17C5)) {
            a = 0x17C1;
            c = ab;
            return true;
        }
        return unicode::decompose(ab, a, c);
    }
    unsigned decompose(char32_t ab, bool shortest) {
        char32_t a = 0;
        char32_t c = 0;
        if (!decomposeChar(ab, a, c)) {
            return 0;
        }
        GlyphId cGlyph = 0;
        if (c && (cGlyph = face.glyphIndex(c)) == 0) {
            return 0;
        }
        const GlyphId aGlyph = face.glyphIndex(a);
        if (shortest && aGlyph) {
            output(a, aGlyph);
            if (c) {
                output(c, cGlyph);
                return 2;
            }
            return 1;
        }
        if (const unsigned n = decompose(a, shortest)) {
            if (c) {
                output(c, cGlyph);
                return n + 1;
            }
            return n;
        }
        if (aGlyph) {
            output(a, aGlyph);
            if (c) {
                output(c, cGlyph);
                return 2;
            }
            return 1;
        }
        return 0;
    }
    void decomposeCurrent(bool shortest) {
        const char32_t u = b.cur().codepoint;
        const GlyphId glyph = face.glyphIndex(u);
        if (shortest && glyph) {
            pass(glyph);
            return;
        }
        if (decompose(u, shortest)) {
            b.skipGlyph();
            return;
        }
        if (glyph) {
            pass(glyph);
            return;
        }
        if (b.cur().generalCategory == static_cast<std::uint8_t>(GeneralCategory::Zs)) {
            const SpaceType t = spaceType(u);
            if (const GlyphId space = face.glyphIndex(U' '); t != NotSpace && space != 0) {
                b.cur().spaceFallback = t;
                pass(space);
                return;
            }
        }
        if (u == 0x2011) { // the one no-break character that is not a space
            if (const GlyphId hyphen = face.glyphIndex(0x2010)) {
                pass(hyphen);
                return;
            }
        }
        pass(0);
    }
    // A cluster with variation selectors: no normalisation, but the variant
    // glyph if the font has the sequence.
    void variationCluster(std::size_t end) {
        while (b.idx + 1 < end) {
            const char32_t u = b.cur().codepoint;
            const char32_t vs = b.info[b.idx + 1].codepoint;
            if (!isVariationSelector(vs)) {
                pass(face.glyphIndex(u));
                continue;
            }
            if (const std::optional<GlyphId> g = face.variationGlyph(u, vs)) {
                b.cur().glyph = *g;
                b.nextGlyph();
                b.skipGlyph(); // the selector joins its base
            } else {
                pass(face.glyphIndex(u));
                pass(face.glyphIndex(vs));
            }
            while (b.idx < end && isVariationSelector(b.cur().codepoint)) {
                pass(face.glyphIndex(b.cur().codepoint));
            }
        }
        if (b.idx < end) {
            pass(face.glyphIndex(b.cur().codepoint));
        }
    }

    void run() {
        // Round 1: decompose what the font lacks (all of it next to marks).
        b.clearOutput();
        b.idx = 0;
        bool allSimple = true;
        const std::size_t count = b.len();
        const auto isMark = [&](std::size_t i) { return isMarkCategory(b.info[i].generalCategory); };
        while (b.idx < count) {
            std::size_t end = b.idx + 1;
            while (end < count && !isMark(end)) {
                ++end;
            }
            if (end < count) {
                --end; // leave one base for the marks
            }
            while (b.idx < end) {
                decomposeCurrent(shortCircuit);
            }
            if (b.idx == count) {
                break;
            }
            allSimple = false;
            for (end = b.idx + 1; end < count && isMark(end); ++end) {
            }
            bool hasSelector = false;
            for (std::size_t i = b.idx; i < end; ++i) {
                hasSelector = hasSelector || isVariationSelector(b.info[i].codepoint);
            }
            if (hasSelector) {
                variationCluster(end);
            } else {
                while (b.idx < end) {
                    decomposeCurrent(!recompose);
                }
            }
        }
        b.swapBuffers();

        // Round 2: reorder marks by (modified) combining class.
        std::vector<ot::GlyphInfo> &info = b.info;
        bool hasCgj = false;
        if (!allSimple) {
            for (std::size_t i = 0; i < info.size(); ++i) {
                if (info[i].combiningClass == 0) {
                    continue;
                }
                std::size_t end = i + 1;
                while (end < info.size() && info[end].combiningClass != 0) {
                    ++end;
                }
                if (end - i <= kMaxCombiningMarks) {
                    for (std::size_t k = i + 1; k < end; ++k) {
                        std::size_t j = k;
                        while (j > i && info[j - 1].combiningClass > info[k].combiningClass) {
                            --j;
                        }
                        if (j == k) {
                            continue;
                        }
                        b.mergeClusters(j, k + 1);
                        const ot::GlyphInfo t = info[k];
                        std::memmove(&info[j + 1], &info[j], (k - j) * sizeof(ot::GlyphInfo));
                        info[j] = t;
                    }
                    if (engine == Engine::Arabic) {
                        reorderArabicMarks(i, end);
                    } else if (engine == Engine::Hebrew) {
                        reorderHebrewMarks(i, end);
                    }
                }
                i = end;
            }
        }
        for (const ot::GlyphInfo &g : info) {
            hasCgj = hasCgj || g.codepoint == 0x034F;
        }
        if (hasCgj) { // a CGJ that did not prevent reordering becomes skippable
            for (std::size_t i = 1; i + 1 < info.size(); ++i) {
                if (info[i].codepoint == 0x034F &&
                    (info[i + 1].combiningClass == 0 || info[i - 1].combiningClass <= info[i + 1].combiningClass)) {
                    info[i].flags &= static_cast<std::uint16_t>(~ot::kHidden);
                }
            }
        }
        if (allSimple || !recompose) {
            return;
        }

        // Round 3: recompose where the font has the composite (marks with
        // their starter only; blocked by a mark of the same or higher class).
        b.clearOutput();
        b.idx = 0;
        std::size_t starter = 0;
        b.nextGlyph();
        while (b.idx < b.len()) {
            const ot::GlyphInfo &cur = b.cur();
            if (isMarkCategory(cur.generalCategory)) {
                char32_t composed = 0;
                GlyphId glyph = 0;
                if ((starter == b.out.size() - 1 || b.out.back().combiningClass < cur.combiningClass) &&
                    compose(b.out[starter].codepoint, cur.codepoint, composed) && (glyph = face.glyphIndex(composed)) != 0) {
                    b.nextGlyph();
                    b.mergeOutClusters(starter, b.out.size());
                    b.out.pop_back();
                    b.out[starter].codepoint = composed;
                    b.out[starter].glyph = glyph;
                    setUnicodeProps(b.out[starter]);
                    continue;
                }
            }
            b.nextGlyph();
            if (b.out.back().combiningClass == 0) {
                starter = b.out.size() - 1;
            }
        }
        b.swapBuffers();
    }

    // Hebrew: patah or qamats, then sheva or hiriq, then meteg or a below
    // mark: the last two swap (HarfBuzz's order for fonts).
    void reorderHebrewMarks(std::size_t start, std::size_t end) {
        std::vector<ot::GlyphInfo> &info = b.info;
        for (std::size_t i = start + 2; i < end; ++i) {
            const unsigned c0 = info[i - 2].combiningClass;
            const unsigned c1 = info[i - 1].combiningClass;
            const unsigned c2 = info[i].combiningClass;
            if ((c0 == 20 || c0 == 21) && (c1 == 22 || c1 == 23) && (c2 == 25 || c2 == 220)) {
                b.mergeClusters(i - 1, i + 1);
                std::swap(info[i - 1], info[i]);
                break;
            }
        }
    }

    void reorderArabicMarks(std::size_t start, std::size_t end) {
        std::vector<ot::GlyphInfo> &info = b.info;
        std::size_t i = start;
        for (unsigned cc = 220; cc <= 230; cc += 10) {
            while (i < end && info[i].combiningClass < cc) {
                ++i;
            }
            if (i == end) {
                break;
            }
            if (info[i].combiningClass > cc) {
                continue;
            }
            std::size_t j = i;
            while (j < end && info[j].combiningClass == cc && isArabicModifierMark(info[j].codepoint)) {
                ++j;
            }
            if (i == j) {
                continue;
            }
            b.mergeClusters(start, j);
            std::rotate(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(i),
                        info.begin() + static_cast<std::ptrdiff_t>(j));
            const std::size_t newStart = start + j - i;
            const std::uint8_t newCc = cc == 220 ? 22 : 26; // sorts before every Arabic class
            for (; start < newStart; ++start) {
                info[start].combiningClass = newCc;
            }
            i = j;
        }
    }
};

// Reverses the order of graphemes (runs joined by continuation), keeping
// each one's characters in order.
void reverseGraphemes(ot::Buffer &b) {
    std::vector<ot::GlyphInfo> &info = b.info;
    for (std::size_t start = 0; start < info.size();) {
        std::size_t end = start + 1;
        while (end < info.size() && (info[end].flags & ot::kContinuation)) {
            ++end;
        }
        std::reverse(info.begin() + static_cast<std::ptrdiff_t>(start), info.begin() + static_cast<std::ptrdiff_t>(end));
        start = end;
    }
    std::reverse(info.begin(), info.end());
}

// Thai and Lao SARA AM becomes NIKHAHIT + SARA AA, the nikhahit moving before
// any above-base marks (as HarfBuzz's Thai shaper does).
void decomposeSaraAm(ot::Buffer &b) {
    const auto isSaraAm = [](char32_t u) { return (u & ~char32_t{0x80}) == 0x0E33; };
    const auto isAboveBase = [](char32_t u) {
        u &= ~char32_t{0x80};
        return (u >= 0x0E34 && u <= 0x0E37) || (u >= 0x0E47 && u <= 0x0E4E) || u == 0x0E31 || u == 0x0E3B;
    };
    if (std::none_of(b.info.begin(), b.info.end(), [&](const ot::GlyphInfo &g) { return isSaraAm(g.codepoint); })) {
        return;
    }
    b.clearOutput();
    for (b.idx = 0; b.idx < b.len();) {
        const char32_t u = b.cur().codepoint;
        if (!isSaraAm(u)) {
            b.nextGlyph();
            continue;
        }
        ot::GlyphInfo nikhahit = b.cur();
        nikhahit.codepoint = u - 0x0E33 + 0x0E4D;
        nikhahit.flags |= ot::kContinuation;
        nikhahit.generalCategory = static_cast<std::uint8_t>(GeneralCategory::Mn);
        b.out.push_back(nikhahit);
        ot::GlyphInfo aa = b.cur();
        aa.codepoint = u - 1;
        b.out.push_back(aa);
        ++b.idx;
        const std::size_t end = b.out.size();
        std::size_t start = end - 2;
        while (start > 0 && isAboveBase(b.out[start - 1].codepoint)) {
            --start;
        }
        if (start + 2 < end) {
            b.mergeOutClusters(start, end);
            const ot::GlyphInfo t = b.out[end - 2];
            std::memmove(&b.out[start + 1], &b.out[start], (end - start - 2) * sizeof(ot::GlyphInfo));
            b.out[start] = t;
        }
        if (start > 0) { // the nikhahit is combining: it joins the previous cluster
            b.mergeOutClusters(start - 1, end);
        }
    }
    b.swapBuffers();
}

// ---- Hangul (HarfBuzz's Hangul shaper) ----

constexpr char32_t kLBase = 0x1100;
constexpr char32_t kVBase = 0x1161;
constexpr char32_t kTBase = 0x11A7;
constexpr char32_t kLCount = 19;
constexpr char32_t kVCount = 21;
constexpr char32_t kTCount = 28;
constexpr char32_t kSBase = 0xAC00;
constexpr char32_t kNCount = kVCount * kTCount;
constexpr char32_t kSCount = kLCount * kNCount;

bool isHangulL(char32_t u) { return (u >= 0x1100 && u <= 0x115F) || (u >= 0xA960 && u <= 0xA97C); }
bool isHangulV(char32_t u) { return (u >= 0x1160 && u <= 0x11A7) || (u >= 0xD7B0 && u <= 0xD7C6); }
bool isHangulT(char32_t u) { return (u >= 0x11A8 && u <= 0x11FF) || (u >= 0xD7CB && u <= 0xD7FB); }

// Composes jamo sequences into syllables the font has, and decomposes
// syllables it lacks (or that a trailing jamo follows) into jamo, marking
// them for ljmo/vjmo/tjmo; tone marks move before their syllable.
void hangulPreprocess(const FontFace &face, ot::Buffer &b) {
    const auto has = [&](char32_t u) { return face.glyphIndex(u) != 0; };
    const auto zeroWidth = [&](char32_t u) {
        const GlyphId g = face.glyphIndex(u);
        return g != 0 && face.advanceWidth(g) == 0;
    };
    // Replaces `in` characters at idx with `out`, each a copy of the current one.
    const auto replace = [&](std::size_t in, std::initializer_list<char32_t> out) {
        b.mergeClusters(b.idx, b.idx + in);
        const ot::GlyphInfo orig = b.idx < b.len() ? b.cur() : b.out.back();
        for (const char32_t u : out) {
            ot::GlyphInfo g = orig;
            g.codepoint = u;
            b.out.push_back(g);
        }
        b.idx += in;
    };
    b.clearOutput();
    std::size_t start = 0;
    std::size_t end = 0;
    const std::size_t count = b.len();
    for (b.idx = 0; b.idx < count;) {
        const char32_t u = b.cur().codepoint;
        if (u == 0x302E || u == 0x302F) { // tone marks
            if (start < end && end == b.out.size()) {
                b.nextGlyph();
                if (!zeroWidth(u)) {
                    b.mergeOutClusters(start, end + 1);
                    const ot::GlyphInfo tone = b.out[end];
                    std::memmove(&b.out[start + 1], &b.out[start], (end - start) * sizeof(ot::GlyphInfo));
                    b.out[start] = tone;
                }
            } else if (has(0x25CC)) {
                if (!zeroWidth(u)) {
                    replace(1, {u, 0x25CC});
                } else {
                    replace(1, {0x25CC, u});
                }
            } else {
                b.nextGlyph();
            }
            start = end = b.out.size();
            continue;
        }
        start = b.out.size();
        if (isHangulL(u) && b.idx + 1 < count) {
            const char32_t l = u;
            const char32_t v = b.info[b.idx + 1].codepoint;
            if (isHangulV(v)) {
                char32_t t = 0;
                char32_t tindex = 0;
                if (b.idx + 2 < count) {
                    t = b.info[b.idx + 2].codepoint;
                    if (isHangulT(t)) {
                        tindex = t - kTBase;
                    } else {
                        t = 0;
                    }
                }
                const bool combiningT = t == 0 || (t > kTBase && t < kTBase + kTCount);
                if (l >= kLBase && l < kLBase + kLCount && v >= kVBase && v < kVBase + kVCount && combiningT) {
                    const char32_t syllable = kSBase + (l - kLBase) * kNCount + (v - kVBase) * kTCount + tindex;
                    if (has(syllable)) {
                        replace(t ? 3 : 2, {syllable});
                        end = start + 1;
                        continue;
                    }
                }
                b.cur().shaperAction = 1;
                b.nextGlyph();
                b.cur().shaperAction = 2;
                b.nextGlyph();
                if (t) {
                    b.cur().shaperAction = 3;
                    b.nextGlyph();
                    end = start + 3;
                } else {
                    end = start + 2;
                }
                b.mergeOutClusters(start, end);
                continue;
            }
        } else if (u >= kSBase && u < kSBase + kSCount) {
            const bool hasSyllable = has(u);
            const char32_t lindex = (u - kSBase) / kNCount;
            const char32_t nindex = (u - kSBase) % kNCount;
            const char32_t vindex = nindex / kTCount;
            const char32_t tindex = nindex % kTCount;
            const char32_t next = b.idx + 1 < count ? b.info[b.idx + 1].codepoint : 0;
            if (!tindex && next > kTBase && next < kTBase + kTCount) {
                const char32_t composed = u + (next - kTBase);
                if (has(composed)) {
                    replace(2, {composed});
                    end = start + 1;
                    continue;
                }
            }
            if (!hasSyllable || (!tindex && isHangulT(next))) {
                const char32_t parts[3] = {kLBase + lindex, kVBase + vindex, kTBase + tindex};
                if (has(parts[0]) && has(parts[1]) && (!tindex || has(parts[2]))) {
                    std::size_t length = tindex ? 3 : 2;
                    if (tindex) {
                        replace(1, {parts[0], parts[1], parts[2]});
                    } else {
                        replace(1, {parts[0], parts[1]});
                    }
                    if (hasSyllable && !tindex) {
                        b.nextGlyph(); // the trailing jamo that follows
                        ++length;
                    }
                    end = start + length;
                    std::size_t i = start;
                    b.out[i++].shaperAction = 1;
                    b.out[i++].shaperAction = 2;
                    if (i < end) {
                        b.out[i++].shaperAction = 3;
                    }
                    b.mergeOutClusters(start, end);
                    continue;
                }
            }
            if (hasSyllable) {
                end = start + 1;
            }
        }
        b.nextGlyph();
    }
    b.swapBuffers();
}

// ---- Fallback mark positioning (HarfBuzz's, for fonts without GPOS) ----

enum : std::uint8_t {
    kAttachedBelowLeft = 200,
    kAttachedBelow = 202,
    kAttachedAbove = 214,
    kAttachedAboveRight = 216,
    kBelowLeft = 218,
    kBelow = 220,
    kBelowRight = 222,
    kAboveLeft = 228,
    kAbove = 230,
    kAboveRight = 232,
    kDoubleBelow = 233,
    kDoubleAbove = 234,
};

// Maps (modified) combining classes to positions around the base.
std::uint8_t fallbackCombiningClass(char32_t u, std::uint8_t klass) {
    if (klass >= 200) {
        return klass;
    }
    if ((u & ~char32_t{0xFF}) == 0x0E00) { // Thai and Lao
        if (klass == 0) {
            switch (u) {
            case 0x0E31: case 0x0E34: case 0x0E35: case 0x0E36: case 0x0E37: case 0x0E47: case 0x0E4C: case 0x0E4D:
            case 0x0E4E: klass = kAboveRight; break;
            case 0x0EB1: case 0x0EB4: case 0x0EB5: case 0x0EB6: case 0x0EB7: case 0x0EBB: case 0x0ECC:
            case 0x0ECD: klass = kAbove; break;
            case 0x0EBC: klass = kBelow; break;
            default: break;
            }
        } else if (u == 0x0E3A) {
            klass = kBelowRight;
        }
    }
    switch (klass) {
    // Hebrew (the modified classes).
    case 22: case 15: case 16: case 17: case 23: case 18: case 19: case 20: case 21: case 24: case 25: return kBelow;
    case 13: return kAttachedAbove;    // rafe
    case 10: return kAboveRight;       // shin dot
    case 11: case 14: return kAboveLeft; // sin dot, holam
    case 26: return kAbove;            // point varika
    case 12: return klass;             // dagesh
    // Arabic and Syriac.
    case 28: case 29: case 31: case 32: case 27: case 34: case 35: case 36: return kAbove;
    case 30: case 33: return kBelow;   // kasratan, kasra
    // Thai, Lao, Tibetan.
    case 3: return kBelowRight;
    case 107: return kAboveRight;
    case 118: return kBelow;
    case 122: return kAbove;
    case 129: return kBelow;
    case 132: return kAbove;
    case 131: return kBelow;
    default: return klass;
    }
}

void positionMark(const FontFace &face, ot::Buffer &b, FontFace::GlyphExtents &base, std::size_t i, unsigned klass) {
    FontFace::GlyphExtents mark;
    if (!face.glyphExtents(b.info[i].glyph, mark)) {
        return;
    }
    const int yGap = face.unitsPerEm() / 16;
    ot::GlyphPosition &pos = b.pos[i];
    pos.xOffset = 0;
    pos.yOffset = 0;
    switch (klass) {
    case kDoubleBelow:
    case kDoubleAbove:
        if (!b.backward) {
            pos.xOffset += base.xBearing + base.width - mark.width / 2 - mark.xBearing;
        } else {
            pos.xOffset += base.xBearing - mark.width / 2 - mark.xBearing;
        }
        break;
    case kAttachedBelowLeft:
    case kBelowLeft:
    case kAboveLeft:
        pos.xOffset += base.xBearing - mark.xBearing;
        break;
    case kAttachedAboveRight:
    case kBelowRight:
    case kAboveRight:
        pos.xOffset += base.xBearing + base.width - mark.width - mark.xBearing;
        break;
    default:
        pos.xOffset += base.xBearing + (base.width - mark.width) / 2 - mark.xBearing;
        break;
    }
    switch (klass) {
    case kDoubleBelow:
    case kBelowLeft:
    case kBelow:
    case kBelowRight:
        base.height -= yGap;
        [[fallthrough]];
    case kAttachedBelowLeft:
    case kAttachedBelow:
        pos.yOffset = base.yBearing + base.height - mark.yBearing;
        if ((yGap > 0) == (pos.yOffset > 0)) { // never shift "below" marks up
            base.height -= pos.yOffset;
            pos.yOffset = 0;
        }
        base.height += mark.height;
        break;
    case kDoubleAbove:
    case kAboveLeft:
    case kAbove:
    case kAboveRight:
        base.yBearing += yGap;
        base.height -= yGap;
        [[fallthrough]];
    case kAttachedAbove:
    case kAttachedAboveRight:
        pos.yOffset = base.yBearing - (mark.yBearing + mark.height);
        if ((yGap > 0) != (pos.yOffset > 0)) { // nor "above" marks down much
            const int correction = -pos.yOffset / 2;
            base.yBearing += correction;
            base.height -= correction;
            pos.yOffset += correction;
        }
        base.yBearing -= mark.height;
        base.height += mark.height;
        break;
    default: break;
    }
}

bool isUnicodeMark(const ot::GlyphInfo &g) { return isMarkCategory(g.generalCategory); }
// Hidden and default-ignorable characters do not break a base's mark run.
bool isTransparent(const ot::GlyphInfo &g) { return (g.flags & ot::kHidden) || ot::isDefaultIgnorable(g); }

void positionAroundBase(const FontFace &face, ot::Buffer &b, std::size_t base, std::size_t end, TextDirection direction) {
    FontFace::GlyphExtents baseExtents;
    if (!face.glyphExtents(b.info[base].glyph, baseExtents)) {
        const bool adjust = !b.backward;
        for (std::size_t i = base + 1; i < end; ++i) {
            if (b.info[i].generalCategory == static_cast<std::uint8_t>(GeneralCategory::Mn)) {
                if (adjust) {
                    b.pos[i].xOffset -= b.pos[i].xAdvance;
                    b.pos[i].yOffset -= b.pos[i].yAdvance;
                }
                b.pos[i].xAdvance = 0;
                b.pos[i].yAdvance = 0;
            }
        }
        return;
    }
    baseExtents.yBearing += b.pos[base].yOffset;
    baseExtents.xBearing = 0;
    baseExtents.width = face.advanceWidth(b.info[base].glyph);
    const unsigned ligId = ot::ligId(b.info[base]);
    const int components = static_cast<int>(ot::ligNumComps(b.info[base]));
    int xOffset = 0;
    int yOffset = 0;
    if (!b.backward) {
        xOffset -= b.pos[base].xAdvance;
        yOffset -= b.pos[base].yAdvance;
    }
    FontFace::GlyphExtents componentExtents = baseExtents;
    FontFace::GlyphExtents clusterExtents = baseExtents;
    int lastComponent = -1;
    unsigned lastClass = 255;
    for (std::size_t i = base + 1; i < end; ++i) {
        const unsigned klass = b.info[i].combiningClass;
        if (klass != 0) {
            if (components > 1) {
                int component = static_cast<int>(ot::ligComp(b.info[i])) - 1;
                if (!ligId || ligId != ot::ligId(b.info[i]) || component >= components) {
                    component = components - 1;
                }
                if (lastComponent != component) {
                    lastComponent = component;
                    lastClass = 255;
                    componentExtents = baseExtents;
                    if (direction == TextDirection::LeftToRight) {
                        componentExtents.xBearing += (component * componentExtents.width) / components;
                    } else {
                        componentExtents.xBearing += ((components - 1 - component) * componentExtents.width) / components;
                    }
                    componentExtents.width /= components;
                }
            }
            if (lastClass != klass) {
                lastClass = klass;
                clusterExtents = componentExtents;
            }
            positionMark(face, b, clusterExtents, i, klass);
            b.pos[i].xAdvance = 0;
            b.pos[i].yAdvance = 0;
            b.pos[i].xOffset += xOffset;
            b.pos[i].yOffset += yOffset;
        } else if (!b.backward) {
            xOffset -= b.pos[i].xAdvance;
            yOffset -= b.pos[i].yAdvance;
        } else {
            xOffset += b.pos[i].xAdvance;
            yOffset += b.pos[i].yAdvance;
        }
    }
}

void positionCluster(const FontFace &face, ot::Buffer &b, std::size_t start, std::size_t end, TextDirection direction) {
    if (end - start < 2) {
        return;
    }
    for (std::size_t i = start; i < end; ++i) {
        if (!isUnicodeMark(b.info[i])) {
            std::size_t j = i + 1;
            while (j < end && (isUnicodeMark(b.info[j]) || isTransparent(b.info[j]))) {
                ++j;
            }
            positionAroundBase(face, b, i, j, direction);
            i = j - 1;
        }
    }
}

void positionMarksByExtents(const FontFace &face, ot::Buffer &b, TextDirection direction) {
    std::size_t start = 0;
    for (std::size_t i = 1; i < b.len(); ++i) {
        if (!isUnicodeMark(b.info[i]) && !isTransparent(b.info[i])) {
            positionCluster(face, b, start, i, direction);
            start = i;
        }
    }
    positionCluster(face, b, start, b.len(), direction);
}

// Thai without Thai GSUB: marks move to the fonts' presentation forms in
// the Private Use Area (Windows or Mac layout), as HarfBuzz does.
void thaiPuaShaping(const FontFace &face, ot::Buffer &b) {
    enum Consonant { NC, AC, RC, DC, NotConsonant };
    enum MarkType { AV, BV, T, NotMark };
    enum Action { NOP, SD, SL, SDL, RD };
    const auto consonant = [](char32_t u) {
        if (u == 0x0E1B || u == 0x0E1D || u == 0x0E1F) {
            return AC;
        }
        if (u == 0x0E0D || u == 0x0E10) {
            return RC;
        }
        if (u == 0x0E0E || u == 0x0E0F) {
            return DC;
        }
        return u >= 0x0E01 && u <= 0x0E2E ? NC : NotConsonant;
    };
    const auto markType = [](char32_t u) {
        if (u == 0x0E31 || (u >= 0x0E34 && u <= 0x0E37) || u == 0x0E47 || (u >= 0x0E4D && u <= 0x0E4E)) {
            return AV;
        }
        if (u >= 0x0E38 && u <= 0x0E3A) {
            return BV;
        }
        return u >= 0x0E48 && u <= 0x0E4C ? T : NotMark;
    };
    struct Pua {
        char32_t u;
        char32_t win;
        char32_t mac;
    };
    static constexpr Pua kSD[] = {{0x0E48, 0xF70A, 0xF88B}, {0x0E49, 0xF70B, 0xF88E}, {0x0E4A, 0xF70C, 0xF891},
                                  {0x0E4B, 0xF70D, 0xF894}, {0x0E4C, 0xF70E, 0xF897}, {0x0E38, 0xF718, 0xF89B},
                                  {0x0E39, 0xF719, 0xF89C}, {0x0E3A, 0xF71A, 0xF89D}};
    static constexpr Pua kSDL[] = {{0x0E48, 0xF705, 0xF88C}, {0x0E49, 0xF706, 0xF88F}, {0x0E4A, 0xF707, 0xF892},
                                   {0x0E4B, 0xF708, 0xF895}, {0x0E4C, 0xF709, 0xF898}};
    static constexpr Pua kSL[] = {{0x0E48, 0xF713, 0xF88A}, {0x0E49, 0xF714, 0xF88D}, {0x0E4A, 0xF715, 0xF890},
                                  {0x0E4B, 0xF716, 0xF893}, {0x0E4C, 0xF717, 0xF896}, {0x0E31, 0xF710, 0xF884},
                                  {0x0E34, 0xF701, 0xF885}, {0x0E35, 0xF702, 0xF886}, {0x0E36, 0xF703, 0xF887},
                                  {0x0E37, 0xF704, 0xF888}, {0x0E47, 0xF712, 0xF889}, {0x0E4D, 0xF711, 0xF899}};
    static constexpr Pua kRD[] = {{0x0E0D, 0xF70F, 0xF89A}, {0x0E10, 0xF700, 0xF89E}};
    const auto shape = [&](char32_t u, Action action) -> char32_t {
        Span<const Pua> table;
        switch (action) {
        case SD: table = kSD; break;
        case SDL: table = kSDL; break;
        case SL: table = kSL; break;
        case RD: table = kRD; break;
        case NOP: return u;
        }
        for (const Pua &m : table) {
            if (m.u == u) {
                if (face.glyphIndex(m.win)) {
                    return m.win;
                }
                if (face.glyphIndex(m.mac)) {
                    return m.mac;
                }
                break;
            }
        }
        return u;
    };
    enum Above { T0, T1, T2, T3 };
    enum Below { B0, B1, B2 };
    static constexpr Above kAboveStart[] = {T0, T1, T0, T0, T3};
    static constexpr Below kBelowStart[] = {B0, B0, B1, B2, B2};
    struct AboveEdge {
        Action action;
        Above next;
    };
    struct BelowEdge {
        Action action;
        Below next;
    };
    static constexpr AboveEdge kAbove[4][3] = {{{NOP, T3}, {NOP, T0}, {SD, T3}},
                                               {{SL, T2}, {NOP, T1}, {SDL, T2}},
                                               {{NOP, T3}, {NOP, T2}, {SL, T3}},
                                               {{NOP, T3}, {NOP, T3}, {NOP, T3}}};
    static constexpr BelowEdge kBelow[3][3] = {
        {{NOP, B0}, {NOP, B2}, {NOP, B0}}, {{NOP, B1}, {RD, B2}, {NOP, B1}}, {{NOP, B2}, {SD, B2}, {NOP, B2}}};
    Above above = kAboveStart[NotConsonant];
    Below below = kBelowStart[NotConsonant];
    std::size_t base = 0;
    for (std::size_t i = 0; i < b.len(); ++i) {
        const MarkType mt = markType(b.info[i].codepoint);
        if (mt == NotMark) {
            const Consonant ct = consonant(b.info[i].codepoint);
            above = kAboveStart[ct];
            below = kBelowStart[ct];
            base = i;
            continue;
        }
        const AboveEdge &ae = kAbove[above][mt];
        const BelowEdge &be = kBelow[below][mt];
        above = ae.next;
        below = be.next;
        const Action action = ae.action != NOP ? ae.action : be.action;
        if (action == RD) {
            b.info[base].codepoint = shape(b.info[base].codepoint, action);
        } else {
            b.info[i].codepoint = shape(b.info[i].codepoint, action);
        }
    }
}

} // namespace

void Shaper::shape(const FontFace &face, Span<const char32_t> text, const ShapeOptions &options,
                   std::vector<ShapedGlyph> &out) {
    out.clear();
    if (text.empty()) {
        return;
    }
    Script script = options.script;
    if (script == Script::Unknown) {
        script = Script::Common;
        for (const char32_t c : text) {
            const Script s = unicode::script(c);
            if (s != Script::Common && s != Script::Inherited && s != Script::Unknown) {
                script = s;
                break;
            }
        }
    }
    const TextDirection direction = options.direction.value_or(scriptDirection(script));
    const Plan &p = plan(face, script, direction, options.language, options.features);
    ot::Buffer &b = m_scratch->buffer;
    b.info.clear();
    b.out.clear();
    b.pos.clear();
    b.idx = 0;
    b.haveOutput = false;
    b.backward = direction == TextDirection::RightToLeft;
    b.serial = 0;
    b.randomState = 1;
    b.maxOps = static_cast<int>(std::clamp<std::size_t>(text.size() * 64, 16384, 0x1FFFFFFF));
    b.maxLen = std::clamp<std::size_t>(text.size() * 64, 16384, 0x3FFFFFFF);

    // Characters with their Unicode properties; graphemes (as HarfBuzz
    // approximates them) are clusters.
    b.info.resize(text.size());
    bool hasFractionSlash = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        ot::GlyphInfo &g = b.info[i];
        g.codepoint = text[i];
        g.cluster = static_cast<std::uint32_t>(i);
        g.mask = p.globalMask;
        setUnicodeProps(g);
        const char32_t u = g.codepoint;
        if (u < 0x80) {
            continue;
        }
        const auto gc = static_cast<GeneralCategory>(g.generalCategory);
        if (gc == GeneralCategory::Ll || gc == GeneralCategory::Lu || gc == GeneralCategory::Lt ||
            gc == GeneralCategory::Lo || gc == GeneralCategory::Zs) {
            continue;
        }
        if (gc == GeneralCategory::Sk && u >= 0x1F3FB && u <= 0x1F3FF) {
            g.flags |= ot::kContinuation; // emoji modifiers
        } else if (i > 0 && isRegionalIndicator(u)) {
            if (isRegionalIndicator(b.info[i - 1].codepoint) && !(b.info[i - 1].flags & ot::kContinuation)) {
                g.flags |= ot::kContinuation; // the second of a pair
            }
        } else if (g.flags & ot::kZwj) {
            g.flags |= ot::kContinuation;
            if (i + 1 < text.size() && unicode::properties(text[i + 1]).extendedPictographic) {
                ++i;
                ot::GlyphInfo &next = b.info[i];
                next.codepoint = text[i];
                next.cluster = static_cast<std::uint32_t>(i);
                next.mask = p.globalMask;
                setUnicodeProps(next);
                next.flags |= ot::kContinuation;
            }
        } else if ((u >= 0xFF9E && u <= 0xFF9F) || (u >= 0xE0020 && u <= 0xE007F)) {
            g.flags |= ot::kContinuation;
        } else if (u == 0x2044) {
            hasFractionSlash = true;
        }
    }
    for (std::size_t start = 0; start < b.len();) {
        std::size_t end = start + 1;
        while (end < b.len() && (b.info[end].flags & ot::kContinuation)) {
            ++end;
        }
        b.mergeClusters(start, end);
        start = end;
    }

    // Like HarfBuzz, shape in the script's native direction: text asked for
    // the other way round is reversed (grapheme by grapheme) now and back at
    // the end, so contextual lookups and kerning see the native order. (Runs
    // of digits in right-to-left scripts are left-to-right.)
    std::optional<TextDirection> native;
    if (hasNativeDirection(script)) {
        native = scriptDirection(script);
    }
    if (native == TextDirection::RightToLeft && direction == TextDirection::LeftToRight) {
        bool number = false;
        bool letter = false;
        for (const ot::GlyphInfo &g : b.info) {
            const auto gc = static_cast<GeneralCategory>(g.generalCategory);
            if (gc == GeneralCategory::Lu || gc == GeneralCategory::Ll || gc == GeneralCategory::Lt ||
                gc == GeneralCategory::Lm || gc == GeneralCategory::Lo) {
                letter = true;
                break;
            }
            number = number || gc == GeneralCategory::Nd || isRegionalIndicator(g.codepoint);
        }
        if (number && !letter) {
            native = TextDirection::LeftToRight;
        }
    }
    if (native && *native != direction) {
        reverseGraphemes(b);
        b.backward = !b.backward;
    }
    if (p.engine == Engine::Hangul) {
        hangulPreprocess(face, b);
    }
    if (p.engine == Engine::Use || p.engine == Engine::Indic) {
        syllabic::insertVowelConstraintCircles(b, script);
    }
    if (p.engine == Engine::Thai) {
        decomposeSaraAm(b);
        if (p.thaiPua) {
            thaiPuaShaping(face, b);
        }
    }
    // Right-to-left: mirrored characters, if the font has them.
    if (direction == TextDirection::RightToLeft) {
        for (ot::GlyphInfo &g : b.info) {
            const char32_t m = unicode::mirrored(g.codepoint);
            if (m != g.codepoint && face.glyphIndex(m) != 0) {
                g.codepoint = m;
            } else {
                g.mask |= p.rtlmMask;
            }
        }
    }

    Normalizer{face, p.engine, p.hasGposMark, b, p.composeCharacters, p.shortCircuit}.run();
    if (p.engine == Engine::Hangul) {
        for (ot::GlyphInfo &g : b.info) {
            g.mask |= p.hangulMasks[g.shaperAction & 3];
            if (isHangulL(g.codepoint) || isHangulV(g.codepoint) || isHangulT(g.codepoint)) {
                g.mask &= ~p.caltMask;
            }
        }
    }

    // Masks: automatic fractions, Arabic joining forms, ranged features.
    if (hasFractionSlash && (p.fracMask || (p.numrMask && p.dnomMask))) {
        const std::uint32_t pre = b.backward ? p.fracMask | p.dnomMask : p.numrMask | p.fracMask;
        const std::uint32_t post = b.backward ? p.numrMask | p.fracMask : p.fracMask | p.dnomMask;
        const auto digit = [&](std::size_t k) {
            return b.info[k].generalCategory == static_cast<std::uint8_t>(GeneralCategory::Nd);
        };
        for (std::size_t i = 0; i < b.len(); ++i) {
            if (b.info[i].codepoint != 0x2044) {
                continue;
            }
            std::size_t start = i;
            std::size_t end = i + 1;
            while (start > 0 && digit(start - 1)) {
                --start;
            }
            while (end < b.len() && digit(end)) {
                ++end;
            }
            if (start == i || end == i + 1) {
                continue;
            }
            for (std::size_t k = start; k < i; ++k) {
                b.info[k].mask |= pre;
            }
            b.info[i].mask |= p.fracMask;
            for (std::size_t k = i + 1; k < end; ++k) {
                b.info[k].mask |= post;
            }
            i = end - 1;
        }
    }
    if (p.engine == Engine::Arabic || p.arabicJoining) {
        std::size_t prev = static_cast<std::size_t>(-1);
        unsigned state = 0;
        for (std::size_t i = 0; i < b.len(); ++i) {
            ot::GlyphInfo &g = b.info[i];
            const int column = joiningColumn(g.codepoint);
            if (column < 0) {
                g.shaperAction = None;
                continue;
            }
            const JoinEntry &e = kJoining[state][column];
            if (e.prevAction != None && prev != static_cast<std::size_t>(-1)) {
                b.info[prev].shaperAction = e.prevAction;
            }
            g.shaperAction = e.currAction;
            prev = i;
            state = e.next;
        }
        if (script == Script::Mongolian) { // free variation selectors take their base's form
            for (std::size_t i = 1; i < b.len(); ++i) {
                const char32_t u = b.info[i].codepoint;
                if ((u >= 0x180B && u <= 0x180D) || u == 0x180F) {
                    b.info[i].shaperAction = b.info[i - 1].shaperAction;
                }
            }
        }
        for (ot::GlyphInfo &g : b.info) {
            if (g.shaperAction < 7) {
                g.mask |= p.arabicMasks[g.shaperAction];
            }
        }
    }
    if (p.engine == Engine::Use) {
        syllabic::setUseCategories(b);
    } else if (p.engine == Engine::Indic || p.engine == Engine::Khmer || p.engine == Engine::Myanmar) {
        syllabic::setMachineCategories(b, p.engine == Engine::Indic);
    }
    for (const Plan::RangedFeature &f : p.ranged) {
        b.maxOps -= static_cast<int>(b.len()); // HarfBuzz's set_masks
        const std::uint32_t value = (f.value << f.shift) & f.mask;
        for (ot::GlyphInfo &g : b.info) {
            if (g.cluster >= f.start && g.cluster < f.end) {
                g.mask = (g.mask & ~f.mask) | value;
            }
        }
    }
    if (p.fallbackMarkPositioning) {
        for (ot::GlyphInfo &g : b.info) {
            if (g.generalCategory == static_cast<std::uint8_t>(GeneralCategory::Mn)) {
                g.combiningClass = fallbackCombiningClass(g.codepoint, g.combiningClass);
            }
        }
    }

    // Glyph classes, then substitution.
    for (ot::GlyphInfo &g : b.info) {
        g.ligProps = 0;
        if (p.fallbackGlyphClasses) {
            // Default ignorables are never marks (so lookups ignoring marks
            // do not skip them).
            g.glyphProps = (g.generalCategory != static_cast<std::uint8_t>(GeneralCategory::Mn) || ot::isDefaultIgnorable(g))
                               ? ot::kBaseGlyph
                               : ot::kMark;
        } else {
            g.glyphProps = p.gdef.glyphProps(g.glyph);
        }
    }
    // What runs after each GSUB stage.
    bool broken = false;
    const auto forEachSyllable = [&](auto &&f) {
        for (std::size_t start = 0; start < b.len();) {
            const std::size_t end = syllabic::nextSyllable(b, start);
            f(start, end);
            start = end;
        }
    };
    const auto runPause = [&](Pause pause) {
        switch (pause) {
        case PauseNone: break;
        case PauseClearSubstitution:
            for (ot::GlyphInfo &g : b.info) {
                g.glyphProps &= static_cast<std::uint16_t>(~ot::kSubstituted);
            }
            break;
        case PauseClearSyllables:
            for (ot::GlyphInfo &g : b.info) {
                g.syllable = 0;
            }
            break;
        case PauseUseSyllables: {
            broken = syllabic::findUseSyllables(b);
            // rphf applies to a leading repha, else to the first three glyphs.
            if (p.rphfMask) {
                forEachSyllable([&](std::size_t start, std::size_t end) {
                    const bool repha = static_cast<syllabic::UseCat>(b.info[start].category) == syllabic::UseCat::R;
                    const std::size_t limit = repha ? 1 : std::min<std::size_t>(3, end - start);
                    for (std::size_t i = start; i < start + limit; ++i) {
                        b.info[i].mask |= p.rphfMask;
                    }
                });
            }
            // Scripts that do not join like Arabic: clusters take isolated,
            // initial, medial and final forms.
            std::uint32_t all = 0;
            for (const std::uint32_t m : p.topographicalMasks) {
                all |= m;
            }
            if (all == 0) {
                break;
            }
            enum { Isolated, Initial, Medial, Final, NoForm };
            int last = NoForm;
            std::size_t lastStart = 0;
            forEachSyllable([&](std::size_t start, std::size_t end) {
                const unsigned type = b.info[start].syllable & 0x0Fu;
                if (type == syllabic::UseHieroglyph || type == syllabic::UseNonCluster) {
                    last = NoForm;
                } else {
                    const bool join = last == Final || last == Isolated;
                    if (join) {
                        last = last == Final ? Medial : Initial;
                        for (std::size_t i = lastStart; i < start; ++i) {
                            b.info[i].mask = (b.info[i].mask & ~all) | p.topographicalMasks[last];
                        }
                    }
                    last = join ? Final : Isolated;
                    for (std::size_t i = start; i < end; ++i) {
                        b.info[i].mask = (b.info[i].mask & ~all) | p.topographicalMasks[last];
                    }
                }
                lastStart = start;
            });
            break;
        }
        case PauseUseRphf:
            if (p.rphfMask) {
                forEachSyllable([&](std::size_t start, std::size_t end) {
                    for (std::size_t i = start; i < end && (b.info[i].mask & p.rphfMask); ++i) {
                        if (ot::isSubstituted(b.info[i])) {
                            b.info[i].category = static_cast<std::uint8_t>(syllabic::UseCat::R);
                            break;
                        }
                    }
                });
            }
            break;
        case PauseUsePref:
            forEachSyllable([&](std::size_t start, std::size_t end) {
                for (std::size_t i = start; i < end; ++i) {
                    if (ot::isSubstituted(b.info[i])) {
                        b.info[i].category = static_cast<std::uint8_t>(syllabic::UseCat::VPre);
                        break;
                    }
                }
            });
            break;
        case PauseUseReorder:
            if (broken) {
                syllabic::insertDottedCircles(face, b, syllabic::UseBroken,
                                              static_cast<std::uint8_t>(syllabic::UseCat::B),
                                              static_cast<int>(syllabic::UseCat::R));
            }
            syllabic::reorderUse(b);
            break;
        case PauseIndicSyllables: broken = syllabic::findIndicSyllables(b); break;
        case PauseIndicInitial: syllabic::initialReorderingIndic(face, p.indic, b, broken); break;
        case PauseIndicFinal: syllabic::finalReorderingIndic(p.indic, b); break;
        case PauseKhmerSyllables: broken = syllabic::findKhmerSyllables(b); break;
        case PauseKhmerReorder: syllabic::reorderKhmer(face, p.khmer, b, broken); break;
        case PauseMyanmarSyllables: broken = syllabic::findMyanmarSyllables(b); break;
        case PauseMyanmarReorder: syllabic::reorderMyanmar(face, b, broken); break;
        }
    };
    for (ot::GlyphInfo &g : b.info) {
        g.syllable = 0;
    }
    for (std::size_t stage = 0; stage < p.gsubStages.size(); ++stage) {
        ot::applyLookups(face, p.gdef, p.gsub, p.gsubStages[stage], b);
        b.idx = 0;
        runPause(static_cast<Pause>(p.pauses[stage]));
        if (stage == p.fallbackStage && !p.fallbackLookups.empty()) {
            for (const ot::PlannedLookup &l : p.fallbackLookups) {
                ot::applyLookups(face, p.gdef, p.fallbackTable, Span<const ot::PlannedLookup>(&l, 1), b);
            }
        }
    }

    // Positioning: advances, fallback spaces, GPOS or 'kern', marks
    // zeroed, attachments resolved, and marks placed by their boxes if the
    // font cannot place them.
    b.pos.assign(b.len(), ot::GlyphPosition{});
    const int upem = face.unitsPerEm();
    for (std::size_t i = 0; i < b.len(); ++i) {
        b.pos[i].xAdvance = face.advanceWidth(b.info[i].glyph);
    }
    for (std::size_t i = 0; i < b.len(); ++i) {
        const std::uint8_t t = b.info[i].spaceFallback;
        if (t == NotSpace || t == SpacePlain || b.info[i].generalCategory != static_cast<std::uint8_t>(GeneralCategory::Zs) ||
            (b.info[i].glyphProps & ot::kLigated)) {
            continue;
        }
        ot::GlyphPosition &pos = b.pos[i];
        if (t <= SpaceEm16) {
            pos.xAdvance = (upem + t / 2) / t;
        } else if (t == Space4Em18) {
            pos.xAdvance = upem * 4 / 18;
        } else if (t == SpaceFigure) {
            for (char32_t d = U'0'; d <= U'9'; ++d) {
                if (const GlyphId g = face.glyphIndex(d)) {
                    pos.xAdvance = face.advanceWidth(g);
                    break;
                }
            }
        } else if (t == SpacePunctuation) {
            GlyphId g = face.glyphIndex(U'.');
            if (!g) {
                g = face.glyphIndex(U',');
            }
            if (g) {
                pos.xAdvance = face.advanceWidth(g);
            }
        } else if (t == SpaceNarrow) {
            pos.xAdvance /= 2;
        }
    }
    b.idx = 0;
    b.haveOutput = false;
    if (p.zeroMarks && p.zeroMarksEarly) { // the USE zeroes marks before GPOS
        zeroMarkWidths(b, p.adjustMarkOffsets && !b.backward);
    }
    if (p.applyGpos) {
        ot::applyLookups(face, p.gdef, p.gpos, p.gposLookups, b);
    }
    if (p.applyKern && p.kernMask != 0) {
        (void)ot::applyKernTable(face, p.gdef, p.kernMask, b);
    }
    const bool adjust = p.adjustMarkOffsets && !b.backward;
    if (p.zeroMarks && !p.zeroMarksEarly) { // marks take no space (by GDEF class, after positioning)
        zeroMarkWidths(b, adjust);
    }
    for (std::size_t i = 0; i < b.len(); ++i) {
        if (ot::isDefaultIgnorable(b.info[i])) {
            b.pos[i].xAdvance = 0;
            b.pos[i].yAdvance = 0;
            b.pos[i].xOffset = 0;
        }
    }
    ot::propagateAttachments(b);
    if (p.fallbackMarkPositioning) {
        positionMarksByExtents(face, b, direction);
    }
    if (b.backward) {
        b.reverse();
    }
    // Default ignorables draw nothing: the space glyph, or removed if the
    // font has none.
    if (const GlyphId space = face.glyphIndex(U' ')) {
        for (ot::GlyphInfo &g : b.info) {
            if (ot::isDefaultIgnorable(g)) {
                g.glyph = space;
            }
        }
    } else {
        std::size_t w = 0;
        for (std::size_t i = 0; i < b.len(); ++i) {
            if (!ot::isDefaultIgnorable(b.info[i])) {
                b.info[w] = b.info[i];
                b.pos[w] = b.pos[i];
                ++w;
            }
        }
        b.info.resize(w);
        b.pos.resize(w);
    }

    out.resize(b.len());
    for (std::size_t i = 0; i < b.len(); ++i) {
        out[i] = {b.info[i].glyph, b.info[i].cluster, b.pos[i].xAdvance, b.pos[i].yAdvance, b.pos[i].xOffset,
                  b.pos[i].yOffset};
    }
}

} // namespace cfw
