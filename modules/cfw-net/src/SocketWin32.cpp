// Winsock implementation of Socket.h. The only cfw-net file that includes
// <winsock2.h>.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <system_error>

#include "Socket.h"

namespace cfw::detail {

namespace {

SOCKET native(SocketHandle s) noexcept { return static_cast<SOCKET>(s); }
SocketHandle handle(SOCKET s) noexcept { return s == INVALID_SOCKET ? kInvalidSocket : static_cast<SocketHandle>(s); }

int lastError() noexcept { return WSAGetLastError(); }

bool setNonBlocking(SOCKET s) noexcept {
    u_long on = 1;
    // FIONBIO is an unsigned expression in the Winsock headers; ioctlsocket takes a long.
    return ioctlsocket(s, static_cast<long>(FIONBIO), &on) == 0;
}

void setNoDelay(SOCKET s) noexcept {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof on);
}

SocketAddress fromSockaddr(const sockaddr *address, int length) {
    SocketAddress out;
    out.length = std::min<int>(length, static_cast<int>(out.storage.size()));
    out.family = address->sa_family;
    std::memcpy(out.storage.data(), address, static_cast<std::size_t>(out.length));
    return out;
}

const sockaddr *asSockaddr(const SocketAddress &a) noexcept { return reinterpret_cast<const sockaddr *>(a.storage.data()); }

} // namespace

Error networkError(StringView action, int osError) {
    ErrorCode code = ErrorCode::NetworkError;
    switch (osError) {
    case WSAETIMEDOUT: code = ErrorCode::Timeout; break;
    case WSAEACCES: code = ErrorCode::PermissionDenied; break;
    case WSAEADDRINUSE: code = ErrorCode::AlreadyExists; break;
    case WSAHOST_NOT_FOUND:
    case WSANO_DATA: code = ErrorCode::NotFound; break;
    case WSA_NOT_ENOUGH_MEMORY:
    case WSAENOBUFS: code = ErrorCode::OutOfMemory; break;
    default: break;
    }
    String message(action);
    message += " failed: ";
    message += std::system_category().message(osError);
    return Error(code, std::move(message)).with("os-error", std::to_string(osError));
}

Result<NetLibrary> NetLibrary::acquire() {
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        return networkError("WSAStartup", rc);
    }
    NetLibrary library;
    library.m_held = true;
    return library;
}

NetLibrary::~NetLibrary() {
    if (m_held) {
        WSACleanup();
    }
}

String describeAddress(const SocketAddress &address) {
    char text[INET6_ADDRSTRLEN] = {};
    std::uint16_t port = 0;
    if (address.family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(address.storage.data());
        inet_ntop(AF_INET, &in->sin_addr, text, sizeof text);
        port = ntohs(in->sin_port);
        return String(text) + ":" + std::to_string(port);
    }
    if (address.family == AF_INET6) {
        const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(address.storage.data());
        inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof text);
        port = ntohs(in6->sin6_port);
        return "[" + String(text) + "]:" + std::to_string(port);
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
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *list = nullptr;
    const String hostText(host);
    const String portText = std::to_string(port);
    const int rc = getaddrinfo(hostText.c_str(), portText.c_str(), &hints, &list);
    if (rc != 0) {
        return networkError("resolve", rc).with("host", hostText);
    }
    std::vector<SocketAddress> out;
    for (const addrinfo *entry = list; entry != nullptr; entry = entry->ai_next) {
        out.push_back(fromSockaddr(entry->ai_addr, static_cast<int>(entry->ai_addrlen)));
    }
    freeaddrinfo(list);
    if (out.empty()) {
        return Error(ErrorCode::NotFound, "host has no addresses").with("host", hostText);
    }
    return out;
}

void closeSocket(SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        closesocket(native(socket));
    }
}

Result<SocketHandle> listenTcp(const SocketAddress &address, int backlog) {
    SOCKET s = ::socket(address.family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return networkError("socket", lastError());
    }
    // Exclusive: no other process may bind the same port and steal traffic.
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&on), sizeof on);
    if (!setNonBlocking(s) || ::bind(s, asSockaddr(address), address.length) != 0 || ::listen(s, backlog) != 0) {
        const int error = lastError();
        closesocket(s);
        return networkError("listen", error).with("address", describeAddress(address));
    }
    return handle(s);
}

