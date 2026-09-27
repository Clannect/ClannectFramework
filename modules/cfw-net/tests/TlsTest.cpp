// TLS end to end on one event loop, through the operating system's TLS
// (Schannel on Windows, the system OpenSSL elsewhere): a local server with a
// throwaway certificate, clients that pin it or trust it through the trust
// store, and the ways verification must fail. Then https:// through
// HttpClient and wss:// through WebSocket over the same machinery.

#include "cfw/net/TlsStream.h"

#include <cstdlib>
#include <random>

#include "cfw/core/Base64.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/TemporaryDirectory.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/HttpClient.h"
#include "cfw/net/TcpListener.h"
#include "cfw/net/WebSocket.h"
#include "cfw/net/WebSocketHandshake.h"
#include "cfw/test/Check.h"

using namespace cfw;
using namespace std::chrono_literals;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Span<const std::byte> bytesOf(StringView s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }
String textOf(Span<const std::byte> b) { return String(reinterpret_cast<const char *>(b.data()), b.size()); }

// A TLS server on 127.0.0.1 with a self-signed certificate for "localhost".
struct Harness {
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    Executor executor{1};
    std::unique_ptr<TlsServerIdentity> identity = TlsServerIdentity::createSelfSigned("localhost").value();
    std::unique_ptr<TcpListener> listener = TcpListener::listen(*loop, "127.0.0.1", 0).value();
    std::vector<ConnectRequest> serverHandshakes;
    std::vector<std::unique_ptr<ByteStream>> serverStreams;
    std::vector<Error> serverErrors;
    std::function<void(ByteStream &)> onServerStream;

    Harness() {
        listener->setOnAccept([this](std::unique_ptr<TcpConnection> connection) {
            serverHandshakes.push_back(TlsStream::startServer(
                *loop, std::move(connection), *identity, 5s, [this](Result<std::unique_ptr<ByteStream>> r) {
                    if (!r) {
                        serverErrors.push_back(r.error());
                        return;
                    }
                    serverStreams.push_back(std::move(r).value());
                    if (onServerStream) {
                        onServerStream(*serverStreams.back());
                    }
                }));
        });
    }

    Result<std::unique_ptr<ByteStream>> connect(StringView serverName, TlsOptions options) {
        std::optional<Result<std::unique_ptr<ByteStream>>> result;
        ConnectRequest tcp;
        ConnectRequest tls;
        tcp = TcpConnection::connect(*loop, executor, "127.0.0.1", listener->port(),
                                     [&](Result<std::unique_ptr<TcpConnection>> c) {
                                         if (!c) {
                                             result = std::move(c).error();
                                             return;
                                         }
                                         tls = TlsStream::startClient(*loop, executor, std::move(c).value(), serverName,
                                                                      options, [&](Result<std::unique_ptr<ByteStream>> r) {
                                                                          result = std::move(r);
                                                                      });
                                     });
        if (!loop->runUntil([&] { return result.has_value(); }, 10s)) {
            return Error(ErrorCode::Timeout, "test: no handshake result");
        }
        return std::move(*result);
    }

    TlsOptions pinned() const {
        TlsOptions o;
        o.pinnedCertificates = {identity->fingerprint()};
        return o;
    }
};

#if !defined(_WIN32)
String pem(const std::vector<std::byte> &der) {
    const String b64 = base64Encode(der);
    String out = "-----BEGIN CERTIFICATE-----\n";
    for (std::size_t i = 0; i < b64.size(); i += 64) {
        out += b64.substr(i, 64) + "\n";
    }
    return out + "-----END CERTIFICATE-----\n";
}
#endif

void exchangesDataWhenPinned() {
    Harness h;
    Result<std::unique_ptr<ByteStream>> client = h.connect("localhost", h.pinned());
    check(client.ok(), "the handshake succeeds when the certificate is pinned");
    if (!client) {
        std::printf("      %s\n", client.error().describe().c_str());
        return;
    }
    check(h.loop->runUntil([&] { return !h.serverStreams.empty(); }, 5s), "the server side completes too");
    const String protocol = static_cast<TlsStream &>(*client.value()).protocolName();
    check(protocol == "TLS 1.3" || protocol == "TLS 1.2", "TLS 1.2 or 1.3 is negotiated");
    std::printf("      negotiated %s\n", protocol.c_str());

    // Echo, including one message far larger than a TLS record.
    ByteStream &server = *h.serverStreams[0];
    server.setOnData([&](Span<const std::byte> data) { server.send(data); });
    String received;
    client.value()->setOnData([&](Span<const std::byte> data) { received += textOf(data); });
    String big(1u << 20, 'x');
    std::mt19937 rng(1);
    for (char &c : big) {
        c = static_cast<char>('a' + rng() % 26);
    }
    client.value()->send(StringView("hello over TLS"));
    client.value()->send(StringView(big));
    check(h.loop->runUntil([&] { return received.size() == 14 + big.size(); }, 10s), "1 MB round-trips");
    check(received == "hello over TLS" + big, "byte for byte");

    // close() sends close_notify; the server sees an orderly end.
    bool serverClosed = false;
    std::optional<Error> serverCloseError;
    server.setOnClosed([&](const std::optional<Error> &e) {
        serverClosed = true;
        serverCloseError = e;
    });
    client.value()->close();
    check(h.loop->runUntil([&] { return serverClosed; }, 5s) && !serverCloseError, "an orderly close reaches the server");
}

