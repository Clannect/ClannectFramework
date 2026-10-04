// MP3: MPEG-1, MPEG-2 and MPEG-2.5 audio Layer III, after ISO/IEC 11172-3
// and 13818-3, in the standard's own order: side information, the bit
// reservoir, scalefactors, Huffman-coded spectrum, requantisation, stereo
// (mid/side and intensity), reordering of short blocks, alias reduction, the
// inverse MDCT with its overlap, and the 32-band synthesis filter.
//
// The file is indexed when opened: every frame header is found once, so the
// length is exact for variable bit rates and a seek goes straight to its
// frame. A frame whose header does not parse, or that does not belong to the
// stream, is skipped; a frame whose main data is missing or damaged plays as
// silence. Nothing in a frame is trusted: counts are clamped to the 576
// lines a granule has, and the spectrum reader stops at the frame's bits.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

#include "DecoderCommon.h"

namespace cfw::detail {

namespace {

#include "Mp3Tables.inc"

constexpr int kGranuleLines = 576;
constexpr int kMaxBands = 40;
constexpr int kDecoderDelay = 529; // samples a Layer III decoder adds before the first one

struct FrameInfo {
    int version = 0;   // 0: MPEG-1, 1: MPEG-2, 2: MPEG-2.5
    int rateIndex = 0; // into the band tables: version * 3 + the header's index
    int sampleRate = 0;
    int channels = 0;
    int mode = 0;      // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    int modeExtension = 0;
    bool crc = false;
    std::size_t frameBytes = 0;
    int sideBytes = 0;
    [[nodiscard]] int samples() const noexcept { return version == 0 ? 1152 : 576; }
};

bool parseHeader(const std::uint8_t *p, FrameInfo &info) noexcept {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const int versionBits = (p[1] >> 3) & 3;
    const int layerBits = (p[1] >> 1) & 3;
    const int bitrateIndex = p[2] >> 4;
    const int rateBits = (p[2] >> 2) & 3;
    // Layer III only; no reserved values; free format is not supported.
    if (versionBits == 1 || layerBits != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateBits == 3) {
        return false;
    }
    static constexpr int kRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    static constexpr int kBitrates[2][15] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
                                             {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};
    info.version = versionBits == 3 ? 0 : versionBits == 2 ? 1 : 2;
    info.rateIndex = info.version * 3 + rateBits;
    info.sampleRate = kRates[info.version][rateBits];
    info.mode = p[3] >> 6;
    info.modeExtension = (p[3] >> 4) & 3;
    info.channels = info.mode == 3 ? 1 : 2;
    info.crc = (p[1] & 1) == 0;
    const int bitrate = kBitrates[info.version == 0 ? 0 : 1][bitrateIndex] * 1000;
    const int padding = (p[2] >> 1) & 1;
    info.frameBytes = std::size_t((info.version == 0 ? 144 : 72) * bitrate / info.sampleRate + padding);
    info.sideBytes = info.version == 0 ? (info.channels == 1 ? 17 : 32) : (info.channels == 1 ? 9 : 17);
    return info.frameBytes >= std::size_t(4 + (info.crc ? 2 : 0) + info.sideBytes);
}

struct Granule {
    int part23Length = 0;
    int bigValues = 0;
    int globalGain = 0;
    int scalefacCompress = 0;
    bool windowSwitching = false;
    int blockType = 0;
    bool mixed = false;
    int tableSelect[3] = {0, 0, 0};
    int subblockGain[3] = {0, 0, 0};
    int region0Count = 0;
    int region1Count = 0;
    bool preflag = false;
    bool scalefacScale = false;
    int count1Table = 0;
};

// The scalefactor bands of one granule in the order its spectrum is coded:
// long bands, then (for short blocks) each short band once per window.
struct Bands {
    int count = 0;
    int start[kMaxBands];
    int width[kMaxBands];
    int window[kMaxBands]; // -1: a long band
    int sfb[kMaxBands];    // its index among long or short bands
    int shortStart = kGranuleLines; // the first line of the short part
};

// A prefix code as a binary tree: child[node][bit] is the next node (> 0),
// a value (-1 - value), or 0 where no code goes.
struct HuffTree {
    std::vector<std::array<std::int16_t, 2>> nodes;

