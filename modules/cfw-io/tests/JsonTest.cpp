// JSON: parse and write, and above all byte-identical round-trips of files the
// Qt build wrote (spec M1 exit criterion). testdata/qt_*.json were produced by
// Qt 6.8.3's QJsonDocument from an edge-case document (numbers, escapes, empty
// containers, UTF-16 key order); roundtrip_test.cescene is a real scene file
// saved by the Qt build of the engine.

#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

#include <cmath>
#include <limits>

#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

String fixture(StringView name) {
    const Result<String> text = readTextFile(Path(CFW_IO_TESTDATA) / name);
    check(text.ok(), "fixture readable");
    return text ? text.value() : String();
}

void roundTripIsByteIdentical(StringView name, JsonFormat format) {
    const String original = fixture(name);
    const Result<JsonValue> parsed = parseJson(original);
    check(parsed.ok(), "Qt-written file parses");
    if (!parsed) {
        std::printf("      %s\n", parsed.error().describe().c_str());
        return;
    }
    const String rewritten = writeJson(parsed.value(), format);
    check(rewritten == original, "parse + write reproduces the Qt build's bytes exactly");
    if (rewritten != original) {
        std::size_t i = 0;
        while (i < rewritten.size() && i < original.size() && rewritten[i] == original[i]) {
            ++i;
        }
        std::printf("      %s: first difference at byte %zu:\n      qt: [%s]\n      ce: [%s]\n", String(name).c_str(), i,
                    original.substr(i > 20 ? i - 20 : 0, 60).c_str(), rewritten.substr(i > 20 ? i - 20 : 0, 60).c_str());
    }
}

void qtFilesRoundTrip() {
    roundTripIsByteIdentical("qt_indented.json", JsonFormat::Indented);
    roundTripIsByteIdentical("qt_compact.json", JsonFormat::Compact);
    roundTripIsByteIdentical("roundtrip_test.cescene", JsonFormat::Indented);
}

void parsesTheSceneStructure() {
    const JsonValue scene = parseJson(fixture("roundtrip_test.cescene")).value();
    checkEqual(scene["formatVersion"].toInteger().value_or(-1), std::int64_t(1), "formatVersion");
    const JsonValue &part = scene["root"]["children"][0]["children"][0];
    checkEqual(part["class"].toString(""), StringView("Part"), "nested lookup reaches a Part");
    check(scene["root"]["nope"]["deeper"][7].isNull(), "missing paths chain to null without crashing");
}

void numbersFollowTheQtFormat() {
    const auto format = [](double d) {
        String out;
        appendJsonNumber(out, d);
        return out;
    };
    checkEqual(format(-0.0), String("0"), "-0");
    checkEqual(format(1e15), String("1000000000000000"), "integral below 2^53 is an integer");
    checkEqual(format(9.5e15), String("9.5e+15"), "integral above 2^53 is a double");
    checkEqual(format(1e16), String("1e+16"), "exponent form when shorter");
    checkEqual(format(12345678901234568.0), String("12345678901234568"), "plain form when shorter");
    checkEqual(format(9223372036854775808.0), String("9223372036854776000"), "trailing zeros if still shorter");
    checkEqual(format(0.0001), String("1e-04"), "small: exponent when shorter");
    checkEqual(format(0.00012345), String("0.00012345"), "tie goes to plain");
    checkEqual(format(0.001), String("0.001"), "tie goes to plain (2)");
    checkEqual(format(1.5e-5), String("1.5e-05"), "two-digit exponent");
    checkEqual(format(-2.5e-300), String("-2.5e-300"), "three-digit exponent");
    checkEqual(format(0.1 + 0.2), String("0.30000000000000004"), "shortest round-trip digits");
    checkEqual(format(std::numeric_limits<double>::quiet_NaN()), String("null"), "NaN");
    checkEqual(format(-std::numeric_limits<double>::infinity()), String("null"), "-inf");
}

void keepsIntegerPrecision() {
    const JsonValue big = parseJson("[9007199254740993, 9223372036854775807, 9223372036854775808, 5.0]").value();
    checkEqual(big[0u].toInteger().value_or(0), std::int64_t(9007199254740993), "2^53 + 1 stays exact");
    check(big[1u].asInteger() != nullptr, "int64 max is an Integer");
    check(big[2u].asDouble() != nullptr, "beyond int64 becomes a Double");
    check(big[3u].asDouble() != nullptr && big[3u].toInteger() == 5, "5.0 is a Double with an integer value");
    checkEqual(writeJson(big, JsonFormat::Compact), String("[9007199254740993,9223372036854775807,9223372036854776000,5]"),
               "and they write back as the Qt build wrote them");
}

