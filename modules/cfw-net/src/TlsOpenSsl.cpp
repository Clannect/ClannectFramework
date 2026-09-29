// TLS on Linux and other POSIX systems via the system's OpenSSL (libssl 3,
// or 1.1 for the client side), loaded at runtime with dlopen. CFW ships no
// crypto: like Schannel on Windows, the TLS implementation is the one the
// operating system provides and patches. See docs/decisions/0013.
//
// No OpenSSL headers are needed to build: the few functions used are
// declared here with their documented C signatures, and constants that are
// part of OpenSSL's stable ABI are spelled out.

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TlsStream.h"

namespace cfw {

namespace {

// --- The OpenSSL functions CFW uses --------------------------------------------

constexpr int kSslErrorSsl = 1;
constexpr int kSslErrorWantRead = 2;
constexpr int kSslErrorWantWrite = 3;
constexpr int kSslErrorZeroReturn = 6;
constexpr int kSslVerifyNone = 0;
constexpr int kSslVerifyPeer = 1;
constexpr long kTls12Version = 0x0303;
constexpr int kCtrlSetMinProtoVersion = 123;
constexpr int kCtrlSetTlsextHostname = 55;
constexpr long kTlsextNametypeHostName = 0;
constexpr int kBioCtrlPending = 10;
constexpr long kX509VOk = 0;
constexpr int kMbstringAsc = 0x1001;
constexpr int kNidSubjectAltName = 85;
constexpr int kNidExtKeyUsage = 126;

struct OpenSsl {
    // libssl
    void *(*TLS_client_method)() = nullptr;
    void *(*TLS_server_method)() = nullptr;
    void *(*SSL_CTX_new)(const void *) = nullptr;
    void (*SSL_CTX_free)(void *) = nullptr;
    long (*SSL_CTX_ctrl)(void *, int, long, void *) = nullptr;
    int (*SSL_CTX_set_default_verify_paths)(void *) = nullptr;
    void (*SSL_CTX_set_verify)(void *, int, void *) = nullptr;
    int (*SSL_CTX_use_certificate)(void *, void *) = nullptr;
    int (*SSL_CTX_use_PrivateKey)(void *, void *) = nullptr;
    void *(*SSL_new)(void *) = nullptr;
    void (*SSL_free)(void *) = nullptr;
    long (*SSL_ctrl)(void *, int, long, void *) = nullptr;
    void (*SSL_set_bio)(void *, void *, void *) = nullptr;
    void (*SSL_set_connect_state)(void *) = nullptr;
    void (*SSL_set_accept_state)(void *) = nullptr;
    int (*SSL_do_handshake)(void *) = nullptr;
    int (*SSL_read)(void *, void *, int) = nullptr;
    int (*SSL_write)(void *, const void *, int) = nullptr;
    int (*SSL_shutdown)(void *) = nullptr;
    int (*SSL_get_error)(const void *, int) = nullptr;
    long (*SSL_get_verify_result)(const void *) = nullptr;
    int (*SSL_set1_host)(void *, const char *) = nullptr;
    void *(*SSL_get0_param)(void *) = nullptr;
    void *(*SSL_get_peer_certificate)(const void *) = nullptr; // SSL_get1_peer_certificate in 3.x
    const char *(*SSL_get_version)(const void *) = nullptr;
    // libcrypto
    void *(*BIO_s_mem)() = nullptr;
    void *(*BIO_new)(const void *) = nullptr;
    int (*BIO_read)(void *, void *, int) = nullptr;
    int (*BIO_write)(void *, const void *, int) = nullptr;
    long (*BIO_ctrl)(void *, int, long, void *) = nullptr;
    unsigned long (*ERR_get_error)() = nullptr;
    void (*ERR_clear_error)() = nullptr;
    void (*ERR_error_string_n)(unsigned long, char *, std::size_t) = nullptr;
    int (*i2d_X509)(const void *, unsigned char **) = nullptr;
    void (*X509_free)(void *) = nullptr;
    const char *(*X509_verify_cert_error_string)(long) = nullptr;
    int (*X509_VERIFY_PARAM_set1_ip_asc)(void *, const char *) = nullptr;
    void (*CRYPTO_free)(void *, const char *, int) = nullptr;
    // libcrypto, server identity (OpenSSL 3 only)
    void *(*EVP_PKEY_Q_keygen)(void *, const char *, const char *, ...) = nullptr;
    void (*EVP_PKEY_free)(void *) = nullptr;
    void *(*X509_new)() = nullptr;
    int (*X509_set_version)(void *, long) = nullptr;
    void *(*X509_get_serialNumber)(void *) = nullptr;
    int (*ASN1_INTEGER_set)(void *, long) = nullptr;
    void *(*X509_getm_notBefore)(const void *) = nullptr;
    void *(*X509_getm_notAfter)(const void *) = nullptr;
    void *(*X509_gmtime_adj)(void *, long) = nullptr;
    int (*X509_set_pubkey)(void *, void *) = nullptr;
    void *(*X509_get_subject_name)(const void *) = nullptr;
    int (*X509_NAME_add_entry_by_txt)(void *, const char *, int, const unsigned char *, int, int, int) = nullptr;
    int (*X509_set_issuer_name)(void *, const void *) = nullptr;
    void *(*X509V3_EXT_conf_nid)(void *, void *, int, const char *) = nullptr;
    int (*X509_add_ext)(void *, void *, int) = nullptr;
    void (*X509_EXTENSION_free)(void *) = nullptr;
    const void *(*EVP_sha256)() = nullptr;
    int (*X509_sign)(void *, void *, const void *) = nullptr;

