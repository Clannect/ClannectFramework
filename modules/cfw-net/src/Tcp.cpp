#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "EventLoopImpl.h"
#include "Socket.h"
#include "cfw/core/Contract.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TcpConnection.h"
#include "cfw/net/TcpListener.h"

namespace cfw {

namespace detail {

// One connect() attempt. Shared between the ConnectRequest (which can cancel
// it), the resolver thread (which must not touch a cancelled attempt's loop
// after the loop is gone) and the loop callbacks. See docs/decisions/0010.
struct ConnectState final : Cancellable, std::enable_shared_from_this<ConnectState> {
    EventLoop *loop = nullptr;
    TcpConnection::ConnectCallback callback;
    std::vector<SocketAddress> addresses;
    std::size_t next = 0;
    SocketHandle socket = kInvalidSocket;
    WatchId watch = 0;
    TimerId timeout = 0;
    std::optional<Error> lastError;
    std::atomic<bool> cancelled{false};
    // Held while the resolver thread posts to the loop, and while cancelling:
    // once cancel() returns, the resolver can no longer touch the loop, so the
    // loop may be destroyed.
    std::mutex postMutex;
    bool done = false;

    void cleanupSocket() {
        if (watch != 0) {
            loop->impl().removeWatch(watch);
            watch = 0;
        }
        closeSocket(socket);
        socket = kInvalidSocket;
    }

    void complete(Result<std::unique_ptr<TcpConnection>> result) {
        if (done) {
            return;
        }
        done = true;
        if (timeout != 0) {
            loop->cancelTimer(timeout);
            timeout = 0;
        }
        TcpConnection::ConnectCallback cb = std::move(callback);
        callback = nullptr;
        if (!cancelled.load() && cb) {
            cb(std::move(result));
        }
    }

    void tryNext() {
        while (next < addresses.size()) {
            const SocketAddress &address = addresses[next++];
            Result<SocketHandle> started = startConnect(address);
            if (!started) {
                lastError = std::move(started).error();
                continue;
            }
            socket = started.value();
            auto self = shared_from_this();
            watch = loop->impl().addWatch(socket, false, true, [self](Readiness) { self->onWritable(); });
            return;
        }
        complete(lastError ? std::move(*lastError) : Error(ErrorCode::NetworkError, "no address to connect to"));
    }

    void onWritable() {
        if (done) {
            return;
        }
        const Result<void> connected = finishConnect(socket);
        if (!connected) {
            lastError = connected.error();
            cleanupSocket();
            tryNext();
            return;
        }
        loop->impl().removeWatch(watch);
        watch = 0;
        const SocketHandle s = socket;
        socket = kInvalidSocket;
        complete(TcpConnection::adopt(*loop, s));
    }

    [[nodiscard]] bool isDone() const override { return done; }

