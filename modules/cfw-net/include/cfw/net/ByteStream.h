#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw {

// A reliable, ordered, bidirectional byte stream on an EventLoop: a TCP
// connection, or (later) TLS over one. Protocols (WebSocket, HTTP) are written
// against this, so they run over either unchanged.
//
// The contract is TcpConnection's (see TcpConnection.h): send() queues and
// never blocks; the closed callback fires exactly once; any callback may
// destroy the stream or replace any callback.
//
// Threads: the loop thread only.
class ByteStream {
public:
    virtual ~ByteStream() = default;

    virtual void setOnData(std::function<void(Span<const std::byte>)> callback) = 0;
    virtual void setOnClosed(std::function<void(const std::optional<Error> &)> callback) = 0;
    virtual void setOnDrained(std::function<void()> callback) = 0;

    virtual void send(Span<const std::byte> data) = 0;
    void send(StringView text) {
        send(Span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
    }
    [[nodiscard]] virtual std::size_t queuedBytes() const noexcept = 0;

    virtual void setReadPaused(bool paused) = 0;
    // Sends what is queued, then ends the stream.
    virtual void close() = 0;
    // Ends the stream now, dropping queued data. No closed callback.
    virtual void abort() = 0;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual const String &peerAddress() const noexcept = 0;
};

} // namespace cfw