    void add(std::uint32_t code, int length, int value) {
        std::size_t node = 0;
        for (int bit = length - 1; bit >= 0; --bit) {
            const std::size_t side = (code >> bit) & 1u;
            if (bit == 0) {
                nodes[node][side] = std::int16_t(-1 - value);
            } else {
                if (nodes[node][side] <= 0) {
                    nodes[node][side] = std::int16_t(nodes.size());
                    nodes.push_back({0, 0});
                }
                node = std::size_t(nodes[node][side]);
            }
        }
    }
    [[nodiscard]] int decode(MsbBitReader &bits) const noexcept {
        std::size_t node = 0;
        for (int depth = 0; depth < 24; ++depth) {
            const int next = nodes[node][bits.bit()];
            if (next < 0) {
                return -1 - next;
            }
            if (next == 0) {
                break;
            }
            node = std::size_t(next);
        }
        return 0; // no such code: damaged data decodes as silence
    }
};

// Everything computed once: code trees, band boundaries, transform tables.
struct Tables {
    HuffTree big[16];
    HuffTree quad[2];
    int longStart[9][23];
    int shortStart[9][14];
    std::vector<float> pow43;       // |v| ^ (4/3)
    float synthMatrix[64][32];
    float synthWindow[512];
    float imdctLong[36][18];
    float imdctShort[12][6];
    float window[4][36];
    float aliasCs[8];
    float aliasCa[8];

    Tables() {
        for (int b = 1; b < 16; ++b) {
            const HuffBook &book = kHuffBooks[b];
            big[b].nodes.push_back({0, 0});
            for (int i = 0; i < book.size * book.size; ++i) {
                big[b].add(book.codes[i], book.lengths[i], i);
            }
        }
        for (int q = 0; q < 2; ++q) {
            quad[q].nodes.push_back({0, 0});
            for (int i = 0; i < 16; ++i) {
                quad[q].add(kQuadCodes[q * 16 + i], kQuadLengths[q * 16 + i], i);
            }
        }
        for (int r = 0; r < 9; ++r) {
            longStart[r][0] = 0;
            for (int b = 0; b < 22; ++b) {
                longStart[r][b + 1] = std::min(kGranuleLines, longStart[r][b] + kBandWidthLong[r][b]);
            }
            shortStart[r][0] = 0;
            for (int b = 0; b < 13; ++b) {
                shortStart[r][b + 1] = std::min(kGranuleLines / 3, shortStart[r][b] + kBandWidthShort[r][b]);
            }
        }
        pow43.resize(8207);
        for (std::size_t i = 0; i < pow43.size(); ++i) {
            pow43[i] = float(std::pow(double(i), 4.0 / 3.0));
        }
        const double pi = std::numbers::pi;
        for (int i = 0; i < 64; ++i) {
            for (int k = 0; k < 32; ++k) {
                synthMatrix[i][k] = float(std::cos(double((16 + i) * (2 * k + 1)) * pi / 64.0));
            }
        }
        for (int i = 0; i <= 256; ++i) {
            synthWindow[i] = float(kSynthWindowHalf[i]) / 65536.0f;
        }
        for (int i = 1; i < 256; ++i) {
            synthWindow[512 - i] = (i % 64 == 0 ? 1.0f : -1.0f) * float(kSynthWindowHalf[i]) / 65536.0f;
        }
        for (int i = 0; i < 36; ++i) {
            for (int k = 0; k < 18; ++k) {
                imdctLong[i][k] = float(std::cos(pi / 72.0 * double((2 * i + 19) * (2 * k + 1))));
            }
        }
        for (int i = 0; i < 12; ++i) {
            for (int k = 0; k < 6; ++k) {
                imdctShort[i][k] = float(std::cos(pi / 24.0 * double((2 * i + 7) * (2 * k + 1))));
            }
        }
        // Block types 0 (normal), 1 (start), 3 (stop); 2 holds the short window.
        for (int i = 0; i < 36; ++i) {
            const float sine = float(std::sin(pi / 36.0 * (double(i) + 0.5)));
            window[0][i] = sine;
            window[1][i] = i < 18 ? sine : i < 24 ? 1.0f : i < 30 ? float(std::sin(pi / 12.0 * (double(i - 18) + 0.5))) : 0.0f;
            window[3][i] = i < 6 ? 0.0f : i < 12 ? float(std::sin(pi / 12.0 * (double(i - 6) + 0.5))) : i < 18 ? 1.0f : sine;
            window[2][i] = i < 12 ? float(std::sin(pi / 12.0 * (double(i) + 0.5))) : 0.0f;
        }
        static constexpr double kAlias[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        for (int i = 0; i < 8; ++i) {
            const double root = std::sqrt(1.0 + kAlias[i] * kAlias[i]);
            aliasCs[i] = float(1.0 / root);
            aliasCa[i] = float(kAlias[i] / root);
        }
    }
};

const Tables &tables() {
    static const Tables instance;
    return instance;
}

constexpr int kPretab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};

void buildBands(const FrameInfo &frame, const Granule &granule, Bands &bands) {
    const Tables &t = tables();
    const int *longStart = t.longStart[frame.rateIndex];
    const int *shortStart = t.shortStart[frame.rateIndex];
    bands.count = 0;
    bands.shortStart = kGranuleLines;
    const auto add = [&bands](int start, int width, int window, int sfb) {
        bands.start[bands.count] = start;
        bands.width[bands.count] = width;
        bands.window[bands.count] = window;
        bands.sfb[bands.count] = sfb;
        ++bands.count;
    };
    int firstShort = 0;
    if (granule.blockType != 2) {
        for (int b = 0; b < 22; ++b) {
            add(longStart[b], longStart[b + 1] - longStart[b], -1, b);
        }
        return;
    }
    if (granule.mixed) {
        // The lowest two subbands stay long; short bands take over at band 3.
        const int longBands = frame.version == 0 ? 8 : 6;
        for (int b = 0; b < longBands; ++b) {
            add(longStart[b], longStart[b + 1] - longStart[b], -1, b);
        }
        firstShort = 3;
    }
    bands.shortStart = shortStart[firstShort] * 3;
    for (int b = firstShort; b < 13; ++b) {
        const int width = shortStart[b + 1] - shortStart[b];
        for (int w = 0; w < 3; ++w) {
            add(shortStart[b] * 3 + w * width, width, w, b);
        }
    }
}

class Mp3Decoder {
public:
    explicit Mp3Decoder(int channels) : m_channels(channels) { reset(); }