    void cancel() override {
        {
            const std::lock_guard lock(postMutex);
            cancelled = true;
        }
        if (!done) {
            done = true;
            if (timeout != 0) {
                loop->cancelTimer(timeout);
                timeout = 0;
            }
            cleanupSocket();
            callback = nullptr;
        }
    }
};

} // namespace detail

// --- ConnectRequest ---------------------------------------------------------

ConnectRequest &ConnectRequest::operator=(ConnectRequest &&other) noexcept {
    if (this != &other) {
        cancel();
        m_state = std::move(other.m_state);
    }
    return *this;
}

void ConnectRequest::cancel() noexcept {
    if (m_state) {
        m_state->cancel();
        m_state.reset();
    }
}

bool ConnectRequest::pending() const noexcept { return m_state && !m_state->isDone(); }

// --- TcpConnection ----------------------------------------------------------

ConnectRequest TcpConnection::connect(EventLoop &loop, Executor &resolver, StringView host, std::uint16_t port,
                                      ConnectCallback callback, ConnectOptions options) {
    auto state = std::make_shared<detail::ConnectState>();
    state->loop = &loop;
    state->callback = std::move(callback);
    std::weak_ptr<detail::ConnectState> weak = state;
    state->timeout = loop.startTimer(options.timeout, [weak] {
        if (auto s = weak.lock()) {
            s->timeout = 0;
            s->cleanupSocket();
            s->complete(Error(ErrorCode::Timeout, "connect timed out"));
        }
    });

    // Numeric addresses need no lookup; names go to the resolver thread.
    if (Result<detail::SocketAddress> numeric = detail::numericAddress(host, port)) {
        state->addresses.push_back(numeric.value());
        state->tryNext();
    } else {
        resolver.submit([weak, &loop, name = String(host), port] {
            Result<std::vector<detail::SocketAddress>> addresses = detail::resolve(name, port);
            // Hand the answer to the loop; drop it if the attempt was cancelled.
            const auto s = weak.lock();
            if (!s) {
                return;
            }
            const std::lock_guard lock(s->postMutex);
            if (s->cancelled.load()) {
                return;
            }
            loop.post([weak, result = std::move(addresses)]() mutable {
                auto st = weak.lock();
                if (!st || st->done) {
                    return;
                }
                if (!result) {
                    st->complete(std::move(result).error());
                    return;
                }
                st->addresses = std::move(result).value();
                st->tryNext();
            });
        });
    }
    return ConnectRequest(std::move(state));
}

std::unique_ptr<TcpConnection> TcpConnection::adopt(EventLoop &loop, std::uintptr_t socket) {
    return std::unique_ptr<TcpConnection>(new TcpConnection(loop, socket));
}

TcpConnection::TcpConnection(EventLoop &loop, std::uintptr_t socket)
    : m_loop(loop), m_socket(socket), m_peer(detail::peerName(socket)) {
    m_watch = m_loop.impl().addWatch(socket, true, false, [this](detail::Readiness r) {
        onReady(r.readable, r.writable, r.failed);
    });
}

TcpConnection::~TcpConnection() {
    *m_alive = false;
    if (m_flushScheduled) {
        m_loop.impl().cancelFlush(this);
    }
    if (m_watch != 0) {
        m_loop.impl().removeWatch(m_watch);
    }
    detail::closeSocket(m_socket);
}

void TcpConnection::onReady(bool readable, bool writable, bool failed) {
    const std::shared_ptr<bool> alive = m_alive;
    if (writable) {
        flush();
        if (!*alive || m_closed) {
            return;
        }
    }
    if (readable || failed) {
        readAvailable();
    }
}

void TcpConnection::readAvailable() {
    const std::shared_ptr<bool> alive = m_alive;
    std::array<std::byte, 64 * 1024> buffer; // uninitialised on purpose: recv fills it
    // A few reads per wake-up, then yield so one busy peer cannot starve others.
    for (int i = 0; i < 4 && !m_closed && !m_readPaused; ++i) {
        const detail::IoResult r = detail::receiveSome(m_socket, buffer);
        switch (r.status) {
        case detail::IoResult::Status::Ok:
            if (m_onData) {
                // Copied: the callback may replace itself (e.g. a handshake
                // handing the connection to a WebSocket).
                const auto onData = m_onData;
                onData(Span<const std::byte>(buffer.data(), r.bytes));
                if (!*alive) {
                    return;
                }
            }
            break;
        case detail::IoResult::Status::WouldBlock:
            return;
        case detail::IoResult::Status::Closed:
            finish(std::nullopt);
            return;
        case detail::IoResult::Status::Failed:
            finish(detail::networkError("receive", r.osError).with("peer", m_peer));
            return;
        }
    }
}

void TcpConnection::send(Span<const std::byte> data) {
    if (m_closed || m_closing || data.empty()) {
        return;
    }
    if (m_queueOffset > 0 && m_queueOffset == m_queue.size()) {
        m_queue.clear();
        m_queueOffset = 0;
    }
    m_queue.insert(m_queue.end(), data.begin(), data.end());
    scheduleFlush();
}

void TcpConnection::scheduleFlush() {
    if (!m_flushScheduled) {
        m_flushScheduled = true;
        m_loop.impl().scheduleFlush(this);
    }
}

void TcpConnection::flushDeferred() {
    m_flushScheduled = false;
    if (!m_closed && m_socket != detail::kInvalidSocket) {
        flush();
    }
}

void TcpConnection::flush() {
    const std::shared_ptr<bool> alive = m_alive;
    while (queuedBytes() > 0) {
        const detail::IoResult r = detail::sendSome(m_socket, Span<const std::byte>(m_queue).subspan(m_queueOffset));
        if (r.status == detail::IoResult::Status::Ok) {
            m_queueOffset += r.bytes;
            continue;
        }
        if (r.status == detail::IoResult::Status::WouldBlock) {
            break;
        }
        finish(detail::networkError("send", r.osError).with("peer", m_peer));
        return;
    }
    if (queuedBytes() == 0) {
        m_queue.clear();
        m_queueOffset = 0;
        updateInterest();
        if (m_closing) {
            detail::shutdownSend(m_socket);
            finish(std::nullopt);
            return;
        }
        if (m_onDrained) {
            const auto onDrained = m_onDrained;
            onDrained();
            if (!*alive) {
                return;
            }
        }
    } else {
        // The socket buffer is full: ask the loop for writability so the rest
        // goes out when the peer reads. Without this a deferred flush that
        // cannot send everything stalls until the next send() (Linux's
        // loopback buffers are far smaller than Windows' auto-tuned ones).
        updateInterest();
        if (m_queueOffset > 64 * 1024 && m_queueOffset * 2 > m_queue.size()) {
            // Compact so a long-lived backlog does not grow without bound.
            m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(m_queueOffset));
            m_queueOffset = 0;
        }
    }
}

