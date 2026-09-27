#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// Little-endian binary writer for the network protocol and binary scene data.
// The encoding is the engine's existing wire format, byte for byte:
// fixed-width little-endian integers, IEEE-754 floats, and strings as a u16
// byte length followed by UTF-8.
//
// Threads: one instance per thread. Allocates: the growing byte buffer.
class ByteWriter {
public:
    void u8(std::uint8_t value) { m_bytes.push_back(static_cast<std::byte>(value)); }
    void i8(std::int8_t value) { u8(static_cast<std::uint8_t>(value)); }
    void u16(std::uint16_t value);
    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }
    void u32(std::uint32_t value);
    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }
    void u64(std::uint64_t value);
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void f32(float value);
    void f64(double value);
    // UTF-8 with a u16 byte-length prefix. Text longer than 65535 bytes is cut
    // at the last whole character that fits, never mid-character; callers keep
    // strings under their message's limit, and the reader enforces it.
    void string(StringView value);
    void bytes(Span<const std::byte> data) { m_bytes.insert(m_bytes.end(), data.begin(), data.end()); }

    [[nodiscard]] Span<const std::byte> data() const noexcept { return m_bytes; }
    [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(m_bytes); }
    void clear() noexcept { m_bytes.clear(); }

private:
    std::vector<std::byte> m_bytes;
};

} // namespace cfw
