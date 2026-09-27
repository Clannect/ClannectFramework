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

} // namespace cfw::syllabic
