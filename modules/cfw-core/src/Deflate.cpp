#include "cfw/core/Deflate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <queue>

#include "cfw/core/Checksum.h"

namespace cfw {

namespace {

// --- Shared tables (RFC 1951 §3.2.5) ---------------------------------------

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                     33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                     1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<std::uint8_t, 19> kCodeLengthOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr int kMaxBits = 15;
constexpr std::size_t kWindow = 32768;

Error corrupt(const char *what) { return Error(ErrorCode::Corrupt, String("deflate: ") + what); }

// --- Decoder ----------------------------------------------------------------

// LSB-first bit reader. Reading past the end yields zero bits and counts them,
// so the decoder can report truncation instead of reading out of bounds.
class BitReader {
public:
    explicit BitReader(Span<const std::byte> data) noexcept
        : m_data(reinterpret_cast<const std::uint8_t *>(data.data())), m_size(data.size()) {}

    void refill() noexcept {
        while (m_count <= 56) {
            const std::uint64_t byte = m_pos < m_size ? m_data[m_pos] : 0u;
            ++m_pos;
            m_bits |= byte << m_count;
            m_count += 8;
        }
    }
    [[nodiscard]] std::uint32_t peek(unsigned n) const noexcept {
        return static_cast<std::uint32_t>(m_bits & ((std::uint64_t{1} << n) - 1u));
    }
    void drop(unsigned n) noexcept {
        m_bits >>= n;
        m_count -= n;
    }
    [[nodiscard]] std::uint32_t bits(unsigned n) noexcept {
        if (n == 0) {
            return 0;
        }
        if (m_count < n) {
            refill();
        }
        const std::uint32_t v = peek(n);
        drop(n);
        return v;
    }
    [[nodiscard]] std::uint64_t buffered() const noexcept { return m_bits; }
    [[nodiscard]] unsigned count() const noexcept { return m_count; }

    // Bytes consumed so far, rounded up to a whole byte.
    [[nodiscard]] std::size_t consumedBytes() const noexcept { return (m_pos * 8 - m_count + 7) / 8; }
    // True once more bits were consumed than the input holds.
    [[nodiscard]] bool overrun() const noexcept { return m_pos * 8 - m_count > m_size * 8; }

    // Stored blocks: skip to the next byte boundary and read raw bytes.
    void alignToByte() noexcept { drop(m_count % 8); }
    [[nodiscard]] bool takeBytes(std::size_t n, const std::uint8_t *&out) noexcept {
        // Un-read the whole bytes still buffered, then read straight from the input.
        const std::size_t position = consumedBytes();
        m_bits = 0;
        m_count = 0;
        if (position > m_size || m_size - position < n) {
            m_pos = m_size + 1;
            return false;
        }
        out = m_data + position;
        m_pos = position + n;
        return true;
    }

private:
    const std::uint8_t *m_data;
    std::size_t m_size;
    std::size_t m_pos = 0;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
};

// A canonical Huffman decoder: codes up to kFastBits long resolve with one
// table lookup; longer ones walk the canonical code counts (as in zlib's
// "puff" reference decoder).
class Huffman {
public:
    static constexpr unsigned kFastBits = 10;

    // Builds from code lengths (0 = unused). Rejects over-subscribed codes
    // and incomplete ones, except a single code of length 1, which the RFC
    // allows. All-zero lengths build an empty table that rejects every
    // symbol (a block with no distance codes).
    [[nodiscard]] bool build(const std::uint8_t *lengths, std::size_t n) {
        m_counts.fill(0);
        m_fast.fill(0);
        for (std::size_t i = 0; i < n; ++i) {
            ++m_counts[lengths[i]];
        }
        if (m_counts[0] == n) {
            return true;
        }
        int left = 1;
        int maxLength = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= m_counts[static_cast<std::size_t>(len)];
            if (left < 0) {
                return false; // over-subscribed
            }
            if (m_counts[static_cast<std::size_t>(len)] != 0) {
                maxLength = len;
            }
        }
        if (left > 0 && maxLength != 1) {
            return false; // incomplete
        }
        std::array<std::uint16_t, kMaxBits + 2> offsets{};
        for (std::size_t len = 1; len <= kMaxBits; ++len) {
            offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + m_counts[len]);
        }
        for (std::size_t sym = 0; sym < n; ++sym) {
            if (lengths[sym] != 0) {
                m_symbols[offsets[lengths[sym]]++] = static_cast<std::uint16_t>(sym);
            }
        }
        // Fast table: canonical codes, bit-reversed because DEFLATE packs
        // Huffman codes most-significant bit first into an LSB-first stream.
        std::array<std::uint32_t, kMaxBits + 1> nextCode{};
        std::uint32_t code = 0;
        for (std::size_t len = 1; len <= kMaxBits; ++len) {
            nextCode[len] = code;
            code = (code + m_counts[len]) << 1;
        }
        for (std::size_t sym = 0; sym < n; ++sym) {
            const unsigned len = lengths[sym];
            if (len == 0) {
                continue;
            }
            const std::uint32_t c = nextCode[len]++;
            if (len > kFastBits) {
                continue;
            }
            std::uint32_t reversed = 0;
            for (unsigned i = 0; i < len; ++i) {
                reversed |= ((c >> i) & 1u) << (len - 1 - i);
            }
            const auto entry = static_cast<std::uint16_t>(sym << 4 | len);
            for (std::uint32_t fill = reversed; fill < (1u << kFastBits); fill += 1u << len) {
                m_fast[fill] = entry;
            }
        }
        return true;
    }

