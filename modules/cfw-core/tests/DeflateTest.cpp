// DEFLATE and zlib: decoding streams real zlib produced (captured once with
// Python's zlib, see docs/decisions/0012), round trips at every level,
// checksums against published values, and hostile streams that must fail
// cleanly.

#include "cfw/core/Checksum.h"
#include "cfw/core/Deflate.h"

#include <random>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Span<const std::byte> bytesOf(StringView s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }
String textOf(const std::vector<std::byte> &b) { return String(reinterpret_cast<const char *>(b.data()), b.size()); }

// Mirrors the generator the zlib vector was made from: compressible text with
// short and long repeats.
String generated(std::size_t n) {
    const char *words[] = {"instance", "Part", "Position", "Vector3", "{\"name\":", "0.5,", "Color", "\n  "};
    String out;
    std::uint64_t x = 12345;
    while (out.size() < n) {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        if (x % 7 == 0) {
            out += static_cast<char>((x >> 8) & 255u);
        } else {
            out += words[x % 8];
        }
    }
    out.resize(n);
    return out;
}

void checksumsMatchPublishedValues() {
    checkEqual(crc32(bytesOf("123456789")), 0xCBF43926u, "CRC-32 check value");
    checkEqual(crc32(bytesOf("")), 0u, "CRC-32 of nothing");
    checkEqual(crc32(bytesOf("56789"), crc32(bytesOf("1234"))), 0xCBF43926u, "CRC-32 continues incrementally");
    checkEqual(adler32(bytesOf("Wikipedia")), 0x11E60398u, "Adler-32 of Wikipedia");
    checkEqual(adler32(bytesOf("")), 1u, "Adler-32 of nothing");
    const String big(100000, '\xFF');
    checkEqual(adler32(bytesOf(big)), adler32(bytesOf(big.substr(50000)), adler32(bytesOf(big.substr(0, 50000)))),
               "Adler-32 continues across its overflow block");
}

void decodesStreamsFromZlib() {
    const StringView hello("\x78\xda\xcb\x48\xcd\xc9\xc9\x57\xc8\x40\x27\x01\x68\x03\x08\xb1", 16);
    Result<std::vector<std::byte>> r = zlibDecompress(bytesOf(hello));
    check(r && textOf(r.value()) == "hello hello hello hello", "fixed-Huffman stream with a back-reference");

    const StringView empty("\x78\x9c\x03\x00\x00\x00\x00\x01", 8);
    r = zlibDecompress(bytesOf(empty));
    check(r && r.value().empty(), "empty stream");

    const StringView stored("\x78\x01\x01\x03\x00\xfc\xff\x61\x62\x63\x02\x4d\x01\x27", 14);
    r = zlibDecompress(bytesOf(stored));
    check(r && textOf(r.value()) == "abc", "stored block");

    const StringView gen("\x78\xda\x73\xce\xcf\xc9\x2f\xe2\x52\x50\xa8\x56\xca\x4b\xcc\x4d\x55\xb2\x32\xd0\x33\xd5\x91\x09\x4b\x4d\x2e\xc9\x2f\x32\x3e\x1f\x90\x58\x54\xe2\x0c\x52\x60\x8c\x2c\xbd\x09\x2a\x9d\x99\x57\x5c\x92\x98\x97\x9c\x0a\x57\x75\x0c\x59\xd5\x1e\x5c\xaa\xd0\x2c\x0b\xc8\x2f\xce\x2c\xc9\xcc\xcf\xc3\xa3\x5c\x01\x8f\xba\x1d\x30\x45\x5f\x89\x34\x8c\x08\xbb\xc3\x49\x50\x6b\x8b\xae\xf6\x40\x32\xf5\x3c\x8e\xac\xdc\x08\x97\x3a\x9e\xe5\x24\x9a\xb6\x17\x4d\xed\x3c\x6c\x8a\xb8\x38\xc9\xf3\x06\x2b\x36\x75\x0a\x30\x45\x66\x54\x0c\x94\xcd\x06\x78\x14\x55\x52\x9e\x1a\xc2\xc9\x75\xe3\x42\x32\x3d\xf4\xde\x17\xaa\x28\x8a\x44\x9b\x67\x12\xa1\x76\x2e\xb2\x99\xe6\xc8\x0a\xfe\x52\x39\xa1\xb6\x50\x9e\xfe\x27\xa0\x29\x3d\x86\xa6\xe6\x27\x3e\xf3\x12\x90\x4d\x52\xc6\x6f\xa9\x31\xb2\xe3\xea\xf1\xb8\xca\x85\x0a\x01\x83\xa6\xbc\x91\x90\x3a\x47\x62\x0d\xac\xa4\x72\x0c\xc2\x94\xf7\x63\x53\xf8\x89\x7a\xe5\x2c\x6d\x94\x6f\x03\xa9\xeb\x47\x52\x63\x06\x53\xd0\x49\xac\x49\x3f\x61\x0a\xb7\x92\xe9\x3c\x03\x90\xba\x89\x83\x31\x74\x60\xca\xcf\x80\xd5\xd6\xe2\x51\xb9\x8a\x48\x13\xb9\x48\xb4\x39\x0a\xa6\xf6\x26\xb2\xa2\x0a\x98\xf4\x64\x6c\x46\x69\x90\xe7\xd9\xca\x41\x15\xe4\x44\x2a\x9f\x5e\x8f\xae\x68\xc7\x77\x2c\x26\x45\xe3\x30\x65\xc3\xa0\x4c\x6e\x58\xdb\x21\x2b\xa0\x8a\xfc\xdb\x0b\x91\x84\xbf\xa2\x19\x71\x05\xae\x57\x0e\x8f\x45\x4f\xa8\xe4\x7e\x07\xca\xfd\x59\x88\xa6\xd4\x16\x97\x91\x99\xc8\xaa\x66\xe2\xb1\x38\x1a\xd9\xc6\x33\x60\x15\x00\x57\x57\x23\xf2", 357);
    r = zlibDecompress(bytesOf(gen));
    check(r && textOf(r.value()) == generated(3000), "dynamic-Huffman stream (zlib level 9)");
}