    bool client = false; // everything a client needs was found
    bool identity = false; // and certificate creation (OpenSSL 3)
    String error;
};

template <class F>
bool bind(void *library, const char *name, F &target) {
    target = reinterpret_cast<F>(dlsym(library, name));
    return target != nullptr;
}

OpenSsl load() {
    OpenSsl s;
    // The versioned names first: the unversioned symlink only exists where
    // development packages are installed.
    void *ssl = nullptr;
    void *crypto = nullptr;
    // macOS has no system OpenSSL: Homebrew's (Apple silicon, then Intel)
    // or MacPorts', when installed.
    for (const auto &[sslName, cryptoName] :
         {std::pair{"libssl.so.3", "libcrypto.so.3"}, std::pair{"libssl.so.1.1", "libcrypto.so.1.1"},
          std::pair{"libssl.so", "libcrypto.so"},
          std::pair{"/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib", "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib"},
          std::pair{"/usr/local/opt/openssl@3/lib/libssl.3.dylib", "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib"},
          std::pair{"/opt/local/lib/libssl.3.dylib", "/opt/local/lib/libcrypto.3.dylib"}}) {
        ssl = dlopen(sslName, RTLD_NOW | RTLD_LOCAL);
        crypto = ssl != nullptr ? dlopen(cryptoName, RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (ssl != nullptr && crypto != nullptr) {
            break;
        }
        if (ssl != nullptr) {
            dlclose(ssl);
            ssl = nullptr;
        }
    }
    if (ssl == nullptr) {
        s.error = "no system OpenSSL (libssl.so.3 or libssl.so.1.1) was found";
        return s;
    }
    // Never dlclose'd: OpenSSL registers exit handlers, and unloading it while
    // they are pending crashes at exit.
    bool ok = bind(ssl, "TLS_client_method", s.TLS_client_method) && bind(ssl, "TLS_server_method", s.TLS_server_method) &&
              bind(ssl, "SSL_CTX_new", s.SSL_CTX_new) && bind(ssl, "SSL_CTX_free", s.SSL_CTX_free) &&
              bind(ssl, "SSL_CTX_ctrl", s.SSL_CTX_ctrl) &&
              bind(ssl, "SSL_CTX_set_default_verify_paths", s.SSL_CTX_set_default_verify_paths) &&
              bind(ssl, "SSL_CTX_set_verify", s.SSL_CTX_set_verify) &&
              bind(ssl, "SSL_CTX_use_certificate", s.SSL_CTX_use_certificate) &&
              bind(ssl, "SSL_CTX_use_PrivateKey", s.SSL_CTX_use_PrivateKey) && bind(ssl, "SSL_new", s.SSL_new) &&
              bind(ssl, "SSL_free", s.SSL_free) && bind(ssl, "SSL_ctrl", s.SSL_ctrl) &&
              bind(ssl, "SSL_set_bio", s.SSL_set_bio) && bind(ssl, "SSL_set_connect_state", s.SSL_set_connect_state) &&
              bind(ssl, "SSL_set_accept_state", s.SSL_set_accept_state) &&
              bind(ssl, "SSL_do_handshake", s.SSL_do_handshake) && bind(ssl, "SSL_read", s.SSL_read) &&
              bind(ssl, "SSL_write", s.SSL_write) && bind(ssl, "SSL_shutdown", s.SSL_shutdown) &&
              bind(ssl, "SSL_get_error", s.SSL_get_error) && bind(ssl, "SSL_get_verify_result", s.SSL_get_verify_result) &&
              bind(ssl, "SSL_set1_host", s.SSL_set1_host) && bind(ssl, "SSL_get0_param", s.SSL_get0_param) &&
              bind(ssl, "SSL_get_version", s.SSL_get_version) &&
              (bind(ssl, "SSL_get1_peer_certificate", s.SSL_get_peer_certificate) ||
               bind(ssl, "SSL_get_peer_certificate", s.SSL_get_peer_certificate)) &&
              bind(crypto, "BIO_s_mem", s.BIO_s_mem) && bind(crypto, "BIO_new", s.BIO_new) &&
              bind(crypto, "BIO_read", s.BIO_read) && bind(crypto, "BIO_write", s.BIO_write) &&
              bind(crypto, "BIO_ctrl", s.BIO_ctrl) && bind(crypto, "ERR_get_error", s.ERR_get_error) &&
              bind(crypto, "ERR_clear_error", s.ERR_clear_error) &&
              bind(crypto, "ERR_error_string_n", s.ERR_error_string_n) && bind(crypto, "i2d_X509", s.i2d_X509) &&
              bind(crypto, "X509_free", s.X509_free) &&
              bind(crypto, "X509_verify_cert_error_string", s.X509_verify_cert_error_string) &&
              bind(crypto, "X509_VERIFY_PARAM_set1_ip_asc", s.X509_VERIFY_PARAM_set1_ip_asc) &&
              bind(crypto, "CRYPTO_free", s.CRYPTO_free);
    if (!ok) {
        s.error = "the system OpenSSL lacks functions CFW needs (OpenSSL 1.1 or 3 is required)";
        return s;
    }
    s.client = true;
    s.identity = bind(crypto, "EVP_PKEY_Q_keygen", s.EVP_PKEY_Q_keygen) && bind(crypto, "EVP_PKEY_free", s.EVP_PKEY_free) &&
                 bind(crypto, "X509_new", s.X509_new) && bind(crypto, "X509_set_version", s.X509_set_version) &&
                 bind(crypto, "X509_get_serialNumber", s.X509_get_serialNumber) &&
                 bind(crypto, "ASN1_INTEGER_set", s.ASN1_INTEGER_set) &&
                 bind(crypto, "X509_getm_notBefore", s.X509_getm_notBefore) &&
                 bind(crypto, "X509_getm_notAfter", s.X509_getm_notAfter) &&
                 bind(crypto, "X509_gmtime_adj", s.X509_gmtime_adj) && bind(crypto, "X509_set_pubkey", s.X509_set_pubkey) &&
                 bind(crypto, "X509_get_subject_name", s.X509_get_subject_name) &&
                 bind(crypto, "X509_NAME_add_entry_by_txt", s.X509_NAME_add_entry_by_txt) &&
                 bind(crypto, "X509_set_issuer_name", s.X509_set_issuer_name) &&
                 bind(crypto, "X509V3_EXT_conf_nid", s.X509V3_EXT_conf_nid) && bind(crypto, "X509_add_ext", s.X509_add_ext) &&
                 bind(crypto, "X509_EXTENSION_free", s.X509_EXTENSION_free) && bind(crypto, "EVP_sha256", s.EVP_sha256) &&
                 bind(crypto, "X509_sign", s.X509_sign);
    return s;
}

// Loaded once, on first use, and immutable afterwards (thread-safe static
// initialisation). The one piece of process-wide state in cfw-net, because
// the library itself is process-wide; see docs/decisions/0013.
const OpenSsl &openSsl() {
    static const OpenSsl instance = load();
    return instance;
}

Error unavailable() { return Error(ErrorCode::Unsupported, "TLS: " + openSsl().error); }

// The most recent OpenSSL error, and clears the queue.
Error sslError(StringView what) {
    const OpenSsl &s = openSsl();
    String message(what);
    unsigned long code = 0;
    unsigned long last = 0;
    while ((code = s.ERR_get_error()) != 0) {
        last = code;
    }
    if (last != 0) {
        char text[256];
        s.ERR_error_string_n(last, text, sizeof text);
        message += ": ";
        message += text;
    }
    return Error(ErrorCode::NetworkError, std::move(message));
}

std::vector<std::byte> derOf(void *x509) {
    const OpenSsl &s = openSsl();
    unsigned char *out = nullptr;
    const int n = s.i2d_X509(x509, &out);
    if (n <= 0 || out == nullptr) {
        return {};
    }
    std::vector<std::byte> der(reinterpret_cast<std::byte *>(out), reinterpret_cast<std::byte *>(out) + n);
    s.CRYPTO_free(out, __FILE__, __LINE__);
    return der;
}

bool isIpLiteral(StringView host) {
    return !host.empty() && (host.find(':') != StringView::npos ||
                             host.find_first_not_of("0123456789.") == StringView::npos);
}

} // namespace

// --- Server identity ---------------------------------------------------------

namespace detail {

struct TlsIdentityState {
    void *key = nullptr;
    void *certificate = nullptr;
    std::vector<std::byte> der;
    Sha256::Digest fingerprint{};