    void reset() noexcept {
        m_main.clear();
        std::memset(m_overlap, 0, sizeof m_overlap);
        std::memset(m_synth, 0, sizeof m_synth);
        std::memset(m_scalefactors, 0, sizeof m_scalefactors);
        m_synthOffset[0] = m_synthOffset[1] = 0;
    }

    // Decodes the frame at `data` (frame.frameBytes long) into `out`:
    // frame.samples() frames, interleaved. Silence if it cannot be decoded.
    void decode(const std::uint8_t *data, const FrameInfo &frame, float *out) {
        const int samples = frame.samples();
        std::fill_n(out, std::size_t(samples) * std::size_t(m_channels), 0.0f);
        const std::size_t headerBytes = 4 + (frame.crc ? 2u : 0u);
        MsbBitReader side(data + headerBytes, std::size_t(frame.sideBytes));
        const int granules = frame.version == 0 ? 2 : 1;
        const int channels = frame.channels;
        Granule info[2][2];
        int scfsi[2][4] = {};
        int mainDataBegin = 0;
        if (frame.version == 0) {
            mainDataBegin = int(side.read(9));
            side.skip(channels == 1 ? 5 : 3);
            for (int c = 0; c < channels; ++c) {
                for (int band = 0; band < 4; ++band) {
                    scfsi[c][band] = int(side.bit());
                }
            }
        } else {
            mainDataBegin = int(side.read(8));
            side.skip(channels == 1 ? 1 : 2);
        }
        bool valid = true;
        for (int g = 0; g < granules; ++g) {
            for (int c = 0; c < channels; ++c) {
                Granule &gr = info[g][c];
                gr.part23Length = int(side.read(12));
                gr.bigValues = std::min(int(side.read(9)), kGranuleLines / 2);
                gr.globalGain = int(side.read(8));
                gr.scalefacCompress = int(side.read(frame.version == 0 ? 4 : 9));
                gr.windowSwitching = side.bit() != 0;
                if (gr.windowSwitching) {
                    gr.blockType = int(side.read(2));
                    gr.mixed = side.bit() != 0;
                    gr.tableSelect[0] = int(side.read(5));
                    gr.tableSelect[1] = int(side.read(5));
                    for (int &gain : gr.subblockGain) {
                        gain = int(side.read(3));
                    }
                    valid = valid && gr.blockType != 0;
                    gr.mixed = gr.mixed && gr.blockType == 2;
                } else {
                    for (int &table : gr.tableSelect) {
                        table = int(side.read(5));
                    }
                    gr.region0Count = int(side.read(4));
                    gr.region1Count = int(side.read(3));
                }
                if (frame.version == 0) {
                    gr.preflag = side.bit() != 0;
                }
                gr.scalefacScale = side.bit() != 0;
                gr.count1Table = int(side.bit());
            }
        }

        // The bit reservoir: this frame's audio may begin in the unused space
        // of the frames before it, mainDataBegin bytes back.
        const std::size_t mainOffset = headerBytes + std::size_t(frame.sideBytes);
        const std::size_t mainBytes = frame.frameBytes - mainOffset;
        const bool reachable = std::size_t(mainDataBegin) <= m_main.size();
        if (reachable) {
            m_main.erase(m_main.begin(), m_main.end() - mainDataBegin);
        }
        m_main.insert(m_main.end(), data + mainOffset, data + mainOffset + mainBytes);
        if (!reachable || !valid || channels != m_channels) {
            // After a seek or a lost frame: nothing to play, and what would
            // have been overlapped is gone too.
            if (m_main.size() > 2048) {
                m_main.erase(m_main.begin(), m_main.end() - 2048);
            }
            std::memset(m_overlap, 0, sizeof m_overlap);
            return;
        }
        MsbBitReader bits(m_main.data(), m_main.size());
        for (int g = 0; g < granules; ++g) {
            Bands bands[2];
            int limits[2][kMaxBands];
            for (int c = 0; c < channels; ++c) {
                const Granule &gr = info[g][c];
                const std::uint64_t end = bits.position() + std::uint64_t(gr.part23Length);
                buildBands(frame, gr, bands[c]);
                readScalefactors(bits, frame, gr, bands[c], c, g == 1 ? scfsi[c] : nullptr, limits[c]);
                readSpectrum(bits, frame, gr, end, m_lines[c]);
                bits.seek(end);
                requantise(gr, bands[c], c);
            }
            if (channels == 2 && frame.mode == 1) {
                stereo(frame, bands[1], limits[1], info[g][1].scalefacCompress);
            }
            for (int c = 0; c < channels; ++c) {
                hybrid(info[g][c], bands[c], c);
                synthesise(c, out + std::size_t(g) * kGranuleLines * std::size_t(m_channels) + std::size_t(c));
            }
        }
        if (m_main.size() > 2048) {
            m_main.erase(m_main.begin(), m_main.end() - 2048);
        }
    }

private:
    // Scalefactors into m_scalefactors[channel], one per coded band, and
    // for each band the value that means "no intensity position".
    void readScalefactors(MsbBitReader &bits, const FrameInfo &frame, const Granule &gr, const Bands &bands,
                          int channel, const int *scfsi, int *limits) {
        int *sf = m_scalefactors[channel];
        if (frame.version == 0) {
            static constexpr int kLengths[2][16] = {{0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4},
                                                    {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}};
            const int length1 = kLengths[0][gr.scalefacCompress];
            const int length2 = kLengths[1][gr.scalefacCompress];
            for (int b = 0; b < bands.count; ++b) {
                limits[b] = 7;
                const bool isLong = bands.window[b] < 0;
                const int index = bands.sfb[b];
                if (isLong && gr.blockType != 2) {
                    if (index >= 21) {
                        sf[b] = 0;
                        continue;
                    }
                    // The four groups a second granule may share with the first.
                    const int group = index < 6 ? 0 : index < 11 ? 1 : index < 16 ? 2 : 3;
                    if (scfsi && scfsi[group]) {
                        continue; // kept from the first granule
                    }
                    sf[b] = int(bits.read(index < 11 ? length1 : length2));
                } else if (isLong) {
                    sf[b] = int(bits.read(length1)); // the long part of a mixed block
                } else {
                    sf[b] = index >= 12 ? 0 : int(bits.read(index < 6 ? length1 : length2));
                }
            }
            return;
        }
        // MPEG-2: four partitions of bands, each with its own length, chosen
        // by scalefac_compress; the right channel of an intensity pair has
        // its own set.
        int compress = gr.scalefacCompress;
        int lengths[4] = {0, 0, 0, 0};
        int set = 0;
        const bool intensity = channel == 1 && frame.mode == 1 && (frame.modeExtension & 1) != 0;
        if (intensity) {
            compress >>= 1;
            if (compress < 180) {
                lengths[0] = compress / 36, lengths[1] = (compress % 36) / 6, lengths[2] = (compress % 36) % 6;
                set = 3;
            } else if (compress < 244) {
                compress -= 180;
                lengths[0] = (compress % 64) >> 4, lengths[1] = (compress % 16) >> 2, lengths[2] = compress % 4;
                set = 4;
            } else {
                compress -= 244;
                lengths[0] = compress / 3, lengths[1] = compress % 3;
                set = 5;
            }
        } else if (compress < 400) {
            lengths[0] = (compress >> 4) / 5, lengths[1] = (compress >> 4) % 5, lengths[2] = (compress % 16) >> 2;
            lengths[3] = compress % 4;
            set = 0;
        } else if (compress < 500) {
            compress -= 400;
            lengths[0] = (compress >> 2) / 5, lengths[1] = (compress >> 2) % 5, lengths[2] = compress % 4;
            set = 1;
        } else {
            compress -= 500;
            lengths[0] = compress / 3, lengths[1] = compress % 3;
            set = 2;
        }
        m_lsfPreflag[channel] = !intensity && gr.scalefacCompress >= 500;
        const int shape = gr.blockType != 2 ? 0 : gr.mixed ? 2 : 1;
        int b = 0;
        for (int partition = 0; partition < 4; ++partition) {
            for (int i = 0; i < kLsfPartitions[set][shape][partition] && b < bands.count; ++i, ++b) {
                sf[b] = int(bits.read(lengths[partition]));
                limits[b] = (1 << lengths[partition]) - 1;
            }
        }
        for (; b < bands.count; ++b) {
            sf[b] = 0;
            limits[b] = 0;
        }
    }

