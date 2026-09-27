// UTF-8 validation and UTF-16 conversion, including the malformed sequences
// that decoders classically get wrong (overlongs, surrogates, truncation).

#include "cfw/core/Utf8.h"

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

StringView bytes(const char *text) { return StringView(text); }

void acceptsWellFormedText() {
    check(isValidUtf8(""), "empty text is valid");
    check(isValidUtf8("plain ASCII"), "ASCII is valid");
    check(isValidUtf8("caf\xC3\xA9"), "2-byte sequence (é)");
    check(isValidUtf8("\xE6\x97\xA5\xE6\x9C\xAC"), "3-byte sequences (日本)");
    check(isValidUtf8("\xF0\x9F\x98\x80"), "4-byte sequence (emoji)");
    check(isValidUtf8("\xF4\x8F\xBF\xBF"), "U+10FFFF is the last valid code point");
    check(isValidUtf8("\xEF\xBF\xBD"), "U+FFFD itself is valid");
}

void rejectsMalformedText() {
    check(!isValidUtf8("\x80"), "stray continuation byte");
    check(!isValidUtf8("\xC0\xAF"), "overlong '/' (C0)");
    check(!isValidUtf8("\xC1\xBF"), "overlong (C1)");
    check(!isValidUtf8("\xE0\x80\xAF"), "overlong 3-byte");
    check(!isValidUtf8("\xF0\x80\x80\xAF"), "overlong 4-byte");
    check(!isValidUtf8("\xED\xA0\x80"), "encoded high surrogate");
    check(!isValidUtf8("\xED\xBF\xBF"), "encoded low surrogate");
    check(!isValidUtf8("\xF4\x90\x80\x80"), "above U+10FFFF");
    check(!isValidUtf8("\xF5\x80\x80\x80"), "F5 lead byte");
    check(!isValidUtf8("\xFF"), "FF byte");
    check(!isValidUtf8("\xE6\x97"), "truncated 3-byte sequence");
    check(!isValidUtf8("ok\xC3"), "truncated at end of text");
    check(!isValidUtf8(StringView("a\0\xC3", 3)), "truncated after an embedded NUL");
}

void reportsWhereTheErrorIs() {
    checkEqual(findInvalidUtf8("abc\xFF" "def"), std::size_t(3), "offset of first bad byte");
    checkEqual(findInvalidUtf8("abc"), std::size_t(3), "size() when valid");
}

void decodesAndResynchronises() {
    const StringView text = bytes("\xE6\x97\xA5x");
    const Utf8Char first = decodeUtf8At(text, 0);
    check(first.valid, "decodes 日");
    checkEqual(static_cast<std::uint32_t>(first.codepoint), 0x65E5u, "日 is U+65E5");
    checkEqual(static_cast<int>(first.length), 3, "日 is three bytes");

    // E6 97 followed by 'x': the maximal subpart is two bytes, so 'x' survives.
    const StringView broken = bytes("\xE6\x97x");
    const Utf8Char bad = decodeUtf8At(broken, 0);
    check(!bad.valid, "truncated sequence is invalid");
    checkEqual(static_cast<int>(bad.length), 2, "skips the maximal ill-formed subpart");
    checkEqual(sanitizeUtf8(broken), String("\xEF\xBF\xBDx"), "sanitize keeps the following character");
    checkEqual(countCodepoints(broken), std::size_t(2), "one replacement plus 'x'");
}

void appendsEveryRange() {
    String out;
    appendUtf8(out, U'A');
    appendUtf8(out, U'é');
    appendUtf8(out, U'日');
    appendUtf8(out, U'\U0001F600');
    checkEqual(out, String("A\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"), "1-4 byte encodings");

    String surrogate;
    appendUtf8(surrogate, static_cast<char32_t>(0xD800));
    checkEqual(surrogate, String("\xEF\xBF\xBD"), "a surrogate appends U+FFFD");
}

void roundTripsThroughUtf16() {
    const String text = "Clannect \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x98\x80";
    const Result<std::u16string> wide = utf8ToUtf16(text);
    check(wide.ok(), "valid text converts to UTF-16");
    checkEqual(wide.value().size(), std::size_t(14), "emoji becomes a surrogate pair");
    const Result<String> back = utf16ToUtf8(wide.value());
    check(back.ok() && back.value() == text, "UTF-8 -> UTF-16 -> UTF-8 round-trips");

    check(!utf8ToUtf16("bad\xFF").ok(), "strict conversion rejects invalid UTF-8");
}

void handlesUnpairedSurrogates() {
    const std::u16string lone = {u'a', static_cast<char16_t>(0xD800), u'b'};
    const Result<String> strict = utf16ToUtf8(lone);
    check(!strict.ok(), "strict conversion rejects an unpaired surrogate");
    checkEqual(strict.error().context()[0].second, String("1"), "reports the unit offset");
    checkEqual(utf16ToUtf8Lossy(lone), String("a\xEF\xBF\xBD" "b"), "lossy conversion substitutes U+FFFD");
}

void validatesEveryCodepointEncoding() {
    // Exhaustive: every scalar value encodes to valid UTF-8 and decodes back.
    int failures = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            continue;
        }
        String out;
        appendUtf8(out, cp);
        const Utf8Char decoded = decodeUtf8At(out, 0);
        if (!decoded.valid || decoded.codepoint != cp || decoded.length != out.size()) {
            ++failures;
        }
    }
    checkEqual(failures, 0, "every Unicode scalar value round-trips");
}

void ordersLikeUtf16() {
    check(compareUtf16Order("B", "a") < 0, "ASCII order is byte order");
    check(compareUtf16Order("ab", "abc") < 0, "prefix sorts first");
    const StringView emoji = "\xF0\x9F\x98\x80";   // U+1F600, a surrogate pair in UTF-16
    const StringView fullwidthA = "\xEF\xBC\xA1"; // U+FF21
    check(compareUtf16Order(emoji, fullwidthA) < 0, "U+1F600 sorts before U+FF21 in UTF-16 order");
    check(compareUtf16Order(fullwidthA, emoji) > 0, "and the reverse comparison agrees");
    check(compareUtf16Order("a\xF0\x9F\x98\x80", "a\xF0\x9F\x98\x81") < 0, "two supplementary characters: code point order");
    check(compareUtf16Order("\xED\x9F\xBF", "\xF0\x9F\x98\x80") < 0, "U+D7FF still sorts before a surrogate pair");
    check(emoji > fullwidthA, "(byte order says the opposite)");
    check(compareUtf16Order("\xC3\xA9", "\xC3\xA9") == 0, "equal");
}

} // namespace

int main() {
    acceptsWellFormedText();
    rejectsMalformedText();
    reportsWhereTheErrorIs();
    decodesAndResynchronises();
    appendsEveryRange();
    roundTripsThroughUtf16();
    handlesUnpairedSurrogates();
    validatesEveryCodepointEncoding();
    ordersLikeUtf16();
    return cfw::test::finish("Utf8Test");
}