    ~TlsIdentityState() {
        if (certificate != nullptr) {
            openSsl().X509_free(certificate);
        }
        if (key != nullptr) {
            openSsl().EVP_PKEY_free(key);
        }
    }
};

} // namespace detail

TlsServerIdentity::TlsServerIdentity(std::unique_ptr<detail::TlsIdentityState> state) : m_state(std::move(state)) {}
TlsServerIdentity::~TlsServerIdentity() = default;
const Sha256::Digest &TlsServerIdentity::fingerprint() const noexcept { return m_state->fingerprint; }
const std::vector<std::byte> &TlsServerIdentity::certificateDer() const noexcept { return m_state->der; }

Result<std::unique_ptr<TlsServerIdentity>> TlsServerIdentity::createSelfSigned(StringView dnsName) {
    const OpenSsl &s = openSsl();
    if (!s.client) {
        return unavailable();
    }
    if (!s.identity) {
        return Error(ErrorCode::Unsupported, "TLS: creating certificates needs OpenSSL 3");
    }
    s.ERR_clear_error();
    auto state = std::make_unique<detail::TlsIdentityState>();
    state->key = s.EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    state->certificate = s.X509_new();
    if (state->key == nullptr || state->certificate == nullptr) {
        return sslError("creating the key");
    }
    void *x = state->certificate;
    std::random_device entropy;
    const String cn(dnsName);
    bool ok = s.X509_set_version(x, 2) == 1 &&
              s.ASN1_INTEGER_set(s.X509_get_serialNumber(x), static_cast<long>(entropy() & 0x7FFFFFFFu)) == 1 &&
              s.X509_gmtime_adj(s.X509_getm_notBefore(x), -24L * 3600) != nullptr &&
              s.X509_gmtime_adj(s.X509_getm_notAfter(x), 24L * 3600) != nullptr && s.X509_set_pubkey(x, state->key) == 1;
    void *name = ok ? s.X509_get_subject_name(x) : nullptr;
    ok = ok && name != nullptr &&
         s.X509_NAME_add_entry_by_txt(name, "CN", kMbstringAsc, reinterpret_cast<const unsigned char *>(cn.c_str()), -1,
                                      -1, 0) == 1 &&
         s.X509_set_issuer_name(x, name) == 1;
    const String san = "DNS:" + cn + ",IP:127.0.0.1,IP:::1";
    for (const auto &[nid, value] : {std::pair{kNidSubjectAltName, san}, std::pair{kNidExtKeyUsage, String("serverAuth")}}) {
        void *ext = ok ? s.X509V3_EXT_conf_nid(nullptr, nullptr, nid, value.c_str()) : nullptr;
        ok = ok && ext != nullptr && s.X509_add_ext(x, ext, -1) == 1;
        if (ext != nullptr) {
            s.X509_EXTENSION_free(ext);
        }
    }
    ok = ok && s.X509_sign(x, state->key, s.EVP_sha256()) > 0;
    if (!ok) {
        return sslError("creating the self-signed certificate");
    }
    state->der = derOf(x);
    state->fingerprint = Sha256::hash(state->der);
    return std::unique_ptr<TlsServerIdentity>(new TlsServerIdentity(std::move(state)));
}

// --- The stream --------------------------------------------------------------

namespace detail {

struct TlsState {
    enum class Phase : std::uint8_t { Handshake, Open, Closed };