    // The next symbol, or -1 for a code the table does not contain.
    [[nodiscard]] int decode(BitReader &in) const noexcept {
        if (in.count() < kMaxBits) {
            in.refill();
        }
        const std::uint16_t entry = m_fast[in.peek(kFastBits)];
        if (entry != 0) {
            in.drop(entry & 15u);
            return entry >> 4;
        }
        const std::uint64_t bits = in.buffered();
        int code = 0;
        int first = 0;
        int index = 0;
        for (unsigned len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>((bits >> (len - 1)) & 1u);
            const int count = m_counts[len];
            if (code - count < first) {
                in.drop(len);
                return m_symbols[static_cast<std::size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

private:
    std::array<std::uint16_t, kMaxBits + 1> m_counts{};
    std::array<std::uint16_t, 288> m_symbols{};
    std::array<std::uint16_t, 1u << kFastBits> m_fast{};
};

// Output buffer with an enforced limit; grows geometrically.
class Output {
public:
    explicit Output(std::size_t limit, std::size_t sizeHint) : m_limit(limit) {
        m_data.resize(std::min(limit, std::max<std::size_t>(sizeHint, 1024)));
    }
    [[nodiscard]] bool reserve(std::size_t extra) {
        if (extra > m_limit - m_size) {
            return false;
        }
        if (m_size + extra > m_data.size()) {
            m_data.resize(std::min(m_limit, std::max(m_size + extra, m_data.size() * 2)));
        }
        return true;
    }
    void put(std::uint8_t byte) noexcept { m_data[m_size++] = static_cast<std::byte>(byte); }
    void append(const std::uint8_t *bytes, std::size_t n) noexcept {
        std::memcpy(m_data.data() + m_size, bytes, n);
        m_size += n;
    }
    // Copies `length` bytes from `distance` back; the ranges may overlap.
    void copyBack(std::size_t distance, std::size_t length) noexcept {
        std::byte *out = m_data.data() + m_size;
        const std::byte *from = out - distance;
        if (distance >= length) {
            std::memcpy(out, from, length);
        } else {
            for (std::size_t i = 0; i < length; ++i) {
                out[i] = from[i];
            }
        }
        m_size += length;
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::vector<std::byte> take() {
        m_data.resize(m_size);
        return std::move(m_data);
    }

private:
    std::vector<std::byte> m_data;
    std::size_t m_size = 0;
    std::size_t m_limit;
};

const Huffman &fixedLiteralTable() {
    static const Huffman table = [] {
        std::array<std::uint8_t, 288> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
        std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
        std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
        std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
        Huffman h;
        (void)h.build(lengths.data(), lengths.size());
        return h;
    }();
    return table;
}

const Huffman &fixedDistanceTable() {
    static const Huffman table = [] {
        // 32 codes, as RFC 1951 §3.2.6 defines them: 30 and 31 decode but
        // are rejected as symbols. With only 30 the code would be incomplete.
        std::array<std::uint8_t, 32> lengths{};
        lengths.fill(5);
        Huffman h;
        (void)h.build(lengths.data(), lengths.size());
        return h;
    }();
    return table;
}

Result<void> readDynamicTables(BitReader &in, Huffman &literals, Huffman &distances) {
    const std::uint32_t literalCount = in.bits(5) + 257;
    const std::uint32_t distanceCount = in.bits(5) + 1;
    const std::uint32_t codeLengthCount = in.bits(4) + 4;
    if (literalCount > 286 || distanceCount > 30) {
        return corrupt("too many length or distance codes");
    }
    std::array<std::uint8_t, 19> codeLengthLengths{};
    for (std::uint32_t i = 0; i < codeLengthCount; ++i) {
        codeLengthLengths[kCodeLengthOrder[i]] = static_cast<std::uint8_t>(in.bits(3));
    }
    Huffman codeLengths;
    if (!codeLengths.build(codeLengthLengths.data(), codeLengthLengths.size())) {
        return corrupt("invalid code-length code");
    }
    std::array<std::uint8_t, 286 + 30> lengths{};
    const std::uint32_t total = literalCount + distanceCount;
    std::uint32_t i = 0;
    while (i < total) {
        const int sym = codeLengths.decode(in);
        if (sym < 0 || in.overrun()) {
            return corrupt("invalid code length");
        }
        if (sym < 16) {
            lengths[i++] = static_cast<std::uint8_t>(sym);
            continue;
        }
        std::uint8_t value = 0;
        std::uint32_t repeat = 0;
        if (sym == 16) {
            if (i == 0) {
                return corrupt("repeat with no previous length");
            }
            value = lengths[i - 1];
            repeat = 3 + in.bits(2);
        } else if (sym == 17) {
            repeat = 3 + in.bits(3);
        } else {
            repeat = 11 + in.bits(7);
        }
        if (repeat > total - i) {
            return corrupt("code lengths overflow");
        }
        while (repeat-- > 0) {
            lengths[i++] = value;
        }
    }
    if (lengths[256] == 0) {
        return corrupt("no end-of-block code");
    }
    if (!literals.build(lengths.data(), literalCount)) {
        return corrupt("invalid literal/length code");
    }
    if (!distances.build(lengths.data() + literalCount, distanceCount)) {
        return corrupt("invalid distance code");
    }
    return success();
}

Result<void> inflateBlock(BitReader &in, Output &out, const Huffman &literals, const Huffman &distances) {
    for (;;) {
        const int sym = literals.decode(in);
        if (in.overrun()) {
            return corrupt("truncated stream");
        }
        if (sym < 0) {
            return corrupt("invalid literal/length code");
        }
        if (sym < 256) {
            if (!out.reserve(1)) {
                return Error(ErrorCode::LimitExceeded, "deflate: output exceeds the limit");
            }
            out.put(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) {
            return success();
        }
        const auto lengthCode = static_cast<std::size_t>(sym - 257);
        if (lengthCode >= kLengthBase.size()) {
            return corrupt("invalid length symbol");
        }
        const std::size_t length = kLengthBase[lengthCode] + in.bits(kLengthExtra[lengthCode]);
        const int distanceSym = distances.decode(in);
        if (distanceSym < 0 || static_cast<std::size_t>(distanceSym) >= kDistBase.size()) {
            return corrupt("invalid distance symbol");
        }
        const auto dcode = static_cast<std::size_t>(distanceSym);
        const std::size_t distance = kDistBase[dcode] + in.bits(kDistExtra[dcode]);
        if (in.overrun()) {
            return corrupt("truncated stream");
        }
        if (distance > out.size()) {
            return corrupt("distance too far back");
        }
        if (!out.reserve(length)) {
            return Error(ErrorCode::LimitExceeded, "deflate: output exceeds the limit");
        }
        out.copyBack(distance, length);
    }
}

Result<std::vector<std::byte>> inflateImpl(Span<const std::byte> data, DecompressLimits limits, std::size_t &consumed) {
    BitReader in(data);
    Output out(limits.maxOutputBytes, data.size() * 4);
    Huffman literals;
    Huffman distances;
    bool final = false;
    while (!final) {
        final = in.bits(1) != 0;
        const std::uint32_t type = in.bits(2);
        if (in.overrun()) {
            return corrupt("truncated stream");
        }
        if (type == 0) {
            in.alignToByte();
            const std::uint8_t *header = nullptr;
            if (!in.takeBytes(4, header)) {
                return corrupt("truncated stored block");
            }
            const auto length = static_cast<std::size_t>(header[0] | header[1] << 8);
            const auto check = static_cast<std::size_t>(header[2] | header[3] << 8);
            if ((length ^ 0xFFFFu) != check) {
                return corrupt("stored block length check failed");
            }
            const std::uint8_t *bytes = nullptr;
            if (!in.takeBytes(length, bytes)) {
                return corrupt("truncated stored block");
            }
            if (!out.reserve(length)) {
                return Error(ErrorCode::LimitExceeded, "deflate: output exceeds the limit");
            }
            out.append(bytes, length);
        } else if (type == 1) {
            if (Result<void> r = inflateBlock(in, out, fixedLiteralTable(), fixedDistanceTable()); !r) {
                return std::move(r).error();
            }
        } else if (type == 2) {
            if (Result<void> r = readDynamicTables(in, literals, distances); !r) {
                return std::move(r).error();
            }
            if (Result<void> r = inflateBlock(in, out, literals, distances); !r) {
                return std::move(r).error();
            }
        } else {
            return corrupt("invalid block type");
        }
    }
    consumed = in.consumedBytes();
    return out.take();
}

// --- Encoder ----------------------------------------------------------------

class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte> &out) noexcept : m_out(out) {}
    void put(std::uint32_t value, unsigned n) {
        m_bits |= static_cast<std::uint64_t>(value) << m_count;
        m_count += n;
        while (m_count >= 8) {
            m_out.push_back(static_cast<std::byte>(m_bits & 0xFFu));
            m_bits >>= 8;
            m_count -= 8;
        }
    }
    void alignToByte() {
        if (m_count > 0) {
            put(0, 8 - m_count);
        }
    }

private:
    std::vector<std::byte> &m_out;
    std::uint64_t m_bits = 0;
    unsigned m_count = 0;
};

// Code lengths for `freq`, at most `maxBits` long, with at least two codes
// (so every tree is complete and every decoder accepts it).
std::vector<std::uint8_t> buildLengths(std::vector<std::uint32_t> freq, unsigned maxBits) {
    const std::size_t n = freq.size();
    std::size_t used = 0;
    for (const std::uint32_t f : freq) {
        used += f != 0 ? 1u : 0u;
    }
    for (std::size_t i = 0; used < 2 && i < n; ++i) {
        if (freq[i] == 0) {
            freq[i] = 1;
            ++used;
        }
    }
    // Plain Huffman tree over the used symbols; record each leaf's depth.
    struct Node {
        std::uint64_t weight;
        int left;
        int right;
    };
    std::vector<Node> nodes;
    nodes.reserve(2 * n);
    using Item = std::pair<std::uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
    std::vector<int> leafSymbol;
    for (std::size_t i = 0; i < n; ++i) {
        if (freq[i] != 0) {
            leafSymbol.push_back(static_cast<int>(i));
            nodes.push_back({freq[i], -1, -1});
            heap.emplace(freq[i], static_cast<int>(nodes.size() - 1));
        }
    }
    while (heap.size() > 1) {
        const Item a = heap.top();
        heap.pop();
        const Item b = heap.top();
        heap.pop();
        nodes.push_back({a.first + b.first, a.second, b.second});
        heap.emplace(a.first + b.first, static_cast<int>(nodes.size() - 1));
    }
    std::vector<unsigned> depth(nodes.size(), 0);
    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (nodes[i].left >= 0) {
            depth[static_cast<std::size_t>(nodes[i].left)] = depth[i] + 1;
            depth[static_cast<std::size_t>(nodes[i].right)] = depth[i] + 1;
        }
    }
    // Count leaves per depth, fold anything deeper than maxBits into maxBits,
    // then repair the Kraft sum (the approach miniz and zlib use).
    std::array<std::uint32_t, 64> counts{};
    for (std::size_t leaf = 0; leaf < leafSymbol.size(); ++leaf) {
        ++counts[std::min<unsigned>(depth[leaf], 63)];
    }
    for (std::size_t d = maxBits + 1; d < counts.size(); ++d) {
        counts[maxBits] += counts[d];
        counts[d] = 0;
    }
    std::uint64_t total = 0;
    for (unsigned d = 1; d <= maxBits; ++d) {
        total += static_cast<std::uint64_t>(counts[d]) << (maxBits - d);
    }
    while (total > (std::uint64_t{1} << maxBits)) {
        --counts[maxBits];
        for (unsigned d = maxBits - 1; d > 0; --d) {
            if (counts[d] != 0) {
                --counts[d];
                counts[d + 1] += 2;
                break;
            }
        }
        --total;
    }
    // Shortest codes to the most frequent symbols.
    std::vector<int> order = leafSymbol;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return freq[static_cast<std::size_t>(a)] > freq[static_cast<std::size_t>(b)];
    });
    std::vector<std::uint8_t> lengths(n, 0);
    std::size_t next = 0;
    for (unsigned d = 1; d <= maxBits; ++d) {
        for (std::uint32_t k = 0; k < counts[d]; ++k) {
            lengths[static_cast<std::size_t>(order[next++])] = static_cast<std::uint8_t>(d);
        }
    }
    return lengths;
}

// Canonical codes for `lengths`, bit-reversed for LSB-first output.
std::vector<std::uint16_t> buildCodes(const std::vector<std::uint8_t> &lengths) {
    std::array<std::uint32_t, kMaxBits + 2> counts{};
    for (const std::uint8_t l : lengths) {
        ++counts[l];
    }
    counts[0] = 0;
    std::array<std::uint32_t, kMaxBits + 2> next{};
    std::uint32_t code = 0;
    for (std::size_t len = 1; len <= kMaxBits; ++len) {
        code = (code + counts[len - 1]) << 1;
        next[len] = code;
    }
    std::vector<std::uint16_t> codes(lengths.size(), 0);
    for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
        const unsigned len = lengths[sym];
        if (len == 0) {
            continue;
        }
        const std::uint32_t c = next[len]++;
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < len; ++i) {
            reversed |= ((c >> i) & 1u) << (len - 1 - i);
        }
        codes[sym] = static_cast<std::uint16_t>(reversed);
    }
    return codes;
}

