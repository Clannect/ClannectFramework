#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <random>

#include "cfw/core/Clock.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/core/Url.h"
#include "cfw/net/ByteStream.h"
#include "cfw/net/TcpConnection.h"
#include "cfw/net/TlsStream.h"
#include "cfw/net/WebSocketFrame.h"

namespace cfw {

class EventLoop;
class Executor;

// An open WebSocket (RFC 6455), either end. Obtained from WebSocketServer
// (server side) or WebSocket::connect (client side).
//
// Messages arrive whole: fragmented messages are reassembled (within the
// limits) before the callback sees them. Pings are answered automatically.
//
// Closing follows the protocol: close() sends a close frame and waits (up to
// closeTimeout) for the peer's; a close frame from the peer is echoed. The
// closed callback fires exactly once with the code and reason that ended the
// connection. The code is 1006 if TCP dropped without a close handshake; for
// protocol violations it is the code this side sent (1002, 1007, 1009).
//
// Backpressure: bufferedAmount() is how much is queued for the socket; the
// drained callback fires when that reaches zero.
//
// Any callback may destroy the WebSocket. Destroying an open WebSocket drops
// the connection without a close handshake.
//
// Threads: the loop thread only. Allocates: frames and message buffers,
// bounded by the limits.
class WebSocket {
public:
    struct Options {
        WsLimits limits;
        Duration closeTimeout = std::chrono::seconds(5);
    };

    struct ConnectOptions {
        Options socket;
        Duration timeout = std::chrono::seconds(10); // TCP connect + handshake
        TlsOptions tls;                              // wss:// only
    };

    using ConnectCallback = std::function<void(Result<std::unique_ptr<WebSocket>>)>;

    // Connects to a ws:// or wss:// URL. wss:// runs over TlsStream and fails
    // with Unsupported where TlsStream::available() is false.
    [[nodiscard]] static ConnectRequest connect(EventLoop &loop, Executor &resolver, const Url &url,
                                                ConnectCallback callback, ConnectOptions options);
    [[nodiscard]] static ConnectRequest connect(EventLoop &loop, Executor &resolver, const Url &url,
                                                ConnectCallback callback) {
        return connect(loop, resolver, url, std::move(callback), ConnectOptions{});
    }

    enum class State : std::uint8_t { Open, Closing, Closed };

    ~WebSocket();
    WebSocket(const WebSocket &) = delete;
    WebSocket &operator=(const WebSocket &) = delete;

    void setOnBinary(std::function<void(Span<const std::byte>)> callback) { m_onBinary = std::move(callback); }
    void setOnText(std::function<void(StringView)> callback) { m_onText = std::move(callback); }
    void setOnPong(std::function<void(Span<const std::byte>)> callback) { m_onPong = std::move(callback); }
    void setOnClosed(std::function<void(std::uint16_t code, StringView reason)> callback) {
        m_onClosed = std::move(callback);
    }
    void setOnDrained(std::function<void()> callback);

    // Sends are ignored once the socket is closing or closed.
    void sendBinary(Span<const std::byte> data);
    void sendText(StringView text);
    void ping(Span<const std::byte> payload = {});
    // Starts the close handshake. `code` must be sendable (1000-1003,
    // 1007-1014, 3000-4999); the reason is cut to fit the 123-byte limit.
    void close(std::uint16_t code = ws::kNormalClosure, StringView reason = {});

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] std::size_t bufferedAmount() const noexcept;
    [[nodiscard]] const String &peerAddress() const noexcept { return m_peer; }

    // Internal: wraps an upgraded connection; `leftover` is data that arrived
    // after the handshake.
    static std::unique_ptr<WebSocket> adopt(EventLoop &loop, std::unique_ptr<ByteStream> connection, WsRole role,
                                            Options options, Span<const std::byte> leftover);

private:
    WebSocket(EventLoop &loop, std::unique_ptr<ByteStream> connection, WsRole role, Options options);

    void onData(Span<const std::byte> data);
    void sendFrame(WsOpcode opcode, Span<const std::byte> payload);
    void failConnection(std::uint16_t code, StringView reason);
    void finish(std::uint16_t code, StringView reason);

    EventLoop &m_loop;
    std::unique_ptr<ByteStream> m_connection;
    WsRole m_role;
    Options m_options;
    WsDecoder m_decoder;
    State m_state = State::Open;
    String m_peer;
    std::uint16_t m_closeCode = ws::kAbnormalClosure;
    String m_closeReason;
    std::uint64_t m_closeTimer = 0;
    std::mt19937 m_maskRandom;
    std::vector<std::byte> m_frame; // reused for every outgoing frame
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    std::function<void(Span<const std::byte>)> m_onBinary;
    std::function<void(StringView)> m_onText;
    std::function<void(Span<const std::byte>)> m_onPong;
    std::function<void(std::uint16_t, StringView)> m_onClosed;
};

} // namespace cfw
