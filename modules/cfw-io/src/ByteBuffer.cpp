#include <bit>
#include <cmath>
#include <cstring>

#include "cfw/core/Utf8.h"
#include "cfw/io/ByteReader.h"
#include "cfw/io/ByteWriter.h"

namespace cfw {

namespace {

template <class T>
void appendLittleEndian(std::vector<std::byte> &out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFFu));
    }
}

template <class T>
T readLittleEndian(const std::byte *data) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(std::to_integer<std::uint8_t>(data[i])) << (8 * i));
    }
    return value;
}

} // namespace

void ByteWriter::u16(std::uint16_t value) { appendLittleEndian(m_bytes, value); }
void ByteWriter::u32(std::uint32_t value) { appendLittleEndian(m_bytes, value); }
void ByteWriter::u64(std::uint64_t value) { appendLittleEndian(m_bytes, value); }
void ByteWriter::f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
void ByteWriter::f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

void ByteWriter::string(StringView value) {
    std::size_t length = value.size();
    if (length > 0xFFFF) {
        length = 0xFFFF;
        // Back up to a character boundary: never emit half a UTF-8 sequence.
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0u) == 0x80u) {
            --length;
        }
    }
    u16(static_cast<std::uint16_t>(length));
    const auto *first = reinterpret_cast<const std::byte *>(value.data());
    m_bytes.insert(m_bytes.end(), first, first + length);
}

const std::byte *ByteReader::take(std::size_t count) noexcept {
    if (!m_ok || m_data.size() - m_offset < count) {
        m_ok = false;
        return nullptr;
    }
    const std::byte *start = m_data.data() + m_offset;
    m_offset += count;
    return start;
}

std::uint8_t ByteReader::u8() noexcept {
    const std::byte *data = take(1);
    return data ? std::to_integer<std::uint8_t>(data[0]) : 0;
}

std::uint16_t ByteReader::u16() noexcept {
    const std::byte *data = take(2);
    return data ? readLittleEndian<std::uint16_t>(data) : 0;
}

std::uint32_t ByteReader::u32() noexcept {
    const std::byte *data = take(4);
    return data ? readLittleEndian<std::uint32_t>(data) : 0;
}

std::uint64_t ByteReader::u64() noexcept {
    const std::byte *data = take(8);
    return data ? readLittleEndian<std::uint64_t>(data) : 0;
}

float ByteReader::f32() noexcept {
    const float value = std::bit_cast<float>(u32());
    if (!std::isfinite(value)) {
        m_ok = false;
        return 0.0f;
    }
    return m_ok ? value : 0.0f;
}

double ByteReader::f64() noexcept {
    const double value = std::bit_cast<double>(u64());
    if (!std::isfinite(value)) {
        m_ok = false;
        return 0.0;
    }
    return m_ok ? value : 0.0;
}

String ByteReader::string(std::size_t maxBytes) {
    const std::uint16_t length = u16();
    if (length > maxBytes) {
        m_ok = false;
        return String();
    }
    const std::byte *data = take(length);
    if (!data) {
        return String();
    }
    const StringView text(reinterpret_cast<const char *>(data), length);
    if (!isValidUtf8(text)) {
        m_ok = false;
        return String();
    }
    return String(text);
}

Span<const std::byte> ByteReader::bytes(std::size_t count) noexcept {
    const std::byte *data = take(count);
    return data ? Span<const std::byte>(data, count) : Span<const std::byte>();
}

} // namespace cfw
