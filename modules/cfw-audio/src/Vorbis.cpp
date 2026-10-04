// Ogg Vorbis: the Ogg container (pages with a CRC, packets laced across
// them) and the Vorbis I codec inside it, after the Xiph.Org specification.
//
// A Vorbis stream describes its own entropy code: the setup header holds the
// Huffman code books, the floor and residue configurations and the channel
// mappings, all of them sized by the stream. Each is validated against the
// packet that carries it and against fixed bounds before any table is built,
// and every index one table holds into another is checked once, here, so the
// audio path can use them without rechecking.
//
// An audio packet is: a mode, a floor curve per channel (the spectral
// envelope, as line segments in a dB scale), the residue (the spectrum
// divided by that envelope, vector quantised), channel coupling undone, an
// inverse MDCT, and a windowed overlap with the block before. Reading past
// the end of a packet is not an error in Vorbis: decoding of that part stops
// and what was read is used.
//
// Floor type 0 is not implemented (see AudioDecoder.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "DecoderCommon.h"

namespace cfw::detail {

namespace {

constexpr std::size_t kNoPage = ~std::size_t(0);
constexpr int kFastBits = 10;
constexpr std::size_t kMaxCodebookEntries = std::size_t(1) << 21; // over the whole setup

int ilog(std::uint32_t value) noexcept {
    int bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

std::uint32_t reverseBits(std::uint32_t v) noexcept {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

struct OggCrcTable {
    std::array<std::uint32_t, 256> values{};
    constexpr OggCrcTable() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i << 24;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
            }
            values[i] = crc;
        }
    }
};
constexpr OggCrcTable kOggCrc;

// Bits least significant first, as Vorbis packs them. Past the end of the
// packet it reads zeros, and ended() says so.
class LsbBitReader {
public:
    LsbBitReader(const std::uint8_t *data, std::size_t bytes) noexcept : m_data(data), m_bytes(bytes) {}

    [[nodiscard]] bool ended() const noexcept { return m_position > std::uint64_t(m_bytes) * 8; }
    [[nodiscard]] std::uint32_t peek32() const noexcept {
        const std::size_t byte = std::size_t(m_position >> 3);
        std::uint64_t window = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            if (byte < m_bytes && i < m_bytes - byte) {
                window |= std::uint64_t(m_data[byte + i]) << (8 * i);
            }
        }
        return std::uint32_t(window >> (m_position & 7));
    }
    void skip(int bits) noexcept { m_position += std::uint64_t(bits); }
    [[nodiscard]] std::uint32_t read(int bits) noexcept {
        if (bits <= 0) {
            return 0;
        }
        const std::uint32_t value = bits >= 32 ? peek32() : peek32() & ((1u << bits) - 1u);
        m_position += std::uint64_t(bits);
        return value;
    }

private:
    const std::uint8_t *m_data;
    std::size_t m_bytes;
    std::uint64_t m_position = 0;
};

float unpackFloat(std::uint32_t packed) noexcept {
    const double mantissa = double(packed & 0x1FFFFFu);
    const int exponent = int((packed & 0x7FE00000u) >> 21);
    return float(std::ldexp((packed & 0x80000000u) ? -mantissa : mantissa, exponent - 788));
}

struct Codebook {
    int dimensions = 0;
    int entries = 0;
    std::vector<std::uint8_t> lengths; // per entry; 0: unused
    std::vector<std::int32_t> fast;    // by the next kFastBits bits: the entry, or -1
    // Used entries by code, first bit highest, for codes the fast table misses.
    std::vector<std::uint32_t> sortedCodes;
    std::vector<std::int32_t> sortedEntries;
    int lookupType = 0;
    bool sequence = false;
    std::uint32_t lookupValues = 0;
    std::vector<float> values; // multiplicand * delta + minimum

    // The next entry, or -1 at the end of the packet or on a code no entry has.
    [[nodiscard]] int decode(LsbBitReader &bits) const noexcept {
        const std::uint32_t window = bits.peek32();
        int entry = fast[window & ((1u << kFastBits) - 1u)];
        if (entry < 0) {
            const std::uint32_t code = reverseBits(window);
            const auto after = std::upper_bound(sortedCodes.begin(), sortedCodes.end(), code);
            if (after == sortedCodes.begin()) {
                return -1;
            }
            const std::size_t index = std::size_t(after - sortedCodes.begin()) - 1;
            entry = sortedEntries[index];
            const int length = lengths[std::size_t(entry)];
            if (((code ^ sortedCodes[index]) >> (32 - length)) != 0) {
                return -1;
            }
        }
        bits.skip(lengths[std::size_t(entry)]);
        return bits.ended() ? -1 : entry;
    }

    // Element `i` of the vector that `entry` stands for; `last` carries the
    // running sum of a sequence book.
    [[nodiscard]] float element(int entry, int i, std::uint64_t &divisor, float &last) const noexcept {
        std::size_t offset;
        if (lookupType == 1) {
            offset = std::size_t((std::uint64_t(entry) / divisor) % lookupValues);
            divisor = divisor > 0xFFFFFFFFu ? divisor : divisor * lookupValues;
        } else {
            offset = std::size_t(entry) * std::size_t(dimensions) + std::size_t(i);
        }
        const float value = values[offset] + last;
        if (sequence) {
            last = value;
        }
        return value;
    }
};

struct Floor1 {
    int partitions = 0;
    std::uint8_t partitionClass[32] = {};
    std::uint8_t classDimensions[16] = {};
    std::uint8_t classSubclasses[16] = {};
    std::uint8_t classMasterbook[16] = {};
    std::int16_t subclassBooks[16][8] = {};
    int multiplier = 1;
    int values = 0;
    std::uint16_t x[65] = {};
    std::uint8_t sorted[65] = {}; // indices into x, by x
    std::uint8_t low[65] = {};    // the neighbours of point i among points 0..i-1
    std::uint8_t high[65] = {};
};

struct Residue {
    int type = 0;
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::uint32_t partitionSize = 1;
    int classifications = 1;
    int classbook = 0;
    std::int16_t books[64][8] = {};
};

struct Mapping {
    int submaps = 1;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> coupling; // magnitude, angle
    std::vector<std::uint8_t> mux;                               // per channel: its submap
    std::uint8_t floor[16] = {};
    std::uint8_t residue[16] = {};
};

struct Mode {
    bool longBlock = false;
    std::uint8_t mapping = 0;
};

