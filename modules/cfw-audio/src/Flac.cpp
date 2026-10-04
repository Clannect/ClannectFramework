// FLAC (the native container): STREAMINFO, then frames of independently
// coded blocks. Each frame's header carries a CRC-8 and the whole frame a
// CRC-16; a frame failing either is dropped and decoding resynchronises on
// the next header, so damage costs a block of sound, never memory safety.
//
// Samples are computed in 64 bits: a side channel of 32-bit audio needs 33,
// and a hostile predictor must not be able to overflow anything.

#include <algorithm>
#include <array>
#include <vector>

#include "DecoderCommon.h"

namespace cfw::detail {

namespace {

constexpr int kMaxFlacChannels = 8;
// No sample or residual of a valid stream reaches this (33 bits at most).
constexpr std::int64_t kSampleBound = std::int64_t(1) << 34;

std::uint8_t crc8(const std::uint8_t *data, std::size_t size) noexcept {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = std::uint8_t(crc ^ data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = std::uint8_t((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc;
}

struct Crc16Table {
    std::array<std::uint16_t, 256> values{};
    constexpr Crc16Table() {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned crc = i << 8;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
            }
            values[i] = std::uint16_t(crc);
        }
    }
};
constexpr Crc16Table kCrc16;

std::uint16_t crc16(const std::uint8_t *data, std::size_t size) noexcept {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = std::uint16_t((crc << 8) ^ kCrc16.values[(crc >> 8) ^ data[i]]);
    }
    return crc;
}

struct FrameHeader {
    std::size_t headerBytes = 0;
    int blockSize = 0;
    int sampleRate = 0; // 0: the stream's
    int channels = 0;
    int assignment = 0; // 0 independent, 1 left/side, 2 side/right, 3 mid/side
    int bitsPerSample = 0; // 0: the stream's
    std::uint64_t firstSample = 0;
};

class FlacStream final : public AudioStream {
public:
    FlacStream(Span<const std::byte> data, std::size_t firstFrame, int sampleRate, int channels, int bits,
               std::uint64_t totalSamples, int minBlock, int maxBlock)
        : m_data(reinterpret_cast<const std::uint8_t *>(data.data())), m_size(data.size()), m_firstFrame(firstFrame),
          m_sampleRate(sampleRate), m_channels(channels), m_bits(bits), m_total(totalSamples), m_minBlock(minBlock),
          m_maxBlock(maxBlock), m_offset(firstFrame) {}

    AudioFormat format() const noexcept override { return AudioFormat::Flac; }
    int sampleRate() const noexcept override { return m_sampleRate; }
    int channels() const noexcept override { return m_channels; }
    std::optional<std::uint64_t> totalFrames() const noexcept override {
        return m_total > 0 ? std::optional<std::uint64_t>(m_total) : std::nullopt;
    }
    std::uint64_t position() const noexcept override { return m_position; }

    Result<std::size_t> read(Span<float> out) override {
        const std::size_t channels = std::size_t(m_channels);
        const std::size_t wanted = out.size() / channels;
        std::size_t done = 0;
        while (done < wanted) {
            if (m_blockRead == m_blockFrames && !nextBlock()) {
                break;
            }
            const std::size_t take = std::min(wanted - done, m_blockFrames - m_blockRead);
            std::copy_n(m_block.data() + m_blockRead * channels, take * channels, out.data() + done * channels);
            m_blockRead += take;
            m_position += take;
            done += take;
        }
        return done;
    }

