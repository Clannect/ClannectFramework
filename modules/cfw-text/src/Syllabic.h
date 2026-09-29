#pragma once

// Pieces shared by the syllabic shapers (USE, Indic, Khmer, Myanmar):
// character categories, a longest-match scanner for their syllable grammars,
// dotted circles for broken syllables and prohibited vowel sequences.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "OtLayout.h"
#include "cfw/text/FontFace.h"
#include "cfw/text/UnicodeEnums.h"

#include "SyllabicTables.inc"

namespace cfw::syllabic {

[[nodiscard]] UseCat useCategory(char32_t u) noexcept;
[[nodiscard]] IndicCat indicCategory(char32_t u) noexcept;
[[nodiscard]] IndicPos indicPosition(char32_t u) noexcept;

// A regular expression over category numbers (0-63).
struct Re {
    enum class Kind : std::uint8_t { Set, Seq, Alt, Star };
    Kind kind = Kind::Seq;
    std::uint64_t set = 0;
    std::vector<Re> parts;
};

template <typename... C> Re cat(C... c) {
    Re r;
    r.kind = Re::Kind::Set;
    r.set = ((std::uint64_t{1} << static_cast<unsigned>(c)) | ...);
    return r;
}
inline Re any() {
    Re r;
    r.kind = Re::Kind::Set;
    r.set = ~std::uint64_t{0};
    return r;
}
template <typename... R> Re seq(R... parts) { return {Re::Kind::Seq, 0, {parts...}}; }
template <typename... R> Re alt(R... parts) { return {Re::Kind::Alt, 0, {parts...}}; }
inline Re star(Re r) { return {Re::Kind::Star, 0, {std::move(r)}}; }
inline Re opt(Re r) { return alt(std::move(r), Re{}); }
inline Re plus(const Re &r) { return seq(r, star(r)); }

// A scanner over a list of patterns with Ragel's longest-match semantics:
// at each position the longest non-empty match wins, and of equally long
// ones the pattern listed first. Compiled to a DFA once.
class Scanner {
public:
    explicit Scanner(const std::vector<Re> &patterns);
    // The pattern matching longest at cats[0..n), and its length; -1 if none.
    int match(const std::uint8_t *cats, std::size_t n, std::size_t &length) const noexcept;

private:
    std::vector<std::int16_t> m_next; // state * 64 + category -> state, -1 dead
    std::vector<std::int16_t> m_accept;
};

// The end of the syllable starting at `start`, as HarfBuzz iterates them (at
// most 64 glyphs at a time).
std::size_t nextSyllable(const ot::Buffer &b, std::size_t start) noexcept;

// Inserts a dotted circle into each broken syllable (after a leading repha).
// Returns whether any went in.
bool insertDottedCircles(const FontFace &face, ot::Buffer &b, std::uint8_t brokenType, std::uint8_t circleCategory,
                         int rephaCategory, int circlePosition = -1);

// Before normalisation: a dotted circle into vowel sequences the script's
// spec prohibits (they look like another vowel).
void insertVowelConstraintCircles(ot::Buffer &b, unicode::Script script);

// ---- Universal Shaping Engine ----

enum UseSyllable : std::uint8_t {
    UseViramaTerminated,
    UseSakotTerminated,
    UseStandard,
    UseNumberJoinerTerminated,
    UseNumeral,
    UseSymbol,
    UseHieroglyph,
    UseBroken,
    UseNonCluster,
};

// Categories from code points (after normalisation).
void setUseCategories(ot::Buffer &b);
// Marks syllables; true if a broken one was found.
bool findUseSyllables(ot::Buffer &b);
// Reorders each syllable (repha to the end of the base cluster, pre-base
// vowels before the base).
void reorderUse(ot::Buffer &b);

// ---- Indic, Khmer and Myanmar ----
//
// These three engines use the character categories with HarfBuzz's machine
// numbering, which folds some together (A and VD; Myanmar's IV, DB and GB
// with V, N and PLACEHOLDER), so the syllable grammars treat them alike.

namespace mcat {
enum : std::uint8_t {
    X = 0, C = 1, V = 2, N = 3, H = 4, ZWNJ = 5, ZWJ = 6, M = 7, SM = 8, A = 9, PLACEHOLDER = 10, DOTTEDCIRCLE = 11,
    RS = 12, MPst = 13, Repha = 14, Ra = 15, CM = 16, Symbol = 17, CS = 18, VAbv = 20, VBlw = 21, VPre = 22,
    VPst = 23, Robatic = 25, Xgroup = 26, Ygroup = 27, As = 32, MH = 35, MR = 36, MW = 37, MY = 38, PT = 39, VS = 40,
    ML = 41, SMPst = 57,
    // Myanmar names for the shared numbers.
    IV = V, DB = N, GB = PLACEHOLDER,
};
} // namespace mcat

// Categories (and, for Indic, positions) from code points, after normalisation.
void setMachineCategories(ot::Buffer &b, bool positions);

enum IndicSyllable : std::uint8_t { IndicConsonant, IndicVowel, IndicStandalone, IndicSymbol, IndicBroken, IndicNonIndic };
enum KhmerSyllable : std::uint8_t { KhmerConsonant, KhmerBroken, KhmerNonKhmer };
enum MyanmarSyllable : std::uint8_t { MyanmarConsonant, MyanmarBroken, MyanmarNonMyanmar };

// Marks syllables; true if a broken one was found.
bool findIndicSyllables(ot::Buffer &b);
bool findKhmerSyllables(ot::Buffer &b);
bool findMyanmarSyllables(ot::Buffer &b);

// What the Indic engine knows about a script and a font's lookups.
struct IndicPlan {
    enum class RephMode : std::uint8_t { Implicit, Explicit, LogRepha };
    unicode::Script script = unicode::Script::Common;
    bool oldSpec = false;                 // the font has the first spec's tags (deva, not dev2)
    GlyphId viramaGlyph = 0;
    std::uint8_t rephPosition = static_cast<std::uint8_t>(IndicPos::BEFORE_POST);
    RephMode rephMode = RephMode::Implicit;
    bool blwfPostOnly = false;            // below-base forms only after the base
    std::uint32_t rphfMask = 0, prefMask = 0, blwfMask = 0, abvfMask = 0, halfMask = 0, pstfMask = 0, initMask = 0;
    // Each feature's GSUB stage, for "would it substitute" tests.
    const ot::LayoutTable *gsub = nullptr;
    std::vector<std::uint16_t> rphf, pref, blwf, pstf, vatu;
    bool zeroContext = false;

    // Script configuration (virama, reph position and mode, below-forms mode).
    void configure(unicode::Script s, bool oldSpecTags);
    [[nodiscard]] bool wouldSubstitute(const std::vector<std::uint16_t> &lookups, Span<const GlyphId> glyphs) const;
};

// Before the basic features: consonant positions from the font, dotted
// circles into broken syllables, reordering, and the feature masks.
void initialReorderingIndic(const FontFace &face, const IndicPlan &plan, ot::Buffer &b, bool broken);
// After them: pre-base matras, reph and pre-base-reordering consonants move
// to where the font's forms need them.
void finalReorderingIndic(const IndicPlan &plan, ot::Buffer &b);

struct KhmerPlan {
    std::uint32_t prefMask = 0, blwfMask = 0, abvfMask = 0, pstfMask = 0, cfarMask = 0;
};
void reorderKhmer(const FontFace &face, const KhmerPlan &plan, ot::Buffer &b, bool broken);
void reorderMyanmar(const FontFace &face, ot::Buffer &b, bool broken);

} // namespace cfw::syllabic