// The inverse MDCT of Vorbis (no scaling): `size` outputs from size / 2
// coefficients, through a complex FFT of size / 4 points.
class InverseMdct {
public:
    explicit InverseMdct(int size) : m_size(size), m_quarter(size / 4) {
        const int half = size / 2;
        const double pi = std::numbers::pi;
        m_pre.resize(std::size_t(m_quarter) * 2);
        m_post.resize(std::size_t(m_quarter) * 2);
        for (int j = 0; j < m_quarter; ++j) {
            const double a = -pi * double(4 * j + 1) / double(4 * half);
            m_pre[std::size_t(j) * 2] = float(std::cos(a));
            m_pre[std::size_t(j) * 2 + 1] = float(std::sin(a));
            const double b = -pi * double(j) / double(half);
            m_post[std::size_t(j) * 2] = float(std::cos(b));
            m_post[std::size_t(j) * 2 + 1] = float(std::sin(b));
        }
        m_roots.resize(std::size_t(std::max(1, m_quarter / 2)) * 2);
        for (int k = 0; k < m_quarter / 2; ++k) {
            const double a = -2.0 * pi * double(k) / double(m_quarter);
            m_roots[std::size_t(k) * 2] = float(std::cos(a));
            m_roots[std::size_t(k) * 2 + 1] = float(std::sin(a));
        }
        m_reversed.resize(std::size_t(m_quarter));
        const int bits = ilog(std::uint32_t(m_quarter)) - 1;
        for (int i = 0; i < m_quarter; ++i) {
            m_reversed[std::size_t(i)] = bits == 0 ? 0u : reverseBits(std::uint32_t(i)) >> (32 - bits);
        }
        m_work.resize(std::size_t(m_quarter) * 2);
        m_folded.resize(std::size_t(half));
    }

    // `spectrum` has size / 2 values, `out` room for size.
    void run(const float *spectrum, float *out) noexcept {
        const int half = m_size / 2;
        const int quarter = m_quarter;
        float *w = m_work.data();
        for (int j = 0; j < quarter; ++j) {
            const float re = spectrum[2 * j];
            const float im = spectrum[half - 1 - 2 * j];
            const float c = m_pre[std::size_t(j) * 2];
            const float s = m_pre[std::size_t(j) * 2 + 1];
            const std::size_t to = std::size_t(m_reversed[std::size_t(j)]) * 2;
            w[to] = re * c - im * s;
            w[to + 1] = re * s + im * c;
        }
        for (int span = 1; span < quarter; span *= 2) {
            const int stride = quarter / (span * 2);
            for (int start = 0; start < quarter; start += span * 2) {
                for (int k = 0; k < span; ++k) {
                    const float c = m_roots[std::size_t(k * stride) * 2];
                    const float s = m_roots[std::size_t(k * stride) * 2 + 1];
                    float *a = w + std::size_t(start + k) * 2;
                    float *b = w + std::size_t(start + k + span) * 2;
                    const float re = b[0] * c - b[1] * s;
                    const float im = b[0] * s + b[1] * c;
                    b[0] = a[0] - re;
                    b[1] = a[1] - im;
                    a[0] += re;
                    a[1] += im;
                }
            }
        }
        // The DCT-IV of the spectrum, then its odd and even extensions.
        float *u = m_folded.data();
        for (int k = 0; k < quarter; ++k) {
            const float re = w[std::size_t(k) * 2];
            const float im = w[std::size_t(k) * 2 + 1];
            const float c = m_post[std::size_t(k) * 2];
            const float s = m_post[std::size_t(k) * 2 + 1];
            u[2 * k] = re * c - im * s;
            u[half - 1 - 2 * k] = -(re * s + im * c);
        }
        const int h2 = half / 2;
        for (int n = 0; n < h2; ++n) {
            out[n] = u[n + h2];
        }
        for (int n = h2; n < half + h2; ++n) {
            out[n] = -u[half + h2 - 1 - n];
        }
        for (int n = half + h2; n < m_size; ++n) {
            out[n] = -u[n - half - h2];
        }
    }

private:
    int m_size;
    int m_quarter;
    std::vector<float> m_pre, m_post, m_roots, m_work, m_folded;
    std::vector<std::uint32_t> m_reversed;
};

struct Page {
    std::size_t offset = 0;
    std::size_t headerBytes = 0;
    std::size_t bodyBytes = 0;
    std::uint8_t flags = 0; // 1 continued packet, 2 first page, 4 last page
    std::int64_t granule = -1;
    std::uint32_t serial = 0;
    int segments = 0;
    [[nodiscard]] std::size_t end() const noexcept { return offset + headerBytes + bodyBytes; }
};

class VorbisStream final : public AudioStream {
public:
    explicit VorbisStream(Span<const std::byte> data)
        : m_data(reinterpret_cast<const std::uint8_t *>(data.data())), m_size(data.size()) {}

    Result<void> open(const AudioLimits &limits);

    AudioFormat format() const noexcept override { return AudioFormat::OggVorbis; }
    int sampleRate() const noexcept override { return m_sampleRate; }
    int channels() const noexcept override { return m_channels; }
    std::optional<std::uint64_t> totalFrames() const noexcept override { return m_total; }
    std::uint64_t position() const noexcept override { return m_position; }

    Result<std::size_t> read(Span<float> out) override {
        const std::size_t channels = std::size_t(m_channels);
        const std::size_t wanted = out.size() / channels;
        std::size_t done = 0;
        while (done < wanted) {
            if (m_queueRead == m_queueFrames && !decodeNext()) {
                break;
            }
            const std::size_t take = std::min(wanted - done, m_queueFrames - m_queueRead);
            std::copy_n(m_queue.data() + m_queueRead * channels, take * channels, out.data() + done * channels);
            m_queueRead += take;
            m_position += take;
            done += take;
        }
        return done;
    }

    Result<void> seekToFrame(std::uint64_t frame) override {
        if (m_total) {
            frame = std::min(frame, *m_total);
        }
        const std::int64_t target = std::int64_t(std::min<std::uint64_t>(frame, std::uint64_t(1) << 62)) + m_base;
        // Start from a page that ends at or before the target, decode to the
        // end of a page (which says where that is), then on to the target. A
        // page whose end cannot be placed sends the search one page earlier.
        std::int64_t wanted = target;
        for (int attempt = 0;; ++attempt) {
            std::int64_t pageGranule = -1;
            const std::size_t page = (wanted <= m_firstPosition || attempt >= 6) ? kNoPage
                                                                                 : pageEndingBy(wanted, pageGranule);
            restart(page);
            while (!m_positionKnown && decodeNext()) {
            }
            if (page == kNoPage || (m_positionKnown && m_decoded - std::int64_t(queued()) <= target)) {
                break;
            }
            wanted = pageGranule - 1;
        }
        // Drop what lies before the target.
        for (;;) {
            const std::int64_t queueStart = m_decoded - std::int64_t(queued());
            if (m_queueRead < m_queueFrames && queueStart < target) {
                const std::size_t drop = std::size_t(std::min<std::int64_t>(target - queueStart, std::int64_t(queued())));
                m_queueRead += drop;
            }
            if (m_queueRead < m_queueFrames || !decodeNext()) {
                break;
            }
        }
        const std::int64_t at = m_decoded - std::int64_t(queued());
        m_position = at > m_base ? std::uint64_t(at - m_base) : 0;
        return success();
    }

private:
    // ---- Ogg ----
    [[nodiscard]] bool pageAt(std::size_t offset, Page &page) const noexcept;
    [[nodiscard]] bool findPage(std::size_t from, std::size_t limit, Page &page) const noexcept;
    [[nodiscard]] std::size_t pageEndingBy(std::int64_t granule, std::int64_t &pageGranule) const noexcept;
    bool nextPacket();
    void restart(std::size_t pageOffset) noexcept;

