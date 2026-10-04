// FLAC: files written by the reference encoder decode bit for bit to the
// WAV they were made from; a small encoder here writes the subframe kinds
// and stereo modes one by one; frames with a bad CRC are dropped and decoding
// carries on; seeking lands on the exact sample.

#include "AudioTestSupport.h"

using namespace cfw;
using namespace audiotest;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// ---- A minimal FLAC writer ----------------------------------------------------

struct BitWriter {
    std::vector<std::uint8_t> bytes;
    int used = 8; // bits used in the last byte
    void bits(std::uint64_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            if (used == 8) {
                bytes.push_back(0);
                used = 0;
            }
            bytes.back() = std::uint8_t(bytes.back() | (((value >> i) & 1) << (7 - used)));
            ++used;
        }
    }
    void align() { used = 8; }
};

std::uint8_t crc8(const std::uint8_t *data, std::size_t size) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = std::uint8_t(crc ^ data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = std::uint8_t((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc;
}

std::uint16_t crc16(const std::uint8_t *data, std::size_t size) {
    unsigned crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= unsigned(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
        }
        crc &= 0xFFFF;
    }
    return std::uint16_t(crc);
}

enum class Kind { Constant, Verbatim, Fixed2, Fixed2Escape, Lpc1 };

// One subframe of `samples` (`bits` wide), with `wasted` low zero bits.
void subframe(BitWriter &w, Kind kind, std::vector<std::int64_t> samples, int bits, int wasted = 0) {
    const int type = kind == Kind::Constant ? 0 : kind == Kind::Verbatim ? 1 : kind == Kind::Lpc1 ? 32 : 10;
    w.bits(0, 1);
    w.bits(std::uint64_t(type), 6);
    if (wasted > 0) {
        w.bits(1, 1);
        w.bits(1, wasted); // wasted - 1 zeros, then a one
        bits -= wasted;
        for (std::int64_t &sample : samples) {
            sample >>= wasted;
        }
    } else {
        w.bits(0, 1);
    }
    const auto sample = [&](std::int64_t v) { w.bits(std::uint64_t(v) & ((std::uint64_t(1) << bits) - 1), bits); };
    const auto residuals = [&](int order, const std::vector<std::int64_t> &residual, bool escape) {
        w.bits(0, 2); // 4-bit Rice parameters
        w.bits(1, 4); // two partitions
        const int half = int(samples.size()) / 2;
        std::size_t index = 0;
        for (int partition = 0; partition < 2; ++partition) {
            const int count = half - (partition == 0 ? order : 0);
            if (escape && partition == 1) {
                w.bits(15, 4);
                w.bits(std::uint64_t(bits + 3), 5);
                for (int i = 0; i < count; ++i, ++index) {
                    w.bits(std::uint64_t(residual[index]) & ((std::uint64_t(1) << (bits + 3)) - 1), bits + 3);
                }
                continue;
            }
            const int parameter = 3;
            w.bits(std::uint64_t(parameter), 4);
            for (int i = 0; i < count; ++i, ++index) {
                const std::int64_t r = residual[index];
                const std::uint64_t folded = r < 0 ? std::uint64_t(-r) * 2 - 1 : std::uint64_t(r) * 2;
                for (std::uint64_t q = folded >> parameter; q > 0; --q) {
                    w.bits(0, 1);
                }
                w.bits(1, 1);
                w.bits(folded & 7, parameter);
            }
        }
    };
    if (kind == Kind::Constant) {
        sample(samples[0]);
    } else if (kind == Kind::Verbatim) {
        for (const std::int64_t v : samples) {
            sample(v);
        }
    } else if (kind == Kind::Lpc1) {
        // One coefficient: 7/8 of the sample before.
        sample(samples[0]);
        w.bits(3, 4);  // precision 4 bits
        w.bits(3, 5);  // shift 3
        w.bits(7, 4);  // the coefficient
        std::vector<std::int64_t> residual;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            residual.push_back(samples[i] - ((7 * samples[i - 1]) >> 3));
        }
        residuals(1, residual, false);
    } else {
        sample(samples[0]);
        sample(samples[1]);
        std::vector<std::int64_t> residual;
        for (std::size_t i = 2; i < samples.size(); ++i) {
            residual.push_back(samples[i] - 2 * samples[i - 1] + samples[i - 2]);
        }
        residuals(2, residual, kind == Kind::Fixed2Escape);
    }
}

struct Frame {
    int assignment = 0; // 0 independent, 8 left/side, 9 side/right, 10 mid/side
    std::vector<Kind> kinds;
    std::vector<int> wasted;
};

