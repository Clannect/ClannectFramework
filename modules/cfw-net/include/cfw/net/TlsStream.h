#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "cfw/core/Clock.h"
#include "cfw/core/Result.h"
#include "cfw/core/Sha256.h"
#include "cfw/core/String.h"
#include "cfw/net/ByteStream.h"
#include "cfw/net/TcpConnection.h"

namespace cfw {

class EventLoop;
class Executor;

namespace detail {
struct TlsState;
struct TlsIdentityState;
} // namespace detail

struct TlsOptions {
    // Pinning: when non-empty, the server's own (leaf) certificate must have
    // one of these SHA-256 fingerprints, and the system trust store is not
    // consulted. For Clannect's own servers and for tests. When empty, the
    // certificate must chain to a root the operating system trusts and be
    // valid for the server name.
    std::vector<Sha256::Digest> pinnedCertificates;
    Duration handshakeTimeout = std::chrono::seconds(15);
};

// A certificate and private key for the server side of TLS.
//
// Threads: create on any thread; use from the loop thread.
class TlsServerIdentity {
public:
    // A throwaway self-signed certificate for `dnsName` (plus 127.0.0.1 and
    // ::1), valid for one day, with a fresh RSA-2048 key that is deleted again
    // when this object is destroyed. For tests and local development only:
    // no client trusts it unless it pins fingerprint().
    [[nodiscard]] static Result<std::unique_ptr<TlsServerIdentity>> createSelfSigned(StringView dnsName);

    ~TlsServerIdentity();
    TlsServerIdentity(const TlsServerIdentity &) = delete;
    TlsServerIdentity &operator=(const TlsServerIdentity &) = delete;

    [[nodiscard]] const Sha256::Digest &fingerprint() const noexcept;
    // The certificate in DER form (for exporting it as a trust anchor in tests).
    [[nodiscard]] const std::vector<std::byte> &certificateDer() const noexcept;
    [[nodiscard]] detail::TlsIdentityState &state() noexcept { return *m_state; }

private:
    explicit TlsServerIdentity(std::unique_ptr<detail::TlsIdentityState> state);
    std::unique_ptr<detail::TlsIdentityState> m_state;
};

// TLS 1.2+ over another ByteStream (normally a TcpConnection). Built on the
// operating system's TLS implementation, so CFW ships no crypto: Schannel on
// Windows, and elsewhere the system's OpenSSL (libssl 3, or 1.1 for clients),
// loaded at runtime. See docs/decisions/0013.
//
// Security properties:
//  - TLS 1.2 and 1.3 only, strong cipher suites only, SNI always sent;
//  - the certificate is always verified: by default the chain must lead to a
//    root the system trusts and match the server name (DNS name or IP
//    address); with TlsOptions::pinnedCertificates, the leaf must match a
//    pinned fingerprint instead;
//  - on Windows verification runs on the Executor, because building a chain
//    may fetch intermediates or revocation data over the network. OpenSSL
//    verifies offline, during the handshake. On Linux the trust store is the
//    distribution's (OpenSSL honours SSL_CERT_FILE and SSL_CERT_DIR).
//
// After the handshake it is an ordinary ByteStream: protocols run over it
// unchanged. close() sends a TLS close_notify before closing the transport.
//
// available() is false when the system has no usable OpenSSL; then every
// handshake fails with Unsupported, saying why. Server identities need
// OpenSSL 3 on Linux.
//
// Threads: the loop thread only.
class TlsStream final : public ByteStream {
public:
    using HandshakeCallback = std::function<void(Result<std::unique_ptr<ByteStream>>)>;

    // Client side. `serverName` is the host the user asked for (SNI and name
    // verification), not a resolved address.
    [[nodiscard]] static ConnectRequest startClient(EventLoop &loop, Executor &verifier,
                                                    std::unique_ptr<ByteStream> transport, StringView serverName,
                                                    TlsOptions options, HandshakeCallback callback);
    // Server side, with `identity` (which must outlive the stream).
    [[nodiscard]] static ConnectRequest startServer(EventLoop &loop, std::unique_ptr<ByteStream> transport,
                                                    TlsServerIdentity &identity, Duration handshakeTimeout,
                                                    HandshakeCallback callback);

    // True if this platform has a TLS backend.
    [[nodiscard]] static bool available() noexcept;

    ~TlsStream() override;
    TlsStream(const TlsStream &) = delete;
    TlsStream &operator=(const TlsStream &) = delete;

    void setOnData(std::function<void(Span<const std::byte>)> callback) override;
    void setOnClosed(std::function<void(const std::optional<Error> &)> callback) override;
    void setOnDrained(std::function<void()> callback) override;
    using ByteStream::send;
    void send(Span<const std::byte> data) override;
    [[nodiscard]] std::size_t queuedBytes() const noexcept override;
    void setReadPaused(bool paused) override;
    void close() override;
    void abort() override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] const String &peerAddress() const noexcept override;

    // The negotiated protocol, e.g. "TLS 1.3" (empty before the handshake).
    [[nodiscard]] String protocolName() const;

    // Internal.
    explicit TlsStream(std::unique_ptr<detail::TlsState> state);

private:
    std::unique_ptr<detail::TlsState> m_state;
};

} // namespace cfw
