#pragma once

// Shared by the decoder tests: reading test files, building WAV files in
// memory, and comparing decoded sound with what was expected.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

#include "cfw/audio/AudioDecoder.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"

namespace audiotest {

using Bytes = std::vector<std::byte>;

inline cfw::Span<const std::byte> span(const Bytes &bytes) { return {bytes.data(), bytes.size()}; }

#ifdef CFW_AUDIO_TESTDATA
inline Bytes testFile(const char *name) {
    auto bytes = cfw::readFile(cfw::Path(CFW_AUDIO_TESTDATA) / name);
    cfw::test::check(bytes.ok(), name);
    return bytes ? std::move(bytes).value() : Bytes();
}

// source-frames.txt: how many frames the file each lossy test file was
// encoded from had.
inline std::uint64_t sourceFrames(const char *name) {
    static const std::map<std::string, std::uint64_t> table = [] {
        std::map<std::string, std::uint64_t> entries;
        const Bytes text = testFile("source-frames.txt");
        std::istringstream lines(std::string(reinterpret_cast<const char *>(text.data()), text.size()));
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string key;
            std::uint64_t frames = 0;
            if (line[0] != '#' && fields >> key >> frames) {
                entries[key] = frames;
            }
        }
        return entries;
    }();
    const auto found = table.find(name);
    cfw::test::check(found != table.end(), "the source length is listed");
    return found == table.end() ? 0 : found->second;
}
#endif

inline void put16(Bytes &out, std::uint32_t v) {
    out.push_back(std::byte(v & 0xFF));
    out.push_back(std::byte((v >> 8) & 0xFF));
}
inline void put32(Bytes &out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}
inline void putTag(Bytes &out, const char *tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(std::byte(tag[i]));
    }
}

// A WAV file around `samples` (already in the file's sample encoding).
// `validBits` > 0 writes a WAVE_FORMAT_EXTENSIBLE header.
inline Bytes makeWav(std::uint32_t formatTag, std::uint32_t channels, std::uint32_t rate, std::uint32_t bits,
                     const Bytes &samples, std::uint32_t validBits = 0) {
    const std::uint32_t blockAlign = channels * (bits / 8);
    Bytes fmt;
    put16(fmt, validBits ? 0xFFFE : formatTag);
    put16(fmt, channels);
    put32(fmt, rate);
    put32(fmt, rate * blockAlign);
    put16(fmt, blockAlign);
    put16(fmt, bits);
    if (validBits) {
        put16(fmt, 22);
        put16(fmt, validBits);
        put32(fmt, 0); // channel mask
        put16(fmt, formatTag);
        static const unsigned char tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        for (const unsigned char byte : tail) {
            fmt.push_back(std::byte(byte));
        }
    }
    Bytes out;
    putTag(out, "RIFF");
    put32(out, std::uint32_t(4 + 8 + fmt.size() + 8 + samples.size() + (samples.size() & 1)));
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    put32(out, std::uint32_t(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    putTag(out, "data");
    put32(out, std::uint32_t(samples.size()));
    out.insert(out.end(), samples.begin(), samples.end());
    if (samples.size() & 1) {
        out.push_back(std::byte(0));
    }
    return out;
}

// The largest difference between two sample runs (infinity if their lengths differ).
inline double maxDifference(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size()) {
        return INFINITY;
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, double(std::abs(a[i] - b[i])));
    }
    return worst;
}