void rejectsUntrustedOrWrongCertificates() {
    Harness h;
    const Result<std::unique_ptr<ByteStream>> untrusted = h.connect("localhost", {});
    check(!untrusted && untrusted.error().code() == ErrorCode::PermissionDenied,
          "a self-signed certificate is not trusted by default");
    if (!untrusted.ok()) {
        std::printf("      %s\n", untrusted.error().describe().c_str());
    }
    TlsOptions wrongPin;
    wrongPin.pinnedCertificates = {Sha256::hash(StringView("some other certificate"))};
    const Result<std::unique_ptr<ByteStream>> wrong = h.connect("localhost", wrongPin);
    check(!wrong && wrong.error().code() == ErrorCode::PermissionDenied, "a different pinned fingerprint is refused");
}

void verifiesNamesThroughTheTrustStore() {
#if !defined(_WIN32)
    // Make the throwaway certificate a trusted root for this process
    // (OpenSSL reads SSL_CERT_FILE for every new context).
    Harness h;
    TemporaryDirectory dir = TemporaryDirectory::create().value();
    const Path file = dir.path() / "root.pem";
    const String text = pem(h.identity->certificateDer());
    check(writeFileAtomic(file, bytesOf(text)).ok(), "the trust anchor is written");
    const char *previous = std::getenv("SSL_CERT_FILE");
    const String saved = previous != nullptr ? String(previous) : String();
    setenv("SSL_CERT_FILE", file.toString().c_str(), 1);

    const Result<std::unique_ptr<ByteStream>> named = h.connect("localhost", {});
    check(named.ok(), "a trusted certificate for the right name is accepted");
    if (!named) {
        std::printf("      %s\n", named.error().describe().c_str());
    }
    const Result<std::unique_ptr<ByteStream>> ip = h.connect("127.0.0.1", {});
    check(ip.ok(), "and for its IP address");
    const Result<std::unique_ptr<ByteStream>> other = h.connect("other.example", {});
    check(!other && other.error().code() == ErrorCode::PermissionDenied, "the wrong name is refused");
    if (!other.ok()) {
        std::printf("      %s\n", other.error().describe().c_str());
    }
    if (previous != nullptr) {
        setenv("SSL_CERT_FILE", saved.c_str(), 1);
    } else {
        unsetenv("SSL_CERT_FILE");
    }
#endif
}

void refusesPlainTextPeers() {
    Harness h;
    // A server that answers in plain text, like an HTTP server on the TLS port.
    std::unique_ptr<TcpListener> plain = TcpListener::listen(*h.loop, "127.0.0.1", 0).value();
    std::vector<std::unique_ptr<TcpConnection>> accepted;
    plain->setOnAccept([&](std::unique_ptr<TcpConnection> c) {
        c->send(StringView("HTTP/1.1 400 Bad Request\r\n\r\n"));
        c->close();
        accepted.push_back(std::move(c));
    });
    std::optional<Result<std::unique_ptr<ByteStream>>> result;
    ConnectRequest tls;
    ConnectRequest tcp = TcpConnection::connect(*h.loop, h.executor, "127.0.0.1", plain->port(),
                                                [&](Result<std::unique_ptr<TcpConnection>> c) {
                                                    tls = TlsStream::startClient(*h.loop, h.executor, std::move(c).value(),
                                                                                 "localhost", h.pinned(),
                                                                                 [&](Result<std::unique_ptr<ByteStream>> r) {
                                                                                     result = std::move(r);
                                                                                 });
                                                });
    check(h.loop->runUntil([&] { return result.has_value(); }, 5s) && !result->ok(), "a non-TLS peer fails the handshake");

    // A client that connects and says nothing: the server gives up.
    std::unique_ptr<TcpConnection> silent;
    ConnectRequest s = TcpConnection::connect(*h.loop, h.executor, "127.0.0.1", h.listener->port(),
                                              [&](Result<std::unique_ptr<TcpConnection>> c) { silent = std::move(c).value(); });
    check(h.loop->runUntil([&] { return silent != nullptr; }, 3s), "the silent client connects");
}