    // The Huffman-coded lines of one granule and channel, as integers.
    void readSpectrum(MsbBitReader &bits, const FrameInfo &frame, const Granule &gr, std::uint64_t end,
                      std::int16_t *lines) const {
        const Tables &t = tables();
        const int *longStart = t.longStart[frame.rateIndex];
        int region1 = 0;
        int region2 = kGranuleLines;
        if (gr.windowSwitching) {
            // Short blocks: 36 lines (72 at 8 kHz); the other switched blocks: 8 long bands.
            region1 = gr.blockType == 2 && !gr.mixed ? (frame.rateIndex == 8 ? 72 : 36) : longStart[8];
        } else {
            region1 = longStart[std::min(gr.region0Count + 1, 22)];
            region2 = longStart[std::min(gr.region0Count + gr.region1Count + 2, 22)];
        }
        const int bigEnd = gr.bigValues * 2;
        int line = 0;
        while (line < bigEnd) {
            const int region = line < region1 ? 0 : line < region2 ? 1 : 2;
            const int regionEnd = std::min(bigEnd, region == 0 ? region1 : region == 1 ? region2 : kGranuleLines);
            const int select = gr.tableSelect[region];
            const int bookIndex = kHuffSelect[select][0];
            const int linbits = kHuffSelect[select][1];
            if (bookIndex == 0) {
                for (; line < regionEnd; ++line) {
                    lines[line] = 0;
                }
                continue;
            }
            const HuffTree &tree = t.big[bookIndex];
            const int size = kHuffBooks[bookIndex].size;
            for (; line < regionEnd && bits.position() < end; line += 2) {
                const int pair = tree.decode(bits);
                int values[2] = {pair / size, pair % size};
                for (int i = 0; i < 2; ++i) {
                    int value = values[i];
                    if (value == 15 && linbits > 0) {
                        value += int(bits.read(linbits));
                    }
                    if (value != 0 && bits.bit()) {
                        value = -value;
                    }
                    lines[line + i] = std::int16_t(value);
                }
            }
            if (bits.position() >= end) {
                break;
            }
        }
        if (bits.position() > end && line >= 2) {
            line -= 2; // the last pair ran past the granule's bits
            lines[line] = lines[line + 1] = 0;
        }
        // Then quadruples of -1, 0 and 1 until the bits run out.
        const HuffTree &quad = t.quad[gr.count1Table];
        while (line + 4 <= kGranuleLines && bits.position() < end) {
            const int packed = quad.decode(bits);
            for (int i = 0; i < 4; ++i) {
                int value = (packed >> (3 - i)) & 1;
                if (value != 0 && bits.bit()) {
                    value = -1;
                }
                lines[line + i] = std::int16_t(value);
            }
            if (bits.position() > end) {
                std::fill_n(lines + line, 4, std::int16_t(0)); // ran past: not real
                break;
            }
            line += 4;
        }
        std::fill(lines + line, lines + kGranuleLines, std::int16_t(0));
    }