    EventLoop *loop = nullptr;
    std::unique_ptr<ByteStream> transport;
    bool server = false;
    String serverName;
    TlsOptions options;
    String peer;

    void *ctx = nullptr;
    void *ssl = nullptr;
    void *networkIn = nullptr;  // ciphertext from the peer (owned by ssl)
    void *networkOut = nullptr; // ciphertext for the peer (owned by ssl)

    Phase phase = Phase::Handshake;
    std::array<std::byte, 16 * 1024> buffer{};
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::function<void(Span<const std::byte>)> onData;
    std::function<void(const std::optional<Error> &)> onClosed;
    std::function<void(Result<void>)> handshakeDone;

    ~TlsState() {
        *alive = false;
        if (ssl != nullptr) {
            openSsl().SSL_free(ssl); // frees both BIOs
        }
        if (ctx != nullptr) {
            openSsl().SSL_CTX_free(ctx);
        }
    }

    // Sends everything OpenSSL has written to the outgoing BIO.
    void flushOut() {
        const OpenSsl &s = openSsl();
        while (s.BIO_ctrl(networkOut, kBioCtrlPending, 0, nullptr) > 0) {
            const int n = s.BIO_read(networkOut, buffer.data(), static_cast<int>(buffer.size()));
            if (n <= 0) {
                break;
            }
            transport->send(Span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
        }
    }

    void reportHandshake(Result<void> result) {
        if (auto done = std::move(handshakeDone)) {
            handshakeDone = nullptr;
            done(std::move(result));
        }
    }

    void handshakeStep() {
        const OpenSsl &s = openSsl();
        s.ERR_clear_error();
        const int r = s.SSL_do_handshake(ssl);
        flushOut(); // the next flight, or an alert
        if (r == 1) {
            handshakeComplete();
            return;
        }
        const int error = s.SSL_get_error(ssl, r);
        if (error == kSslErrorWantRead || error == kSslErrorWantWrite) {
            return;
        }
        if (!server && error == kSslErrorSsl) {
            const long verify = s.SSL_get_verify_result(ssl);
            if (verify != kX509VOk) {
                s.ERR_clear_error();
                fail(Error(ErrorCode::PermissionDenied, "the server certificate was rejected")
                         .with("reason", s.X509_verify_cert_error_string(verify)));
                return;
            }
        }
        fail(sslError(server ? "TLS handshake (server)" : "TLS handshake"));
    }

    void handshakeComplete() {
        const OpenSsl &s = openSsl();
        if (!server && !options.pinnedCertificates.empty()) {
            void *remote = s.SSL_get_peer_certificate(ssl);
            if (remote == nullptr) {
                fail(Error(ErrorCode::PermissionDenied, "the server presented no certificate"));
                return;
            }
            const Sha256::Digest fingerprint = Sha256::hash(derOf(remote));
            s.X509_free(remote);
            if (std::find(options.pinnedCertificates.begin(), options.pinnedCertificates.end(), fingerprint) ==
                options.pinnedCertificates.end()) {
                fail(Error(ErrorCode::PermissionDenied, "the server certificate does not match the pinned fingerprint")
                         .with("fingerprint", Sha256::toHex(fingerprint)));
                return;
            }
        }
        phase = Phase::Open;
        reportHandshake(success());
    }

    void onTransportData(Span<const std::byte> data) {
        const OpenSsl &s = openSsl();
        // A memory BIO grows as needed, so a write always takes everything.
        while (!data.empty()) {
            const int chunk = static_cast<int>(std::min<std::size_t>(data.size(), 1u << 30));
            const int n = s.BIO_write(networkIn, data.data(), chunk);
            if (n <= 0) {
                fail(sslError("buffering TLS data"));
                return;
            }
            data = data.subspan(static_cast<std::size_t>(n));
        }
        // A failed handshake reports to the owner, which may destroy this
        // stream (found by ASan): check before touching state again.
        const std::shared_ptr<bool> guard = alive;
        if (phase == Phase::Handshake) {
            handshakeStep();
            if (!*guard) {
                return;
            }
        }
        if (phase == Phase::Open) {
            readAvailable();
        }
    }

    void readAvailable() {
        const OpenSsl &s = openSsl();
        const std::shared_ptr<bool> guard = alive;
        while (phase == Phase::Open) {
            s.ERR_clear_error();
            const int n = s.SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
            if (n > 0) {
                if (onData) {
                    const auto callback = onData;
                    callback(Span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)));
                    if (!*guard) {
                        return;
                    }
                }
                continue;
            }
            const int error = s.SSL_get_error(ssl, n);
            flushOut(); // TLS 1.3 post-handshake messages may need answers
            if (error == kSslErrorWantRead || error == kSslErrorWantWrite) {
                return;
            }
            if (error == kSslErrorZeroReturn) {
                // The peer sent close_notify: answer it and end cleanly.
                s.SSL_shutdown(ssl);
                flushOut();
                phase = Phase::Closed;
                transport->close();
                return;
            }
            fail(sslError("TLS read"));
            return;
        }
    }

