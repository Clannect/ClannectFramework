#pragma once

// Private to cfw-net: a thin, non-blocking socket layer over Winsock and BSD
// sockets. Implemented in SocketWin32.cpp / SocketPosix.cpp; this header
// includes no platform headers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/Span.h"
#include "cfw/core/String.h"

namespace cfw::detail {

using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = ~SocketHandle(0);

// Keeps the platform socket library initialised (Winsock is reference counted
// per process); every EventLoop holds one.
class NetLibrary {
public:
    [[nodiscard]] static Result<NetLibrary> acquire();
    NetLibrary(NetLibrary &&other) noexcept : m_held(other.m_held) { other.m_held = false; }
    NetLibrary &operator=(NetLibrary &&) = delete;
    NetLibrary(const NetLibrary &) = delete;
    ~NetLibrary();

private:
    NetLibrary() = default;
    bool m_held = false;
};

// A resolved socket address (IPv4 or IPv6), opaque outside the platform files.
struct SocketAddress {
    std::array<std::byte, 128> storage{};
    int length = 0;
    int family = 0;
};

[[nodiscard]] String describeAddress(const SocketAddress &address);

// Numeric addresses only ("127.0.0.1", "::1"); no DNS. Nothing if not numeric.
[[nodiscard]] Result<SocketAddress> numericAddress(StringView host, std::uint16_t port);
// Blocking DNS lookup (getaddrinfo). Run it off the event-loop thread.
[[nodiscard]] Result<std::vector<SocketAddress>> resolve(StringView host, std::uint16_t port);

void closeSocket(SocketHandle socket) noexcept;

// A listening, non-blocking TCP socket.
[[nodiscard]] Result<SocketHandle> listenTcp(const SocketAddress &address, int backlog);
[[nodiscard]] std::uint16_t localPort(SocketHandle socket) noexcept;
[[nodiscard]] String peerName(SocketHandle socket);

// kInvalidSocket when no connection is pending.
[[nodiscard]] Result<SocketHandle> acceptTcp(SocketHandle listener);

// Starts a non-blocking connect. Completion shows as writability; then call
// finishConnect for the outcome.
[[nodiscard]] Result<SocketHandle> startConnect(const SocketAddress &address);
[[nodiscard]] Result<void> finishConnect(SocketHandle socket);

struct IoResult {
    enum class Status : std::uint8_t { Ok, WouldBlock, Closed, Failed };
    Status status = Status::Ok;
    std::size_t bytes = 0;
    int osError = 0;
};
[[nodiscard]] IoResult receiveSome(SocketHandle socket, Span<std::byte> buffer) noexcept;
[[nodiscard]] IoResult sendSome(SocketHandle socket, Span<const std::byte> data) noexcept;
void shutdownSend(SocketHandle socket) noexcept;

struct PollEntry {
    SocketHandle socket = kInvalidSocket;
    bool wantRead = false;
    bool wantWrite = false;
    // Results.
    bool readable = false;
    bool writable = false;
    bool failed = false; // error or hang-up
};
// Waits up to `timeoutMs` (-1 = forever) for readiness. Returns the number of
// entries with events, or an error. `scratch` is reused between calls so a
// steady-state loop does not allocate.
[[nodiscard]] Result<int> pollSockets(std::vector<PollEntry> &entries, int timeoutMs, std::vector<std::byte> &scratch);

// A connected pair used to wake a blocked poll from another thread.
struct WakePair {
    SocketHandle readEnd = kInvalidSocket;
    SocketHandle writeEnd = kInvalidSocket;
};
[[nodiscard]] Result<WakePair> createWakePair();
void signalWake(SocketHandle writeEnd) noexcept;
void drainWake(SocketHandle readEnd) noexcept;

[[nodiscard]] Error networkError(StringView action, int osError);

} // namespace cfw::detail