void TcpConnection::updateInterest() {
    if (m_watch != 0) {
        m_loop.impl().updateWatch(m_watch, !m_readPaused && !m_closed, queuedBytes() > 0 && !m_closed);
    }
}

void TcpConnection::setReadPaused(bool paused) {
    m_readPaused = paused;
    updateInterest();
}

void TcpConnection::close() {
    if (m_closed || m_closing) {
        return;
    }
    m_closing = true;
    if (queuedBytes() > 0) {
        scheduleFlush(); // finish() follows once the queue has drained
    } else {
        detail::shutdownSend(m_socket);
        const std::shared_ptr<bool> alive = m_alive;
        m_loop.post([this, alive] {
            if (*alive) {
                finish(std::nullopt);
            }
        });
    }
}

void TcpConnection::abort() {
    if (m_closed) {
        return;
    }
    m_closed = true;
    m_queue.clear();
    m_queueOffset = 0;
    if (m_flushScheduled) {
        m_loop.impl().cancelFlush(this);
        m_flushScheduled = false;
    }
    if (m_watch != 0) {
        m_loop.impl().removeWatch(m_watch);
        m_watch = 0;
    }
    detail::closeSocket(m_socket);
    m_socket = detail::kInvalidSocket;
}

bool TcpConnection::finish(std::optional<Error> reason) {
    if (m_socket == detail::kInvalidSocket) {
        return true;
    }
    m_closed = true;
    if (m_watch != 0) {
        m_loop.impl().removeWatch(m_watch);
        m_watch = 0;
    }
    detail::closeSocket(m_socket);
    m_socket = detail::kInvalidSocket;
    const std::shared_ptr<bool> alive = m_alive;
    if (m_onClosed) {
        auto callback = std::move(m_onClosed);
        m_onClosed = nullptr;
        callback(reason);
    }
    return *alive;
}

// --- TcpListener ------------------------------------------------------------

Result<std::unique_ptr<TcpListener>> TcpListener::listen(EventLoop &loop, StringView address, std::uint16_t port,
                                                          int backlog) {
    Result<detail::SocketAddress> bindAddress = detail::numericAddress(address, port);
    if (!bindAddress) {
        return std::move(bindAddress).error();
    }
    Result<detail::SocketHandle> socket = detail::listenTcp(bindAddress.value(), backlog);
    if (!socket) {
        return std::move(socket).error();
    }
    return std::unique_ptr<TcpListener>(new TcpListener(loop, socket.value()));
}

TcpListener::TcpListener(EventLoop &loop, std::uintptr_t socket)
    : m_loop(loop), m_socket(socket), m_port(detail::localPort(socket)) {
    m_watch = m_loop.impl().addWatch(socket, true, false, [this](detail::Readiness) { acceptPending(); });
}

TcpListener::~TcpListener() {
    *m_alive = false;
    m_loop.impl().removeWatch(m_watch);
    detail::closeSocket(m_socket);
}

void TcpListener::acceptPending() {
    const std::shared_ptr<bool> alive = m_alive;
    for (int i = 0; i < 64; ++i) {
        Result<detail::SocketHandle> accepted = detail::acceptTcp(m_socket);
        if (!accepted || accepted.value() == detail::kInvalidSocket) {
            return; // nothing more pending (errors on one accept do not stop the listener)
        }
        auto connection = TcpConnection::adopt(m_loop, accepted.value());
        if (m_onAccept) {
            m_onAccept(std::move(connection));
            if (!*alive) {
                return;
            }
        }
    }
}

} // namespace cfw