    // Integers to spectral values: |v|^(4/3), scaled by the granule's gain
    // and each band's scalefactor.
    void requantise(const Granule &gr, const Bands &bands, int channel) {
        const Tables &t = tables();
        const std::int16_t *lines = m_lines[channel];
        float *xr = m_spectrum[channel];
        const int *sf = m_scalefactors[channel];
        const int shift = gr.scalefacScale ? 4 : 2; // in quarter powers of two
        const bool preflag = gr.preflag || m_lsfPreflag[channel];
        int line = 0;
        for (int b = 0; b < bands.count; ++b) {
            int exponent = gr.globalGain - 210;
            if (bands.window[b] < 0) {
                exponent -= shift * (sf[b] + (preflag ? kPretab[bands.sfb[b]] : 0));
            } else {
                exponent -= 8 * gr.subblockGain[bands.window[b]] + shift * sf[b];
            }
            const float gain = float(std::pow(2.0, 0.25 * double(exponent)));
            const int end = std::min(kGranuleLines, bands.start[b] + bands.width[b]);
            for (line = bands.start[b]; line < end; ++line) {
                const int value = lines[line];
                const float magnitude = t.pow43[std::size_t(std::min(std::abs(value), 8206))] * gain;
                xr[line] = value < 0 ? -magnitude : magnitude;
            }
        }
        std::fill(xr + line, xr + kGranuleLines, 0.0f);
    }

