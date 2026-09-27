#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>

#include "cfw/core/Result.h"
#include "cfw/net/TcpListener.h"
#include "cfw/net/WebSocket.h"

namespace cfw {

class EventLoop;

// Accepts WebSocket connections: TCP accept, then the HTTP upgrade handshake,
// then hands each open WebSocket to the connection callback, which owns it.
//
// Defences for a public server:
//  - a client that does not finish the handshake within handshakeTimeout is
//    dropped;
//  - the handshake head is capped (8 KB) and malformed ones get a 400;
//  - at most maxPendingHandshakes handshakes run at once; beyond that new TCP
//    connections are closed immediately.
// Limiting open WebSockets (e.g. "server full", close code 1013) is the
// application's call; it owns the sockets.
//
// Threads: the loop thread only. Allocates: per pending handshake.
class WebSocketServer {
public:
    struct Options {
        WebSocket::Options socket;
        Duration handshakeTimeout = std::chrono::seconds(10);
        std::size_t maxPendingHandshakes = 256;
    };

    [[nodiscard]] static Result<std::unique_ptr<WebSocketServer>> listen(EventLoop &loop, StringView address,
                                                                         std::uint16_t port, Options options);
    [[nodiscard]] static Result<std::unique_ptr<WebSocketServer>> listen(EventLoop &loop, StringView address,
                                                                         std::uint16_t port) {
        return listen(loop, address, port, Options{});
    }
    ~WebSocketServer();
    WebSocketServer(const WebSocketServer &) = delete;
    WebSocketServer &operator=(const WebSocketServer &) = delete;

    void setOnConnection(std::function<void(std::unique_ptr<WebSocket>, const String &target)> callback) {
        m_onConnection = std::move(callback);
    }
    [[nodiscard]] std::uint16_t port() const noexcept { return m_listener->port(); }
    [[nodiscard]] std::size_t pendingHandshakes() const noexcept { return m_pending.size(); }

private:
    struct Pending;

    WebSocketServer(EventLoop &loop, std::unique_ptr<TcpListener> listener, Options options);
    void accept(std::unique_ptr<TcpConnection> connection);
    void onHandshakeData(std::list<Pending>::iterator it, Span<const std::byte> data);
    void drop(std::list<Pending>::iterator it);

    EventLoop &m_loop;
    std::unique_ptr<TcpListener> m_listener;
    Options m_options;
    std::list<Pending> m_pending;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void(std::unique_ptr<WebSocket>, const String &)> m_onConnection;
};

} // namespace cfw
