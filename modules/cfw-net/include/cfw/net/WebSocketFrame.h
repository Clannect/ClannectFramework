#pragma once

// The WebSocket wire protocol (RFC 6455 §5): frame encoding and an
// incremental decoder with the protocol's validation rules. Pure code, no
// sockets: WebSocket.h drives it over a TcpConnection.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

enum class WsOpcode : std::uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// Close status codes (RFC 6455 §7.4.1). Applications use 4000-4999.
namespace ws {
inline constexpr std::uint16_t kNormalClosure = 1000;
inline constexpr std::uint16_t kGoingAway = 1001;
inline constexpr std::uint16_t kProtocolError = 1002;
inline constexpr std::uint16_t kUnsupportedData = 1003;
inline constexpr std::uint16_t kNoStatus = 1005;       // never sent; reported when a close had no code
inline constexpr std::uint16_t kAbnormalClosure = 1006; // never sent; reported when TCP dropped
inline constexpr std::uint16_t kInvalidPayload = 1007;
inline constexpr std::uint16_t kPolicyViolation = 1008;
inline constexpr std::uint16_t kMessageTooBig = 1009;
inline constexpr std::uint16_t kInternalError = 1011;
inline constexpr std::uint16_t kTryAgainLater = 1013;

// True for codes a peer may legitimately send in a close frame.
[[nodiscard]] bool isSendableCloseCode(std::uint16_t code) noexcept;
} // namespace ws

// Which end of the connection this is. Clients mask every frame they send;
// servers never do. Each side rejects frames that break that rule.
enum class WsRole : std::uint8_t { Client, Server };

struct WsLimits {
    // Largest payload of a single frame, and of a whole (reassembled)
    // message. The engine's runtime sets both to its protocol limit.
    std::size_t maxFrameBytes = 16u * 1024u * 1024u;
    std::size_t maxMessageBytes = 16u * 1024u * 1024u;
};

// One complete, validated message or control frame.
struct WsMessage {
    enum class Kind : std::uint8_t { Text, Binary, Ping, Pong, Close };
    Kind kind = Kind::Binary;
    std::vector<std::byte> payload; // Text: valid UTF-8. Close: the reason text only.
    std::uint16_t closeCode = ws::kNoStatus;

    [[nodiscard]] StringView text() const noexcept {
        return StringView(reinterpret_cast<const char *>(payload.data()), payload.size());
    }
};

// Builds one frame. `mask` is required when role is Client and forbidden when
// Server. Control frames (Close, Ping, Pong) must have payloads <= 125 bytes.
[[nodiscard]] std::vector<std::byte> encodeWsFrame(WsOpcode opcode, Span<const std::byte> payload, bool final,
                                                   std::optional<std::array<std::byte, 4>> mask);
// Appends one frame to `out` (no allocation once `out` has capacity).
void appendWsFrame(std::vector<std::byte> &out, WsOpcode opcode, Span<const std::byte> payload, bool final,
                   std::optional<std::array<std::byte, 4>> mask);
// A close frame's payload: the code (big-endian) then the UTF-8 reason,
// truncated so the payload fits the 125-byte control-frame limit.
[[nodiscard]] std::vector<std::byte> encodeWsClosePayload(std::uint16_t code, StringView reason);

// Incremental decoder: feed() raw bytes as they arrive, then call next()
// until it returns nothing. Enforces RFC 6455 and the limits:
//
//   - reserved bits set, unknown opcode, wrong masking for the role,
//     fragmented or oversized (>125) control frame, continuation without a
//     message, new message inside a fragmented one, 64-bit length with the
//     top bit set, invalid close code        -> 1002 protocol error
//   - frame or message over the limits        -> 1009 message too big
//   - text or close reason not valid UTF-8    -> 1007 invalid payload
//
// A violation is a failed Result whose Error carries "close-code" in its
// context; the decoder then stays failed. Oversized frames are rejected from
// their header, before any of the payload is buffered.
//
// Threads: one instance per connection. Allocates: the receive buffer and
// message payloads, bounded by the limits.
class WsDecoder {
public:
    WsDecoder(WsRole localRole, WsLimits limits) noexcept : m_role(localRole), m_limits(limits) {}

    void feed(Span<const std::byte> data);
    [[nodiscard]] Result<std::optional<WsMessage>> next();

    [[nodiscard]] bool failed() const noexcept { return m_failed; }
    [[nodiscard]] std::size_t bufferedBytes() const noexcept { return m_buffer.size() - m_offset; }

private:
    [[nodiscard]] Error fail(std::uint16_t closeCode, const char *reason);

    WsRole m_role;
    WsLimits m_limits;
    std::vector<std::byte> m_buffer;
    std::size_t m_offset = 0;
    // The fragmented message being reassembled, if any.
    bool m_inMessage = false;
    WsOpcode m_messageOpcode = WsOpcode::Binary;
    std::vector<std::byte> m_message;
    bool m_failed = false;
};

// The close code carried by a decoder error, or 1002 if it has none.
[[nodiscard]] std::uint16_t wsCloseCodeOf(const Error &error) noexcept;

} // namespace cfw