    // ---- Vorbis ----
    Result<void> parseIdentification();
    Result<void> parseSetup(const AudioLimits &limits);
    Result<void> parseCodebook(LsbBitReader &bits, Codebook &book, std::size_t &totalEntries);
    [[nodiscard]] int packetBlockSize() const noexcept;
    bool decodeNext();
    int decodePacket();
    bool decodeFloor(LsbBitReader &bits, const Floor1 &floor, int channel);
    void renderFloor(const Floor1 &floor, int channel, int half);
    void decodeResidue(LsbBitReader &bits, const Residue &residue, const std::vector<int> &channelList, int half);
    [[nodiscard]] std::size_t queued() const noexcept { return m_queueFrames - m_queueRead; }

    const std::uint8_t *m_data;
    std::size_t m_size;

    // The Ogg reader: the page being read and where in it.
    std::uint32_t m_serial = 0;
    Page m_page;
    bool m_havePage = false;
    int m_segment = 0;
    std::size_t m_bodyOffset = 0;
    std::size_t m_nextPage = 0;
    std::vector<std::uint8_t> m_packet;
    bool m_packetLastOnPage = false;
    std::int64_t m_packetGranule = -1;
    bool m_packetEndsStream = false;
    // Where the audio packets start, to come back to.
    Page m_audioPage;
    bool m_audioHavePage = false;
    int m_audioSegment = 0;
    std::size_t m_audioBodyOffset = 0;
    std::size_t m_audioNextPage = 0;

    int m_sampleRate = 0;
    int m_channels = 0;
    int m_blockSize[2] = {0, 0};
    std::vector<Codebook> m_codebooks;
    std::vector<Floor1> m_floors;
    std::vector<Residue> m_residues;
    std::vector<Mapping> m_mappings;
    std::vector<Mode> m_modes;
    std::vector<InverseMdct> m_mdct;       // short, long
    std::vector<float> m_slope[2];         // the rising half of each window
    std::vector<float> m_inverseDb;

    // Per channel.
    std::vector<std::vector<float>> m_spectrum; // residue, then the spectrum
    std::vector<std::vector<float>> m_previous; // the windowed second half of the block before
    std::vector<std::array<std::int32_t, 65>> m_floorY;
    std::vector<std::array<bool, 65>> m_floorUsed;
    std::vector<char> m_silent;                 // the floor said: no energy
    std::vector<float> m_time;
    std::vector<float> m_interleaved;           // residue type 2
    std::vector<std::uint8_t> m_classes;
    std::vector<int> m_submapChannels;
    int m_previousSize = 0; // 0: no block before (the next one only primes the overlap)

    // Positions are in the stream's granule numbering; m_base is the first
    // sample the caller sees.
    std::int64_t m_firstPosition = 0;
    std::int64_t m_base = 0;
    std::int64_t m_decoded = 0; // the end of what has been decoded
    bool m_positionKnown = true;
    std::optional<std::uint64_t> m_total;
    std::uint64_t m_position = 0;
    std::vector<float> m_queue;
    std::size_t m_queueFrames = 0;
    std::size_t m_queueRead = 0;
};

// ---- Ogg -------------------------------------------------------------------

bool VorbisStream::pageAt(std::size_t offset, Page &page) const noexcept {
    if (offset > m_size || m_size - offset < 27 || m_data[offset] != 'O' || m_data[offset + 1] != 'g' ||
        m_data[offset + 2] != 'g' || m_data[offset + 3] != 'S' || m_data[offset + 4] != 0) {
        return false;
    }
    const int segments = m_data[offset + 26];
    const std::size_t headerBytes = 27 + std::size_t(segments);
    if (m_size - offset < headerBytes) {
        return false;
    }
    std::size_t bodyBytes = 0;
    for (int i = 0; i < segments; ++i) {
        bodyBytes += m_data[offset + 27 + std::size_t(i)];
    }
    if (m_size - offset - headerBytes < bodyBytes) {
        return false;
    }
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < headerBytes + bodyBytes; ++i) {
        const std::uint8_t byte = (i >= 22 && i < 26) ? std::uint8_t(0) : m_data[offset + i];
        crc = (crc << 8) ^ kOggCrc.values[(crc >> 24) ^ byte];
    }
    const Span<const std::byte> all(reinterpret_cast<const std::byte *>(m_data), m_size);
    if (crc != le32(all, offset + 22)) {
        return false;
    }
    page.offset = offset;
    page.headerBytes = headerBytes;
    page.bodyBytes = bodyBytes;
    page.flags = m_data[offset + 5];
    page.granule = std::int64_t(le64(all, offset + 6));
    page.serial = le32(all, offset + 14);
    page.segments = segments;
    return true;
}

// The first page of this stream that starts in [from, limit).
bool VorbisStream::findPage(std::size_t from, std::size_t limit, Page &page) const noexcept {
    for (std::size_t offset = from; offset < limit && m_size - offset >= 27; ++offset) {
        if (m_data[offset] == 'O' && pageAt(offset, page)) {
            if (page.serial == m_serial) {
                return true;
            }
            offset = page.end() - 1; // another stream's page
        }
    }
    return false;
}

// The last page whose packets end at or before `granule`: its offset and
// the position it ends at. kNoPage if the audio's first page ends later.
std::size_t VorbisStream::pageEndingBy(std::int64_t granule, std::int64_t &pageGranule) const noexcept {
    std::size_t low = m_audioNextPage;
    std::size_t high = m_size;
    std::size_t best = kNoPage;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        Page page;
        bool found = findPage(middle, high, page);
        const std::size_t first = page.offset;
        // A page no packet ends on has no position: look at the next.
        while (found && page.granule < 0) {
            found = findPage(page.end(), high, page);
        }
        if (!found || page.granule > granule) {
            high = middle;
        } else {
            best = first;
            pageGranule = page.granule;
            low = page.end();
        }
    }
    return best;
}

// Starts reading packets afresh at a page, or at the first audio packet.
void VorbisStream::restart(std::size_t pageOffset) noexcept {
    if (pageOffset == kNoPage) {
        m_page = m_audioPage;
        m_havePage = m_audioHavePage;
        m_segment = m_audioSegment;
        m_bodyOffset = m_audioBodyOffset;
        m_nextPage = m_audioNextPage;
        m_positionKnown = true;
        m_decoded = m_firstPosition;
    } else {
        m_havePage = false;
        m_nextPage = pageOffset;
        m_positionKnown = false;
        m_decoded = 0;
    }
    m_packet.clear();
    m_previousSize = 0;
    m_queueFrames = m_queueRead = 0;
}

// Assembles the next packet into m_packet. False at the end of the stream.
bool VorbisStream::nextPacket() {
    m_packet.clear();
    bool skipping = false;
    for (;;) {
        if (!m_havePage || m_segment >= m_page.segments) {
            Page page;
            if (!findPage(m_nextPage, m_size, page)) {
                m_havePage = false;
                m_nextPage = m_size;
                return false;
            }
            if ((page.flags & 1) == 0) {
                // A page that starts a packet: whatever was being gathered
                // (or skipped) lost its end with a missing page.
                m_packet.clear();
                skipping = false;
            } else if (m_packet.empty()) {
                skipping = true; // the rest of a packet whose start we do not have
            }
            m_page = page;
            m_havePage = true;
            m_segment = 0;
            m_bodyOffset = page.offset + page.headerBytes;
            m_nextPage = page.end();
        }
        while (m_segment < m_page.segments) {
            const std::size_t length = m_data[m_page.offset + 27 + std::size_t(m_segment)];
            ++m_segment;
            if (!skipping) {
                m_packet.insert(m_packet.end(), m_data + m_bodyOffset, m_data + m_bodyOffset + length);
            }
            m_bodyOffset += length;
            if (length < 255) {
                if (skipping) {
                    skipping = false;
                    continue;
                }
                // Does another packet end on this page?
                bool last = true;
                for (int i = m_segment; i < m_page.segments; ++i) {
                    last = last && m_data[m_page.offset + 27 + std::size_t(i)] == 255;
                }
                m_packetLastOnPage = last;
                m_packetGranule = m_page.granule;
                m_packetEndsStream = last && (m_page.flags & 4) != 0;
                return true;
            }
        }
    }
}