void roundTripsAtEveryLevel() {
    std::mt19937 rng(42);
    String random(70000, '\0');
    for (char &c : random) {
        c = static_cast<char>(rng() & 0xFF);
    }
    // Short inputs with repeats end up in fixed-Huffman blocks.
    const String inputs[] = {String(), String("a"), String("hello hello hello"), String("abcabcabcXabcabcabc"),
                             String(100000, 'z'), generated(200000), random,
                             generated(1000) + random.substr(0, 1000) + generated(5000)};
    bool allRoundTrip = true;
    for (int level = 0; level <= 9; ++level) {
        for (const String &input : inputs) {
            const std::vector<std::byte> packed = zlibCompress(bytesOf(input), {level});
            const Result<std::vector<std::byte>> back = zlibDecompress(packed);
            if (!back || textOf(back.value()) != input) {
                allRoundTrip = false;
                std::printf("      level %d, %zu bytes: %s\n", level, input.size(),
                            back ? "wrong output" : back.error().describe().c_str());
            }
        }
    }
    check(allRoundTrip, "every input round-trips at every level");

    const String text = generated(200000);
    const std::size_t fast = zlibCompress(bytesOf(text), {1}).size();
    const std::size_t best = zlibCompress(bytesOf(text), {9}).size();
    check(best < text.size() / 4, "level 9 compresses repetitive text well");
    check(best <= fast, "level 9 is no larger than level 1");
    check(zlibCompress(bytesOf(random), {6}).size() < random.size() + 100, "random data costs little over stored");
}

void rejectsHostileStreams() {
    const auto fails = [](StringView s, ErrorCode code) {
        const Result<std::vector<std::byte>> r = zlibDecompress(bytesOf(s));
        return !r && r.error().code() == code;
    };
    check(fails(StringView("\x78", 1), ErrorCode::Corrupt), "truncated header");
    check(fails(StringView("\x78\x9d\x03\x00", 4), ErrorCode::Corrupt), "header check bits wrong");
    check(fails(StringView("\x78\xbb\x00\x00\x00\x00", 6), ErrorCode::Unsupported), "preset dictionary");
    check(fails(StringView("\x78\x9c\x03\x00\x00\x00\x00\x02", 8), ErrorCode::Corrupt), "Adler-32 mismatch");
    check(fails(StringView("\x78\x9c\x03\x00\x00\x00", 6), ErrorCode::Corrupt), "truncated checksum");
    check(fails(StringView("\x78\x9c\x07\x00", 4), ErrorCode::Corrupt), "block type 3");
    check(fails(StringView("\x78\x01\x01\x03\x00\xfc\xfe\x61\x62\x63", 10), ErrorCode::Corrupt), "stored length check");
    check(fails(StringView("\x78\x01\x01\x03\x00\xfc\xff\x61", 8), ErrorCode::Corrupt), "truncated stored block");
    check(fails(StringView("\x78\xda\xcb\x48\xcd\xc9", 6), ErrorCode::Corrupt), "stream cut mid-block");
    // Fixed block: literal 'a', then length 3 at distance 2 when only one byte exists.
    check(!inflate(bytesOf(StringView("\x4b\x04\x42\x00", 4))), "distance before the start of the output");

    // A bomb: 10 MB of zeros compresses to about 10 KB; a 1 MB limit stops it.
    const String zeros(10u * 1024u * 1024u, '\0');
    const std::vector<std::byte> bomb = zlibCompress(bytesOf(zeros), {9});
    check(bomb.size() < 20000, "the bomb is small");
    const Result<std::vector<std::byte>> limited = zlibDecompress(bomb, {1024u * 1024u});
    check(!limited && limited.error().code() == ErrorCode::LimitExceeded, "output limit stops it");
    const Result<std::vector<std::byte>> exact = zlibDecompress(bomb, {zeros.size()});
    check(exact && exact.value().size() == zeros.size(), "a limit of exactly the size passes");

    // Random garbage after a valid header: must fail or succeed, never crash.
    std::mt19937 rng(9);
    int failures = 0;
    for (int i = 0; i < 2000; ++i) {
        String s("\x78\x9c");
        const std::size_t n = rng() % 64;
        for (std::size_t k = 0; k < n; ++k) {
            s += static_cast<char>(rng() & 0xFF);
        }
        failures += zlibDecompress(bytesOf(s), {65536}) ? 0 : 1;
    }
    check(failures > 1900, "random streams are rejected");
}

} // namespace

int main() {
    checksumsMatchPublishedValues();
    decodesStreamsFromZlib();
    roundTripsAtEveryLevel();
    rejectsHostileStreams();
    return cfw::test::finish("DeflateTest");
}
