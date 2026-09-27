#pragma once

#include <cstddef>
#include <cstdint>

#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// Bounds-checked reader for ByteWriter's format. Data comes from untrusted
// peers and files, so nothing here may crash, read out of bounds, or allocate
// without a bound.
//
// Failure is sticky: any read past the end, a string over its limit or not
// valid UTF-8, or a non-finite float puts the reader in a failed state; that
// read and every later one return zero values. Decoders read a whole message
// and check ok() (or finished()) once at the end.
//
// Threads: one instance per thread. Allocates: only string(), bounded by its
// limit. The data is borrowed and must outlive the reader.
class ByteReader {
public:
    explicit ByteReader(Span<const std::byte> data) noexcept : m_data(data) {}

    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::int8_t i8() noexcept { return static_cast<std::int8_t>(u8()); }
    [[nodiscard]] std::uint16_t u16() noexcept;
    [[nodiscard]] std::int16_t i16() noexcept { return static_cast<std::int16_t>(u16()); }
    [[nodiscard]] std::uint32_t u32() noexcept;
    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    [[nodiscard]] std::uint64_t u64() noexcept;
    [[nodiscard]] std::int64_t i64() noexcept { return static_cast<std::int64_t>(u64()); }
    // Non-finite values fail the read: the simulation never sees NaN or infinity.
    [[nodiscard]] float f32() noexcept;
    [[nodiscard]] double f64() noexcept;
    // Fails if the prefixed length exceeds `maxBytes` or the text is not UTF-8.
    [[nodiscard]] String string(std::size_t maxBytes);
    // A view of the next `count` bytes (valid while the data is).
    [[nodiscard]] Span<const std::byte> bytes(std::size_t count) noexcept;

    // Marks the data invalid (e.g. a field value the decoder rejects).
    void fail() noexcept { m_ok = false; }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] bool atEnd() const noexcept { return m_offset == m_data.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return m_data.size() - m_offset; }
    // ok() and every byte consumed: decoders reject trailing garbage.
    [[nodiscard]] bool finished() const noexcept { return m_ok && atEnd(); }

private:
    // The next `count` bytes, or null (and failed) if they are not all there.
    [[nodiscard]] const std::byte *take(std::size_t count) noexcept;

    Span<const std::byte> m_data;
    std::size_t m_offset = 0;
    bool m_ok = true;
};

} // namespace cfw