// ---- Headers ---------------------------------------------------------------

Result<void> VorbisStream::open(const AudioLimits &limits) {
    Page first;
    if (!pageAt(0, first)) {
        return Error(ErrorCode::ParseError, "not an Ogg file");
    }
    m_serial = first.serial;
    m_nextPage = 0;
    if (!nextPacket()) {
        return Error(ErrorCode::Corrupt, "Ogg: no packets");
    }
    if (Result<void> id = parseIdentification(); !id) {
        return id;
    }
    if (Result<void> shape = checkStreamShape(m_channels, m_sampleRate, limits, "Vorbis"); !shape) {
        return shape;
    }
    if (!nextPacket() || m_packet.size() < 7 || m_packet[0] != 3) {
        return Error(ErrorCode::Corrupt, "Vorbis: the comment header is missing");
    }
    if (!nextPacket() || m_packet.size() < 7 || m_packet[0] != 5 ||
        !std::equal(m_packet.begin() + 1, m_packet.begin() + 7, "vorbis")) {
        return Error(ErrorCode::Corrupt, "Vorbis: the setup header is missing");
    }
    if (Result<void> setup = parseSetup(limits); !setup) {
        return setup;
    }

    const std::size_t channels = std::size_t(m_channels);
    const std::size_t longHalf = std::size_t(m_blockSize[1]) / 2;
    m_mdct.emplace_back(m_blockSize[0]);
    m_mdct.emplace_back(m_blockSize[1]);
    for (int size = 0; size < 2; ++size) {
        const int half = m_blockSize[size] / 2;
        m_slope[size].resize(std::size_t(half));
        for (int i = 0; i < half; ++i) {
            const double s = std::sin((double(i) + 0.5) / double(half) * (std::numbers::pi / 2.0));
            m_slope[size][std::size_t(i)] = float(std::sin(std::numbers::pi / 2.0 * s * s));
        }
    }
    // The floor's dB scale: 256 steps of 140/256 dB up to 1.
    m_inverseDb.resize(256);
    for (int i = 0; i < 256; ++i) {
        m_inverseDb[std::size_t(i)] = float(std::pow(10.0, double(i - 255) * (140.0 / 256.0) / 20.0));
    }
    m_spectrum.assign(channels, std::vector<float>(longHalf));
    m_previous.assign(channels, std::vector<float>(longHalf));
    m_floorY.resize(channels);
    m_floorUsed.resize(channels);
    m_silent.assign(channels, 0);
    m_time.resize(std::size_t(m_blockSize[1]));
    m_queue.resize(longHalf * channels);

    // The audio starts here.
    m_audioPage = m_page;
    m_audioHavePage = m_havePage;
    m_audioSegment = m_segment;
    m_audioBodyOffset = m_bodyOffset;
    m_audioNextPage = m_nextPage;

    // Where the first sample is. The first page's position is where its
    // last packet ends, so it says what position the stream starts at: 0
    // ordinarily, later for a stream cut from a longer one, and earlier
    // (negative) when the first samples are to be dropped.
    std::int64_t samples = 0;
    int previous = 0;
    while (nextPacket()) {
        const int size = packetBlockSize();
        if (size > 0) {
            samples += previous > 0 ? previous / 4 + size / 4 : 0;
            previous = size;
        }
        if (m_packetLastOnPage && m_packetGranule >= 0) {
            m_firstPosition = m_packetGranule - samples;
            if (m_packetEndsStream) {
                m_firstPosition = std::max<std::int64_t>(m_firstPosition, 0); // the end is trimmed instead
            }
            break;
        }
        if (samples > (std::int64_t(1) << 40)) {
            break;
        }
    }
    m_base = std::max<std::int64_t>(m_firstPosition, 0);

    // The last page's position is the length.
    const std::size_t tail = m_size > 262144 ? m_size - 262144 : 0;
    for (std::size_t offset = m_size >= 27 ? m_size - 27 : 0;; --offset) {
        Page page;
        if (m_data[offset] == 'O' && pageAt(offset, page) && page.serial == m_serial && page.granule >= 0) {
            const bool audio = page.offset >= m_audioNextPage && page.granule >= m_base;
            m_total = audio ? std::uint64_t(page.granule - m_base) : std::uint64_t(0);
            break;
        }
        if (offset <= tail) {
            break;
        }
    }
    restart(kNoPage);
    return success();
}

Result<void> VorbisStream::parseIdentification() {
    if (m_packet.size() < 30 || m_packet[0] != 1 || !std::equal(m_packet.begin() + 1, m_packet.begin() + 7, "vorbis")) {
        return Error(ErrorCode::Unsupported, "Ogg: the stream is not Vorbis");
    }
    const Span<const std::byte> packet(reinterpret_cast<const std::byte *>(m_packet.data()), m_packet.size());
    if (le32(packet, 7) != 0) {
        return Error(ErrorCode::Unsupported, "Vorbis: unknown version");
    }
    m_channels = m_packet[11];
    const std::uint32_t rate = le32(packet, 12);
    m_sampleRate = rate > 0x7FFFFFFFu ? 0x7FFFFFFF : int(rate);
    const int exponent0 = m_packet[28] & 15;
    const int exponent1 = m_packet[28] >> 4;
    if (exponent0 < 6 || exponent0 > 13 || exponent1 < exponent0 || exponent1 > 13 || (m_packet[29] & 1) == 0) {
        return Error(ErrorCode::Corrupt, "Vorbis: bad block sizes");
    }
    m_blockSize[0] = 1 << exponent0;
    m_blockSize[1] = 1 << exponent1;
    return success();
}