std::size_t lengthCode(std::size_t length) noexcept {
    std::size_t code = 0;
    while (code + 1 < kLengthBase.size() && kLengthBase[code + 1] <= length) {
        ++code;
    }
    return code;
}

std::size_t distanceCode(std::size_t distance) noexcept {
    std::size_t code = 0;
    while (code + 1 < kDistBase.size() && kDistBase[code + 1] <= distance) {
        ++code;
    }
    return code;
}

struct LengthTables {
    std::array<std::uint8_t, 259> lengthToCode{};
    std::array<std::uint8_t, 512> distanceToCode{}; // [d-1] for d <= 256, [256 + ((d-1) >> 7)] above
    LengthTables() {
        for (std::size_t len = 3; len <= 258; ++len) {
            lengthToCode[len] = static_cast<std::uint8_t>(lengthCode(len));
        }
        for (std::size_t d = 1; d <= 256; ++d) {
            distanceToCode[d - 1] = static_cast<std::uint8_t>(distanceCode(d));
        }
        for (std::size_t i = 2; i < 256; ++i) {
            distanceToCode[256 + i] = static_cast<std::uint8_t>(distanceCode((i << 7) + 1));
        }
    }
    [[nodiscard]] std::size_t distance(std::size_t d) const noexcept {
        return d <= 256 ? distanceToCode[d - 1] : distanceToCode[256 + ((d - 1) >> 7)];
    }
};

