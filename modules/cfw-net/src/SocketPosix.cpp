// POSIX (Linux, macOS) implementation of Socket.h.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

#include "Socket.h"

namespace cfw::detail {

namespace {

int native(SocketHandle s) noexcept { return static_cast<int>(s); }
SocketHandle handle(int s) noexcept { return s < 0 ? kInvalidSocket : static_cast<SocketHandle>(s); }

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL; // a closed peer must not kill the process with SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif

void prepare(int s) noexcept {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    fcntl(s, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

void setNoDelay(int s) noexcept {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

SocketAddress fromSockaddr(const sockaddr *address, socklen_t length) {
    SocketAddress out;
    out.length = static_cast<int>(std::min<std::size_t>(length, out.storage.size()));
    out.family = address->sa_family;
    std::memcpy(out.storage.data(), address, static_cast<std::size_t>(out.length));
    return out;
}

const sockaddr *asSockaddr(const SocketAddress &a) noexcept { return reinterpret_cast<const sockaddr *>(a.storage.data()); }

} // namespace

Error networkError(StringView action, int osError) {
    ErrorCode code = ErrorCode::NetworkError;
    if (osError == ETIMEDOUT) {
        code = ErrorCode::Timeout;
    } else if (osError == EACCES || osError == EPERM) {
        code = ErrorCode::PermissionDenied;
    } else if (osError == EADDRINUSE) {
        code = ErrorCode::AlreadyExists;
    } else if (osError == ENOMEM || osError == ENOBUFS) {
        code = ErrorCode::OutOfMemory;
    }
    String message(action);
    message += " failed: ";
    message += std::generic_category().message(osError);
    return Error(code, std::move(message)).with("os-error", std::to_string(osError));
}

Result<NetLibrary> NetLibrary::acquire() {
    NetLibrary library;
    library.m_held = true;
    return library;
}

NetLibrary::~NetLibrary() = default;

String describeAddress(const SocketAddress &address) {
    char text[INET6_ADDRSTRLEN] = {};
    if (address.family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(address.storage.data());
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
        return String(text) + ":" + std::to_string(ntohs(in->sin_port));
    }
    if (address.family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(address.storage.data());
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
        return "[" + String(text) + "]:" + std::to_string(ntohs(in6->sin6_port));
    }
    return "?";
}

Result<SocketAddress> numericAddress(StringView host, std::uint16_t port) {
    const String text = host == "localhost" ? String("127.0.0.1") : String(host);
    sockaddr_in in{};
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    if (inet_pton(AF_INET, text.c_str(), &in.sin_addr) == 1) {
        return fromSockaddr(reinterpret_cast<const sockaddr *>(&in), sizeof in);
    }
    String v6 = text;
    if (v6.size() >= 2 && v6.front() == '[' && v6.back() == ']') {
        v6 = v6.substr(1, v6.size() - 2);
    }
    sockaddr_in6 in6{};
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, v6.c_str(), &in6.sin6_addr) == 1) {
        return fromSockaddr(reinterpret_cast<const sockaddr *>(&in6), sizeof in6);
    }
    return Error(ErrorCode::InvalidArgument, "not a numeric address").with("host", String(host));
}

Result<std::vector<SocketAddress>> resolve(StringView host, std::uint16_t port) {
    if (Result<SocketAddress> numeric = numericAddress(host, port)) {
        return std::vector<SocketAddress>{numeric.value()};
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = nullptr;
    const String hostText(host);
    const String portText = std::to_string(port);
    const int rc = getaddrinfo(hostText.c_str(), portText.c_str(), &hints, &list);
    if (rc != 0) {
        return Error(ErrorCode::NotFound, String("resolve failed: ") + gai_strerror(rc)).with("host", hostText);
    }
    std::vector<SocketAddress> out;
    for (const addrinfo *entry = list; entry != nullptr; entry = entry->ai_next) {
        out.push_back(fromSockaddr(entry->ai_addr, entry->ai_addrlen));
    }
    freeaddrinfo(list);
    return out;
}

void closeSocket(SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        ::close(native(socket));
    }
}

Result<SocketHandle> listenTcp(const SocketAddress &address, int backlog) {
    const int s = ::socket(address.family, SOCK_STREAM, 0);
    if (s < 0) {
        return networkError("socket", errno);
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on); // POSIX semantics: only TIME_WAIT reuse
    prepare(s);
    if (::bind(s, asSockaddr(address), static_cast<socklen_t>(address.length)) != 0 || ::listen(s, backlog) != 0) {
        const int error = errno;
        ::close(s);
        return networkError("listen", error).with("address", describeAddress(address));
    }
    return handle(s);
}

std::uint16_t localPort(SocketHandle socket) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof storage;
    if (getsockname(native(socket), reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return 0;
    }
    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in *>(&storage)->sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_port);
}

String peerName(SocketHandle socket) {
    sockaddr_storage storage{};
    socklen_t length = sizeof storage;
    if (getpeername(native(socket), reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return "?";
    }
    return describeAddress(fromSockaddr(reinterpret_cast<const sockaddr *>(&storage), length));
}

Result<SocketHandle> acceptTcp(SocketHandle listener) {
    const int s = ::accept(native(listener), nullptr, nullptr);
    if (s < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED || errno == EINTR) {
            return kInvalidSocket;
        }
        return networkError("accept", errno);
    }
    prepare(s);
    setNoDelay(s);
    return handle(s);
}

Result<SocketHandle> startConnect(const SocketAddress &address) {
    const int s = ::socket(address.family, SOCK_STREAM, 0);
    if (s < 0) {
        return networkError("socket", errno);
    }
    prepare(s);
    setNoDelay(s);
    if (::connect(s, asSockaddr(address), static_cast<socklen_t>(address.length)) != 0 && errno != EINPROGRESS) {
        const int error = errno;
        ::close(s);
        return networkError("connect", error).with("address", describeAddress(address));
    }
    return handle(s);
}

Result<void> finishConnect(SocketHandle socket) {
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(native(socket), SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return networkError("connect", errno);
    }
    if (error != 0) {
        return networkError("connect", error);
    }
    return success();
}

IoResult receiveSome(SocketHandle socket, Span<std::byte> buffer) noexcept {
    const ssize_t n = ::recv(native(socket), buffer.data(), buffer.size(), 0);
    if (n > 0) {
        return {IoResult::Status::Ok, static_cast<std::size_t>(n), 0};
    }
    if (n == 0) {
        return {IoResult::Status::Closed, 0, 0};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return {IoResult::Status::WouldBlock, 0, 0};
    }
    return {IoResult::Status::Failed, 0, errno};
}

IoResult sendSome(SocketHandle socket, Span<const std::byte> data) noexcept {
    const ssize_t n = ::send(native(socket), data.data(), data.size(), kSendFlags);
    if (n >= 0) {
        return {IoResult::Status::Ok, static_cast<std::size_t>(n), 0};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return {IoResult::Status::WouldBlock, 0, 0};
    }
    return {IoResult::Status::Failed, 0, errno};
}

void shutdownSend(SocketHandle socket) noexcept { ::shutdown(native(socket), SHUT_WR); }

Result<int> pollSockets(std::vector<PollEntry> &entries, int timeoutMs, std::vector<std::byte> &scratch) {
    scratch.resize(entries.size() * sizeof(pollfd));
    const Span<pollfd> fds(reinterpret_cast<pollfd *>(scratch.data()), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        fds[i].fd = native(entries[i].socket);
        fds[i].events = static_cast<short>((entries[i].wantRead ? POLLIN : 0) | (entries[i].wantWrite ? POLLOUT : 0));
        fds[i].revents = 0;
    }
    int rc = 0;
    do {
        rc = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        return networkError("poll", errno);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const short r = fds[i].revents;
        entries[i].failed = (r & (POLLERR | POLLNVAL)) != 0;
        entries[i].readable = (r & (POLLIN | POLLHUP)) != 0;
        entries[i].writable = (r & POLLOUT) != 0;
    }
    return rc;
}

Result<WakePair> createWakePair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return networkError("create wake pair", errno);
    }
    prepare(fds[0]);
    prepare(fds[1]);
    return WakePair{handle(fds[0]), handle(fds[1])};
}

void signalWake(SocketHandle writeEnd) noexcept {
    const char byte = 1;
    (void)::send(native(writeEnd), &byte, 1, kSendFlags);
}

void drainWake(SocketHandle readEnd) noexcept {
    char buffer[256];
    while (::recv(native(readEnd), buffer, sizeof buffer, 0) > 0) {
    }
}

} // namespace cfw::detail