Result<void> VorbisStream::parseCodebook(LsbBitReader &bits, Codebook &book, std::size_t &totalEntries) {
    const auto corrupt = [](const char *what) { return Error(ErrorCode::Corrupt, String("Vorbis: ") + what); };
    if (bits.read(24) != 0x564342) {
        return corrupt("a code book does not start where it should");
    }
    book.dimensions = int(bits.read(16));
    book.entries = int(bits.read(24));
    totalEntries += std::size_t(book.entries);
    if (bits.ended() || totalEntries > kMaxCodebookEntries) {
        return Error(ErrorCode::LimitExceeded, "Vorbis: the code books are too large");
    }
    const std::size_t entries = std::size_t(book.entries);
    book.lengths.assign(entries, 0);
    if (bits.read(1) == 0) {
        const bool sparse = bits.read(1) != 0;
        for (std::size_t i = 0; i < entries; ++i) {
            if (!sparse || bits.read(1) != 0) {
                book.lengths[i] = std::uint8_t(bits.read(5) + 1);
            }
        }
    } else {
        std::size_t entry = 0;
        int length = int(bits.read(5)) + 1;
        while (entry < entries) {
            const std::size_t count = bits.read(ilog(std::uint32_t(entries - entry)));
            if (count > entries - entry || length > 32 || bits.ended()) {
                return corrupt("a code book's lengths run past its entries");
            }
            std::fill_n(book.lengths.begin() + std::ptrdiff_t(entry), count, std::uint8_t(length));
            entry += count;
            ++length;
        }
    }
    if (bits.ended()) {
        return corrupt("a code book is cut short");
    }

    // Code words: each entry takes the lowest code still free at its length.
    std::uint32_t available[33] = {};
    std::vector<std::pair<std::uint32_t, std::int32_t>> codes;
    bool first = true;
    for (std::size_t i = 0; i < entries; ++i) {
        const int length = book.lengths[i];
        if (length == 0) {
            continue;
        }
        std::uint32_t code = 0;
        if (first) {
            for (int bit = 1; bit <= length; ++bit) {
                available[bit] = 1u << (32 - bit);
            }
            first = false;
        } else {
            int from = length;
            while (from > 0 && available[from] == 0) {
                --from;
            }
            if (from == 0) {
                return corrupt("a code book has more codes than fit");
            }
            code = available[from];
            available[from] = 0;
            for (int bit = length; bit > from; --bit) {
                available[bit] = code + (1u << (32 - bit));
            }
        }
        codes.emplace_back(code, std::int32_t(i));
    }
    std::sort(codes.begin(), codes.end());
    book.fast.assign(std::size_t(1) << kFastBits, -1);
    book.sortedCodes.reserve(codes.size());
    book.sortedEntries.reserve(codes.size());
    for (const auto &[code, entry] : codes) {
        book.sortedCodes.push_back(code);
        book.sortedEntries.push_back(entry);
        const int length = book.lengths[std::size_t(entry)];
        if (length <= kFastBits) {
            // As read: first bit lowest, then every pattern of the bits after.
            const std::uint32_t pattern = reverseBits(code);
            for (std::uint32_t rest = 0; rest < (1u << (kFastBits - length)); ++rest) {
                book.fast[pattern | (rest << length)] = entry;
            }
        }
    }

    book.lookupType = int(bits.read(4));
    if (book.lookupType > 2) {
        return corrupt("a code book has an unknown lookup type");
    }
    if (book.lookupType != 0) {
        const float minimum = unpackFloat(bits.read(32));
        const float delta = unpackFloat(bits.read(32));
        const int valueBits = int(bits.read(4)) + 1;
        book.sequence = bits.read(1) != 0;
        if (book.dimensions == 0) {
            return corrupt("a vector code book has no dimensions");
        }
        std::uint64_t count = 0;
        if (book.lookupType == 1) {
            // The largest r with r ^ dimensions <= entries.
            std::uint32_t r = 0;
            for (;;) {
                std::uint64_t power = 1;
                bool fits = true;
                for (int d = 0; d < book.dimensions && fits; ++d) {
                    power *= std::uint64_t(r) + 1;
                    fits = power <= entries;
                }
                if (!fits) {
                    break;
                }
                ++r;
            }
            book.lookupValues = r;
            count = r;
        } else {
            count = std::uint64_t(entries) * std::uint64_t(book.dimensions);
        }
        // Each value takes at least a bit of the packet, so the packet bounds
        // the table; checked as it is read.
        book.values.reserve(std::size_t(std::min<std::uint64_t>(count, 65536)));
        for (std::uint64_t i = 0; i < count; ++i) {
            book.values.push_back(float(bits.read(valueBits)) * delta + minimum);
            if (bits.ended()) {
                return corrupt("a code book's values are cut short");
            }
        }
        if (book.lookupType == 1 && book.lookupValues == 0 && entries > 0) {
            return corrupt("a code book has no values");
        }
    }
    return success();
}