    Result<void> seekToFrame(std::uint64_t frame) override {
        if (m_total > 0) {
            frame = std::min(frame, m_total);
        }
        // Bisect on the bytes for the last frame that starts at or before the
        // target, then decode forward to it.
        std::size_t low = m_firstFrame;
        std::size_t high = m_size;
        while (high - low > 4096) {
            const std::size_t middle = low + (high - low) / 2;
            FrameHeader header;
            const std::size_t found = findFrame(middle, high, header);
            if (found >= high || header.firstSample > frame) {
                high = middle;
            } else {
                low = found;
            }
        }
        m_offset = low;
        m_blockFrames = m_blockRead = 0;
        m_position = low == m_firstFrame ? 0 : frame; // corrected by the first block decoded
        m_pendingSeek = low != m_firstFrame;
        while (m_position < frame || m_pendingSeek) {
            if (m_blockRead == m_blockFrames && !nextBlock()) {
                break;
            }
            if (m_position > frame) {
                break; // the bisection was misled by damage: start here
            }
            const std::size_t skip = std::size_t(std::min<std::uint64_t>(frame - m_position, m_blockFrames - m_blockRead));
            m_blockRead += skip;
            m_position += skip;
            if (skip == 0) {
                break;
            }
        }
        return success();
    }

private:
    // Parses a frame header at `offset`; false if there is none there.
    bool parseHeader(std::size_t offset, FrameHeader &header) const noexcept {
        if (m_size - offset < 6 || m_data[offset] != 0xFF || (m_data[offset + 1] & 0xFE) != 0xF8) {
            return false;
        }
        const bool variable = (m_data[offset + 1] & 1) != 0;
        const int blockCode = m_data[offset + 2] >> 4;
        const int rateCode = m_data[offset + 2] & 0x0F;
        const int channelCode = m_data[offset + 3] >> 4;
        const int sizeCode = (m_data[offset + 3] >> 1) & 7;
        if (blockCode == 0 || rateCode == 15 || channelCode > 10 || sizeCode == 3 || (m_data[offset + 3] & 1) != 0) {
            return false;
        }
        // The frame or sample number, coded like UTF-8 (up to 36 bits).
        std::size_t at = offset + 4;
        const std::uint8_t lead = m_data[at++];
        int extra = 0;
        std::uint64_t number = 0;
        if (lead < 0x80) {
            number = lead;
        } else if (lead >= 0xC0 && lead <= 0xFE) {
            for (std::uint8_t mask = 0x40; lead & mask; mask >>= 1) {
                ++extra;
            }
            number = lead & (0x3Fu >> extra);
        } else {
            return false;
        }
        for (int i = 0; i < extra; ++i) {
            if (at >= m_size || (m_data[at] & 0xC0) != 0x80) {
                return false;
            }
            number = number << 6 | (m_data[at++] & 0x3Fu);
        }
        const auto need = [&](std::size_t bytes) { return m_size - at >= bytes; };
        int blockSize = 0;
        if (blockCode == 1) {
            blockSize = 192;
        } else if (blockCode <= 5) {
            blockSize = 576 << (blockCode - 2);
        } else if (blockCode == 6) {
            if (!need(1)) return false;
            blockSize = m_data[at++] + 1;
        } else if (blockCode == 7) {
            if (!need(2)) return false;
            blockSize = (m_data[at] << 8 | m_data[at + 1]) + 1;
            at += 2;
        } else {
            blockSize = 256 << (blockCode - 8);
        }
        static constexpr int kRates[12] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
        int rate = 0;
        if (rateCode < 12) {
            rate = kRates[rateCode];
        } else if (rateCode == 12) {
            if (!need(1)) return false;
            rate = m_data[at++] * 1000;
        } else {
            if (!need(2)) return false;
            rate = m_data[at] << 8 | m_data[at + 1];
            rate = rateCode == 14 ? rate * 10 : rate;
            at += 2;
        }
        if (!need(1) || crc8(m_data + offset, at - offset) != m_data[at]) {
            return false;
        }
        static constexpr int kBits[8] = {0, 8, 12, 0, 16, 20, 24, 32};
        header.headerBytes = at + 1 - offset;
        header.blockSize = blockSize;
        header.sampleRate = rate;
        header.channels = channelCode < 8 ? channelCode + 1 : 2;
        header.assignment = channelCode < 8 ? 0 : channelCode - 7;
        header.bitsPerSample = kBits[sizeCode];
        // A fixed-blocksize stream numbers its frames, not its samples.
        header.firstSample = variable ? number : number * std::uint64_t(m_minBlock > 0 ? m_minBlock : blockSize);
        return true;
    }