    // Joint stereo. Mid/side bands become left and right; in intensity
    // bands (the top of the spectrum, where the right channel is coded as
    // zero) the left channel's lines are split between the two by the
    // position in the right channel's scalefactor.
    void stereo(const FrameInfo &frame, const Bands &bands, const int *limits, int rightCompress) {
        const bool midSide = (frame.modeExtension & 2) != 0;
        const bool intensity = (frame.modeExtension & 1) != 0;
        float *left = m_spectrum[0];
        float *right = m_spectrum[1];
        bool isBand[kMaxBands] = {};
        if (intensity) {
            // A band is an intensity band if the right channel is zero in it
            // and in every band above it (of its window, for short blocks).
            bool above = true;
            bool aboveWindow[3] = {true, true, true};
            for (int b = bands.count - 1; b >= 0; --b) {
                const float *lines = right + bands.start[b];
                const int width = std::min(bands.width[b], kGranuleLines - bands.start[b]);
                const bool zero = std::all_of(lines, lines + std::max(width, 0), [](float v) { return v == 0.0f; });
                if (bands.window[b] < 0) {
                    isBand[b] = zero && above;
                } else {
                    isBand[b] = zero && aboveWindow[bands.window[b]];
                    aboveWindow[bands.window[b]] = aboveWindow[bands.window[b]] && zero;
                }
                above = above && zero;
            }
        }
        const int *positions = m_scalefactors[1];
        constexpr float kRoot = 0.70710678118654752440f;
        for (int b = 0; b < bands.count; ++b) {
            const int start = bands.start[b];
            const int end = std::min(kGranuleLines, start + bands.width[b]);
            bool done = false;
            if (isBand[b]) {
                // The topmost band has no scalefactor of its own: it follows
                // the one below it.
                int source = b;
                const int last = bands.window[b] < 0 ? 21 : 12;
                if (bands.sfb[b] >= last) {
                    source = bands.window[b] < 0 ? b - 1 : b - 3;
                }
                const int position = source >= 0 ? positions[source] : 0;
                const bool legal = source >= 0 && position < limits[source];
                if (legal) {
                    float kl = 1.0f, kr = 1.0f;
                    if (frame.version == 0) {
                        const float ratio = position >= 6 ? 1.0f : float(std::tan(double(position) * std::numbers::pi / 12.0));
                        kl = position >= 6 ? 1.0f : ratio / (1.0f + ratio);
                        kr = position >= 6 ? 0.0f : 1.0f / (1.0f + ratio);
                    } else if (position > 0) {
                        // Steps of 2^(-1/4), or 2^(-1/2) when the low bit of the
                        // right channel's scalefac_compress is set.
                        const double base = (rightCompress & 1) ? 0.5 : 0.25;
                        const float factor = float(std::pow(2.0, -base * double((position + 1) / 2)));
                        (position & 1 ? kl : kr) = factor;
                    }
                    for (int i = start; i < end; ++i) {
                        right[i] = left[i] * kr;
                        left[i] *= kl;
                    }
                    done = true;
                }
            }
            if (!done && midSide) {
                for (int i = start; i < end; ++i) {
                    const float mid = left[i];
                    const float sideValue = right[i];
                    left[i] = (mid + sideValue) * kRoot;
                    right[i] = (mid - sideValue) * kRoot;
                }
            }
        }
    }

    // Spectrum to 18 time slots of 32 subband samples: reorder short blocks,
    // reduce aliasing between long subbands, inverse MDCT and overlap.
    void hybrid(const Granule &gr, const Bands &bands, int channel) {
        const Tables &t = tables();
        float *xr = m_spectrum[channel];
        if (gr.blockType == 2) {
            // Short bands are coded window by window; the transform wants each
            // window's lines interleaved.
            float reordered[kGranuleLines];
            std::copy_n(xr, kGranuleLines, reordered);
            for (int b = 0; b < bands.count; ++b) {
                const int window = bands.window[b];
                if (window < 0) {
                    continue;
                }
                const int first = bands.start[b] - window * bands.width[b]; // the band's first line
                for (int i = 0; i < bands.width[b]; ++i) {
                    const int to = first + 3 * i + window;
                    if (to < kGranuleLines && bands.start[b] + i < kGranuleLines) {
                        reordered[to] = xr[bands.start[b] + i];
                    }
                }
            }
            std::copy_n(reordered, kGranuleLines, xr);
        }
        const int longSubbands = gr.blockType != 2 ? 32 : gr.mixed ? 2 : 0;
        for (int sb = 1; sb < longSubbands; ++sb) {
            for (int i = 0; i < 8; ++i) {
                float &below = xr[18 * sb - 1 - i];
                float &above = xr[18 * sb + i];
                const float a = below, b = above;
                below = a * t.aliasCs[i] - b * t.aliasCa[i];
                above = b * t.aliasCs[i] + a * t.aliasCa[i];
            }
        }
        for (int sb = 0; sb < 32; ++sb) {
            const float *in = xr + 18 * sb;
            float block[36];
            if (gr.blockType == 2 && sb >= longSubbands) {
                std::fill_n(block, 36, 0.0f);
                for (int w = 0; w < 3; ++w) {
                    for (int i = 0; i < 12; ++i) {
                        float sum = 0.0f;
                        for (int k = 0; k < 6; ++k) {
                            sum += in[3 * k + w] * t.imdctShort[i][k];
                        }
                        block[6 + 6 * w + i] += sum * t.window[2][i];
                    }
                }
            } else {
                const float *window = t.window[gr.blockType == 2 ? 0 : gr.blockType];
                for (int i = 0; i < 36; ++i) {
                    float sum = 0.0f;
                    for (int k = 0; k < 18; ++k) {
                        sum += in[k] * t.imdctLong[i][k];
                    }
                    block[i] = sum * window[i];
                }
            }
            float *overlap = m_overlap[channel][sb];
            for (int i = 0; i < 18; ++i) {
                float value = block[i] + overlap[i];
                overlap[i] = block[18 + i];
                // The synthesis filter expects every other sample of the odd
                // subbands negated.
                if ((sb & 1) && (i & 1)) {
                    value = -value;
                }
                m_subband[i][sb] = value;
            }
        }
    }