    void encryptAndSend(Span<const std::byte> data) {
        const OpenSsl &s = openSsl();
        while (!data.empty()) {
            s.ERR_clear_error();
            const int chunk = static_cast<int>(std::min<std::size_t>(data.size(), 1u << 30));
            const int n = s.SSL_write(ssl, data.data(), chunk);
            if (n <= 0) {
                fail(sslError("TLS write"));
                return;
            }
            data = data.subspan(static_cast<std::size_t>(n));
        }
        flushOut();
    }

    void sendCloseNotify() {
        openSsl().SSL_shutdown(ssl);
        flushOut();
    }

    void fail(Error error) {
        if (phase == Phase::Closed) {
            return;
        }
        const bool handshaking = phase != Phase::Open;
        phase = Phase::Closed;
        transport->close(); // flush any alert, then end
        if (handshaking) {
            reportHandshake(std::move(error));
            return;
        }
        if (auto callback = std::move(onClosed)) {
            onClosed = nullptr;
            callback(error);
        }
    }

    void onTransportClosed(const std::optional<Error> &error) {
        if (phase == Phase::Handshake) {
            phase = Phase::Closed;
            reportHandshake(error ? *error : Error(ErrorCode::NetworkError, "connection closed during the TLS handshake"));
            return;
        }
        phase = Phase::Closed;
        if (auto callback = std::move(onClosed)) {
            onClosed = nullptr;
            callback(error);
        }
    }
};

} // namespace detail

namespace {

// The handshake as a cancellable request (see ConnectRequest).
struct TlsHandshake final : detail::Cancellable, std::enable_shared_from_this<TlsHandshake> {
    EventLoop *loop = nullptr;
    std::unique_ptr<TlsStream> stream;
    detail::TlsState *state = nullptr;
    TlsStream::HandshakeCallback callback;
    TimerId timer = 0;
    bool done = false;