    // The first frame header at or after `from` (and before `limit`) that
    // belongs to this stream; `limit` if there is none.
    std::size_t findFrame(std::size_t from, std::size_t limit, FrameHeader &header) const noexcept {
        for (std::size_t offset = from; offset < limit && m_size - offset >= 6; ++offset) {
            if (m_data[offset] == 0xFF && parseHeader(offset, header) && belongs(header)) {
                return offset;
            }
        }
        return limit;
    }

    bool belongs(const FrameHeader &header) const noexcept {
        return header.channels == m_channels && (header.sampleRate == 0 || header.sampleRate == m_sampleRate) &&
               (header.bitsPerSample == 0 || header.bitsPerSample == m_bits) && header.blockSize <= m_maxBlock;
    }

    // Decodes frames until one is good; false at the end of the data.
    bool nextBlock() {
        m_blockFrames = m_blockRead = 0;
        for (;;) {
            FrameHeader header;
            const std::size_t at = findFrame(m_offset, m_size, header);
            if (at >= m_size) {
                m_offset = m_size;
                return false;
            }
            std::size_t frameBytes = 0;
            if (decodeFrame(at, header, frameBytes)) {
                m_offset = at + frameBytes;
                if (m_pendingSeek || header.firstSample > m_position) {
                    m_position = header.firstSample; // after a seek, or over a dropped frame
                    m_pendingSeek = false;
                }
                if (m_total > 0 && m_position >= m_total) {
                    m_blockFrames = 0;
                    return false;
                }
                if (m_total > 0) {
                    m_blockFrames = std::size_t(std::min<std::uint64_t>(m_blockFrames, m_total - m_position));
                }
                if (m_blockFrames > 0) {
                    return true;
                }
            } else {
                m_offset = at + 1; // not a frame after all, or a damaged one
            }
        }
    }

    bool residual(MsbBitReader &bits, std::int64_t *samples, int blockSize, int order) const {
        const int method = int(bits.read(2));
        if (method > 1) {
            return false;
        }
        const int parameterBits = method == 0 ? 4 : 5;
        const int escape = method == 0 ? 15 : 31;
        const int partitionOrder = int(bits.read(4));
        const int partitions = 1 << partitionOrder;
        if ((blockSize >> partitionOrder) << partitionOrder != blockSize || (blockSize >> partitionOrder) < order) {
            return false;
        }
        int index = order;
        for (int partition = 0; partition < partitions; ++partition) {
            const int count = (blockSize >> partitionOrder) - (partition == 0 ? order : 0);
            const int parameter = int(bits.read(parameterBits));
            if (parameter == escape) {
                const int raw = int(bits.read(5));
                for (int i = 0; i < count; ++i) {
                    samples[index++] = raw == 0 ? 0 : bits.readSigned(raw);
                }
            } else {
                for (int i = 0; i < count; ++i) {
                    std::uint64_t quotient = 0;
                    while (bits.bit() == 0) {
                        if (bits.overrun() || ++quotient > (std::uint64_t(1) << 33)) {
                            return false;
                        }
                    }
                    const std::uint64_t folded = (quotient << parameter) | bits.read(parameter);
                    if (folded >= std::uint64_t(kSampleBound)) {
                        return false;
                    }
                    samples[index++] = (folded & 1) ? -std::int64_t(folded >> 1) - 1 : std::int64_t(folded >> 1);
                }
            }
            if (bits.overrun()) {
                return false;
            }
        }
        return true;
    }