    // The polyphase synthesis filter: 32 subband samples in, 32 PCM out, 18
    // times a granule. `out` is this channel's first sample; the stride is
    // the channel count.
    void synthesise(int channel, float *out) {
        const Tables &t = tables();
        float *v = m_synth[channel];
        int &offset = m_synthOffset[channel];
        for (int slot = 0; slot < 18; ++slot) {
            offset = (offset - 64) & 1023;
            const float *s = m_subband[slot];
            for (int i = 0; i < 64; ++i) {
                float sum = 0.0f;
                for (int k = 0; k < 32; ++k) {
                    sum += t.synthMatrix[i][k] * s[k];
                }
                v[offset + i] = sum;
            }
            for (int j = 0; j < 32; ++j) {
                float sum = 0.0f;
                for (int i = 0; i < 8; ++i) {
                    sum += v[(offset + i * 128 + j) & 1023] * t.synthWindow[i * 64 + j];
                    sum += v[(offset + i * 128 + 96 + j) & 1023] * t.synthWindow[i * 64 + 32 + j];
                }
                out[std::size_t(slot * 32 + j) * std::size_t(m_channels)] = sum;
            }
        }
    }

    int m_channels;
    std::vector<std::uint8_t> m_main; // the bit reservoir
    std::int16_t m_lines[2][kGranuleLines] = {};
    float m_spectrum[2][kGranuleLines] = {};
    int m_scalefactors[2][kMaxBands] = {};
    bool m_lsfPreflag[2] = {false, false};
    float m_subband[18][32] = {};
    float m_overlap[2][32][18];
    float m_synth[2][1024];
    int m_synthOffset[2] = {0, 0};
};

class Mp3Stream final : public AudioStream {
public:
    Mp3Stream(Span<const std::byte> data, std::vector<std::size_t> frames, const FrameInfo &first, int delay, int padding,
              bool gapless)
        : m_data(reinterpret_cast<const std::uint8_t *>(data.data())), m_frames(std::move(frames)), m_first(first),
          m_decoder(first.channels) {
        const std::uint64_t raw = std::uint64_t(m_frames.size()) * std::uint64_t(first.samples());
        m_total = raw;
        if (gapless) {
            // The encoder's own delay and the decoder's come off the front,
            // the padding that filled the last frame off the back.
            const std::uint64_t skip = std::uint64_t(delay) + kDecoderDelay;
            const std::uint64_t trimmed = std::uint64_t(delay) + std::uint64_t(padding);
            if (skip < raw && trimmed < raw) {
                m_skip = skip;
                m_total = std::min(raw - trimmed, raw - skip);
            }
        }
        m_block.resize(std::size_t(first.samples()) * std::size_t(first.channels));
        m_discard = m_skip;
    }

    AudioFormat format() const noexcept override { return AudioFormat::Mp3; }
    int sampleRate() const noexcept override { return m_first.sampleRate; }
    int channels() const noexcept override { return m_first.channels; }
    std::optional<std::uint64_t> totalFrames() const noexcept override { return m_total; }
    std::uint64_t position() const noexcept override { return m_position; }

    Result<std::size_t> read(Span<float> out) override {
        const std::size_t channels = std::size_t(m_first.channels);
        const std::size_t wanted = std::size_t(std::min<std::uint64_t>(out.size() / channels, m_total - m_position));
        std::size_t done = 0;
        while (done < wanted) {
            if (m_blockRead == m_blockFrames) {
                if (m_nextFrame >= m_frames.size()) {
                    break;
                }
                FrameInfo frame;
                const std::uint8_t *at = m_data + m_frames[m_nextFrame++];
                if (parseHeader(at, frame) && frame.sampleRate == m_first.sampleRate && frame.version == m_first.version) {
                    m_decoder.decode(at, frame, m_block.data());
                } else {
                    std::fill(m_block.begin(), m_block.end(), 0.0f);
                }
                m_blockFrames = std::size_t(m_first.samples());
                m_blockRead = 0;
            }
            const std::size_t available = m_blockFrames - m_blockRead;
            if (m_discard > 0) {
                const std::size_t drop = std::size_t(std::min<std::uint64_t>(m_discard, available));
                m_blockRead += drop;
                m_discard -= drop;
                continue;
            }
            const std::size_t take = std::min(wanted - done, available);
            std::copy_n(m_block.data() + m_blockRead * channels, take * channels, out.data() + done * channels);
            m_blockRead += take;
            done += take;
        }
        m_position += done;
        return done;
    }