Result<void> VorbisStream::parseSetup(const AudioLimits &) {
    const auto corrupt = [](const char *what) { return Error(ErrorCode::Corrupt, String("Vorbis: ") + what); };
    LsbBitReader bits(m_packet.data() + 7, m_packet.size() - 7);

    const int bookCount = int(bits.read(8)) + 1;
    m_codebooks.resize(std::size_t(bookCount));
    std::size_t totalEntries = 0;
    for (Codebook &book : m_codebooks) {
        if (Result<void> parsed = parseCodebook(bits, book, totalEntries); !parsed) {
            return parsed;
        }
    }
    const auto validBook = [&](int index) { return index >= 0 && index < bookCount; };

    // Time domain transforms: placeholders, all zero.
    for (int i = int(bits.read(6)) + 1; i > 0; --i) {
        if (bits.read(16) != 0) {
            return corrupt("an unknown time domain transform");
        }
    }

    const int floorCount = int(bits.read(6)) + 1;
    m_floors.resize(std::size_t(floorCount));
    for (Floor1 &floor : m_floors) {
        const std::uint32_t type = bits.read(16);
        if (type == 0) {
            return Error(ErrorCode::Unsupported, "Vorbis: floor type 0 is not supported");
        }
        if (type != 1 || bits.ended()) {
            return corrupt("an unknown floor type");
        }
        floor.partitions = int(bits.read(5));
        int maxClass = -1;
        for (int i = 0; i < floor.partitions; ++i) {
            floor.partitionClass[i] = std::uint8_t(bits.read(4));
            maxClass = std::max(maxClass, int(floor.partitionClass[i]));
        }
        for (int c = 0; c <= maxClass; ++c) {
            floor.classDimensions[c] = std::uint8_t(bits.read(3) + 1);
            floor.classSubclasses[c] = std::uint8_t(bits.read(2));
            if (floor.classSubclasses[c] != 0) {
                floor.classMasterbook[c] = std::uint8_t(bits.read(8));
                if (!validBook(floor.classMasterbook[c])) {
                    return corrupt("a floor names a code book that does not exist");
                }
            }
            for (int s = 0; s < (1 << floor.classSubclasses[c]); ++s) {
                floor.subclassBooks[c][s] = std::int16_t(int(bits.read(8)) - 1);
                if (floor.subclassBooks[c][s] >= bookCount) {
                    return corrupt("a floor names a code book that does not exist");
                }
            }
        }
        floor.multiplier = int(bits.read(2)) + 1;
        const int rangeBits = int(bits.read(4));
        floor.x[0] = 0;
        floor.x[1] = std::uint16_t(1u << rangeBits);
        floor.values = 2;
        for (int i = 0; i < floor.partitions; ++i) {
            for (int d = 0; d < floor.classDimensions[floor.partitionClass[i]]; ++d) {
                if (floor.values >= 65) {
                    return corrupt("a floor has too many points");
                }
                floor.x[floor.values++] = std::uint16_t(bits.read(rangeBits));
            }
        }
        for (int i = 0; i < floor.values; ++i) {
            floor.sorted[i] = std::uint8_t(i);
        }
        std::sort(floor.sorted, floor.sorted + floor.values,
                  [&floor](std::uint8_t a, std::uint8_t b) { return floor.x[a] < floor.x[b]; });
        for (int i = 1; i < floor.values; ++i) {
            if (floor.x[floor.sorted[i]] == floor.x[floor.sorted[i - 1]]) {
                return corrupt("a floor has two points at one position");
            }
        }
        for (int i = 2; i < floor.values; ++i) {
            int low = 0, high = 1;
            for (int j = 0; j < i; ++j) {
                if (floor.x[j] < floor.x[i] && floor.x[j] > floor.x[low]) {
                    low = j;
                }
                if (floor.x[j] > floor.x[i] && floor.x[j] < floor.x[high]) {
                    high = j;
                }
            }
            floor.low[i] = std::uint8_t(low);
            floor.high[i] = std::uint8_t(high);
        }
    }

    const int residueCount = int(bits.read(6)) + 1;
    m_residues.resize(std::size_t(residueCount));
    for (Residue &residue : m_residues) {
        residue.type = int(bits.read(16));
        if (residue.type > 2 || bits.ended()) {
            return corrupt("an unknown residue type");
        }
        residue.begin = bits.read(24);
        residue.end = bits.read(24);
        residue.partitionSize = bits.read(24) + 1;
        residue.classifications = int(bits.read(6)) + 1;
        residue.classbook = int(bits.read(8));
        if (!validBook(residue.classbook) || m_codebooks[std::size_t(residue.classbook)].dimensions == 0 ||
            residue.end < residue.begin) {
            return corrupt("a residue's class book is not usable");
        }
        std::uint8_t cascade[64] = {};
        for (int c = 0; c < residue.classifications; ++c) {
            const std::uint32_t low = bits.read(3);
            const std::uint32_t high = bits.read(1) != 0 ? bits.read(5) : 0;
            cascade[c] = std::uint8_t(high * 8 + low);
        }
        for (int c = 0; c < residue.classifications; ++c) {
            for (int pass = 0; pass < 8; ++pass) {
                residue.books[c][pass] = -1;
                if (cascade[c] & (1 << pass)) {
                    const int book = int(bits.read(8));
                    // A residue book must turn entries into vectors.
                    if (!validBook(book) || m_codebooks[std::size_t(book)].lookupType == 0) {
                        return corrupt("a residue names a code book that cannot hold vectors");
                    }
                    residue.books[c][pass] = std::int16_t(book);
                }
            }
        }
    }

    const int mappingCount = int(bits.read(6)) + 1;
    m_mappings.resize(std::size_t(mappingCount));
    for (Mapping &mapping : m_mappings) {
        if (bits.read(16) != 0 || bits.ended()) {
            return corrupt("an unknown mapping type");
        }
        mapping.submaps = bits.read(1) != 0 ? int(bits.read(4)) + 1 : 1;
        if (bits.read(1) != 0) {
            const int steps = int(bits.read(8)) + 1;
            const int channelBits = ilog(std::uint32_t(m_channels - 1));
            for (int i = 0; i < steps; ++i) {
                const std::uint32_t magnitude = bits.read(channelBits);
                const std::uint32_t angle = bits.read(channelBits);
                if (magnitude == angle || magnitude >= std::uint32_t(m_channels) || angle >= std::uint32_t(m_channels)) {
                    return corrupt("a channel is coupled with itself or with none");
                }
                mapping.coupling.emplace_back(std::uint8_t(magnitude), std::uint8_t(angle));
            }
        }
        if (bits.read(2) != 0) {
            return corrupt("a mapping's reserved bits are set");
        }
        mapping.mux.assign(std::size_t(m_channels), 0);
        if (mapping.submaps > 1) {
            for (std::uint8_t &mux : mapping.mux) {
                mux = std::uint8_t(bits.read(4));
                if (mux >= mapping.submaps) {
                    return corrupt("a channel is in a submap that does not exist");
                }
            }
        }
        for (int s = 0; s < mapping.submaps; ++s) {
            bits.skip(8);
            mapping.floor[s] = std::uint8_t(bits.read(8));
            mapping.residue[s] = std::uint8_t(bits.read(8));
            if (mapping.floor[s] >= floorCount || mapping.residue[s] >= residueCount) {
                return corrupt("a mapping names a floor or residue that does not exist");
            }
        }
    }

    const int modeCount = int(bits.read(6)) + 1;
    m_modes.resize(std::size_t(modeCount));
    for (Mode &mode : m_modes) {
        mode.longBlock = bits.read(1) != 0;
        const std::uint32_t windowType = bits.read(16);
        const std::uint32_t transformType = bits.read(16);
        mode.mapping = std::uint8_t(bits.read(8));
        if (windowType != 0 || transformType != 0 || mode.mapping >= mappingCount) {
            return corrupt("a mode is not valid");
        }
    }
    if (bits.read(1) == 0 || bits.ended()) {
        return corrupt("the setup header is cut short");
    }
    return success();
}

// ---- Audio packets ---------------------------------------------------------

// The block size of the audio packet in m_packet; 0 if it is not one.
int VorbisStream::packetBlockSize() const noexcept {
    LsbBitReader bits(m_packet.data(), m_packet.size());
    if (m_packet.empty() || bits.read(1) != 0) {
        return 0;
    }
    const std::uint32_t mode = bits.read(ilog(std::uint32_t(m_modes.size() - 1)));
    if (mode >= m_modes.size() || bits.ended()) {
        return 0;
    }
    return m_blockSize[m_modes[mode].longBlock ? 1 : 0];
}

bool VorbisStream::decodeFloor(LsbBitReader &bits, const Floor1 &floor, int channel) {
    static constexpr int kRanges[4] = {256, 128, 86, 64};
    const int range = kRanges[floor.multiplier - 1];
    std::array<std::int32_t, 65> &y = m_floorY[std::size_t(channel)];
    std::array<bool, 65> &used = m_floorUsed[std::size_t(channel)];
    if (bits.read(1) == 0) {
        return false;
    }
    const int amplitudeBits = ilog(std::uint32_t(range - 1));
    y[0] = std::int32_t(bits.read(amplitudeBits));
    y[1] = std::int32_t(bits.read(amplitudeBits));
    int offset = 2;
    for (int p = 0; p < floor.partitions; ++p) {
        const int cls = floor.partitionClass[p];
        const int dimensions = floor.classDimensions[cls];
        const int subclassBits = floor.classSubclasses[cls];
        int value = 0;
        if (subclassBits > 0) {
            value = m_codebooks[floor.classMasterbook[cls]].decode(bits);
            if (value < 0) {
                return false;
            }
        }
        for (int d = 0; d < dimensions; ++d) {
            const int book = floor.subclassBooks[cls][value & ((1 << subclassBits) - 1)];
            value >>= subclassBits;
            int decoded = 0;
            if (book >= 0) {
                decoded = m_codebooks[std::size_t(book)].decode(bits);
                if (decoded < 0) {
                    return false;
                }
            }
            y[std::size_t(offset + d)] = decoded;
        }
        offset += dimensions;
    }
    if (bits.ended()) {
        return false;
    }

    // The values are differences from the line between each point's
    // neighbours; unwrap them.
    const auto predict = [&](int low, int high, int at) {
        const int dy = y[std::size_t(high)] - y[std::size_t(low)];
        const int adx = floor.x[high] - floor.x[low];
        const int off = std::abs(dy) * (floor.x[at] - floor.x[low]) / adx;
        return dy < 0 ? y[std::size_t(low)] - off : y[std::size_t(low)] + off;
    };
    used[0] = used[1] = true;
    y[0] = std::clamp(y[0], 0, range - 1);
    y[1] = std::clamp(y[1], 0, range - 1);
    for (int i = 2; i < floor.values; ++i) {
        const int low = floor.low[i];
        const int high = floor.high[i];
        const int predicted = predict(low, high, i);
        const int value = y[std::size_t(i)];
        const int highRoom = range - predicted;
        const int lowRoom = predicted;
        const int room = std::min(highRoom, lowRoom) * 2;
        int result = predicted;
        if (value != 0) {
            used[std::size_t(low)] = used[std::size_t(high)] = used[std::size_t(i)] = true;
            if (value >= room) {
                result = highRoom > lowRoom ? value - lowRoom + predicted : predicted - value + highRoom - 1;
            } else {
                result = (value & 1) ? predicted - (value + 1) / 2 : predicted + value / 2;
            }
        } else {
            used[std::size_t(i)] = false;
        }
        y[std::size_t(i)] = std::clamp(result, 0, range - 1);
    }
    return true;
}