const LengthTables &lengthTables() {
    static const LengthTables tables;
    return tables;
}

// One LZ77 token: a literal, or a (length, distance) match.
struct Token {
    std::uint16_t length; // 0 for a literal
    std::uint16_t value;  // the literal byte, or the distance
};

struct LevelParams {
    int maxChain;
    std::size_t niceLength;
    bool lazy;
};

LevelParams paramsFor(int level) {
    switch (level) {
    case 1: return {4, 8, false};
    case 2: return {8, 16, false};
    case 3: return {32, 32, false};
    case 4: return {16, 32, true};
    case 5: return {32, 64, true};
    case 6: return {128, 128, true};
    case 7: return {256, 128, true};
    case 8: return {1024, 258, true};
    default: return {4096, 258, true};
    }
}

class Encoder {
public:
    Encoder(Span<const std::byte> input, int level, std::vector<std::byte> &out)
        : m_in(reinterpret_cast<const std::uint8_t *>(input.data())), m_n(input.size()),
          m_params(paramsFor(level)), m_writer(out) {}

    void run() {
        m_head.assign(kHashSize, -1);
        m_prev.assign(kWindow, -1);
        m_tokens.reserve(kMaxTokens);
        std::size_t pos = 0;
        std::size_t blockStart = 0;
        while (pos < m_n) {
            Match best = findMatch(pos);
            if (m_params.lazy && best.length >= 3 && best.length < m_params.niceLength && pos + 1 < m_n) {
                const Match next = findMatch(pos + 1);
                if (next.length > best.length) {
                    addLiteral(pos);
                    ++pos;
                    best = next;
                }
            }
            if (best.length >= 3) {
                addMatch(best);
                pos += best.length;
            } else {
                addLiteral(pos);
                ++pos;
            }
            if (m_tokens.size() >= kMaxTokens) {
                flushBlock(blockStart, pos, false);
                blockStart = pos;
            }
        }
        flushBlock(blockStart, m_n, true);
        m_writer.alignToByte();
    }

private:
    static constexpr std::size_t kHashBits = 15;
    static constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
    static constexpr std::size_t kMaxTokens = 1u << 15;