    [[nodiscard]] bool isDone() const override { return done; }

    void cancel() override {
        if (done) {
            return;
        }
        done = true;
        callback = nullptr;
        stream.reset();
        if (timer != 0) {
            loop->cancelTimer(timer);
            timer = 0;
        }
    }

    void finish(Result<void> result) {
        if (done) {
            return;
        }
        auto cb = std::move(callback);
        callback = nullptr;
        std::unique_ptr<TlsStream> handed = std::move(stream);
        done = true;
        if (timer != 0) {
            loop->cancelTimer(timer);
            timer = 0;
        }
        if (!result) {
            handed.reset();
            if (cb) {
                cb(std::move(result).error());
            }
            return;
        }
        // Application data that arrived with the last handshake flight is
        // delivered on the next turn, after the owner has set its callbacks.
        detail::TlsState *s = state;
        const std::shared_ptr<bool> alive = s->alive;
        loop->post([s, alive] {
            if (*alive && s->phase == detail::TlsState::Phase::Open) {
                s->readAvailable();
            }
        });
        if (cb) {
            cb(std::unique_ptr<ByteStream>(std::move(handed)));
        }
    }
};

ConnectRequest begin(EventLoop &loop, std::unique_ptr<ByteStream> transport, bool server, StringView serverName,
                     TlsOptions options, TlsServerIdentity *identity, TlsStream::HandshakeCallback callback) {
    auto op = std::make_shared<TlsHandshake>();
    op->loop = &loop;
    op->callback = std::move(callback);
    std::weak_ptr<TlsHandshake> weak = op;
    const auto failSoon = [&](Error error) {
        loop.post([weak, error] {
            if (auto self = weak.lock()) {
                self->finish(error);
            }
        });
        return ConnectRequest(op);
    };
    const OpenSsl &s = openSsl();
    if (!s.client) {
        return failSoon(unavailable());
    }

    auto state = std::make_unique<detail::TlsState>();
    state->loop = &loop;
    state->server = server;
    state->serverName = String(serverName);
    state->options = options;
    state->peer = transport->peerAddress();
    state->transport = std::move(transport);
    detail::TlsState *raw = state.get();
    op->state = raw;
    op->stream = std::make_unique<TlsStream>(std::move(state));

    s.ERR_clear_error();
    raw->ctx = s.SSL_CTX_new(server ? s.TLS_server_method() : s.TLS_client_method());
    if (raw->ctx == nullptr || s.SSL_CTX_ctrl(raw->ctx, kCtrlSetMinProtoVersion, kTls12Version, nullptr) != 1) {
        return failSoon(sslError("creating the TLS context"));
    }
    if (server) {
        if (s.SSL_CTX_use_certificate(raw->ctx, identity->state().certificate) != 1 ||
            s.SSL_CTX_use_PrivateKey(raw->ctx, identity->state().key) != 1) {
            return failSoon(sslError("loading the server certificate"));
        }
    } else if (options.pinnedCertificates.empty()) {
        // The system trust store (SSL_CERT_FILE / SSL_CERT_DIR override it).
        if (s.SSL_CTX_set_default_verify_paths(raw->ctx) != 1) {
            return failSoon(sslError("loading the system trust store"));
        }
        s.SSL_CTX_set_verify(raw->ctx, kSslVerifyPeer, nullptr);
    } else {
        s.SSL_CTX_set_verify(raw->ctx, kSslVerifyNone, nullptr); // checked against the pins afterwards
    }
    raw->ssl = s.SSL_new(raw->ctx);
    raw->networkIn = s.BIO_new(s.BIO_s_mem());
    raw->networkOut = s.BIO_new(s.BIO_s_mem());
    if (raw->ssl == nullptr || raw->networkIn == nullptr || raw->networkOut == nullptr) {
        return failSoon(sslError("creating the TLS session"));
    }
    s.SSL_set_bio(raw->ssl, raw->networkIn, raw->networkOut);
    if (server) {
        s.SSL_set_accept_state(raw->ssl);
    } else {
        s.SSL_set_connect_state(raw->ssl);
        const bool ip = isIpLiteral(serverName);
        if (!ip) {
            s.SSL_ctrl(raw->ssl, kCtrlSetTlsextHostname, kTlsextNametypeHostName,
                       const_cast<char *>(raw->serverName.c_str())); // SNI
        }
        if (options.pinnedCertificates.empty()) {
            String host = raw->serverName;
            if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
                host = host.substr(1, host.size() - 2);
            }
            const int named = ip ? s.X509_VERIFY_PARAM_set1_ip_asc(s.SSL_get0_param(raw->ssl), host.c_str())
                                 : s.SSL_set1_host(raw->ssl, host.c_str());
            if (named != 1) {
                return failSoon(sslError("setting the expected server name"));
            }
        }
    }

