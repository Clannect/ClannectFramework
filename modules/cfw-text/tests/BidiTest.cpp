// The bidi algorithm against the Unicode conformance tests: BidiTest.txt
// (bidi classes, every paragraph direction) and BidiCharacterTest.txt (real
// text with brackets). Every case must give the expected levels and visual
// order.

#include <cstdio>
#include <vector>

#include "cfw/core/Deflate.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"
#include "cfw/text/Bidi.h"
#include "cfw/text/Unicode.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;
using BC = unicode::BidiClass;

namespace {

const Path kData(CFW_TEXT_TESTDATA);

String readCompressed(const char *name) {
    const Result<std::vector<std::byte>> file = readFile(kData / "ucd" / name);
    check(file.ok(), "conformance file reads");
    if (!file) {
        return {};
    }
    const Result<std::vector<std::byte>> text = zlibDecompress(file.value());
    check(text.ok(), "and decompresses");
    return text ? String(reinterpret_cast<const char *>(text.value().data()), text.value().size()) : String();
}

std::vector<StringView> split(StringView s, char sep) {
    std::vector<StringView> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t e = s.find(sep, start);
        out.push_back(s.substr(start, e == StringView::npos ? StringView::npos : e - start));
        if (e == StringView::npos) {
            return out;
        }
        start = e + 1;
    }
}

std::vector<StringView> words(StringView s) {
    std::vector<StringView> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        const std::size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
            ++i;
        }
        if (i > b) {
            out.push_back(s.substr(b, i - b));
        }
    }
    return out;
}

