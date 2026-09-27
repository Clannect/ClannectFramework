// UAX #29 grapheme clusters and UAX #14 line breaking against the Unicode
// conformance tests (every case must pass), plus the property lookups.

#include <cstdio>
#include <vector>

#include "cfw/core/Deflate.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"
#include "cfw/text/TextBreaks.h"
#include "cfw/text/Unicode.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

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

// A test line: "÷ 0041 × 0308 ÷ ..." -> code points and the marks between them
// (marks.size() == text.size() + 1; true = break). Returns false for comments.
bool parseCase(StringView line, std::vector<char32_t> &text, std::vector<bool> &marks) {
    text.clear();
    marks.clear();
    const std::size_t hash = line.find('#');
    StringView body = line.substr(0, hash);
    std::size_t i = 0;
    while (i < body.size()) {
        if (body[i] == ' ' || body[i] == '\t') {
            ++i;
            continue;
        }
        if (body.substr(i, 2) == "\xC3\xB7") { // ÷
            marks.push_back(true);
            i += 2;
        } else if (body.substr(i, 2) == "\xC3\x97") { // ×
            marks.push_back(false);
            i += 2;
        } else {
            char32_t cp = 0;
            while (i < body.size() && body[i] != ' ' && body[i] != '\t') {
                const char c = body[i++];
                cp = cp * 16 + static_cast<char32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            text.push_back(cp);
        }
    }
    return !text.empty() && marks.size() == text.size() + 1;
}

template <class Run>
void conformance(const char *file, const char *what, Run run) {
    const String data = readCompressed(file);
    std::size_t cases = 0;
    std::size_t failures = 0;
    std::vector<char32_t> text;
    std::vector<bool> marks;
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t end = data.find('\n', start);
        if (end == String::npos) {
            end = data.size();
        }
        const StringView line(data.data() + start, end - start);
        start = end + 1;
        if (!parseCase(line, text, marks)) {
            continue;
        }
        ++cases;
        const std::vector<bool> got = run(text);
        if (got != marks) {
            if (++failures <= 12) {
                std::printf("  %s mismatch:", what);
                for (std::size_t i = 0; i < got.size(); ++i) {
                    std::printf(" %s%s", got[i] ? "/" : "x", i < text.size() ? "" : "");
                    if (i < text.size()) {
                        std::printf(" %04X", static_cast<unsigned>(text[i]));
                    }
                }
                std::printf("\n    %s\n", String(line.substr(line.find('#'))).c_str());
            }
        }
    }
    std::printf("  %s: %zu cases, %zu failed\n", what, cases, failures);
    check(cases > 500, "the conformance file has its cases");
    checkEqual(failures, std::size_t{0}, "every conformance case passes");
}

void propertyLookups() {
    using namespace unicode;
    check(generalCategory(U'A') == GeneralCategory::Lu && script(U'A') == Script::Latin, "A is an upper-case Latin letter");
    check(script(U'ا') == Script::Arabic && joiningType(U'ا') == JoiningType::R &&
              bidiClass(U'ا') == BidiClass::AL,
          "alef: Arabic, right-joining, AL");
    check(joiningType(U'ب') == JoiningType::D, "beh joins on both sides");
    check(script(U'̀') == Script::Inherited && combiningClass(U'̀') == 230, "combining grave");
    check(script(U' ') == Script::Common && lineBreakClass(U' ') == LineBreakClass::SP, "space");
    check(properties(U'\U0001F600').extendedPictographic && properties(U'\U0001F600').emojiPresentation, "emoji");
    check(pairedBracket(U'(') == U')' && pairedBracket(U']') == U'[' && pairedBracket(U'a') == U'a', "bracket pairs");
    check(mirrored(U'<') == U'>' && mirrored(U'∈') == U'∋', "mirroring");
    check(properties(U'‍').defaultIgnorable && properties(U'­').defaultIgnorable, "default ignorables");
    check(StringView(iso15924(Script::Latin)) == "Latn" && scriptFromIso15924("arab") == Script::Arabic &&
              !scriptFromIso15924("Qqqq").has_value(),
          "ISO 15924 codes");
    check(generalCategory(0x110000) == GeneralCategory::Cn && generalCategory(0xE0000) == GeneralCategory::Cn,
          "beyond Unicode: unassigned");
}

} // namespace

int main() {
    propertyLookups();
    conformance("GraphemeBreakTest.txt.z", "graphemes", [](const std::vector<char32_t> &text) {
        std::vector<std::uint8_t> out;
        graphemeBoundaries(text, out);
        return std::vector<bool>(out.begin(), out.end());
    });
    conformance("LineBreakTest.txt.z", "line breaks", [](const std::vector<char32_t> &text) {
        std::vector<LineBreak> out;
        lineBreaks(text, out);
        std::vector<bool> marks;
        for (const LineBreak b : out) {
            marks.push_back(b != LineBreak::None);
        }
        return marks;
    });
    return cfw::test::finish("TextBreaksTest");
}