// Checks `decoded` against `expected`: same shape, and no sample further
// apart than `tolerance`.
inline void checkSameSound(const cfw::Result<cfw::AudioBuffer> &decoded, const cfw::Result<cfw::AudioBuffer> &expected,
                           double tolerance, const char *what,
                           std::source_location where = std::source_location::current()) {
    cfw::test::check(decoded.ok(), what, where);
    cfw::test::check(expected.ok(), "the reference decodes", where);
    if (!decoded) {
        std::printf("      %s: %s\n", what, decoded.error().describe().c_str());
    }
    if (!decoded || !expected) {
        return;
    }
    cfw::test::checkEqual(decoded.value().sampleRate, expected.value().sampleRate, "sample rate", where);
    cfw::test::checkEqual(decoded.value().channels, expected.value().channels, "channels", where);
    cfw::test::checkEqual(decoded.value().frames(), expected.value().frames(), "length in frames", where);
    const double difference = maxDifference(decoded.value().samples, expected.value().samples);
    cfw::test::check(difference <= tolerance, what, where);
    if (!(difference <= tolerance)) {
        std::printf("      %s: largest difference %g (allowed %g)\n", what, difference, tolerance);
    }
}

// Reads `stream` to its end in blocks of `blockFrames`.
inline std::vector<float> readAll(cfw::AudioStream &stream, std::size_t blockFrames) {
    std::vector<float> all;
    std::vector<float> block(blockFrames * std::size_t(stream.channels()));
    for (;;) {
        auto got = stream.read(block);
        if (!got || got.value() == 0) {
            break;
        }
        all.insert(all.end(), block.begin(), block.begin() + std::ptrdiff_t(got.value() * std::size_t(stream.channels())));
    }
    return all;
}

// Seeks to `frame`, reads `count` frames, and compares them with the same
// stretch of `whole` (the stream decoded straight through).
inline double seekDifference(cfw::AudioStream &stream, const std::vector<float> &whole, std::uint64_t frame,
                             std::size_t count) {
    const std::size_t channels = std::size_t(stream.channels());
    if (!stream.seekToFrame(frame).ok() || stream.position() != frame) {
        return INFINITY;
    }
    std::vector<float> got(count * channels);
    std::size_t done = 0;
    while (done < count) {
        auto read = stream.read(cfw::Span<float>(got.data() + done * channels, (count - done) * channels));
        if (!read || read.value() == 0) {
            break;
        }
        done += read.value();
    }
    const std::size_t available = std::min<std::size_t>(count, whole.size() / channels - std::size_t(frame));
    if (done != available) {
        return INFINITY;
    }
    double worst = 0.0;
    for (std::size_t i = 0; i < done * channels; ++i) {
        worst = std::max(worst, double(std::abs(got[i] - whole[std::size_t(frame) * channels + i])));
    }
    return worst;
}

// Damages copies of `file` in many ways and decodes each: whatever comes
// back, an error or sound, it must come back.
template <class Decode> void decodeDamaged(const Bytes &file, Decode decode) {
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    int survived = 0;
    for (int round = 0; round < 200; ++round) {
        Bytes damaged = file;
        const int kind = round % 4;
        if (kind == 0) {
            damaged.resize(next() % (damaged.size() + 1)); // cut short
        } else if (kind == 1) {
            for (int i = 0; i < 1 + int(next() % 8); ++i) {
                damaged[next() % damaged.size()] ^= std::byte(1u << (next() % 8)); // flipped bits
            }
        } else if (kind == 2) {
            const std::size_t at = next() % damaged.size();
            const std::size_t count = std::min<std::size_t>(damaged.size() - at, 1 + next() % 64);
            std::fill_n(damaged.begin() + std::ptrdiff_t(at), count, std::byte(next() & 0xFF)); // a smeared run
        } else {
            const std::size_t at = next() % damaged.size();
            damaged.erase(damaged.begin() + std::ptrdiff_t(at),
                          damaged.begin() + std::ptrdiff_t(std::min(damaged.size(), at + 1 + next() % 300))); // a hole
        }
        const cfw::Result<cfw::AudioBuffer> result = decode(span(damaged));
        if (result) {
            for (const float sample : result.value().samples) {
                if (!std::isfinite(sample)) {
                    cfw::test::check(false, "damaged input never decodes to NaN or infinity");
                    return;
                }
            }
        }
        ++survived;
    }
    cfw::test::checkEqual(survived, 200, "every damaged file is handled");
}

} // namespace audiotest
