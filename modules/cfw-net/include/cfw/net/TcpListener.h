#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"
#include "cfw/net/TcpConnection.h"

namespace cfw {

class EventLoop;

// Accepts TCP connections on an EventLoop. Each accepted connection is handed
// to the accept callback, which takes ownership.
//
// On Windows the port is bound exclusively (SO_EXCLUSIVEADDRUSE), so another
// process cannot bind the same port and intercept connections.
//
// Threads: the loop thread only. Allocates: per accepted connection.
class TcpListener {
public:
    // `address` is numeric ("127.0.0.1", "0.0.0.0", "::1") or "localhost".
    // Port 0 picks a free port; port() reports it.
    [[nodiscard]] static Result<std::unique_ptr<TcpListener>> listen(EventLoop &loop, StringView address,
                                                                     std::uint16_t port, int backlog = 128);
    ~TcpListener();
    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;

    void setOnAccept(std::function<void(std::unique_ptr<TcpConnection>)> callback) { m_onAccept = std::move(callback); }
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }

private:
    TcpListener(EventLoop &loop, std::uintptr_t socket);
    void acceptPending();

    EventLoop &m_loop;
    std::uintptr_t m_socket;
    std::uint64_t m_watch = 0;
    std::uint16_t m_port = 0;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void(std::unique_ptr<TcpConnection>)> m_onAccept;
};

} // namespace cfw
