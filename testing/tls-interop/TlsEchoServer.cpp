// A TLS echo server on 127.0.0.1 with a throwaway certificate, built on the
// host's TLS backend. It writes "<port> <sha256 fingerprint>" to the file
// named on the command line and serves for 60 seconds. CI runs it natively
// on Linux and connects SchannelClient.exe (the Windows build, under Wine)
// to it, so the Schannel client path is exercised on every push.

#include <cstdio>
#include <fstream>

#include "cfw/net/EventLoop.h"
#include "cfw/net/TcpListener.h"
#include "cfw/net/TlsStream.h"

using namespace cfw;
using namespace std::chrono_literals;

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: TlsEchoServer <info-file>\n");
        return 2;
    }
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    Result<std::unique_ptr<TlsServerIdentity>> identity = TlsServerIdentity::createSelfSigned("localhost");
    if (!identity) {
        std::fprintf(stderr, "%s\n", identity.error().describe().c_str());
        return 1;
    }
    std::unique_ptr<TcpListener> listener = TcpListener::listen(*loop, "127.0.0.1", 0).value();
    std::vector<ConnectRequest> handshakes;
    std::vector<std::unique_ptr<ByteStream>> streams;
    listener->setOnAccept([&](std::unique_ptr<TcpConnection> connection) {
        handshakes.push_back(TlsStream::startServer(*loop, std::move(connection), *identity.value(), 10s,
                                                    [&](Result<std::unique_ptr<ByteStream>> r) {
                                                        if (!r) {
                                                            return;
                                                        }
                                                        streams.push_back(std::move(r).value());
                                                        ByteStream *s = streams.back().get();
                                                        s->setOnData([s](Span<const std::byte> d) { s->send(d); });
                                                    }));
    });
    std::ofstream(argv[1]) << listener->port() << ' ' << Sha256::toHex(identity.value()->fingerprint()) << '\n';
    loop->runUntil([] { return false; }, 60s);
    return 0;
}
