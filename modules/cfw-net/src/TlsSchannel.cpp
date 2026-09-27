// TLS on Windows via Schannel (SSPI). The only cfw-net file that includes the
// Windows security headers.
//
// References: Microsoft "Creating a Secure Connection Using Schannel",
// InitializeSecurityContext / AcceptSecurityContext (Schannel),
// EncryptMessage / DecryptMessage (Schannel), SCH_CREDENTIALS.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#define SECURITY_WIN32
#include <security.h>
// SCH_CREDENTIALS (needed for TLS 1.3) is only declared with this switch, as
// in the Windows SDK; it needs UNICODE_STRING from subauth.h.
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <schannel.h>
#include <sspi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>
#include <system_error>

#include "cfw/core/Utf8.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TlsStream.h"

namespace cfw {

namespace {

Error tlsError(StringView what, long status) {
    String message(what);
    message += " failed: ";
    message += std::system_category().message(static_cast<int>(status));
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(status));
    return Error(ErrorCode::NetworkError, std::move(message)).with("status", code);
}

std::wstring wide(StringView text) {
    const Result<std::u16string> converted = utf8ToUtf16(text);
    const std::u16string &u16 = converted ? converted.value() : std::u16string();
    return std::wstring(u16.begin(), u16.end());
}

Sha256::Digest fingerprintOf(PCCERT_CONTEXT cert) {
    return Sha256::hash(Span<const std::byte>(reinterpret_cast<const std::byte *>(cert->pbCertEncoded), cert->cbCertEncoded));
}

constexpr ULONG kClientFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                               ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
constexpr ULONG kServerFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                               ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;

// Certificate verification against the system trust store and the SSL policy
// (chain, validity, key usage, name, revocation where it can be checked).
// Blocking: runs on the Executor.
Result<void> verifyChain(PCCERT_CONTEXT leaf, const std::wstring &serverName) {
    LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof para;
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    para.RequestedUsage.Usage.cUsageIdentifier = 1;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(nullptr, leaf, nullptr, leaf->hCertStore, &para,
                                 CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, nullptr, &chain)) {
        return tlsError("building the certificate chain", static_cast<long>(GetLastError()));
    }
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof ssl;
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = const_cast<wchar_t *>(serverName.c_str());
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof policy;
    // Revocation is soft-fail (like browsers): an unreachable CRL/OCSP server
    // is ignored, but a certificate known to be revoked is rejected.
    policy.dwFlags = CERT_CHAIN_POLICY_IGNORE_ALL_REV_UNKNOWN_FLAGS;
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof status;
    const BOOL ran = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status);
    CertFreeCertificateChain(chain);
    if (!ran) {
        return tlsError("verifying the certificate", static_cast<long>(GetLastError()));
    }
    if (status.dwError != 0) {
        const auto code = static_cast<long>(status.dwError);
        const char *why = "the server certificate is not valid";
        switch (static_cast<DWORD>(code)) {
        case static_cast<DWORD>(CERT_E_UNTRUSTEDROOT): why = "the server certificate is not trusted"; break;
        case static_cast<DWORD>(CERT_E_CN_NO_MATCH): why = "the server certificate is for a different host"; break;
        case static_cast<DWORD>(CERT_E_EXPIRED): why = "the server certificate has expired or is not yet valid"; break;
        case static_cast<DWORD>(CRYPT_E_REVOKED): why = "the server certificate has been revoked"; break;
        case static_cast<DWORD>(CERT_E_WRONG_USAGE): why = "the certificate is not for server authentication"; break;
        default: break;
        }
        return Error(ErrorCode::PermissionDenied, why).with("status", std::system_category().message(static_cast<int>(code)));
    }
    return success();
}

} // namespace

// --- Server identity ---------------------------------------------------------

namespace detail {

struct TlsIdentityState {
    NCRYPT_PROV_HANDLE provider = 0;
    NCRYPT_KEY_HANDLE key = 0;
    PCCERT_CONTEXT certificate = nullptr;
    std::vector<std::byte> der;
    Sha256::Digest fingerprint{};

    ~TlsIdentityState() {
        if (certificate != nullptr) {
            CertFreeCertificateContext(certificate);
        }
        if (key != 0) {
            NCryptDeleteKey(key, 0); // removes the persisted test key and frees the handle
        }
        if (provider != 0) {
            NCryptFreeObject(provider);
        }
    }
};

} // namespace detail