    raw->handshakeDone = [weak](Result<void> result) {
        if (auto self = weak.lock()) {
            self->finish(std::move(result));
        }
    };
    const std::shared_ptr<bool> alive = raw->alive;
    raw->transport->setOnData([raw, alive](Span<const std::byte> data) {
        if (*alive) {
            raw->onTransportData(data);
        }
    });
    raw->transport->setOnClosed([raw, alive](const std::optional<Error> &error) {
        if (*alive) {
            raw->onTransportClosed(error);
        }
    });
    op->timer = loop.startTimer(options.handshakeTimeout, [weak] {
        if (auto self = weak.lock()) {
            self->timer = 0;
            self->finish(Error(ErrorCode::Timeout, "TLS handshake timed out"));
        }
    });
    if (!server) {
        raw->handshakeStep(); // sends the ClientHello
    }
    return ConnectRequest(std::move(op));
}

} // namespace

// --- TlsStream ---------------------------------------------------------------

TlsStream::TlsStream(std::unique_ptr<detail::TlsState> state) : m_state(std::move(state)) {}
TlsStream::~TlsStream() = default;

bool TlsStream::available() noexcept { return openSsl().client; }

ConnectRequest TlsStream::startClient(EventLoop &loop, Executor & /*verifier: OpenSSL verifies inline, offline*/,
                                      std::unique_ptr<ByteStream> transport, StringView serverName, TlsOptions options,
                                      HandshakeCallback callback) {
    return begin(loop, std::move(transport), false, serverName, std::move(options), nullptr, std::move(callback));
}