    struct Match {
        std::size_t length = 0;
        std::size_t distance = 0;
    };

    [[nodiscard]] std::size_t hashAt(std::size_t i) const noexcept {
        const std::uint32_t v = static_cast<std::uint32_t>(m_in[i]) << 16 | static_cast<std::uint32_t>(m_in[i + 1]) << 8 |
                                m_in[i + 2];
        return (v * 2654435761u) >> (32 - kHashBits);
    }

    void insertUpTo(std::size_t p) {
        for (; m_inserted < p; ++m_inserted) {
            if (m_inserted + 2 >= m_n) {
                continue;
            }
            const std::size_t h = hashAt(m_inserted);
            m_prev[m_inserted % kWindow] = m_head[h];
            m_head[h] = static_cast<std::int64_t>(m_inserted);
        }
    }

    Match findMatch(std::size_t p) {
        insertUpTo(p);
        Match best;
        if (p + 2 >= m_n) {
            return best;
        }
        const std::size_t maxLength = std::min<std::size_t>(258, m_n - p);
        std::int64_t candidate = m_head[hashAt(p)];
        int chain = m_params.maxChain;
        while (candidate >= 0 && chain-- > 0) {
            const auto c = static_cast<std::size_t>(candidate);
            if (p - c > kWindow) {
                break;
            }
            if (m_in[c + best.length] == m_in[p + best.length] && m_in[c] == m_in[p]) {
                std::size_t len = 0;
                while (len < maxLength && m_in[c + len] == m_in[p + len]) {
                    ++len;
                }
                if (len > best.length) {
                    best = {len, p - c};
                    if (len >= m_params.niceLength || len == maxLength) {
                        break;
                    }
                }
            }
            const std::int64_t next = m_prev[c % kWindow];
            if (next >= candidate) {
                break;
            }
            candidate = next;
        }
        if (best.length < 3 || (best.length == 3 && best.distance > 4096)) {
            return {}; // a far 3-byte match costs more than three literals
        }
        return best;
    }

