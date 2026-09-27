#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/net/WebSocketHandshake.h"

namespace cfw {

// Extracts an HTTP/1.1 response body from the bytes after the head (RFC 9112
// §6): no body (HEAD, 1xx, 204, 304), Content-Length, chunked, or until the
// connection closes. Pure code; HttpClient drives it.
//
// Defensive against hostile servers: conflicting Content-Length values,
// transfer codings other than chunked, chunk sizes that overflow, overlong
// chunk-size or trailer lines, and bodies over the limit are all errors, never
// silent truncation or unbounded buffering.
//
// Threads: one instance per response. Allocates: nothing beyond its small
// line buffer; body bytes go straight to the sink.
class HttpBodyDecoder {
public:
    enum class Framing : std::uint8_t { None, Length, Chunked, UntilClose };

    [[nodiscard]] static Result<HttpBodyDecoder> forResponse(const HttpHead &head, StringView requestMethod,
                                                             std::size_t maxBodyBytes);

    using Sink = std::function<void(Span<const std::byte>)>;

    // Decodes `data`, passing body bytes to `sink`. Returns true once the body
    // is complete (bytes after that are not consumed), false if more is needed.
    [[nodiscard]] Result<bool> feed(Span<const std::byte> data, const Sink &sink);
    // The connection closed: complete for UntilClose, an error (truncated) for
    // an unfinished Length or Chunked body.
    [[nodiscard]] Result<void> finishOnClose();

    [[nodiscard]] Framing framing() const noexcept { return m_framing; }
    [[nodiscard]] bool complete() const noexcept { return m_complete; }
    [[nodiscard]] std::uint64_t bodyBytes() const noexcept { return m_bodyBytes; }

private:
    enum class ChunkState : std::uint8_t { Size, Data, DataEnd, Trailer };

    HttpBodyDecoder(Framing framing, std::uint64_t length, std::size_t maxBodyBytes) noexcept
        : m_framing(framing), m_remaining(length), m_maxBodyBytes(maxBodyBytes), m_complete(framing == Framing::None) {}

    [[nodiscard]] Result<void> deliver(Span<const std::byte> bytes, const Sink &sink);

    Framing m_framing;
    std::uint64_t m_remaining = 0; // Length: bytes left; Chunked: bytes left in this chunk
    std::size_t m_maxBodyBytes;
    std::uint64_t m_bodyBytes = 0;
    bool m_complete = false;
    ChunkState m_chunkState = ChunkState::Size;
    String m_line; // partial chunk-size or trailer line
};

} // namespace cfw