TlsServerIdentity::TlsServerIdentity(std::unique_ptr<detail::TlsIdentityState> state) : m_state(std::move(state)) {}
TlsServerIdentity::~TlsServerIdentity() = default;
const Sha256::Digest &TlsServerIdentity::fingerprint() const noexcept { return m_state->fingerprint; }
const std::vector<std::byte> &TlsServerIdentity::certificateDer() const noexcept { return m_state->der; }

Result<std::unique_ptr<TlsServerIdentity>> TlsServerIdentity::createSelfSigned(StringView dnsName) {
    auto state = std::make_unique<detail::TlsIdentityState>();
    SECURITY_STATUS s = NCryptOpenStorageProvider(&state->provider, MS_KEY_STORAGE_PROVIDER, 0);
    if (s != ERROR_SUCCESS) {
        return tlsError("opening the key storage provider", s);
    }
    // A persisted key (Schannel runs the server handshake in another process,
    // which needs to find the key by name), deleted in ~TlsIdentityState.
    std::random_device entropy;
    wchar_t keyName[64];
    std::swprintf(keyName, 64, L"cfw-tls-%08x%08x", entropy(), entropy());
    s = NCryptCreatePersistedKey(state->provider, &state->key, BCRYPT_RSA_ALGORITHM, keyName, 0, 0);
    if (s != ERROR_SUCCESS) {
        return tlsError("creating the key", s);
    }
    DWORD bits = 2048;
    NCryptSetProperty(state->key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&bits), sizeof bits, 0);
    s = NCryptFinalizeKey(state->key, 0);
    if (s != ERROR_SUCCESS) {
        return tlsError("generating the key", s);
    }

    const std::wstring subjectText = L"CN=" + wide(dnsName);
    DWORD subjectSize = 0;
    CertStrToNameW(X509_ASN_ENCODING, subjectText.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &subjectSize, nullptr);
    std::vector<BYTE> subject(subjectSize);
    if (!CertStrToNameW(X509_ASN_ENCODING, subjectText.c_str(), CERT_X500_NAME_STR, nullptr, subject.data(), &subjectSize,
                        nullptr)) {
        return tlsError("encoding the subject name", static_cast<long>(GetLastError()));
    }
    CERT_NAME_BLOB subjectBlob{subjectSize, subject.data()};

    // Subject Alternative Names: the DNS name plus both loopback addresses.
    std::wstring dnsWide = wide(dnsName);
    BYTE loopback4[4] = {127, 0, 0, 1};
    BYTE loopback6[16] = {};
    loopback6[15] = 1;
    CERT_ALT_NAME_ENTRY names[3]{};
    names[0].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
    names[0].pwszDNSName = dnsWide.data();
    names[1].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
    names[1].IPAddress = {4, loopback4};
    names[2].dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS;
    names[2].IPAddress = {16, loopback6};
    CERT_ALT_NAME_INFO altNames{3, names};
    BYTE *sanEncoded = nullptr;
    DWORD sanSize = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, &altNames, CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             &sanEncoded, &sanSize)) {
        return tlsError("encoding subject alternative names", static_cast<long>(GetLastError()));
    }
    LPSTR serverAuth[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_ENHKEY_USAGE usage{1, serverAuth};
    BYTE *ekuEncoded = nullptr;
    DWORD ekuSize = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &usage, CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             &ekuEncoded, &ekuSize)) {
        LocalFree(sanEncoded);
        return tlsError("encoding key usage", static_cast<long>(GetLastError()));
    }
    CERT_EXTENSION extension[2]{};
    extension[0].pszObjId = const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2);
    extension[0].Value = {sanSize, sanEncoded};
    extension[1].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
    extension[1].Value = {ekuSize, ekuEncoded};
    CERT_EXTENSIONS extensions{2, extension};

    CRYPT_KEY_PROV_INFO keyInfo{};
    keyInfo.pwszContainerName = keyName;
    keyInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    CRYPT_ALGORITHM_IDENTIFIER algorithm{};
    algorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    // Valid from yesterday to tomorrow (tolerates clock skew in tests).
    SYSTEMTIME now{};
    GetSystemTime(&now);
    FILETIME fileNow{};
    SystemTimeToFileTime(&now, &fileNow);
    ULARGE_INTEGER t{};
    t.LowPart = fileNow.dwLowDateTime;
    t.HighPart = fileNow.dwHighDateTime;
    constexpr ULONGLONG kDay = 24ull * 60 * 60 * 10'000'000;
    const auto toSystem = [](ULONGLONG value) {
        FILETIME ft{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
        SYSTEMTIME st{};
        FileTimeToSystemTime(&ft, &st);
        return st;
    };
    SYSTEMTIME start = toSystem(t.QuadPart - kDay);
    SYSTEMTIME end = toSystem(t.QuadPart + kDay);

    state->certificate = CertCreateSelfSignCertificate(state->key, &subjectBlob, 0, &keyInfo, &algorithm, &start,
                                                       &end, &extensions);
    const DWORD createError = GetLastError();
    LocalFree(sanEncoded);
    LocalFree(ekuEncoded);
    if (state->certificate == nullptr) {
        return tlsError("creating the self-signed certificate", static_cast<long>(createError));
    }
    state->fingerprint = fingerprintOf(state->certificate);
    const auto *encoded = reinterpret_cast<const std::byte *>(state->certificate->pbCertEncoded);
    state->der.assign(encoded, encoded + state->certificate->cbCertEncoded);
    return std::unique_ptr<TlsServerIdentity>(new TlsServerIdentity(std::move(state)));
}