    void addLiteral(std::size_t pos) {
        m_tokens.push_back({0, m_in[pos]});
        ++m_literalFreq[m_in[pos]];
    }

    void addMatch(const Match &m) {
        m_tokens.push_back({static_cast<std::uint16_t>(m.length), static_cast<std::uint16_t>(m.distance)});
        ++m_literalFreq[257 + lengthTables().lengthToCode[m.length]];
        ++m_distanceFreq[lengthTables().distance(m.distance)];
    }

    void flushBlock(std::size_t start, std::size_t end, bool final) {
        m_literalFreq[256] = 1;
        const auto literalLengths = buildLengths(std::vector<std::uint32_t>(m_literalFreq.begin(), m_literalFreq.end()), 15);
        const auto distanceLengths =
            buildLengths(std::vector<std::uint32_t>(m_distanceFreq.begin(), m_distanceFreq.end()), 15);

        // Dynamic header: run-length code the combined lengths.
        std::size_t hlit = 286;
        while (hlit > 257 && literalLengths[hlit - 1] == 0) {
            --hlit;
        }
        std::size_t hdist = 30;
        while (hdist > 1 && distanceLengths[hdist - 1] == 0) {
            --hdist;
        }
        std::vector<std::uint8_t> all(literalLengths.begin(), literalLengths.begin() + static_cast<std::ptrdiff_t>(hlit));
        all.insert(all.end(), distanceLengths.begin(), distanceLengths.begin() + static_cast<std::ptrdiff_t>(hdist));
        struct Rle {
            std::uint8_t symbol;
            std::uint8_t extra;
        };
        std::vector<Rle> rle;
        std::vector<std::uint32_t> clFreq(19, 0);
        for (std::size_t i = 0; i < all.size();) {
            const std::uint8_t v = all[i];
            std::size_t run = 1;
            while (i + run < all.size() && all[i + run] == v) {
                ++run;
            }
            std::size_t left = run;
            if (v == 0) {
                while (left >= 11) {
                    const std::size_t r = std::min<std::size_t>(left, 138);
                    rle.push_back({18, static_cast<std::uint8_t>(r - 11)});
                    left -= r;
                }
                if (left >= 3) {
                    rle.push_back({17, static_cast<std::uint8_t>(left - 3)});
                    left = 0;
                }
            } else {
                rle.push_back({v, 0});
                --left;
                while (left >= 3) {
                    const std::size_t r = std::min<std::size_t>(left, 6);
                    rle.push_back({16, static_cast<std::uint8_t>(r - 3)});
                    left -= r;
                }
            }
            while (left-- > 0) {
                rle.push_back({v, 0});
            }
            i += run;
        }
        for (const Rle &r : rle) {
            ++clFreq[r.symbol];
        }
        const auto clLengths = buildLengths(clFreq, 7);
        std::size_t hclen = 19;
        while (hclen > 4 && clLengths[kCodeLengthOrder[hclen - 1]] == 0) {
            --hclen;
        }

        // Costs in bits.
        std::uint64_t extraBits = 0;
        for (std::size_t c = 0; c < 29; ++c) {
            extraBits += static_cast<std::uint64_t>(m_literalFreq[257 + c]) * kLengthExtra[c];
        }
        for (std::size_t c = 0; c < 30; ++c) {
            extraBits += static_cast<std::uint64_t>(m_distanceFreq[c]) * kDistExtra[c];
        }
        std::uint64_t dynamicBits = 3 + 5 + 5 + 4 + 3 * hclen + extraBits;
        for (const Rle &r : rle) {
            dynamicBits += clLengths[r.symbol] + (r.symbol == 16 ? 2u : r.symbol == 17 ? 3u : r.symbol == 18 ? 7u : 0u);
        }
        std::uint64_t fixedBits = 3 + extraBits;
        for (std::size_t s = 0; s < 286; ++s) {
            dynamicBits += static_cast<std::uint64_t>(m_literalFreq[s]) * literalLengths[s];
            fixedBits += static_cast<std::uint64_t>(m_literalFreq[s]) * (s < 144 ? 8u : s < 256 ? 9u : s < 280 ? 7u : 8u);
        }
        for (std::size_t s = 0; s < 30; ++s) {
            dynamicBits += static_cast<std::uint64_t>(m_distanceFreq[s]) * distanceLengths[s];
            fixedBits += static_cast<std::uint64_t>(m_distanceFreq[s]) * 5u;
        }
        const std::size_t raw = end - start;
        const std::uint64_t storedBits = (raw + 5 * (raw / 65535 + 1)) * 8 + 7;

        if (storedBits <= fixedBits && storedBits <= dynamicBits) {
            writeStored(start, end, final);
        } else if (fixedBits <= dynamicBits) {
            std::vector<std::uint8_t> fl(288);
            for (std::size_t s = 0; s < 288; ++s) {
                fl[s] = static_cast<std::uint8_t>(s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
            }
            m_writer.put(final ? 1u : 0u, 1);
            m_writer.put(1, 2);
            writeTokens(fl, buildCodes(fl), std::vector<std::uint8_t>(30, 5), buildCodes(std::vector<std::uint8_t>(30, 5)));
        } else {
            m_writer.put(final ? 1u : 0u, 1);
            m_writer.put(2, 2);
            m_writer.put(static_cast<std::uint32_t>(hlit - 257), 5);
            m_writer.put(static_cast<std::uint32_t>(hdist - 1), 5);
            m_writer.put(static_cast<std::uint32_t>(hclen - 4), 4);
            for (std::size_t i = 0; i < hclen; ++i) {
                m_writer.put(clLengths[kCodeLengthOrder[i]], 3);
            }
            const auto clCodes = buildCodes(clLengths);
            for (const Rle &r : rle) {
                m_writer.put(clCodes[r.symbol], clLengths[r.symbol]);
                if (r.symbol == 16) {
                    m_writer.put(r.extra, 2);
                } else if (r.symbol == 17) {
                    m_writer.put(r.extra, 3);
                } else if (r.symbol == 18) {
                    m_writer.put(r.extra, 7);
                }
            }
            writeTokens(literalLengths, buildCodes(literalLengths), distanceLengths, buildCodes(distanceLengths));
        }
        m_tokens.clear();
        m_literalFreq.fill(0);
        m_distanceFreq.fill(0);
    }

    void writeStored(std::size_t start, std::size_t end, bool final) {
        do {
            const std::size_t n = std::min<std::size_t>(end - start, 65535);
            const bool last = final && start + n == end;
            m_writer.put(last ? 1u : 0u, 1);
            m_writer.put(0, 2);
            m_writer.alignToByte();
            m_writer.put(static_cast<std::uint32_t>(n), 16);
            m_writer.put(static_cast<std::uint32_t>(n ^ 0xFFFFu), 16);
            for (std::size_t i = 0; i < n; ++i) {
                m_writer.put(m_in[start + i], 8);
            }
            start += n;
        } while (start < end);
    }

    void writeTokens(const std::vector<std::uint8_t> &litLengths, const std::vector<std::uint16_t> &litCodes,
                     const std::vector<std::uint8_t> &distLengths, const std::vector<std::uint16_t> &distCodes) {
        const LengthTables &tables = lengthTables();
        for (const Token &t : m_tokens) {
            if (t.length == 0) {
                m_writer.put(litCodes[t.value], litLengths[t.value]);
                continue;
            }
            const std::size_t lc = tables.lengthToCode[t.length];
            m_writer.put(litCodes[257 + lc], litLengths[257 + lc]);
            m_writer.put(static_cast<std::uint32_t>(t.length - kLengthBase[lc]), kLengthExtra[lc]);
            const std::size_t dc = tables.distance(t.value);
            m_writer.put(distCodes[dc], distLengths[dc]);
            m_writer.put(static_cast<std::uint32_t>(t.value - kDistBase[dc]), kDistExtra[dc]);
        }
        m_writer.put(litCodes[256], litLengths[256]);
    }

    const std::uint8_t *m_in;
    std::size_t m_n;
    LevelParams m_params;
    BitWriter m_writer;
    std::vector<std::int64_t> m_head;
    std::vector<std::int64_t> m_prev;
    std::size_t m_inserted = 0;
    std::vector<Token> m_tokens;
    std::array<std::uint32_t, 286> m_literalFreq{};
    std::array<std::uint32_t, 30> m_distanceFreq{};
};

void deflateInto(Span<const std::byte> data, CompressOptions options, std::vector<std::byte> &out) {
    const int level = std::clamp(options.level, 0, 9);
    BitWriter writer(out);
    if (level == 0 || data.empty()) {
        // Stored blocks (an empty input is one empty stored block).
        std::size_t start = 0;
        do {
            const std::size_t n = std::min<std::size_t>(data.size() - start, 65535);
            writer.put(start + n == data.size() ? 1u : 0u, 1);
            writer.put(0, 2);
            writer.alignToByte();
            writer.put(static_cast<std::uint32_t>(n), 16);
            writer.put(static_cast<std::uint32_t>(n ^ 0xFFFFu), 16);
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(start),
                       data.begin() + static_cast<std::ptrdiff_t>(start + n));
            start += n;
        } while (start < data.size());
        return;
    }
    Encoder encoder(data, level, out);
    encoder.run();
}

} // namespace

