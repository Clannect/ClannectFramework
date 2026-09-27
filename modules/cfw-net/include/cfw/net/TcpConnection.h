#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"
#include "cfw/net/ByteStream.h"

namespace cfw {

class EventLoop;
class Executor;
class TcpConnection;

namespace detail {
// Something the event loop flushes once per pass (write coalescing).
class DeferredFlush {
public:
    virtual void flushDeferred() = 0;

protected:
    ~DeferredFlush() = default;
};

// Anything a ConnectRequest can cancel (a TCP connect, a WebSocket connect).
class Cancellable {
public:
    virtual ~Cancellable() = default;
    virtual void cancel() = 0;
    [[nodiscard]] virtual bool isDone() const = 0;
};
} // namespace detail

// A pending TcpConnection::connect(). Destroying it (or calling cancel())
// cancels the attempt: the callback is then never called. Like every object
// bound to a loop, it must be destroyed before the loop is.
//
// Threads: the loop thread. Allocates: the shared attempt state.
class ConnectRequest {
public:
    ConnectRequest() = default;
    explicit ConnectRequest(std::shared_ptr<detail::Cancellable> state) : m_state(std::move(state)) {}
    ConnectRequest(ConnectRequest &&) noexcept = default;
    ConnectRequest &operator=(ConnectRequest &&other) noexcept;
    ConnectRequest(const ConnectRequest &) = delete;
    ConnectRequest &operator=(const ConnectRequest &) = delete;
    ~ConnectRequest() { cancel(); }

    void cancel() noexcept;
    [[nodiscard]] bool pending() const noexcept;

private:
    std::shared_ptr<detail::Cancellable> m_state;
};

// A non-blocking TCP connection on an EventLoop.
//
// Receiving: the data callback is handed each chunk as it arrives (the span is
// valid only during the call). setReadPaused(true) stops reading, so a slow
// consumer pushes back on the peer through TCP flow control.
//
// Sending: send() never blocks and makes no system call: it queues, and the
// loop writes each connection's queue once per pass, before it blocks, so
// many small messages share one send(). What the socket cannot take stays
// queued.
// queuedBytes() is the backlog, and the drained callback fires when it
// empties: callers bound their memory by watching those.
//
// Closing: close() sends what is queued, then closes; abort() drops it. The
// closed callback fires exactly once, with nothing for an orderly close by
// either side, or the error that ended the connection. After it fires the
// connection is inert.
//
// Every callback may destroy this object (for example, erase it from a list),
// or replace any callback including itself; nothing touches `this` after a
// callback that destroyed it.
//
// Threads: the loop thread only. Allocates: the send queue, as it grows.
class TcpConnection final : public ByteStream, private detail::DeferredFlush {
public:
    using ConnectCallback = std::function<void(Result<std::unique_ptr<TcpConnection>>)>;

    struct ConnectOptions {
        Duration timeout = std::chrono::seconds(10);
    };

    // Connects to `host` (a name or a numeric address). Name lookup runs on
    // `resolver`, never on the loop thread. Each resolved address is tried in
    // turn until one connects or the timeout passes.
    [[nodiscard]] static ConnectRequest connect(EventLoop &loop, Executor &resolver, StringView host,
                                                std::uint16_t port, ConnectCallback callback, ConnectOptions options);
    [[nodiscard]] static ConnectRequest connect(EventLoop &loop, Executor &resolver, StringView host,
                                                std::uint16_t port, ConnectCallback callback) {
        return connect(loop, resolver, host, port, std::move(callback), ConnectOptions{});
    }

    ~TcpConnection() override;
    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    void setOnData(std::function<void(Span<const std::byte>)> callback) override { m_onData = std::move(callback); }
    void setOnClosed(std::function<void(const std::optional<Error> &)> callback) override {
        m_onClosed = std::move(callback);
    }
    void setOnDrained(std::function<void()> callback) override { m_onDrained = std::move(callback); }

    using ByteStream::send;
    void send(Span<const std::byte> data) override;
    [[nodiscard]] std::size_t queuedBytes() const noexcept override { return m_queue.size() - m_queueOffset; }

    void setReadPaused(bool paused) override;
    void close() override;
    void abort() override;

    [[nodiscard]] bool isOpen() const noexcept override { return m_socket != ~std::uintptr_t(0) && !m_closed; }
    [[nodiscard]] const String &peerAddress() const noexcept override { return m_peer; }

    // Internal: adopts a connected, non-blocking socket.
    static std::unique_ptr<TcpConnection> adopt(EventLoop &loop, std::uintptr_t socket);

private:
    TcpConnection(EventLoop &loop, std::uintptr_t socket);

    void flushDeferred() override;
    void scheduleFlush();
    void onReady(bool readable, bool writable, bool failed);
    void readAvailable();
    void flush();
    void updateInterest();
    // Closes the socket and reports `reason` once. Returns false if this
    // object was destroyed by the callback.
    bool finish(std::optional<Error> reason);

    EventLoop &m_loop;
    std::uintptr_t m_socket;
    std::uint64_t m_watch = 0;
    String m_peer;
    std::vector<std::byte> m_queue;
    std::size_t m_queueOffset = 0;
    bool m_readPaused = false;
    bool m_closing = false; // close() called: finish once the queue drains
    bool m_flushScheduled = false;
    bool m_closed = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    std::function<void(Span<const std::byte>)> m_onData;
    std::function<void(const std::optional<Error> &)> m_onClosed;
    std::function<void()> m_onDrained;
};

} // namespace cfw