void stringsAndEscapes() {
    const JsonValue v = parseJson(R"(["a\"b\\c\/d\b\f\n\r\t", "é😀", "\ud800x", "\udc00"])").value();
    checkEqual(v[0u].toString(""), StringView("a\"b\\c/d\b\f\n\r\t"), "simple escapes");
    checkEqual(v[1u].toString(""), StringView("\xC3\xA9\xF0\x9F\x98\x80"), "\\u escapes and surrogate pairs");
    checkEqual(v[2u].toString(""), StringView("\xEF\xBF\xBDx"), "lone high surrogate becomes U+FFFD");
    checkEqual(v[3u].toString(""), StringView("\xEF\xBF\xBD"), "lone low surrogate becomes U+FFFD");
    checkEqual(writeJson(JsonValue("tab\t ctl\x01 del\x7f /"), JsonFormat::Compact),
               String("\"tab\\t ctl\\u0001 del\x7f /\""), "writer escapes only what it must");
}

void objectsSortAndDeduplicate() {
    const JsonValue v = parseJson(R"({"b":1,"a":2,"b":3})").value();
    checkEqual(writeJson(v, JsonFormat::Compact), String(R"({"a":2,"b":3})"), "sorted, last duplicate wins");
    JsonObject o{{"z", 1}, {"m", 2}};
    o.set("a", 3);
    o.set("m", 4);
    checkEqual(writeJson(JsonValue(o), JsonFormat::Compact), String(R"({"a":3,"m":4,"z":1})"), "set keeps order");
    check(o.remove("m") && !o.contains("m"), "remove");
}

void reportsErrorsWithPositions() {
    const Result<JsonValue> bad = parseJson("{\n  \"a\": [1, 2,, 3]\n}");
    check(!bad.ok(), "double comma rejected");
    const auto &context = bad.error().context();
    check(context.size() == 3 && context[1].second == "2" && context[2].second == "14", "line 2, column 14 (the second comma)");

    check(!parseJson("").ok(), "empty document");
    check(!parseJson("[1,2,]").ok(), "trailing comma");
    check(!parseJson("{} x").ok(), "trailing garbage");
    check(!parseJson("[01]").ok(), "leading zero");
    check(!parseJson("[1.]").ok(), "digits required after '.'");
    check(!parseJson("[1e400]").ok(), "overflowing number");
    checkEqual(parseJson("[1e-400]").value()[0u].toDouble(-1.0), 0.0, "underflow becomes zero");
    check(!parseJson("[\"a\xFF" "b\"]").ok(), "invalid UTF-8");
    check(!parseJson("[\"unterminated").ok(), "unterminated string");
    check(!parseJson("{\"a\" 1}").ok(), "missing colon");
    check(!parseJson("[tru]").ok(), "bad literal");
    check(parseJson("\xEF\xBB\xBF{}").ok(), "UTF-8 BOM accepted (the Qt build accepted it)");
    check(parseJson("[\"a\x01" "b\"]").ok(), "raw control character accepted (the Qt build accepted it)");
    check(parseJson("  42  ").ok(), "scalar documents are valid JSON");
}

void enforcesLimits() {
    const auto nested = [](int depth) { return String(static_cast<std::size_t>(depth), '[') + String(static_cast<std::size_t>(depth), ']'); };
    check(parseJson(nested(1024)).ok(), "1024 levels, like the Qt build");
    const Result<JsonValue> tooDeep = parseJson(nested(1025));
    check(!tooDeep.ok() && tooDeep.error().code() == ErrorCode::LimitExceeded, "1025 levels rejected");
    check(!parseJson(nested(200000)).ok(), "absurd nesting is rejected, not a stack overflow");

    JsonLimits small;
    small.maxBytes = 10;
    check(!parseJson("[1,2,3,4,5,6,7]", small).ok(), "size limit");
    small = JsonLimits{};
    small.maxStringBytes = 4;
    check(!parseJson("[\"abcdefgh\"]", small).ok(), "string length limit");
}

} // namespace

int main() {
    qtFilesRoundTrip();
    parsesTheSceneStructure();
    numbersFollowTheQtFormat();
    keepsIntegerPrecision();
    stringsAndEscapes();
    objectsSortAndDeduplicate();
    reportsErrorsWithPositions();
    enforcesLimits();
    return cfw::test::finish("JsonTest");
}
