#pragma once

// Shared by the decoders: bounds-checked byte and bit access. Nothing here
// trusts a length it was told; reading past the end yields zeros and says so.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cfw/audio/AudioDecoder.h"

namespace cfw::detail {

[[nodiscard]] Result<std::unique_ptr<AudioStream>> openWavStream(Span<const std::byte> data, const AudioLimits &limits);
[[nodiscard]] Result<std::unique_ptr<AudioStream>> openVorbisStream(Span<const std::byte> data,
                                                                    const AudioLimits &limits);
[[nodiscard]] Result<std::unique_ptr<AudioStream>> openMp3Stream(Span<const std::byte> data, const AudioLimits &limits);
[[nodiscard]] Result<std::unique_ptr<AudioStream>> openFlacStream(Span<const std::byte> data, const AudioLimits &limits);

// True if an MPEG audio Layer III frame starts at `offset` and another
// consistent one follows it (or the data ends exactly there).
[[nodiscard]] bool mp3FrameAt(Span<const std::byte> data, std::size_t offset) noexcept;
// The size of an ID3v2 tag at the start of `data` (0: there is none).
[[nodiscard]] std::size_t id3v2Size(Span<const std::byte> data) noexcept;

// Rejects a stream whose shape passes the limits, before anything is
// allocated for it.
[[nodiscard]] Result<void> checkStreamShape(int channels, int sampleRate, const AudioLimits &limits, const char *format);

// Reads everything `stream` has, failing once the result would pass
// limits.maxDecodedBytes.
[[nodiscard]] Result<AudioBuffer> decodeAll(AudioStream &stream, const AudioLimits &limits);

inline std::uint8_t byteAt(Span<const std::byte> data, std::size_t offset) noexcept {
    return offset < data.size() ? std::to_integer<std::uint8_t>(data[offset]) : std::uint8_t(0);
}
inline std::uint32_t le16(Span<const std::byte> d, std::size_t o) noexcept {
    return std::uint32_t(byteAt(d, o)) | std::uint32_t(byteAt(d, o + 1)) << 8;
}
inline std::uint32_t le32(Span<const std::byte> d, std::size_t o) noexcept {
    return le16(d, o) | le16(d, o + 2) << 16;
}
inline std::uint64_t le64(Span<const std::byte> d, std::size_t o) noexcept {
    return std::uint64_t(le32(d, o)) | std::uint64_t(le32(d, o + 4)) << 32;
}
inline std::uint32_t be16(Span<const std::byte> d, std::size_t o) noexcept {
    return std::uint32_t(byteAt(d, o)) << 8 | std::uint32_t(byteAt(d, o + 1));
}
inline std::uint32_t be24(Span<const std::byte> d, std::size_t o) noexcept {
    return std::uint32_t(byteAt(d, o)) << 16 | be16(d, o + 1);
}
inline std::uint32_t be32(Span<const std::byte> d, std::size_t o) noexcept {
    return be16(d, o) << 16 | be16(d, o + 2);
}
inline bool matches(Span<const std::byte> d, std::size_t o, const char *tag, std::size_t length) noexcept {
    if (o > d.size() || d.size() - o < length) {
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (std::to_integer<char>(d[o + i]) != tag[i]) {
            return false;
        }
    }
    return true;
}

// Bits most significant first (FLAC, MP3). Past the end it returns zeros and
// overrun() turns true; the position keeps counting so callers can compare
// it with a budget.
class MsbBitReader {
public:
    MsbBitReader() = default;
    MsbBitReader(const std::uint8_t *data, std::size_t bytes) noexcept : m_data(data), m_bits(std::uint64_t(bytes) * 8) {}

    [[nodiscard]] std::uint64_t position() const noexcept { return m_position; }
    [[nodiscard]] std::uint64_t size() const noexcept { return m_bits; }
    [[nodiscard]] bool overrun() const noexcept { return m_position > m_bits; }
    void seek(std::uint64_t bit) noexcept { m_position = bit; }
    void skip(std::uint64_t bits) noexcept { m_position += bits; }
    void alignToByte() noexcept { m_position = (m_position + 7) & ~std::uint64_t(7); }

    [[nodiscard]] std::uint32_t bit() noexcept {
        std::uint32_t value = 0;
        if (m_position < m_bits) {
            value = (std::uint32_t(m_data[m_position >> 3]) >> (7 - (m_position & 7))) & 1u;
        }
        ++m_position;
        return value;
    }
    // 0 to 32 bits.
    [[nodiscard]] std::uint32_t read(int count) noexcept {
        std::uint32_t value = 0;
        while (count > 0) {
            if (m_position >= m_bits) {
                value = count >= 32 ? 0 : value << count;
                m_position += std::uint64_t(count);
                return value;
            }
            const int inByte = 8 - int(m_position & 7);
            const int take = count < inByte ? count : inByte;
            const std::uint32_t byte = m_data[m_position >> 3];
            const std::uint32_t piece = (byte >> (inByte - take)) & ((1u << take) - 1u);
            value = take >= 32 ? piece : (value << take) | piece;
            m_position += std::uint64_t(take);
            count -= take;
        }
        return value;
    }
    // A two's complement number of 1 to 32 bits.
    [[nodiscard]] std::int32_t readSigned(int count) noexcept {
        const std::uint32_t raw = read(count);
        if (count >= 32) {
            return std::int32_t(raw);
        }
        const std::uint32_t sign = 1u << (count - 1);
        return std::int32_t((raw ^ sign)) - std::int32_t(sign);
    }

private:
    const std::uint8_t *m_data = nullptr;
    std::uint64_t m_bits = 0;
    std::uint64_t m_position = 0;
};

} // namespace cfw::detail