ConnectRequest TlsStream::startServer(EventLoop &loop, std::unique_ptr<ByteStream> transport, TlsServerIdentity &identity,
                                      Duration handshakeTimeout, HandshakeCallback callback) {
    TlsOptions options;
    options.handshakeTimeout = handshakeTimeout;
    return begin(loop, std::move(transport), true, {}, options, &identity, std::move(callback));
}

void TlsStream::setOnData(std::function<void(Span<const std::byte>)> callback) { m_state->onData = std::move(callback); }
void TlsStream::setOnClosed(std::function<void(const std::optional<Error> &)> callback) {
    m_state->onClosed = std::move(callback);
}
void TlsStream::setOnDrained(std::function<void()> callback) { m_state->transport->setOnDrained(std::move(callback)); }

void TlsStream::send(Span<const std::byte> data) {
    if (m_state->phase == detail::TlsState::Phase::Open && !data.empty()) {
        m_state->encryptAndSend(data);
    }
}

std::size_t TlsStream::queuedBytes() const noexcept { return m_state->transport->queuedBytes(); }
void TlsStream::setReadPaused(bool paused) { m_state->transport->setReadPaused(paused); }

void TlsStream::close() {
    if (m_state->phase == detail::TlsState::Phase::Open) {
        m_state->sendCloseNotify();
        m_state->phase = detail::TlsState::Phase::Closed;
        // The transport's closed callback still reports the end to the owner.
        const std::shared_ptr<bool> alive = m_state->alive;
        detail::TlsState *s = m_state.get();
        s->transport->setOnClosed([s, alive](const std::optional<Error> &error) {
            if (*alive) {
                if (auto callback = std::move(s->onClosed)) {
                    s->onClosed = nullptr;
                    callback(error);
                }
            }
        });
    }
    m_state->transport->close();
}

void TlsStream::abort() {
    m_state->phase = detail::TlsState::Phase::Closed;
    m_state->onClosed = nullptr;
    m_state->transport->abort();
}

bool TlsStream::isOpen() const noexcept {
    return m_state->phase == detail::TlsState::Phase::Open && m_state->transport->isOpen();
}

const String &TlsStream::peerAddress() const noexcept { return m_state->peer; }

String TlsStream::protocolName() const {
    if (m_state->ssl == nullptr || m_state->phase != detail::TlsState::Phase::Open) {
        return {};
    }
    const String version = openSsl().SSL_get_version(m_state->ssl); // "TLSv1.3"
    if (version.rfind("TLSv", 0) == 0) {
        return "TLS " + version.substr(4);
    }
    return version;
}

} // namespace cfw