void httpsThroughHttpClient() {
    Harness h;
    h.onServerStream = [&h](ByteStream &stream) {
        auto request = std::make_shared<String>();
        stream.setOnData([&stream, request](Span<const std::byte> data) {
            *request += textOf(data);
            if (request->find("\r\n\r\n") != String::npos) {
                const String body = request->substr(0, request->find(' ')) + " via TLS";
                stream.send(StringView("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n" + body));
                stream.close();
            }
        });
        (void)h;
    };
    HttpClient client(*h.loop, h.executor);
    HttpOptions options;
    options.tls = h.pinned();
    std::optional<Result<HttpResponse>> response;
    ConnectRequest request = client.get(Url::parse("https://127.0.0.1:" + std::to_string(h.listener->port()) + "/").value(),
                                        [&](Result<HttpResponse> r) { response = std::move(r); }, options);
    check(h.loop->runUntil([&] { return response.has_value(); }, 10s), "the https request completes");
    check(response && response->ok() && response->value().status == 200, "with 200");
    if (response && response->ok()) {
        checkEqual(String(response->value().bodyText()), String("GET via TLS"), "and the server's body");
    } else if (response) {
        std::printf("      %s\n", response->error().describe().c_str());
    }

    std::optional<Result<HttpResponse>> refused;
    ConnectRequest r2 = client.get(Url::parse("https://127.0.0.1:" + std::to_string(h.listener->port()) + "/").value(),
                                   [&](Result<HttpResponse> r) { refused = std::move(r); });
    check(h.loop->runUntil([&] { return refused.has_value(); }, 10s) && !refused->ok() &&
              refused->error().code() == ErrorCode::PermissionDenied,
          "without the pin, the untrusted server is refused");
}

void wssThroughWebSocket() {
    Harness h;
    std::unique_ptr<WebSocket> serverSocket;
    h.onServerStream = [&](ByteStream &stream) {
        auto request = std::make_shared<String>();
        stream.setOnData([&, request](Span<const std::byte> data) {
            *request += textOf(data);
            Result<std::optional<ParsedHead>> head = parseHttpRequestHead(*request);
            if (!head || !head.value()) {
                return;
            }
            stream.send(StringView(wsServerResponse(head.value()->head).value()));
            // Hand the TLS stream over to a server-side WebSocket.
            for (auto &owned : h.serverStreams) {
                if (owned.get() == &stream) {
                    serverSocket = WebSocket::adopt(*h.loop, std::move(owned), WsRole::Server, {},
                                                    bytesOf(StringView(*request).substr(head.value()->consumed)));
                }
            }
            serverSocket->setOnText([&](StringView text) { serverSocket->sendText(String("echo:") + String(text)); });
        });
    };
    WebSocket::ConnectOptions options;
    options.tls = h.pinned();
    std::unique_ptr<WebSocket> client;
    std::optional<Error> error;
    ConnectRequest r = WebSocket::connect(*h.loop, h.executor,
                                          Url::parse("wss://localhost:" + std::to_string(h.listener->port()) + "/game").value(),
                                          [&](Result<std::unique_ptr<WebSocket>> c) {
                                              if (c) {
                                                  client = std::move(c).value();
                                              } else {
                                                  error = c.error();
                                              }
                                          },
                                          options);
    check(h.loop->runUntil([&] { return client != nullptr || error.has_value(); }, 10s) && client != nullptr,
          "wss:// connects");
    if (error) {
        std::printf("      %s\n", error->describe().c_str());
    }
    if (!client) {
        return;
    }
    String got;
    client->setOnText([&](StringView text) { got = String(text); });
    client->sendText("h\xC3\xA9llo");
    check(h.loop->runUntil([&] { return !got.empty(); }, 5s), "a message echoes over wss");
    checkEqual(got, String("echo:h\xC3\xA9llo"), "intact");
}

} // namespace

int main() {
    if (std::getenv("CFW_UNDER_WINE") != nullptr &&
        !TlsServerIdentity::createSelfSigned("localhost")) {
        // Wine's CertCreateSelfSignCertificate does not accept CNG keys, so no
        // Schannel server can run here. The Schannel client is exercised
        // against a native server instead (testing/tls-interop, run by CI).
        std::printf("      under Wine: Schannel server identities are unavailable; see testing/tls-interop\n");
        check(TlsStream::available(), "Schannel is present");
        return cfw::test::finish("TlsTest");
    }
    if (!TlsStream::available()) {
        // No system TLS (a minimal container without libssl): the one thing
        // to check is that the failure says so rather than crashing.
        const auto identity = TlsServerIdentity::createSelfSigned("localhost");
        check(!identity && identity.error().code() == ErrorCode::Unsupported, "no TLS backend is reported clearly");
        std::printf("      TLS is not available here; end-to-end tests skipped\n");
        return cfw::test::finish("TlsTest");
    }
    exchangesDataWhenPinned();
    rejectsUntrustedOrWrongCertificates();
    verifiesNamesThroughTheTrustStore();
    refusesPlainTextPeers();
    httpsThroughHttpClient();
    wssThroughWebSocket();
    return cfw::test::finish("TlsTest");
}