// Decimal or hexadecimal digits, without allocating.
unsigned long number(StringView w, int base = 10) {
    unsigned long v = 0;
    for (const char c : w) {
        v = v * static_cast<unsigned long>(base) +
            static_cast<unsigned long>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

bool isRemoved(BC c) {
    return c == BC::RLE || c == BC::LRE || c == BC::RLO || c == BC::LRO || c == BC::PDF || c == BC::BN;
}

BC classNamed(StringView name) {
    static const char *const names[] = {"L",  "R",  "AL",  "EN", "ES", "ET",  "AN",  "CS",  "NSM", "BN",  "B",  "S",
                                        "WS", "ON", "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"};
    for (std::size_t i = 0; i < std::size(names); ++i) {
        if (name == names[i]) {
            return static_cast<BC>(i);
        }
    }
    check(false, "known bidi class");
    return BC::L;
}

// Levels after L1 (the paragraph as one line) and the visual order without
// removed characters, as the test files state them.
void run(Span<const BC> classes, Span<const char32_t> text, std::optional<TextDirection> dir, BidiParagraph &p,
         std::vector<int> &levels, std::vector<std::uint32_t> &order) {
    resolveBidi(classes, text, dir, p);
    applyBidiLineRules(classes, p.level, p.levels, 0, classes.size());
    levels.clear();
    static std::vector<std::uint8_t> kept;
    static std::vector<std::uint32_t> index;
    static std::vector<std::uint32_t> visual;
    kept.clear();
    index.clear();
    for (std::size_t i = 0; i < classes.size(); ++i) {
        levels.push_back(isRemoved(classes[i]) ? -1 : p.levels[i]);
        if (!isRemoved(classes[i])) {
            kept.push_back(p.levels[i]);
            index.push_back(static_cast<std::uint32_t>(i));
        }
    }
    bidiVisualOrder(kept, visual);
    order.clear();
    for (const std::uint32_t v : visual) {
        order.push_back(index[v]);
    }
}

void bidiTest() {
    const String data = readCompressed("BidiTest.txt.z");
    std::vector<int> expectedLevels;
    std::vector<std::uint32_t> expectedOrder;
    std::size_t cases = 0;
    std::size_t failures = 0;
    BidiParagraph p;
    std::vector<int> levels;
    std::vector<std::uint32_t> order;
    for (const StringView raw : split(data, '\n')) {
        const StringView line = raw.substr(0, raw.find('#'));
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("@Levels:")) {
            expectedLevels.clear();
            for (const StringView w : words(line.substr(8))) {
                expectedLevels.push_back(w == "x" ? -1 : static_cast<int>(number(w)));
            }
            continue;
        }
        if (line.starts_with("@Reorder:")) {
            expectedOrder.clear();
            for (const StringView w : words(line.substr(9))) {
                expectedOrder.push_back(static_cast<std::uint32_t>(number(w)));
            }
            continue;
        }
        if (line.starts_with("@")) {
            continue;
        }
        const std::vector<StringView> fields = split(line, ';');
        if (fields.size() < 2) {
            continue;
        }
        std::vector<BC> classes;
        for (const StringView w : words(fields[0])) {
            classes.push_back(classNamed(w));
        }
        const int set = static_cast<int>(number(words(fields[1]).front()));
        for (int bit = 0; bit < 3; ++bit) {
            if (!(set & (1 << bit))) {
                continue;
            }
            const std::optional<TextDirection> dir =
                bit == 0 ? std::nullopt
                         : std::optional<TextDirection>(bit == 1 ? TextDirection::LeftToRight : TextDirection::RightToLeft);
            run(classes, {}, dir, p, levels, order);
            ++cases;
            if (levels != expectedLevels || order != expectedOrder) {
                if (++failures <= 10) {
                    std::printf("  BidiTest mismatch: %s (bit %d); levels", String(fields[0]).c_str(), bit);
                    for (const int l : levels) {
                        std::printf(" %d", l);
                    }
                    std::printf(" expected");
                    for (const int l : expectedLevels) {
                        std::printf(" %d", l);
                    }
                    std::printf("\n");
                }
            }
        }
    }
    std::printf("  BidiTest: %zu cases, %zu failed\n", cases, failures);
    check(cases > 100000, "BidiTest has its cases");
    checkEqual(failures, std::size_t{0}, "every BidiTest case passes");
}

void bidiCharacterTest() {
    const String data = readCompressed("BidiCharacterTest.txt.z");
    std::size_t cases = 0;
    std::size_t failures = 0;
    BidiParagraph p;
    std::vector<int> levels;
    std::vector<std::uint32_t> order;
    for (const StringView raw : split(data, '\n')) {
        if (raw.empty() || raw[0] == '#') {
            continue;
        }
        const std::vector<StringView> f = split(raw, ';');
        if (f.size() < 5) {
            continue;
        }
        std::vector<char32_t> text;
        std::vector<BC> classes;
        for (const StringView w : words(f[0])) {
            text.push_back(static_cast<char32_t>(number(w, 16)));
            classes.push_back(unicode::bidiClass(text.back()));
        }
        const int d = static_cast<int>(number(f[1]));
        const std::optional<TextDirection> dir =
            d == 2 ? std::nullopt : std::optional<TextDirection>(d == 0 ? TextDirection::LeftToRight : TextDirection::RightToLeft);
        std::vector<int> expectedLevels;
        for (const StringView w : words(f[3])) {
            expectedLevels.push_back(w == "x" ? -1 : static_cast<int>(number(w)));
        }
        std::vector<std::uint32_t> expectedOrder;
        for (const StringView w : words(f[4])) {
            expectedOrder.push_back(static_cast<std::uint32_t>(number(w)));
        }
        run(classes, text, dir, p, levels, order);
        ++cases;
        if (levels != expectedLevels || order != expectedOrder || p.level != static_cast<int>(number(f[2]))) {
            if (++failures <= 10) {
                std::printf("  BidiCharacterTest mismatch: %s\n", String(raw).c_str());
                std::printf("    got levels");
                for (const int l : levels) {
                    std::printf(" %d", l);
                }
                std::printf("\n");
            }
        }
    }
    std::printf("  BidiCharacterTest: %zu cases, %zu failed\n", cases, failures);
    check(cases > 90000, "BidiCharacterTest has its cases");
    checkEqual(failures, std::size_t{0}, "every BidiCharacterTest case passes");
}

void examples() {
    // "car is THE CAR in arabic" with Hebrew letters for the capitals.
    const std::u32string text = U"car אבג is";
    BidiParagraph p;
    resolveBidi(Span<const char32_t>(text.data(), text.size()), std::nullopt, p);
    checkEqual(static_cast<int>(p.level), 0, "a paragraph starting with Latin is left-to-right");
    check(p.levels[4] == 1 && p.levels[5] == 1 && p.levels[0] == 0, "the Hebrew word is right-to-left");
    std::vector<std::uint32_t> order;
    bidiVisualOrder(p.levels, order);
    check(order[4] == 6 && order[6] == 4, "and is displayed reversed");
}

} // namespace

// "classes" runs BidiTest.txt, "characters" BidiCharacterTest.txt (two CTest
// tests, so each stays within the time budget); no argument runs both.
int main(int argc, char **argv) {
    const StringView which = argc > 1 ? StringView(argv[1]) : StringView();
    examples();
    if (which.empty() || which == "classes") {
        bidiTest();
    }
    if (which.empty() || which == "characters") {
        bidiCharacterTest();
    }
    return cfw::test::finish("BidiTest");
}