// A whole FLAC file of `channels` x `blockSize`-sample frames.
Bytes makeFlac(const std::vector<std::vector<std::int64_t>> &channelSamples, int rate, int bits, int blockSize,
               const std::vector<Frame> &frames) {
    const int channels = int(channelSamples.size());
    const std::size_t total = channelSamples[0].size();
    BitWriter w;
    for (const char c : {'f', 'L', 'a', 'C'}) {
        w.bits(std::uint8_t(c), 8);
    }
    w.bits(0x80, 8); // the last metadata block: STREAMINFO
    w.bits(34, 24);
    w.bits(std::uint64_t(blockSize), 16);
    w.bits(std::uint64_t(blockSize), 16);
    w.bits(0, 24);
    w.bits(0, 24);
    w.bits(std::uint64_t(rate), 20);
    w.bits(std::uint64_t(channels - 1), 3);
    w.bits(std::uint64_t(bits - 1), 5);
    w.bits(total, 36);
    for (int i = 0; i < 16; ++i) {
        w.bits(0, 8); // no MD5
    }
    for (std::size_t f = 0; f < frames.size(); ++f) {
        const Frame &frame = frames[f];
        const std::size_t start = w.bytes.size();
        w.bits(0xFFF8, 16);
        w.bits(7, 4); // block size follows, 16 bits
        w.bits(0, 4); // sample rate from STREAMINFO
        w.bits(std::uint64_t(frame.assignment ? frame.assignment : channels - 1), 4);
        w.bits(0, 3); // sample size from STREAMINFO
        w.bits(0, 1);
        w.bits(f, 8); // the frame number (below 128: one byte)
        w.bits(std::uint64_t(blockSize - 1), 16);
        w.bits(crc8(w.bytes.data() + start, w.bytes.size() - start), 8);
        std::vector<std::vector<std::int64_t>> block;
        for (int c = 0; c < channels; ++c) {
            const auto first = channelSamples[std::size_t(c)].begin() + std::ptrdiff_t(f * std::size_t(blockSize));
            block.emplace_back(first, first + blockSize);
        }
        std::vector<int> width(std::size_t(channels), bits);
        if (frame.assignment == 8 || frame.assignment == 9 || frame.assignment == 10) {
            for (int i = 0; i < blockSize; ++i) {
                const std::int64_t left = block[0][std::size_t(i)], right = block[1][std::size_t(i)];
                if (frame.assignment == 8) {
                    block[1][std::size_t(i)] = left - right;
                } else if (frame.assignment == 9) {
                    block[0][std::size_t(i)] = left - right;
                } else {
                    block[0][std::size_t(i)] = (left + right) >> 1;
                    block[1][std::size_t(i)] = left - right;
                }
            }
            width[frame.assignment == 9 ? 0 : 1] = bits + 1;
        }
        for (int c = 0; c < channels; ++c) {
            subframe(w, frame.kinds[std::size_t(c)], block[std::size_t(c)], width[std::size_t(c)],
                     frame.wasted.empty() ? 0 : frame.wasted[std::size_t(c)]);
        }
        w.align();
        const std::uint16_t crc = crc16(w.bytes.data() + start, w.bytes.size() - start);
        w.bits(crc, 16);
    }
    Bytes out(w.bytes.size());
    std::memcpy(out.data(), w.bytes.data(), out.size());
    return out;
}

std::vector<float> interleave(const std::vector<std::vector<std::int64_t>> &channels, int bits) {
    std::vector<float> out;
    const float scale = float(1.0 / double(std::int64_t(1) << (bits - 1)));
    for (std::size_t i = 0; i < channels[0].size(); ++i) {
        for (const auto &channel : channels) {
            out.push_back(float(double(channel[i])) * scale);
        }
    }
    return out;
}

// ---- Tests -------------------------------------------------------------------

void referenceEncoderFilesDecodeExactly() {
    for (const char *name : {"flac-stereo44", "flac-mono48-24", "flac-stereo48-fixed"}) {
        const Bytes flac = testFile((String(name) + ".flac").c_str());
        const Bytes wav = testFile((String(name) + ".wav").c_str());
        checkEqual(detectAudioFormat(span(flac)), AudioFormat::Flac, "detected as FLAC");
        checkSameSound(decodeAudio(span(flac)), decodeWav(span(wav)), 0.0, name);
        auto stream = AudioStream::open(span(flac));
        check(stream.ok(), "streams");
        if (stream) {
            const auto whole = decodeFlac(span(flac)).value();
            check(stream.value()->totalFrames() == whole.frames(), "the length is in the header");
            check(readAll(*stream.value(), 1000) == whole.samples, "read in pieces, the same samples");
            for (const std::uint64_t frame : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(4095), std::uint64_t(4096),
                                              std::uint64_t(9000), std::uint64_t(whole.frames() - 1), std::uint64_t(whole.frames())}) {
                checkEqual(seekDifference(*stream.value(), whole.samples, frame, 700), 0.0, "exact after a seek");
            }
        }
    }
    decodeDamaged(testFile("flac-stereo44.flac"), [](Span<const std::byte> bytes) { return decodeAudio(bytes); });
}

