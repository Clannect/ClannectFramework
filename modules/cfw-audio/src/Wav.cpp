// WAV (RIFF WAVE): the fmt chunk says what the samples are, the data chunk
// holds them. Chunk sizes are untrusted: a data chunk that claims more than
// the file holds (common with files written by a stream, where the size was
// never filled in) is cut to what is there.

#include <algorithm>
#include <cmath>
#include <cstring>

#include "DecoderCommon.h"

namespace cfw::detail {

namespace {

constexpr std::uint32_t kFormatPcm = 1;
constexpr std::uint32_t kFormatFloat = 3;
constexpr std::uint32_t kFormatExtensible = 0xFFFE;

enum class Encoding : std::uint8_t { Unsigned8, Int16, Int24, Int32, Float32, Float64 };

class WavStream final : public AudioStream {
public:
    WavStream(Span<const std::byte> samples, Encoding encoding, int channels, int sampleRate, std::size_t blockAlign)
        : m_samples(samples), m_encoding(encoding), m_channels(channels), m_sampleRate(sampleRate),
          m_blockAlign(blockAlign), m_frames(samples.size() / blockAlign) {}

    AudioFormat format() const noexcept override { return AudioFormat::Wav; }
    int sampleRate() const noexcept override { return m_sampleRate; }
    int channels() const noexcept override { return m_channels; }
    std::optional<std::uint64_t> totalFrames() const noexcept override { return m_frames; }
    std::uint64_t position() const noexcept override { return m_position; }

    Result<void> seekToFrame(std::uint64_t frame) override {
        m_position = std::min<std::uint64_t>(frame, m_frames);
        return success();
    }

    Result<std::size_t> read(Span<float> out) override {
        const std::size_t channels = std::size_t(m_channels);
        const std::size_t frames = std::min<std::uint64_t>(out.size() / channels, m_frames - m_position);
        const std::size_t sampleBytes = m_blockAlign / channels;
        const auto *in = reinterpret_cast<const std::uint8_t *>(m_samples.data()) + std::size_t(m_position) * m_blockAlign;
        float *to = out.data();
        for (std::size_t i = 0; i < frames * channels; ++i, in += sampleBytes) {
            to[i] = sample(in);
        }
        m_position += frames;
        return frames;
    }

private:
    // One little-endian sample. Integer samples wider than their format's
    // bits (20 bits in a 24-bit container) are left-justified, so the
    // container's full range is the scale.
    float sample(const std::uint8_t *in) const noexcept {
        switch (m_encoding) {
        case Encoding::Unsigned8:
            return (float(in[0]) - 128.0f) / 128.0f;
        case Encoding::Int16:
            return float(std::int16_t(std::uint16_t(in[0] | in[1] << 8))) / 32768.0f;
        case Encoding::Int24: {
            const std::uint32_t raw = std::uint32_t(in[0]) | std::uint32_t(in[1]) << 8 | std::uint32_t(in[2]) << 16;
            return float(std::int32_t(raw << 8) >> 8) / 8388608.0f;
        }
        case Encoding::Int32: {
            const std::uint32_t raw = std::uint32_t(in[0]) | std::uint32_t(in[1]) << 8 | std::uint32_t(in[2]) << 16 |
                                      std::uint32_t(in[3]) << 24;
            return float(double(std::int32_t(raw)) / 2147483648.0);
        }
        case Encoding::Float32: {
            float value;
            std::uint32_t raw = std::uint32_t(in[0]) | std::uint32_t(in[1]) << 8 | std::uint32_t(in[2]) << 16 |
                                std::uint32_t(in[3]) << 24;
            std::memcpy(&value, &raw, sizeof value);
            return std::isfinite(value) ? value : 0.0f;
        }
        case Encoding::Float64: {
            double value;
            std::uint64_t raw = 0;
            for (int i = 7; i >= 0; --i) {
                raw = raw << 8 | in[i];
            }
            std::memcpy(&value, &raw, sizeof value);
            // Out of float's range is not a sample either.
            return std::isfinite(value) && std::abs(value) < 3.0e38 ? float(value) : 0.0f;
        }
        }
        return 0.0f;
    }