// --- Stream state ------------------------------------------------------------

namespace detail {

struct TlsState {
    enum class Phase : std::uint8_t { Handshake, Verifying, Open, Closed };

    EventLoop *loop = nullptr;
    std::unique_ptr<ByteStream> transport;
    bool server = false;
    std::wstring serverName;
    TlsOptions options;
    String peer;

    CredHandle credentials{};
    bool haveCredentials = false;
    CtxtHandle context{};
    bool haveContext = false;
    SecPkgContext_StreamSizes sizes{};
    bool renegotiating = false;

    Phase phase = Phase::Handshake;
    std::vector<std::byte> incoming; // received ciphertext not yet consumed
    std::vector<std::byte> record;   // reused for every outgoing record
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::function<void(Span<const std::byte>)> onData;
    std::function<void(const std::optional<Error> &)> onClosed;
    // Set by the handshake operation; cleared once the stream is handed over.
    std::function<void(Result<void>)> handshakeDone;
    std::function<void(PCCERT_CONTEXT)> verifyRemote; // client: starts verification

    ~TlsState() {
        *alive = false;
        if (haveContext) {
            DeleteSecurityContext(&context);
        }
        if (haveCredentials) {
            FreeCredentialsHandle(&credentials);
        }
    }

    void sendToken(SecBuffer &buffer) {
        if (buffer.cbBuffer > 0 && buffer.pvBuffer != nullptr) {
            transport->send(Span<const std::byte>(static_cast<const std::byte *>(buffer.pvBuffer), buffer.cbBuffer));
        }
        if (buffer.pvBuffer != nullptr) {
            FreeContextBuffer(buffer.pvBuffer);
            buffer.pvBuffer = nullptr;
        }
    }

    // Keeps only the SECBUFFER_EXTRA tail (bytes belonging to the next
    // message), or drops everything that was consumed.
    void keepExtra(const SecBuffer &extra) {
        if (extra.BufferType == SECBUFFER_EXTRA && extra.cbBuffer > 0) {
            incoming.erase(incoming.begin(), incoming.end() - static_cast<std::ptrdiff_t>(extra.cbBuffer));
        } else {
            incoming.clear();
        }
    }

    void reportHandshake(Result<void> result) {
        if (auto done = std::move(handshakeDone)) {
            handshakeDone = nullptr;
            done(std::move(result));
        }
    }

    // Runs the handshake as far as the received bytes allow. `first` is the
    // client's opening call (no input yet).
    void handshakeStep(bool first) {
        const std::shared_ptr<bool> guard = alive;
        while (phase == Phase::Handshake || renegotiating) {
            if (!first && incoming.empty()) {
                return;
            }
            SecBuffer in[2]{};
            in[0] = {static_cast<unsigned long>(incoming.size()), SECBUFFER_TOKEN, incoming.data()};
            in[1] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in};
            SecBuffer out[1]{};
            out[0] = {0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc outDesc{SECBUFFER_VERSION, 1, out};
            ULONG attributes = 0;
            SECURITY_STATUS s = 0;
            if (server) {
                s = AcceptSecurityContext(&credentials, haveContext ? &context : nullptr, &inDesc, kServerFlags, 0,
                                          &context, &outDesc, &attributes, nullptr);
            } else {
                s = InitializeSecurityContextW(&credentials, haveContext ? &context : nullptr,
                                               const_cast<wchar_t *>(serverName.c_str()), kClientFlags, 0, 0,
                                               first ? nullptr : &inDesc, 0, &context, &outDesc, &attributes, nullptr);
            }
            if (s == SEC_E_INCOMPLETE_MESSAGE) {
                return; // wait for more bytes; nothing was consumed
            }
            if (s == SEC_E_OK || s == SEC_I_CONTINUE_NEEDED || s == SEC_I_INCOMPLETE_CREDENTIALS) {
                haveContext = true;
                sendToken(out[0]);
                if (!first) {
                    keepExtra(in[1]);
                }
                first = false;
                if (s == SEC_E_OK) {
                    if (renegotiating) {
                        renegotiating = false;
                        return;
                    }
                    handshakeComplete();
                    return;
                }
                continue; // CONTINUE_NEEDED: process any further bytes; INCOMPLETE_CREDENTIALS: retry without
            }
            sendToken(out[0]); // may carry a TLS alert for the peer
            fail(tlsError(server ? "TLS handshake (server)" : "TLS handshake", s));
            return;
        }
    }