    bool subframe(MsbBitReader &bits, std::int64_t *samples, int blockSize, int sampleBits) const {
        if (bits.bit() != 0) {
            return false;
        }
        const int type = int(bits.read(6));
        int wasted = 0;
        if (bits.bit()) {
            wasted = 1;
            while (bits.bit() == 0) {
                if (bits.overrun() || ++wasted >= sampleBits) {
                    return false;
                }
            }
            sampleBits -= wasted;
        }
        if (sampleBits < 1) {
            return false;
        }
        // What a sample of this subframe may reach; the wasted bits go back on
        // afterwards and must still fit.
        const std::int64_t bound = std::int64_t(1) << sampleBits;
        // Wider than 32 bits only for the side channel of 32-bit audio.
        const auto readSample = [&]() -> std::int64_t {
            if (sampleBits <= 32) {
                return bits.readSigned(sampleBits);
            }
            const std::int64_t high = bits.readSigned(sampleBits - 32);
            return high * (std::int64_t(1) << 32) + std::int64_t(bits.read(32));
        };
        if (type == 0) { // constant
            const std::int64_t value = readSample();
            std::fill_n(samples, blockSize, value);
        } else if (type == 1) { // verbatim
            for (int i = 0; i < blockSize; ++i) {
                samples[i] = readSample();
            }
        } else if (type >= 8 && type <= 12) { // fixed predictor
            const int order = type - 8;
            if (order > blockSize) {
                return false;
            }
            for (int i = 0; i < order; ++i) {
                samples[i] = readSample();
            }
            if (!residual(bits, samples, blockSize, order)) {
                return false;
            }
            static constexpr int kFixed[5][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1}};
            for (int i = order; i < blockSize; ++i) {
                std::int64_t value = samples[i];
                for (int k = 0; k < order; ++k) {
                    value += kFixed[order][k] * samples[i - 1 - k];
                }
                if (value >= bound || value < -bound) {
                    return false;
                }
                samples[i] = value;
            }
        } else if (type >= 32) { // linear prediction
            const int order = (type & 31) + 1;
            if (order > blockSize) {
                return false;
            }
            for (int i = 0; i < order; ++i) {
                samples[i] = readSample();
            }
            const int precision = int(bits.read(4)) + 1;
            const int shift = bits.readSigned(5);
            if (precision == 16 || shift < 0) {
                return false;
            }
            std::int64_t coefficients[32];
            for (int i = 0; i < order; ++i) {
                coefficients[i] = bits.readSigned(precision);
            }
            if (!residual(bits, samples, blockSize, order)) {
                return false;
            }
            for (int i = order; i < blockSize; ++i) {
                std::int64_t sum = 0;
                for (int k = 0; k < order; ++k) {
                    sum += coefficients[k] * samples[i - 1 - k];
                }
                const std::int64_t value = samples[i] + (sum >> shift);
                if (value >= bound || value < -bound) {
                    return false;
                }
                samples[i] = value;
            }
        } else {
            return false; // reserved
        }
        if (wasted > 0) {
            for (int i = 0; i < blockSize; ++i) {
                samples[i] *= std::int64_t(1) << wasted;
            }
        }
        return !bits.overrun();
    }

    bool decodeFrame(std::size_t at, const FrameHeader &header, std::size_t &frameBytes) {
        const int blockSize = header.blockSize;
        const std::size_t channels = std::size_t(m_channels);
        m_work.resize(std::size_t(blockSize) * channels);
        MsbBitReader bits(m_data + at, m_size - at);
        bits.skip(std::uint64_t(header.headerBytes) * 8);
        for (int channel = 0; channel < m_channels; ++channel) {
            // The difference channel of a stereo pair has one more bit.
            const bool side = (header.assignment == 1 && channel == 1) || (header.assignment == 2 && channel == 0) ||
                              (header.assignment == 3 && channel == 1);
            if (!subframe(bits, m_work.data() + std::size_t(channel) * std::size_t(blockSize), blockSize,
                          m_bits + (side ? 1 : 0))) {
                return false;
            }
        }
        bits.alignToByte();
        const std::size_t body = std::size_t(bits.position() / 8);
        if (bits.overrun() || m_size - at < body + 2 ||
            crc16(m_data + at, body) != std::uint16_t(m_data[at + body] << 8 | m_data[at + body + 1])) {
            return false;
        }
        frameBytes = body + 2;

        std::int64_t *first = m_work.data();
        std::int64_t *second = m_work.data() + blockSize;
        if (header.assignment == 1) { // left, side
            for (int i = 0; i < blockSize; ++i) {
                second[i] = first[i] - second[i];
            }
        } else if (header.assignment == 2) { // side, right
            for (int i = 0; i < blockSize; ++i) {
                first[i] += second[i];
            }
        } else if (header.assignment == 3) { // mid, side
            for (int i = 0; i < blockSize; ++i) {
                const std::int64_t side = second[i];
                const std::int64_t mid = first[i] * 2 + (side & 1);
                first[i] = (mid + side) >> 1;
                second[i] = (mid - side) >> 1;
            }
        }
        m_block.resize(std::size_t(blockSize) * channels);
        const float scale = float(1.0 / double(std::int64_t(1) << (m_bits - 1)));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const std::int64_t *in = m_work.data() + channel * std::size_t(blockSize);
            for (int i = 0; i < blockSize; ++i) {
                m_block[std::size_t(i) * channels + channel] = float(double(in[i])) * scale;
            }
        }
        m_blockFrames = std::size_t(blockSize);
        m_blockRead = 0;
        return true;
    }

    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_firstFrame;
    int m_sampleRate;
    int m_channels;
    int m_bits;
    std::uint64_t m_total;
    int m_minBlock;
    int m_maxBlock;