// Draws the floor's line segments into the channel's spectrum buffer as
// multipliers, multiplying the residue already there.
void VorbisStream::renderFloor(const Floor1 &floor, int channel, int half) {
    const std::array<std::int32_t, 65> &y = m_floorY[std::size_t(channel)];
    const std::array<bool, 65> &used = m_floorUsed[std::size_t(channel)];
    float *spectrum = m_spectrum[std::size_t(channel)].data();
    const auto scale = [&](int x, int level) { spectrum[x] *= m_inverseDb[std::size_t(std::clamp(level, 0, 255))]; };
    int lx = 0;
    int ly = y[floor.sorted[0]] * floor.multiplier;
    for (int i = 1; i < floor.values; ++i) {
        const int point = floor.sorted[i];
        if (!used[std::size_t(point)]) {
            continue;
        }
        const int hx = floor.x[point];
        const int hy = y[std::size_t(point)] * floor.multiplier;
        // The line from (lx, ly) up to, not including, (hx, hy).
        const int dy = hy - ly;
        const int adx = hx - lx;
        const int base = dy / adx;
        const int step = dy < 0 ? base - 1 : base + 1;
        const int ady = std::abs(dy) - std::abs(base) * adx;
        int error = 0;
        int level = ly;
        const int end = std::min(hx, half);
        if (lx < end) {
            scale(lx, level);
        }
        for (int x = lx + 1; x < end; ++x) {
            error += ady;
            if (error >= adx) {
                error -= adx;
                level += step;
            } else {
                level += base;
            }
            scale(x, level);
        }
        lx = hx;
        ly = hy;
        if (lx >= half) {
            break;
        }
    }
    for (int x = lx; x < half; ++x) {
        scale(x, ly);
    }
}

// Residue types 0 and 1 over the channels in `channelList` (type 2 arrives
// here as one interleaved vector). Vectors accumulate into m_spectrum, or
// m_interleaved for type 2.
void VorbisStream::decodeResidue(LsbBitReader &bits, const Residue &residue, const std::vector<int> &channelList,
                                 int half) {
    const bool interleave = residue.type == 2;
    const std::size_t vectors = interleave ? 1 : channelList.size();
    const std::uint32_t size = interleave ? std::uint32_t(half) * std::uint32_t(channelList.size()) : std::uint32_t(half);
    const std::uint32_t begin = std::min(residue.begin, size);
    const std::uint32_t end = std::min(residue.end, size);
    const Codebook &classbook = m_codebooks[std::size_t(residue.classbook)];
    const std::uint32_t classWords = std::uint32_t(classbook.dimensions);
    const std::uint32_t partitions = (end - begin) / residue.partitionSize;
    if (partitions == 0 || vectors == 0) {
        return;
    }
    const std::size_t classStride = std::size_t(partitions) + classWords;
    m_classes.assign(classStride * vectors, 0);
    const auto target = [&](std::size_t vector) {
        return interleave ? m_interleaved.data() : m_spectrum[std::size_t(channelList[vector])].data();
    };
    for (int pass = 0; pass < 8; ++pass) {
        std::uint32_t partition = 0;
        while (partition < partitions) {
            if (pass == 0) {
                for (std::size_t v = 0; v < vectors; ++v) {
                    int packed = classbook.decode(bits);
                    if (packed < 0) {
                        return;
                    }
                    for (std::uint32_t i = classWords; i-- > 0;) {
                        m_classes[v * classStride + partition + i] = std::uint8_t(packed % residue.classifications);
                        packed /= residue.classifications;
                    }
                }
            }
            for (std::uint32_t word = 0; word < classWords && partition < partitions; ++word, ++partition) {
                for (std::size_t v = 0; v < vectors; ++v) {
                    const int book = residue.books[m_classes[v * classStride + partition]][pass];
                    if (book < 0) {
                        continue;
                    }
                    const Codebook &vq = m_codebooks[std::size_t(book)];
                    float *out = target(v) + begin + partition * residue.partitionSize;
                    const std::uint32_t count = residue.partitionSize;
                    const std::uint32_t dimensions = std::uint32_t(vq.dimensions);
                    if (residue.type == 0) {
                        const std::uint32_t step = count / dimensions;
                        for (std::uint32_t i = 0; i < step; ++i) {
                            const int entry = vq.decode(bits);
                            if (entry < 0) {
                                return;
                            }
                            std::uint64_t divisor = 1;
                            float last = 0.0f;
                            for (std::uint32_t d = 0; d < dimensions; ++d) {
                                out[i + d * step] += vq.element(entry, int(d), divisor, last);
                            }
                        }
                    } else {
                        for (std::uint32_t i = 0; i < count;) {
                            const int entry = vq.decode(bits);
                            if (entry < 0) {
                                return;
                            }
                            std::uint64_t divisor = 1;
                            float last = 0.0f;
                            for (std::uint32_t d = 0; d < dimensions && i < count; ++d, ++i) {
                                out[i] += vq.element(entry, int(d), divisor, last);
                            }
                        }
                    }
                }
            }
        }
    }
}