void everySubframeAndStereoMode() {
    constexpr int kBlock = 64;
    std::vector<std::vector<std::int64_t>> samples(2);
    for (int i = 0; i < kBlock * 6; ++i) {
        samples[0].push_back(std::int64_t(std::lround(12000.0 * std::sin(i * 0.11))));
        samples[1].push_back(std::int64_t(std::lround(9000.0 * std::sin(i * 0.07 + 1.0))) + (i % 5) * 40);
    }
    // Frame 1 is constant on the left; frame 5 has four wasted bits on the right.
    for (int i = 0; i < kBlock; ++i) {
        samples[0][std::size_t(kBlock + i)] = -1234;
        samples[1][std::size_t(5 * kBlock + i)] &= ~std::int64_t(15);
    }
    const std::vector<Frame> frames = {
        {0, {Kind::Verbatim, Kind::Verbatim}, {}},
        {0, {Kind::Constant, Kind::Fixed2}, {}},
        {8, {Kind::Fixed2, Kind::Fixed2}, {}},       // left, side
        {9, {Kind::Fixed2Escape, Kind::Lpc1}, {}},   // side, right
        {10, {Kind::Lpc1, Kind::Fixed2Escape}, {}},  // mid, side
        {0, {Kind::Fixed2, Kind::Verbatim}, {0, 4}}, // wasted bits
    };
    const Bytes flac = makeFlac(samples, 32000, 16, kBlock, frames);
    const auto sound = decodeFlac(span(flac));
    check(sound.ok(), "the hand-made file decodes");
    if (!sound) {
        std::printf("      %s\n", sound.error().describe().c_str());
        return;
    }
    checkEqual(sound.value().sampleRate, 32000, "rate");
    checkEqual(sound.value().channels, 2, "channels");
    const std::vector<float> expected = interleave(samples, 16);
    checkEqual(sound.value().samples.size(), expected.size(), "every frame decoded");
    checkEqual(maxDifference(sound.value().samples, expected), 0.0,
               "constant, verbatim, fixed, LPC, escaped residuals, three stereo modes, wasted bits");

    // A frame with a damaged body fails its CRC-16 and is dropped; the
    // frames around it still decode, and the stream says where they are.
    Bytes damaged = flac;
    const std::size_t frameBytes = (flac.size() - 42) / 6; // roughly; the third frame's middle
    damaged[42 + frameBytes * 2 + frameBytes / 2] ^= std::byte(0x10);
    auto stream = AudioStream::open(span(damaged));
    check(stream.ok(), "a file with a damaged frame opens");
    if (stream) {
        const std::vector<float> got = readAll(*stream.value(), 50);
        checkEqual(got.size(), expected.size() - std::size_t(kBlock) * 2, "one block is missing");
        check(std::equal(got.begin(), got.begin() + kBlock * 2 * 2, expected.begin()), "the frames before it are intact");
        check(std::equal(got.end() - kBlock * 2 * 2, got.end(), expected.end() - kBlock * 2 * 2),
              "and so are the frames after it");
    }

    // 8-bit mono, and a header that does not belong to FLAC at all.
    std::vector<std::vector<std::int64_t>> small(1);
    for (int i = 0; i < kBlock; ++i) {
        small[0].push_back((i * 7) % 200 - 100);
    }
    const auto eight = decodeFlac(span(makeFlac(small, 8000, 8, kBlock, {{0, {Kind::Fixed2}, {}}})));
    check(eight.ok() && maxDifference(eight.value().samples, interleave(small, 8)) == 0.0, "8-bit mono");
    Bytes notFlac = flac;
    notFlac[0] = std::byte('x');
    check(decodeFlac(span(notFlac)).error().code() == ErrorCode::ParseError, "not FLAC is a ParseError");
    check(decodeFlac(span(Bytes(flac.begin(), flac.begin() + 20))).error().code() == ErrorCode::Corrupt,
          "cut-off metadata is Corrupt");
    AudioLimits limits;
    limits.maxDecodedBytes = 100;
    check(decodeFlac(span(flac), limits).error().code() == ErrorCode::LimitExceeded,
          "the header's length is checked against the cap before decoding");
}

} // namespace

int main() {
    referenceEncoderFilesDecodeExactly();
    everySubframeAndStereoMode();
    return cfw::test::finish("FlacTest");
}
