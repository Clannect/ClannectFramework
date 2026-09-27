#include "cfw/text/TextBreaks.h"

#include "cfw/text/Unicode.h"

namespace cfw {

using unicode::GeneralCategory;
using unicode::GraphemeBreak;
using unicode::IndicConjunctBreak;
using LB = unicode::LineBreakClass;

// ---- Grapheme clusters (UAX #29) ----

void graphemeBoundaries(Span<const char32_t> text, std::vector<std::uint8_t> &out) {
    const std::size_t n = text.size();
    out.assign(n + 1, 0);
    out[n] = 1;
    if (n == 0) {
        return;
    }
    out[0] = 1;
    unicode::Properties prev = unicode::properties(text[0]);
    // GB11: inside ExtPict Extend* ZWJ?  GB9c: after Linker Extend*?
    bool pictographic = prev.extendedPictographic;
    bool linker = prev.indicConjunctBreak == IndicConjunctBreak::Linker;
    std::size_t regional = prev.graphemeBreak == GraphemeBreak::RI ? 1 : 0; // RIs in a row
    for (std::size_t i = 1; i < n; ++i) {
        const unicode::Properties cur = unicode::properties(text[i]);
        const GraphemeBreak a = prev.graphemeBreak;
        const GraphemeBreak b = cur.graphemeBreak;
        bool boundary = true;
        if (a == GraphemeBreak::CR && b == GraphemeBreak::LF) {
            boundary = false; // GB3
        } else if (a == GraphemeBreak::CN || a == GraphemeBreak::CR || a == GraphemeBreak::LF) {
            boundary = true; // GB4
        } else if (b == GraphemeBreak::CN || b == GraphemeBreak::CR || b == GraphemeBreak::LF) {
            boundary = true; // GB5
        } else if (a == GraphemeBreak::L && (b == GraphemeBreak::L || b == GraphemeBreak::V ||
                                             b == GraphemeBreak::LV || b == GraphemeBreak::LVT)) {
            boundary = false; // GB6
        } else if ((a == GraphemeBreak::LV || a == GraphemeBreak::V) && (b == GraphemeBreak::V || b == GraphemeBreak::T)) {
            boundary = false; // GB7
        } else if ((a == GraphemeBreak::LVT || a == GraphemeBreak::T) && b == GraphemeBreak::T) {
            boundary = false; // GB8
        } else if (b == GraphemeBreak::EX || b == GraphemeBreak::ZWJ) {
            boundary = false; // GB9
        } else if (b == GraphemeBreak::SM) {
            boundary = false; // GB9a
        } else if (a == GraphemeBreak::PP) {
            boundary = false; // GB9b
        } else if (linker && cur.indicConjunctBreak == IndicConjunctBreak::Consonant) {
            boundary = false; // GB9c
        } else if (pictographic && a == GraphemeBreak::ZWJ && cur.extendedPictographic) {
            boundary = false; // GB11
        } else if (a == GraphemeBreak::RI && b == GraphemeBreak::RI && regional % 2 == 1) {
            boundary = false; // GB12, GB13
        }
        out[i] = boundary ? 1 : 0;

        // Update the context for the next pair.
        if (cur.extendedPictographic) {
            pictographic = true;
        } else if (!(b == GraphemeBreak::EX || (b == GraphemeBreak::ZWJ && a != GraphemeBreak::ZWJ))) {
            pictographic = false; // GB11 allows ExtPict Extend* ZWJ, one ZWJ
        } else if (b == GraphemeBreak::ZWJ && a == GraphemeBreak::ZWJ) {
            pictographic = false;
        }
        linker = cur.indicConjunctBreak == IndicConjunctBreak::Linker ||
                 (linker && cur.indicConjunctBreak == IndicConjunctBreak::Extend);
        regional = b == GraphemeBreak::RI ? regional + 1 : 0;
        prev = cur;
    }
}

// ---- Line breaking (UAX #14) ----

namespace {

struct Unit {
    std::size_t start;   // index of the base character
    LB cls;              // class after LB1 and LB9/LB10
    bool endsWithZwj;    // LB8a
    bool pi;             // quotation mark with General_Category Pi
    bool pf;             // ... Pf
    bool eastAsian;      // East_Asian_Width F, W or H
    bool pictographicCn; // Extended_Pictographic and unassigned (LB30b)
    bool dottedCircle;   // U+25CC (LB28a)
};

// LB1: resolve the classes the default algorithm does not use directly.
LB resolve(const unicode::Properties &p) {
    switch (p.lineBreak) {
    case LB::AI:
    case LB::SG:
    case LB::XX: return LB::AL;
    case LB::SA:
        return p.generalCategory == GeneralCategory::Mn || p.generalCategory == GeneralCategory::Mc ? LB::CM : LB::AL;
    case LB::CJ: return LB::NS;
    default: return p.lineBreak;
    }
}

bool in(LB c, std::initializer_list<LB> set) {
    for (const LB s : set) {
        if (c == s) {
            return true;
        }
    }
    return false;
}

} // namespace

void lineBreaks(Span<const char32_t> text, std::vector<LineBreak> &out) {
    const std::size_t n = text.size();
    out.assign(n + 1, LineBreak::None);
    if (n == 0) {
        out[0] = LineBreak::Mandatory;
        return;
    }
    out[n] = LineBreak::Mandatory; // LB3

    // LB9/LB10: a base with its combining marks is one unit, with the base's
    // class; marks with nothing to attach to become AL.
    thread_local std::vector<Unit> units;
    units.clear();
    for (std::size_t i = 0; i < n; ++i) {
        const unicode::Properties p = unicode::properties(text[i]);
        const LB c = resolve(p);
        if ((c == LB::CM || c == LB::ZWJ) && !units.empty() &&
            !in(units.back().cls, {LB::BK, LB::CR, LB::LF, LB::NL, LB::SP, LB::ZW})) {
            units.back().endsWithZwj = c == LB::ZWJ;
            continue; // LB9: attached, and no break inside
        }
        Unit u{};
        u.start = i;
        u.cls = (c == LB::CM || c == LB::ZWJ) ? LB::AL : c; // LB10
        u.endsWithZwj = c == LB::ZWJ;
        u.pi = c == LB::QU && p.generalCategory == GeneralCategory::Pi;
        u.pf = c == LB::QU && p.generalCategory == GeneralCategory::Pf;
        u.eastAsian = p.eastAsianWidth == unicode::EastAsianWidth::F ||
                      p.eastAsianWidth == unicode::EastAsianWidth::W || p.eastAsianWidth == unicode::EastAsianWidth::H;
        u.pictographicCn = p.extendedPictographic && p.generalCategory == GeneralCategory::Cn;
        u.dottedCircle = text[i] == U'◌';
        units.push_back(u);
    }

    const std::size_t count = units.size();
    const auto cls = [&](std::ptrdiff_t k) { return units[static_cast<std::size_t>(k)].cls; };
    // The unit before `k` skipping spaces, or -1.
    const auto beforeSpaces = [&](std::ptrdiff_t k) {
        while (k >= 0 && cls(k) == LB::SP) {
            --k;
        }
        return k;
    };
    const auto aksara = [&](std::ptrdiff_t k) {
        return k >= 0 && k < static_cast<std::ptrdiff_t>(count) &&
               (cls(k) == LB::AK || cls(k) == LB::AS || units[static_cast<std::size_t>(k)].dottedCircle);
    };

    std::size_t regional = 0; // RI units in a row, ending at the unit before the boundary
    for (std::size_t j = 1; j < count; ++j) {
        const auto J = static_cast<std::ptrdiff_t>(j);
        const Unit &ua = units[j - 1];
        const Unit &ub = units[j];
        const LB a = ua.cls;
        const LB b = ub.cls;
        regional = a == LB::RI ? regional + 1 : 0;
        const LB next = j + 1 < count ? units[j + 1].cls : LB::XX;
        const bool eot = j + 1 >= count;
        const LB before = j >= 2 ? units[j - 2].cls : LB::XX;
        const bool sot = j < 2;
        const std::ptrdiff_t spaced = beforeSpaces(J - 1); // the unit before any spaces
        LineBreak r = LineBreak::Allowed;

        if (a == LB::BK) {
            r = LineBreak::Mandatory; // LB4
        } else if (a == LB::CR && b == LB::LF) {
            r = LineBreak::None; // LB5
        } else if (in(a, {LB::CR, LB::LF, LB::NL})) {
            r = LineBreak::Mandatory; // LB5
        } else if (in(b, {LB::BK, LB::CR, LB::LF, LB::NL})) {
            r = LineBreak::None; // LB6
        } else if (in(b, {LB::SP, LB::ZW})) {
            r = LineBreak::None; // LB7
        } else if (spaced >= 0 && cls(spaced) == LB::ZW) {
            r = LineBreak::Allowed; // LB8
        } else if (ua.endsWithZwj) {
            r = LineBreak::None; // LB8a
        } else if (a == LB::WJ || b == LB::WJ) {
            r = LineBreak::None; // LB11
        } else if (a == LB::GL) {
            r = LineBreak::None; // LB12
        } else if (b == LB::GL && !in(a, {LB::SP, LB::HY, LB::HH})) {
            r = LineBreak::None; // LB12a
        } else if (in(b, {LB::CL, LB::CP, LB::EX, LB::SY})) {
            r = LineBreak::None; // LB13
        } else if (spaced >= 0 && cls(spaced) == LB::OP) {
            r = LineBreak::None; // LB14
        } else if (spaced >= 0 && units[static_cast<std::size_t>(spaced)].pi &&
                   (spaced == 0 || in(cls(spaced - 1), {LB::BK, LB::CR, LB::LF, LB::NL, LB::OP, LB::QU, LB::GL, LB::SP, LB::ZW}))) {
            r = LineBreak::None; // LB15a
        } else if (ub.pf && (eot || in(next, {LB::SP, LB::GL, LB::WJ, LB::CL, LB::QU, LB::CP, LB::EX, LB::IS, LB::SY,
                                              LB::BK, LB::CR, LB::LF, LB::NL, LB::ZW}))) {
            r = LineBreak::None; // LB15b
        } else if (a == LB::SP && b == LB::IS && next == LB::NU && !eot) {
            r = LineBreak::Allowed; // LB15c
        } else if (b == LB::IS) {
            r = LineBreak::None; // LB15d
        } else if (b == LB::NS && spaced >= 0 && (cls(spaced) == LB::CL || cls(spaced) == LB::CP)) {
            r = LineBreak::None; // LB16
        } else if (b == LB::B2 && spaced >= 0 && cls(spaced) == LB::B2) {
            r = LineBreak::None; // LB17
        } else if (a == LB::SP) {
            r = LineBreak::Allowed; // LB18
        } else if ((b == LB::QU && !ub.pi) || (a == LB::QU && !ua.pf)) {
            r = LineBreak::None; // LB19
        } else if ((b == LB::QU && !ua.eastAsian) || (b == LB::QU && (eot || !units[j + 1].eastAsian)) ||
                   (a == LB::QU && !ub.eastAsian) || (a == LB::QU && (sot || !units[j - 2].eastAsian))) {
            r = LineBreak::None; // LB19a
        } else if (a == LB::CB || b == LB::CB) {
            r = LineBreak::Allowed; // LB20
        } else if (in(a, {LB::HY, LB::HH}) && in(b, {LB::AL, LB::HL}) &&
                   (sot || in(before, {LB::BK, LB::CR, LB::LF, LB::NL, LB::SP, LB::ZW, LB::CB, LB::GL}))) {
            r = LineBreak::None; // LB20a
        } else if (in(b, {LB::BA, LB::HY, LB::HH, LB::NS}) || a == LB::BB) {
            r = LineBreak::None; // LB21
        } else if (!sot && before == LB::HL && in(a, {LB::HY, LB::HH}) && b != LB::HL) {
            r = LineBreak::None; // LB21a
        } else if (a == LB::SY && b == LB::HL) {
            r = LineBreak::None; // LB21b
        } else if (b == LB::IN) {
            r = LineBreak::None; // LB22
        } else if ((in(a, {LB::AL, LB::HL}) && b == LB::NU) || (a == LB::NU && in(b, {LB::AL, LB::HL}))) {
            r = LineBreak::None; // LB23
        } else if ((a == LB::PR && in(b, {LB::ID, LB::EB, LB::EM})) || (in(a, {LB::ID, LB::EB, LB::EM}) && b == LB::PO)) {
            r = LineBreak::None; // LB23a
        } else if ((in(a, {LB::PR, LB::PO}) && in(b, {LB::AL, LB::HL})) || (in(a, {LB::AL, LB::HL}) && in(b, {LB::PR, LB::PO}))) {
            r = LineBreak::None; // LB24
        } else if ((in(a, {LB::PR, LB::PO}) && b == LB::NU) ||
                   (in(a, {LB::PR, LB::PO}) && in(b, {LB::OP, LB::HY}) && next == LB::NU && !eot) ||
                   (in(a, {LB::OP, LB::HY, LB::IS}) && b == LB::NU) || (a == LB::NU && in(b, {LB::NU, LB::SY, LB::IS}))) {
            r = LineBreak::None; // LB25 (the simple cases)
        } else if ([&] {
                       // LB25: NU (NU | SY | IS)* (CL | CP)? x (PO | PR), and NU (NU | SY | IS)* x (NU | SY | IS | CL | CP)
                       std::ptrdiff_t k = J - 1;
                       if (in(b, {LB::PO, LB::PR}) && (cls(k) == LB::CL || cls(k) == LB::CP)) {
                           --k;
                       } else if (!in(b, {LB::NU, LB::SY, LB::IS, LB::CL, LB::CP, LB::PO, LB::PR})) {
                           return false;
                       }
                       while (k >= 0 && in(cls(k), {LB::SY, LB::IS})) {
                           --k;
                       }
                       return k >= 0 && cls(k) == LB::NU;
                   }()) {
            r = LineBreak::None; // LB25
        } else if ((a == LB::JL && in(b, {LB::JL, LB::JV, LB::H2, LB::H3})) ||
                   (in(a, {LB::JV, LB::H2}) && in(b, {LB::JV, LB::JT})) || (in(a, {LB::JT, LB::H3}) && b == LB::JT)) {
            r = LineBreak::None; // LB26
        } else if ((in(a, {LB::JL, LB::JV, LB::JT, LB::H2, LB::H3}) && b == LB::PO) ||
                   (a == LB::PR && in(b, {LB::JL, LB::JV, LB::JT, LB::H2, LB::H3}))) {
            r = LineBreak::None; // LB27
        } else if (in(a, {LB::AL, LB::HL}) && in(b, {LB::AL, LB::HL})) {
            r = LineBreak::None; // LB28
        } else if ((a == LB::AP && aksara(J)) || (aksara(J - 1) && in(b, {LB::VF, LB::VI})) ||
                   (a == LB::VI && aksara(J - 2) && (b == LB::AK || ub.dottedCircle)) ||
                   (aksara(J - 1) && aksara(J) && next == LB::VF && !eot)) {
            r = LineBreak::None; // LB28a
        } else if (a == LB::IS && in(b, {LB::AL, LB::HL})) {
            r = LineBreak::None; // LB29
        } else if ((in(a, {LB::AL, LB::HL, LB::NU}) && b == LB::OP && !ub.eastAsian) ||
                   (a == LB::CP && !ua.eastAsian && in(b, {LB::AL, LB::HL, LB::NU}))) {
            r = LineBreak::None; // LB30
        } else if (a == LB::RI && b == LB::RI && regional % 2 == 1) {
            r = LineBreak::None; // LB30a
        } else if ((a == LB::EB || ua.pictographicCn) && b == LB::EM) {
            r = LineBreak::None; // LB30b
        }
        out[ub.start] = r; // LB31: otherwise Allowed
    }
}

} // namespace cfw