std::uint16_t localPort(SocketHandle socket) noexcept {
    sockaddr_storage storage{};
    int length = sizeof storage;
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
    int length = sizeof storage;
    if (getpeername(native(socket), reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return "?";
    }
    return describeAddress(fromSockaddr(reinterpret_cast<const sockaddr *>(&storage), length));
}

Result<SocketHandle> acceptTcp(SocketHandle listener) {
    SOCKET s = ::accept(native(listener), nullptr, nullptr);
    if (s == INVALID_SOCKET) {
        const int error = lastError();
        if (error == WSAEWOULDBLOCK || error == WSAECONNRESET) {
            return kInvalidSocket; // nothing pending (or the client already gave up)
        }
        return networkError("accept", error);
    }
    setNonBlocking(s);
    setNoDelay(s);
    return handle(s);
}

Result<SocketHandle> startConnect(const SocketAddress &address) {
    SOCKET s = ::socket(address.family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return networkError("socket", lastError());
    }
    setNonBlocking(s);
    setNoDelay(s);
    if (::connect(s, asSockaddr(address), address.length) != 0) {
        const int error = lastError();
        if (error != WSAEWOULDBLOCK) {
            closesocket(s);
            return networkError("connect", error).with("address", describeAddress(address));
        }
    }
    return handle(s);
}

Result<void> finishConnect(SocketHandle socket) {
    int error = 0;
    int length = sizeof error;
    if (getsockopt(native(socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) != 0) {
        return networkError("connect", lastError());
    }
    if (error != 0) {
        return networkError("connect", error);
    }
    return success();
}

IoResult receiveSome(SocketHandle socket, Span<std::byte> buffer) noexcept {
    const int size = static_cast<int>(std::min<std::size_t>(buffer.size(), INT_MAX));
    const int n = ::recv(native(socket), reinterpret_cast<char *>(buffer.data()), size, 0);
    if (n > 0) {
        return {IoResult::Status::Ok, static_cast<std::size_t>(n), 0};
    }
    if (n == 0) {
        return {IoResult::Status::Closed, 0, 0};
    }
    const int error = lastError();
    if (error == WSAEWOULDBLOCK) {
        return {IoResult::Status::WouldBlock, 0, 0};
    }
    return {IoResult::Status::Failed, 0, error};
}

IoResult sendSome(SocketHandle socket, Span<const std::byte> data) noexcept {
    const int size = static_cast<int>(std::min<std::size_t>(data.size(), INT_MAX));
    const int n = ::send(native(socket), reinterpret_cast<const char *>(data.data()), size, 0);
    if (n >= 0) {
        return {IoResult::Status::Ok, static_cast<std::size_t>(n), 0};
    }
    const int error = lastError();
    if (error == WSAEWOULDBLOCK) {
        return {IoResult::Status::WouldBlock, 0, 0};
    }
    return {IoResult::Status::Failed, 0, error};
}

void shutdownSend(SocketHandle socket) noexcept { ::shutdown(native(socket), SD_SEND); }

Result<int> pollSockets(std::vector<PollEntry> &entries, int timeoutMs, std::vector<std::byte> &scratch) {
    // WSAPOLLFD is an implicit-lifetime type: the vector's storage holds them.
    scratch.resize(entries.size() * sizeof(WSAPOLLFD));
    const Span<WSAPOLLFD> fds(reinterpret_cast<WSAPOLLFD *>(scratch.data()), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        fds[i].fd = native(entries[i].socket);
        fds[i].events = static_cast<SHORT>((entries[i].wantRead ? POLLRDNORM : 0) | (entries[i].wantWrite ? POLLWRNORM : 0));
        fds[i].revents = 0;
    }
    const int rc = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMs);
    if (rc == SOCKET_ERROR) {
        return networkError("poll", lastError());
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const SHORT r = fds[i].revents;
        entries[i].failed = (r & (POLLERR | POLLNVAL)) != 0;
        // A hang-up still has to be read (recv returns 0 or the error).
        entries[i].readable = (r & (POLLRDNORM | POLLHUP)) != 0;
        entries[i].writable = (r & POLLWRNORM) != 0;
    }
    return rc;
}

Result<WakePair> createWakePair() {
    // Winsock has no socketpair(): connect two loopback sockets through a
    // temporary listener.
    Result<SocketAddress> loopback = numericAddress("127.0.0.1", 0);
    Result<SocketHandle> listener = listenTcp(loopback.value(), 1);
    if (!listener) {
        return std::move(listener).error();
    }
    const std::uint16_t port = localPort(listener.value());
    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (client == INVALID_SOCKET || ::connect(client, reinterpret_cast<const sockaddr *>(&to), sizeof to) != 0) {
        // The listener is non-blocking, but a loopback connect completes or
        // fails immediately; WOULDBLOCK here means we must wait for it.
        if (client == INVALID_SOCKET || lastError() != WSAEWOULDBLOCK) {
            const int error = lastError();
            closeSocket(listener.value());
            if (client != INVALID_SOCKET) {
                closesocket(client);
            }
            return networkError("create wake pair", error);
        }
    }
    SOCKET server = INVALID_SOCKET;
    for (int attempt = 0; attempt < 1000 && server == INVALID_SOCKET; ++attempt) {
        server = ::accept(native(listener.value()), nullptr, nullptr);
        if (server == INVALID_SOCKET) {
            Sleep(1);
        }
    }
    closeSocket(listener.value());
    if (server == INVALID_SOCKET) {
        closesocket(client);
        return networkError("create wake pair", lastError());
    }
    setNonBlocking(client);
    setNonBlocking(server);
    setNoDelay(client);
    return WakePair{handle(server), handle(client)};
}

void signalWake(SocketHandle writeEnd) noexcept {
    const char byte = 1;
    ::send(native(writeEnd), &byte, 1, 0); // a full buffer means a wake is already pending
}

void drainWake(SocketHandle readEnd) noexcept {
    char buffer[256];
    while (::recv(native(readEnd), buffer, sizeof buffer, 0) > 0) {
    }
}

} // namespace cfw::detail
