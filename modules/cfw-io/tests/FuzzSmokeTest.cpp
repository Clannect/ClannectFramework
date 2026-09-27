// Deterministic mutation fuzzing of the untrusted-input parsers: the JSON
// reader (seeded with a real scene file) and ByteReader. Every input must
// either parse or be rejected with an error; none may crash, hang or trip a
// sanitizer. This is the always-on smoke version; coverage-guided fuzzing
// (libFuzzer) runs where the toolchain supports it (docs/decisions/0005).

#include "cfw/io/ByteReader.h"
#include "cfw/io/JsonReader.h"
#include "cfw/io/JsonWriter.h"

#include <random>

#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// Applies 1-8 random edits: flip, insert, delete, duplicate a slice, or splice
// in a JSON-significant character.
String mutate(const String &seed, std::mt19937 &rng) {
    String s = seed;
    const StringView tokens = "{}[]\",:\\0-eE.tfn \x01\xFF\xC3\xED";
    std::uniform_int_distribution<int> edits(1, 8);
    const int count = edits(rng);
    for (int i = 0; i < count && !s.empty(); ++i) {
        std::uniform_int_distribution<std::size_t> pos(0, s.size() - 1);
        const std::size_t at = pos(rng);
        switch (rng() % 5) {
        case 0: s[at] = static_cast<char>(rng() & 0xFF); break;
        case 1: s.insert(at, 1, tokens[rng() % tokens.size()]); break;
        case 2: s.erase(at, 1 + rng() % 8); break;
        case 3: s.insert(at, s.substr(at, 1 + rng() % 32)); break;
        case 4: s[at] = tokens[rng() % tokens.size()]; break;
        }
    }
    return s;
}

void jsonSurvivesMutatedScenes() {
    const String seed = readTextFile(Path(CFW_IO_TESTDATA) / "roundtrip_test.cescene").valueOr(String("{}"));
    std::mt19937 rng(20260927);
    int accepted = 0;
    int rejected = 0;
    int roundTripFailures = 0;
    for (int i = 0; i < 20000; ++i) {
        const String input = mutate(seed, rng);
        const Result<JsonValue> parsed = parseJson(input);
        if (!parsed) {
            ++rejected;
            continue;
        }
        ++accepted;
        // Whatever parses must write and re-parse to the same value.
        const Result<JsonValue> again = parseJson(writeJson(parsed.value()));
        if (!again || !(again.value() == parsed.value())) {
            ++roundTripFailures;
        }
    }
    check(accepted > 0 && rejected > 0, "the mutations exercise both paths");
    checkEqual(roundTripFailures, 0, "every accepted document survives write + re-parse");
    std::printf("      json: %d accepted, %d rejected\n", accepted, rejected);
}

void jsonSurvivesRandomBytes() {
    std::mt19937 rng(7);
    for (int i = 0; i < 5000; ++i) {
        String input(rng() % 64, '\0');
        for (char &c : input) {
            c = static_cast<char>(rng() & 0xFF);
        }
        (void)parseJson(input);
    }
    check(true, "random bytes never crash the parser");
}

void byteReaderSurvivesRandomMessages() {
    std::mt19937 rng(11);
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::byte> data(rng() % 48);
        for (std::byte &b : data) {
            b = static_cast<std::byte>(rng() & 0xFF);
        }
        ByteReader r(data);
        // A decoder-shaped read sequence.
        (void)r.u8();
        (void)r.string(16);
        (void)r.f32();
        (void)r.u64();
        (void)r.bytes(rng() % 64);
        (void)r.string(1024);
        (void)r.f64();
        (void)r.finished();
    }
    check(true, "random messages never crash the reader");
}

} // namespace

int main() {
    jsonSurvivesMutatedScenes();
    jsonSurvivesRandomBytes();
    byteReaderSurvivesRandomMessages();
    return cfw::test::finish("FuzzSmokeTest");
}
