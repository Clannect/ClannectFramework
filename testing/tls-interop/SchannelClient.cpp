// The Windows side of the TLS interop check (see TlsEchoServer.cpp): through
// Schannel, a client that pins the server's certificate must connect and
// echo 300 KB intact, and a client that does not pin it must be refused
// because the certificate is untrusted.

#include <cstdio>
#include <fstream>
#include <string>

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TcpConnection.h"
#include "cfw/net/TlsStream.h"

using namespace cfw;
using namespace std::chrono_literals;

namespace {

Sha256::Digest fromHex(const std::string &hex) {
    Sha256::Digest d{};
    for (std::size_t i = 0; i < d.size(); ++i) {
        d[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return d;
}

bool run(bool pin, std::uint16_t port, const Sha256::Digest &fingerprint) {
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    Executor executor(1);
    std::optional<Result<std::unique_ptr<ByteStream>>> result;
    TlsOptions options;
    if (pin) {
        options.pinnedCertificates = {fingerprint};
    }
    ConnectRequest tls;
    ConnectRequest tcp = TcpConnection::connect(*loop, executor, "127.0.0.1", port,
                                                [&](Result<std::unique_ptr<TcpConnection>> c) {
                                                    if (!c) {
                                                        result = std::move(c).error();
                                                        return;
                                                    }
                                                    tls = TlsStream::startClient(*loop, executor, std::move(c).value(),
                                                                                 "localhost", options,
                                                                                 [&](Result<std::unique_ptr<ByteStream>> r) {
                                                                                     result = std::move(r);
                                                                                 });
                                                });
    loop->runUntil([&] { return result.has_value(); }, 15s);
    if (!result) {
        std::printf("FAIL: no handshake result\n");
        return false;
    }
    if (!pin) {
        const bool refused = !result->ok() && result->error().code() == ErrorCode::PermissionDenied;
        std::printf("%s: an untrusted certificate is refused (%s)\n", refused ? "OK" : "FAIL",
                    result->ok() ? "accepted" : result->error().describe().c_str());
        return refused;
    }
    if (!result->ok()) {
        std::printf("FAIL: pinned handshake: %s\n", result->error().describe().c_str());
        return false;
    }
    ByteStream &stream = *result->value();
    std::string got;
    stream.setOnData([&](Span<const std::byte> d) { got.append(reinterpret_cast<const char *>(d.data()), d.size()); });
    std::string big(300000, 'z');
    big[1234] = 'Q';
    const std::string expected = "hello schannel" + big;
    stream.send(StringView("hello schannel"));
    stream.send(StringView(big));
    loop->runUntil([&] { return got.size() >= expected.size(); }, 15s);
    const bool ok = got == expected;
    std::printf("%s: pinned handshake (%s) and a %zu-byte echo\n", ok ? "OK" : "FAIL",
                static_cast<TlsStream &>(stream).protocolName().c_str(), got.size());
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: SchannelClient <info-file>\n");
        return 2;
    }
    std::ifstream info(argv[1]);
    unsigned port = 0;
    std::string hex;
    info >> port >> hex;
    if (port == 0 || hex.size() != 64) {
        std::printf("FAIL: cannot read %s\n", argv[1]);
        return 1;
    }
    const Sha256::Digest fingerprint = fromHex(hex);
    const bool pinned = run(true, static_cast<std::uint16_t>(port), fingerprint);
    const bool refused = run(false, static_cast<std::uint16_t>(port), fingerprint);
    return pinned && refused ? 0 : 1;
}