    Span<const std::byte> m_samples;
    Encoding m_encoding;
    int m_channels;
    int m_sampleRate;
    std::size_t m_blockAlign;
    std::uint64_t m_frames;
    std::uint64_t m_position = 0;
};

} // namespace

Result<std::unique_ptr<AudioStream>> openWavStream(Span<const std::byte> data, const AudioLimits &limits) {
    if (!matches(data, 0, "RIFF", 4) || !matches(data, 8, "WAVE", 4)) {
        return Error(ErrorCode::ParseError, "not a WAV file");
    }
    bool haveFormat = false;
    std::uint32_t tag = 0, channels = 0, sampleRate = 0, blockAlign = 0, bits = 0;
    Span<const std::byte> samples;
    bool haveData = false;
    // Chunks: a four-character id, a 32-bit size, the body, padded to even.
    for (std::size_t offset = 12; data.size() - offset >= 8;) {
        const std::size_t body = offset + 8;
        const std::size_t claimed = le32(data, offset + 4);
        const std::size_t size = std::min(claimed, data.size() - body);
        if (matches(data, offset, "fmt ", 4)) {
            if (size < 16) {
                return Error(ErrorCode::Corrupt, "WAV: the format chunk is too short");
            }
            tag = le16(data, body);
            channels = le16(data, body + 2);
            sampleRate = le32(data, body + 4);
            blockAlign = le16(data, body + 12);
            bits = le16(data, body + 14);
            if (tag == kFormatExtensible) {
                // The real format is the first two bytes of a GUID whose rest is
                // fixed (KSDATAFORMAT_SUBTYPE_*).
                static const char kGuidTail[] = "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71";
                if (size < 40 || !matches(data, body + 26, kGuidTail, 14)) {
                    return Error(ErrorCode::Unsupported, "WAV: an extensible format this decoder does not know");
                }
                tag = le16(data, body + 24);
            }
            haveFormat = true;
        } else if (matches(data, offset, "data", 4) && !haveData) {
            samples = data.subspan(body, size);
            haveData = true;
        }
        if (size != claimed) {
            break; // the chunk ran off the end of the file: nothing follows
        }
        offset = body + size + (size & 1);
        if (offset > data.size()) {
            break;
        }
    }
    if (!haveFormat) {
        return Error(ErrorCode::Corrupt, "WAV: no format chunk");
    }
    if (!haveData) {
        return Error(ErrorCode::Corrupt, "WAV: no data chunk");
    }
    if (Result<void> shape = checkStreamShape(int(channels), int(std::min<std::uint32_t>(sampleRate, 0x7FFFFFFF)), limits, "WAV");
        !shape) {
        return std::move(shape).error();
    }
    // The size of one sample in the file: the block's, when it is sound,
    // else what the bit depth needs.
    std::size_t sampleBytes = (bits + 7) / 8;
    if (blockAlign != 0 && blockAlign % channels == 0 && blockAlign / channels >= sampleBytes) {
        sampleBytes = blockAlign / channels;
    }
    Encoding encoding = Encoding::Int16;
    if (tag == kFormatPcm && sampleBytes >= 1 && sampleBytes <= 4 && bits >= 1 && bits <= 32) {
        encoding = sampleBytes == 1 ? Encoding::Unsigned8
                 : sampleBytes == 2 ? Encoding::Int16
                 : sampleBytes == 3 ? Encoding::Int24
                                    : Encoding::Int32;
    } else if (tag == kFormatFloat && bits == 32 && sampleBytes == 4) {
        encoding = Encoding::Float32;
    } else if (tag == kFormatFloat && bits == 64 && sampleBytes == 8) {
        encoding = Encoding::Float64;
    } else {
        return Error(ErrorCode::Unsupported, "WAV: only PCM and IEEE float samples are supported")
            .with("format", std::to_string(tag))
            .with("bits", std::to_string(bits));
    }
    return std::unique_ptr<AudioStream>(
        new WavStream(samples, encoding, int(channels), int(sampleRate), sampleBytes * channels));
}

} // namespace cfw::detail