    void handshakeComplete() {
        const SECURITY_STATUS s = QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes);
        if (s != SEC_E_OK) {
            fail(tlsError("querying TLS record sizes", s));
            return;
        }
        if (server) {
            phase = Phase::Open;
            reportHandshake(success());
            return;
        }
        PCCERT_CONTEXT remote = nullptr;
        const SECURITY_STATUS c = QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote);
        if (c != SEC_E_OK || remote == nullptr) {
            fail(Error(ErrorCode::PermissionDenied, "the server presented no certificate"));
            return;
        }
        if (!options.pinnedCertificates.empty()) {
            const Sha256::Digest fingerprint = fingerprintOf(remote);
            CertFreeCertificateContext(remote);
            const bool pinned = std::find(options.pinnedCertificates.begin(), options.pinnedCertificates.end(),
                                          fingerprint) != options.pinnedCertificates.end();
            if (!pinned) {
                fail(Error(ErrorCode::PermissionDenied, "the server certificate does not match the pinned fingerprint")
                         .with("fingerprint", Sha256::toHex(fingerprint)));
                return;
            }
            phase = Phase::Open;
            reportHandshake(success());
            return;
        }
        phase = Phase::Verifying;
        verifyRemote(remote); // takes ownership of `remote`
    }

    void onTransportData(Span<const std::byte> data) {
        incoming.insert(incoming.end(), data.begin(), data.end());
        // A failed handshake reports to the owner, which may destroy this
        // stream (found by ASan in the OpenSSL backend, which shares this
        // shape): check before touching state again.
        const std::shared_ptr<bool> guard = alive;
        if (phase == Phase::Handshake || renegotiating) {
            handshakeStep(false);
            if (!*guard) {
                return;
            }
        }
        if (phase == Phase::Open && !renegotiating) {
            decryptAvailable();
        }
    }

    void decryptAvailable() {
        const std::shared_ptr<bool> guard = alive;
        while (phase == Phase::Open && !renegotiating && !incoming.empty()) {
            SecBuffer buffers[4]{};
            buffers[0] = {static_cast<unsigned long>(incoming.size()), SECBUFFER_DATA, incoming.data()};
            buffers[1] = {0, SECBUFFER_EMPTY, nullptr};
            buffers[2] = {0, SECBUFFER_EMPTY, nullptr};
            buffers[3] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS s = DecryptMessage(&context, &desc, 0, nullptr);
            if (s == SEC_E_INCOMPLETE_MESSAGE) {
                return;
            }
            if (s != SEC_E_OK && s != SEC_I_RENEGOTIATE && s != SEC_I_CONTEXT_EXPIRED) {
                fail(tlsError("TLS decrypt", s));
                return;
            }
            const SecBuffer *plain = nullptr;
            const SecBuffer *extra = nullptr;
            for (const SecBuffer &b : buffers) {
                if (b.BufferType == SECBUFFER_DATA && plain == nullptr) {
                    plain = &b;
                } else if (b.BufferType == SECBUFFER_EXTRA && extra == nullptr) {
                    extra = &b;
                }
            }
            // The plaintext points into `incoming`; the tail after it is saved
            // before the callback runs (it may send, close, or destroy us).
            std::vector<std::byte> rest;
            if (extra != nullptr && extra->cbBuffer > 0) {
                rest.assign(incoming.end() - static_cast<std::ptrdiff_t>(extra->cbBuffer), incoming.end());
            }
            if (plain != nullptr && plain->cbBuffer > 0 && onData) {
                const auto callback = onData;
                callback(Span<const std::byte>(static_cast<const std::byte *>(plain->pvBuffer), plain->cbBuffer));
                if (!*guard) {
                    return;
                }
            }
            incoming = std::move(rest);
            if (s == SEC_I_CONTEXT_EXPIRED) {
                // close_notify from the peer: orderly end.
                phase = Phase::Closed;
                transport->close();
                return;
            }
            if (s == SEC_I_RENEGOTIATE) {
                // TLS 1.3 post-handshake messages (session tickets, key
                // update) go back through the handshake function.
                renegotiating = true;
                handshakeStep(false);
                if (!*guard) {
                    return;
                }
            }
        }
    }

    void encryptAndSend(Span<const std::byte> data) {
        while (!data.empty()) {
            const std::size_t chunk = std::min<std::size_t>(data.size(), sizes.cbMaximumMessage);
            record.resize(sizes.cbHeader + chunk + sizes.cbTrailer);
            std::memcpy(record.data() + sizes.cbHeader, data.data(), chunk);
            SecBuffer buffers[4]{};
            buffers[0] = {sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()};
            buffers[1] = {static_cast<unsigned long>(chunk), SECBUFFER_DATA, record.data() + sizes.cbHeader};
            buffers[2] = {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes.cbHeader + chunk};
            buffers[3] = {0, SECBUFFER_EMPTY, nullptr};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS s = EncryptMessage(&context, 0, &desc, 0);
            if (s != SEC_E_OK) {
                fail(tlsError("TLS encrypt", s));
                return;
            }
            transport->send(Span<const std::byte>(record.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer));
            data = data.subspan(chunk);
        }
    }

    void sendCloseNotify() {
        DWORD type = SCHANNEL_SHUTDOWN;
        SecBuffer control{sizeof type, SECBUFFER_TOKEN, &type};
        SecBufferDesc controlDesc{SECBUFFER_VERSION, 1, &control};
        if (ApplyControlToken(&context, &controlDesc) != SEC_E_OK) {
            return;
        }
        SecBuffer out{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outDesc{SECBUFFER_VERSION, 1, &out};
        ULONG attributes = 0;
        if (server) {
            AcceptSecurityContext(&credentials, &context, nullptr, kServerFlags, 0, nullptr, &outDesc, &attributes, nullptr);
        } else {
            InitializeSecurityContextW(&credentials, &context, const_cast<wchar_t *>(serverName.c_str()), kClientFlags, 0,
                                       0, nullptr, 0, nullptr, &outDesc, &attributes, nullptr);
        }
        sendToken(out);
    }

    // Ends the stream with an error: during the handshake it fails the
    // handshake; afterwards it is reported through the closed callback.
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
        if (phase == Phase::Handshake || phase == Phase::Verifying) {
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

// --- Handshake operation -----------------------------------------------------

namespace {

struct TlsHandshake final : detail::Cancellable, std::enable_shared_from_this<TlsHandshake> {
    EventLoop *loop = nullptr;
    Executor *verifier = nullptr;
    std::unique_ptr<TlsStream> stream;
    detail::TlsState *state = nullptr;
    TlsStream::HandshakeCallback callback;
    TimerId timer = 0;
    bool done = false;
    std::atomic<bool> cancelled{false};
    std::mutex postMutex; // see ConnectState: guards posting from the verifier thread

    [[nodiscard]] bool isDone() const override { return done; }

    void cancel() override {
        {
            const std::lock_guard lock(postMutex);
            cancelled = true;
        }
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
        // Ciphertext that arrived with the last handshake flight is decrypted
        // on the next turn, after the owner has set its callbacks.
        detail::TlsState *s = state;
        const std::shared_ptr<bool> alive = s->alive;
        loop->post([s, alive] {
            if (*alive && s->phase == detail::TlsState::Phase::Open) {
                s->decryptAvailable();
            }
        });
        if (cb) {
            cb(std::unique_ptr<ByteStream>(std::move(handed)));
        }
    }

    void startVerification(PCCERT_CONTEXT remote, std::wstring serverName) {
        std::weak_ptr<TlsHandshake> weak = shared_from_this();
        verifier->submit([weak, remote, serverName, loopPtr = loop] {
            Result<void> verdict = verifyChain(remote, serverName);
            CertFreeCertificateContext(remote);
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            const std::lock_guard lock(self->postMutex);
            if (self->cancelled.load()) {
                return;
            }
            loopPtr->post([weak, verdict = std::move(verdict)]() mutable {
                auto op = weak.lock();
                if (!op || op->done) {
                    return;
                }
                if (!verdict) {
                    op->state->fail(std::move(verdict).error());
                    return;
                }
                op->state->phase = detail::TlsState::Phase::Open;
                op->state->reportHandshake(success());
            });
        });
    }
};

ConnectRequest begin(EventLoop &loop, Executor *verifier, std::unique_ptr<ByteStream> transport, bool server,
                     StringView serverName, TlsOptions options, TlsServerIdentity *identity,
                     TlsStream::HandshakeCallback callback) {
    auto op = std::make_shared<TlsHandshake>();
    op->loop = &loop;
    op->verifier = verifier;
    op->callback = std::move(callback);

    auto state = std::make_unique<detail::TlsState>();
    state->loop = &loop;
    state->server = server;
    state->serverName = wide(serverName);
    state->options = options;
    state->peer = transport->peerAddress();
    state->transport = std::move(transport);
    detail::TlsState *raw = state.get();
    op->state = raw;
    op->stream = std::make_unique<TlsStream>(std::move(state));

    std::weak_ptr<TlsHandshake> weak = op;
    raw->handshakeDone = [weak](Result<void> result) {
        if (auto self = weak.lock()) {
            self->finish(std::move(result));
        }
    };
    raw->verifyRemote = [weak, raw](PCCERT_CONTEXT remote) {
        if (auto self = weak.lock()) {
            self->startVerification(remote, raw->serverName);
        } else {
            CertFreeCertificateContext(remote);
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

    // Credentials: TLS 1.2+ only, strong crypto only. Clients validate the
    // certificate themselves (manual validation) so the result is explicit.
    TLS_PARAMETERS tls{};
    tls.grbitDisabledProtocols = server ? (SP_PROT_SSL2_SERVER | SP_PROT_SSL3_SERVER | SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER)
                                        : (SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT | SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT);
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &tls;
    PCCERT_CONTEXT certificates[1] = {identity != nullptr ? identity->state().certificate : nullptr};
    if (server) {
        credentials.cCreds = 1;
        credentials.paCred = certificates;
        credentials.dwFlags = SCH_USE_STRONG_CRYPTO;
    } else {
        credentials.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    }
    TimeStamp expiry{};
    const SECURITY_STATUS s =
        AcquireCredentialsHandleW(nullptr, const_cast<wchar_t *>(UNISP_NAME_W), server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND,
                                  nullptr, &credentials, nullptr, nullptr, &raw->credentials, &expiry);
    if (s != SEC_E_OK) {
        const Error error = tlsError("acquiring TLS credentials", s);
        loop.post([weak, error] {
            if (auto self = weak.lock()) {
                self->finish(error);
            }
        });
        return ConnectRequest(std::move(op));
    }
    raw->haveCredentials = true;
    if (!server) {
        raw->handshakeStep(true); // sends the ClientHello
    }
    return ConnectRequest(std::move(op));
}

} // namespace

// --- TlsStream ---------------------------------------------------------------

TlsStream::TlsStream(std::unique_ptr<detail::TlsState> state) : m_state(std::move(state)) {}
TlsStream::~TlsStream() = default;

bool TlsStream::available() noexcept { return true; }

ConnectRequest TlsStream::startClient(EventLoop &loop, Executor &verifier, std::unique_ptr<ByteStream> transport,
                                      StringView serverName, TlsOptions options, HandshakeCallback callback) {
    return begin(loop, &verifier, std::move(transport), false, serverName, std::move(options), nullptr, std::move(callback));
}

ConnectRequest TlsStream::startServer(EventLoop &loop, std::unique_ptr<ByteStream> transport, TlsServerIdentity &identity,
                                      Duration handshakeTimeout, HandshakeCallback callback) {
    TlsOptions options;
    options.handshakeTimeout = handshakeTimeout;
    return begin(loop, nullptr, std::move(transport), true, {}, options, &identity, std::move(callback));
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
    SecPkgContext_ConnectionInfo info{};
    if (!m_state->haveContext || QueryContextAttributesW(&m_state->context, SECPKG_ATTR_CONNECTION_INFO, &info) != SEC_E_OK) {
        return {};
    }
    if (info.dwProtocol & SP_PROT_TLS1_3) {
        return "TLS 1.3";
    }
    if (info.dwProtocol & SP_PROT_TLS1_2) {
        return "TLS 1.2";
    }
    return "TLS (other)";
}

} // namespace cfw