// Decodes the packet in m_packet into m_queue (interleaved). Returns the
// frames it produced, or -1 if it is not an audio packet.
int VorbisStream::decodePacket() {
    LsbBitReader bits(m_packet.data(), m_packet.size());
    if (m_packet.empty() || bits.read(1) != 0) {
        return -1;
    }
    const std::uint32_t modeIndex = bits.read(ilog(std::uint32_t(m_modes.size() - 1)));
    if (modeIndex >= m_modes.size()) {
        return -1;
    }
    const Mode &mode = m_modes[modeIndex];
    const Mapping &mapping = m_mappings[mode.mapping];
    const int size = m_blockSize[mode.longBlock ? 1 : 0];
    const int half = size / 2;
    bool previousLong = true, nextLong = true;
    if (mode.longBlock) {
        previousLong = bits.read(1) != 0;
        nextLong = bits.read(1) != 0;
    }
    if (bits.ended()) {
        return -1;
    }
    const std::size_t channels = std::size_t(m_channels);

    for (std::size_t c = 0; c < channels; ++c) {
        const Floor1 &floor = m_floors[mapping.floor[mapping.mux[c]]];
        m_silent[c] = decodeFloor(bits, floor, int(c)) ? 0 : 1;
        std::fill_n(m_spectrum[c].data(), half, 0.0f);
    }
    // A silent channel coupled to one that is not still carries residue.
    std::vector<char> &silent = m_silent;
    std::array<char, 256> skipResidue{};
    for (std::size_t c = 0; c < channels; ++c) {
        skipResidue[c] = silent[c];
    }
    for (const auto &[magnitude, angle] : mapping.coupling) {
        if (!silent[magnitude] || !silent[angle]) {
            skipResidue[magnitude] = skipResidue[angle] = 0;
        }
    }
    for (int s = 0; s < mapping.submaps; ++s) {
        const Residue &residue = m_residues[mapping.residue[s]];
        m_submapChannels.clear();
        bool any = false;
        for (std::size_t c = 0; c < channels; ++c) {
            if (mapping.mux[c] == s) {
                if (residue.type == 2 || !skipResidue[c]) {
                    m_submapChannels.push_back(int(c));
                }
                any = any || !skipResidue[c];
            }
        }
        if (!any || m_submapChannels.empty()) {
            continue;
        }
        if (residue.type == 2) {
            const std::size_t count = m_submapChannels.size();
            m_interleaved.assign(std::size_t(half) * count, 0.0f);
            decodeResidue(bits, residue, m_submapChannels, half);
            for (std::size_t k = 0; k < count; ++k) {
                float *to = m_spectrum[std::size_t(m_submapChannels[k])].data();
                for (int i = 0; i < half; ++i) {
                    to[i] = m_interleaved[std::size_t(i) * count + k];
                }
            }
        } else {
            decodeResidue(bits, residue, m_submapChannels, half);
        }
    }
    // Undo the channel coupling, last step first.
    for (std::size_t step = mapping.coupling.size(); step-- > 0;) {
        float *magnitude = m_spectrum[mapping.coupling[step].first].data();
        float *angle = m_spectrum[mapping.coupling[step].second].data();
        for (int i = 0; i < half; ++i) {
            const float m = magnitude[i];
            const float a = angle[i];
            if (m > 0.0f) {
                if (a > 0.0f) {
                    angle[i] = m - a;
                } else {
                    angle[i] = m;
                    magnitude[i] = m + a;
                }
            } else {
                if (a > 0.0f) {
                    angle[i] = m + a;
                } else {
                    angle[i] = m;
                    magnitude[i] = m - a;
                }
            }
        }
    }

    // The window's slopes: a long block beside a short one has the short
    // block's slope on that side, centred on the quarter point.
    const int shortSize = m_blockSize[0];
    const bool leftShort = mode.longBlock && !previousLong;
    const bool rightShort = mode.longBlock && !nextLong;
    const int leftStart = leftShort ? size / 4 - shortSize / 4 : 0;
    const int leftEnd = leftShort ? size / 4 + shortSize / 4 : half;
    const int rightStart = rightShort ? size * 3 / 4 - shortSize / 4 : half;
    const int rightEnd = rightShort ? size * 3 / 4 + shortSize / 4 : size;
    const float *leftSlope = m_slope[leftShort || !mode.longBlock ? 0 : 1].data();
    const float *rightSlope = m_slope[rightShort || !mode.longBlock ? 0 : 1].data();

    const int previousHalf = m_previousSize / 2;
    int produced = 0;
    if (m_previousSize > 0) {
        produced = previousHalf / 2 + half / 2;
    }
    float *time = m_time.data();
    for (std::size_t c = 0; c < channels; ++c) {
        if (m_silent[c]) {
            std::fill_n(time, size, 0.0f);
        } else {
            renderFloor(m_floors[mapping.floor[mapping.mux[c]]], int(c), half);
            m_mdct[mode.longBlock ? 1 : 0].run(m_spectrum[c].data(), time);
            std::fill_n(time, leftStart, 0.0f);
            for (int i = leftStart; i < leftEnd; ++i) {
                time[i] *= leftSlope[i - leftStart];
            }
            for (int i = rightStart; i < rightEnd; ++i) {
                time[i] *= rightSlope[rightEnd - 1 - i];
            }
            std::fill_n(time + rightEnd, size - rightEnd, 0.0f);
        }
        float *previous = m_previous[c].data();
        if (m_previousSize > 0) {
            // From the centre of the block before to the centre of this one.
            float *out = m_queue.data() + c;
            if (previousHalf >= half) {
                const int start = previousHalf / 2 - half / 2;
                for (int i = 0; i < start; ++i) {
                    out[std::size_t(i) * channels] = previous[i];
                }
                for (int i = 0; i < half; ++i) {
                    out[std::size_t(start + i) * channels] = previous[start + i] + time[i];
                }
            } else {
                const int start = half / 2 - previousHalf / 2;
                for (int i = 0; i < previousHalf; ++i) {
                    out[std::size_t(i) * channels] = previous[i] + time[start + i];
                }
                for (int i = start + previousHalf; i < half; ++i) {
                    out[std::size_t(i - start) * channels] = time[i];
                }
            }
        }
        std::copy_n(time + half, half, previous);
    }
    m_previousSize = size;
    return produced;
}

// Decodes packets until one produces sound for the queue. False at the end.
bool VorbisStream::decodeNext() {
    for (;;) {
        m_queueFrames = m_queueRead = 0;
        if (!nextPacket()) {
            return false;
        }
        const bool primed = m_previousSize > 0;
        const int produced = decodePacket();
        if (produced < 0) {
            continue;
        }
        const bool placed = m_packetLastOnPage && m_packetGranule >= 0;
        if (!m_positionKnown) {
            // After a seek: nothing is kept until a page says where we are.
            if (placed && (primed || produced == 0)) {
                m_decoded = m_packetGranule;
                m_positionKnown = true;
                return true; // with an empty queue: the caller looks at the position
            }
            continue;
        }
        std::int64_t start = m_decoded;
        std::int64_t end = start + produced;
        if (placed && m_packetEndsStream && m_packetGranule < end) {
            end = std::max(start, m_packetGranule); // the last block is cut to the stream's length
        }
        m_decoded = end;
        // Samples before the first one the caller sees are dropped.
        std::size_t skip = 0;
        if (start < m_base) {
            skip = std::size_t(std::min<std::int64_t>(m_base - start, end - start));
        }
        m_queueFrames = std::size_t(end - start);
        m_queueRead = skip;
        if (m_queueRead < m_queueFrames) {
            return true;
        }
    }
}

} // namespace

Result<std::unique_ptr<AudioStream>> openVorbisStream(Span<const std::byte> data, const AudioLimits &limits) {
    auto stream = std::make_unique<VorbisStream>(data);
    if (Result<void> opened = stream->open(limits); !opened) {
        return std::move(opened).error();
    }
    return std::unique_ptr<AudioStream>(std::move(stream));
}

} // namespace cfw::detail