    Result<void> seekToFrame(std::uint64_t frame) override {
        frame = std::min(frame, m_total);
        const std::uint64_t raw = frame + m_skip;
        const std::uint64_t perFrame = std::uint64_t(m_first.samples());
        // A few frames early: the bit reservoir and the overlap of the frame
        // before must be there when the target is decoded.
        const std::uint64_t index = raw / perFrame;
        const std::uint64_t from = index > 10 ? index - 10 : 0;
        m_nextFrame = std::size_t(std::min<std::uint64_t>(from, m_frames.size()));
        m_discard = raw - from * perFrame;
        m_blockFrames = m_blockRead = 0;
        m_decoder.reset();
        m_position = frame;
        return success();
    }

private:
    const std::uint8_t *m_data;
    std::vector<std::size_t> m_frames; // where each audio frame starts
    FrameInfo m_first;
    Mp3Decoder m_decoder;
    std::uint64_t m_skip = 0;  // raw samples before the first one played
    std::uint64_t m_total = 0; // frames the caller sees
    std::uint64_t m_position = 0;
    std::uint64_t m_discard = 0;
    std::size_t m_nextFrame = 0;
    std::vector<float> m_block;
    std::size_t m_blockFrames = 0;
    std::size_t m_blockRead = 0;
};

// A header at `offset` whose frame fits in the data.
bool frameFits(Span<const std::byte> data, std::size_t offset, FrameInfo &info) noexcept {
    if (offset > data.size() || data.size() - offset < 4) {
        return false;
    }
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data()) + offset;
    return parseHeader(p, info) && data.size() - offset >= info.frameBytes;
}

} // namespace

bool mp3FrameAt(Span<const std::byte> data, std::size_t offset) noexcept {
    FrameInfo info, next;
    if (!frameFits(data, offset, info)) {
        return false;
    }
    const std::size_t after = offset + info.frameBytes;
    if (after == data.size()) {
        return true;
    }
    return frameFits(data, after, next) && next.version == info.version && next.sampleRate == info.sampleRate;
}

Result<std::unique_ptr<AudioStream>> openMp3Stream(Span<const std::byte> data, const AudioLimits &limits) {
    std::vector<std::size_t> frames;
    FrameInfo first;
    bool haveFirst = false;
    std::size_t offset = id3v2Size(data);
    bool synced = false;
    while (data.size() - offset >= 4) {
        FrameInfo info;
        const bool here = synced ? frameFits(data, offset, info) : (byteAt(data, offset) == 0xFF && mp3FrameAt(data, offset) &&
                                                                    frameFits(data, offset, info));
        if (here && (!haveFirst || (info.version == first.version && info.sampleRate == first.sampleRate))) {
            if (!haveFirst) {
                first = info;
                haveFirst = true;
            }
            frames.push_back(offset);
            offset += info.frameBytes;
            synced = true;
        } else {
            ++offset; // not a frame: look for the next pair of headers
            synced = false;
        }
    }
    if (!haveFirst) {
        return Error(ErrorCode::ParseError, "not an MP3 file: no MPEG audio Layer III frames");
    }
    if (Result<void> shape = checkStreamShape(first.channels, first.sampleRate, limits, "MP3"); !shape) {
        return std::move(shape).error();
    }

    // A Xing/Info (or VBRI) frame: an empty first frame that describes the
    // file. It is not sound; with LAME's extension it says how many samples
    // the encoder added at each end.
    int delay = 0, padding = 0;
    bool gapless = false;
    {
        const std::size_t frame = frames[0];
        const std::size_t tag = frame + 4 + std::size_t(first.sideBytes);
        bool infoFrame = false;
        for (const std::size_t at : {tag, tag + 2}) {
            if (at + 8 <= frame + first.frameBytes && (matches(data, at, "Xing", 4) || matches(data, at, "Info", 4))) {
                infoFrame = true;
                const std::uint32_t flags = be32(data, at + 4);
                std::size_t lame = at + 8 + ((flags & 1) ? 4u : 0u) + ((flags & 2) ? 4u : 0u) + ((flags & 4) ? 100u : 0u) +
                                   ((flags & 8) ? 4u : 0u);
                if (lame + 24 <= frame + first.frameBytes) {
                    delay = int(byteAt(data, lame + 21) << 4 | byteAt(data, lame + 22) >> 4);
                    padding = int((byteAt(data, lame + 22) & 0x0F) << 8 | byteAt(data, lame + 23));
                    gapless = delay != 0 || padding != 0;
                }
                break;
            }
        }
        if (!infoFrame && frame + 36 + 4 <= frame + first.frameBytes && matches(data, frame + 36, "VBRI", 4)) {
            infoFrame = true;
        }
        if (infoFrame) {
            frames.erase(frames.begin());
        }
    }
    return std::unique_ptr<AudioStream>(new Mp3Stream(data, std::move(frames), first, delay, padding, gapless));
}

} // namespace cfw::detail