Result<std::vector<std::byte>> inflate(Span<const std::byte> data, DecompressLimits limits) {
    std::size_t consumed = 0;
    return inflateImpl(data, limits, consumed);
}

std::vector<std::byte> deflate(Span<const std::byte> data, CompressOptions options) {
    std::vector<std::byte> out;
    out.reserve(data.size() / 2 + 64);
    deflateInto(data, options, out);
    return out;
}

Result<std::vector<std::byte>> zlibDecompress(Span<const std::byte> data, DecompressLimits limits) {
    if (data.size() < 2) {
        return Error(ErrorCode::Corrupt, "zlib: truncated header");
    }
    const auto cmf = static_cast<unsigned>(data[0]);
    const auto flg = static_cast<unsigned>(data[1]);
    if ((cmf & 15u) != 8 || (cmf >> 4) > 7 || (cmf * 256 + flg) % 31 != 0) {
        return Error(ErrorCode::Corrupt, "zlib: invalid header");
    }
    if ((flg & 0x20u) != 0) {
        return Error(ErrorCode::Unsupported, "zlib: preset dictionaries are not supported");
    }
    std::size_t consumed = 0;
    Result<std::vector<std::byte>> out = inflateImpl(data.subspan(2), limits, consumed);
    if (!out) {
        return out;
    }
    const std::size_t at = 2 + consumed;
    if (data.size() < at + 4) {
        return Error(ErrorCode::Corrupt, "zlib: truncated checksum");
    }
    const std::uint32_t expected = static_cast<std::uint32_t>(data[at]) << 24 | static_cast<std::uint32_t>(data[at + 1]) << 16 |
                                   static_cast<std::uint32_t>(data[at + 2]) << 8 | static_cast<std::uint32_t>(data[at + 3]);
    if (adler32(out.value()) != expected) {
        return Error(ErrorCode::Corrupt, "zlib: checksum mismatch");
    }
    return out;
}

std::vector<std::byte> zlibCompress(Span<const std::byte> data, CompressOptions options) {
    const int level = std::clamp(options.level, 0, 9);
    const unsigned flevel = level < 2 ? 0u : level < 6 ? 1u : level == 6 ? 2u : 3u;
    const unsigned cmf = 0x78;
    unsigned flg = flevel << 6;
    flg += 31 - (cmf * 256 + flg) % 31;
    std::vector<std::byte> out;
    out.reserve(data.size() / 2 + 64);
    out.push_back(static_cast<std::byte>(cmf));
    out.push_back(static_cast<std::byte>(flg));
    deflateInto(data, options, out);
    const std::uint32_t a = adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((a >> shift) & 0xFFu));
    }
    return out;
}

} // namespace cfw