    std::size_t m_offset;
    std::uint64_t m_position = 0;
    bool m_pendingSeek = false;
    std::vector<std::int64_t> m_work; // one block, channel after channel
    std::vector<float> m_block;       // the same, interleaved
    std::size_t m_blockFrames = 0;
    std::size_t m_blockRead = 0;
};

} // namespace

Result<std::unique_ptr<AudioStream>> openFlacStream(Span<const std::byte> data, const AudioLimits &limits) {
    std::size_t offset = id3v2Size(data);
    if (!matches(data, offset, "fLaC", 4)) {
        return Error(ErrorCode::ParseError, "not a FLAC file");
    }
    offset += 4;
    bool haveInfo = false;
    int sampleRate = 0, channels = 0, bits = 0, minBlock = 0, maxBlock = 0;
    std::uint64_t total = 0;
    // Metadata blocks: a flag and type byte, a 24-bit length, the body.
    for (bool last = false; !last;) {
        if (data.size() - offset < 4) {
            return Error(ErrorCode::Corrupt, "FLAC: the metadata is cut short");
        }
        const std::uint8_t head = byteAt(data, offset);
        const std::size_t length = be24(data, offset + 1);
        offset += 4;
        if (data.size() - offset < length) {
            return Error(ErrorCode::Corrupt, "FLAC: a metadata block runs past the end of the file");
        }
        last = (head & 0x80) != 0;
        if ((head & 0x7F) == 0) { // STREAMINFO
            if (length < 34) {
                return Error(ErrorCode::Corrupt, "FLAC: the stream information is too short");
            }
            minBlock = int(be16(data, offset));
            maxBlock = int(be16(data, offset + 2));
            const std::uint64_t packed = std::uint64_t(be32(data, offset + 10)) << 32 | be32(data, offset + 14);
            sampleRate = int(packed >> 44);
            channels = int((packed >> 41) & 7) + 1;
            bits = int((packed >> 36) & 31) + 1;
            total = packed & ((std::uint64_t(1) << 36) - 1);
            haveInfo = true;
        }
        offset += length;
    }
    if (!haveInfo) {
        return Error(ErrorCode::Corrupt, "FLAC: no stream information");
    }
    if (Result<void> shape = checkStreamShape(channels, sampleRate, limits, "FLAC"); !shape) {
        return std::move(shape).error();
    }
    if (bits < 4 || channels > kMaxFlacChannels || maxBlock < 16 || minBlock > maxBlock) {
        return Error(ErrorCode::Corrupt, "FLAC: the stream information is not valid");
    }
    return std::unique_ptr<AudioStream>(
        new FlacStream(data, offset, sampleRate, channels, bits, total, minBlock == maxBlock ? minBlock : 0, maxBlock));
}

} // namespace cfw::detail
