// WebSocket client and server end to end on one event loop: messages in
// order, large messages, ping/pong, the close handshake with application
// codes, limits, hostile clients, and teardown inside callbacks.

#include "cfw/net/WebSocket.h"
#include "cfw/net/WebSocketServer.h"

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TcpConnection.h"
#include "cfw/test/Check.h"

using namespace cfw;
using namespace std::chrono_literals;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Span<const std::byte> bytesOf(StringView s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }
String textOf(Span<const std::byte> b) { return String(reinterpret_cast<const char *>(b.data()), b.size()); }

struct Harness {
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    Executor resolver{1};
    std::unique_ptr<WebSocketServer> server;
    std::vector<std::unique_ptr<WebSocket>> serverSockets;
    String lastTarget;

    explicit Harness(WebSocketServer::Options options = {}) {
        server = WebSocketServer::listen(*loop, "127.0.0.1", 0, options).value();
        server->setOnConnection([this](std::unique_ptr<WebSocket> socket, const String &target) {
            lastTarget = target;
            serverSockets.push_back(std::move(socket));
        });
    }

    Url url(StringView path = "/game") const {
        return Url::parse("ws://127.0.0.1:" + std::to_string(server->port()) + String(path)).value();
    }

    std::unique_ptr<WebSocket> connect(StringView path = "/game", WebSocket::ConnectOptions options = {}) {
        std::unique_ptr<WebSocket> socket;
        std::optional<Error> error;
        ConnectRequest request = WebSocket::connect(*loop, resolver, url(path),
                                                    [&](Result<std::unique_ptr<WebSocket>> r) {
                                                        if (r) {
                                                            socket = std::move(r).value();
                                                        } else {
                                                            error = r.error();
                                                        }
                                                    },
                                                    options);
        const std::size_t before = serverSockets.size();
        loop->runUntil([&] { return (socket && serverSockets.size() > before) || error; }, 5s);
        if (error) {
            std::printf("      connect failed: %s\n", error->describe().c_str());
        }
        return socket;
    }
};

void echoesMessagesInOrder() {
    Harness h;
    auto client = h.connect("/game?ticket=abc");
    check(client != nullptr && h.serverSockets.size() == 1, "client and server sockets open");
    checkEqual(h.lastTarget, String("/game?ticket=abc"), "the server sees the request target (path + query)");

    WebSocket &server = *h.serverSockets[0];
    server.setOnBinary([&](Span<const std::byte> data) { server.sendBinary(data); });
    server.setOnText([&](StringView text) { server.sendText(String("echo:") + String(text)); });

    std::vector<String> got;
    String textGot;
    client->setOnBinary([&](Span<const std::byte> data) { got.push_back(textOf(data)); });
    client->setOnText([&](StringView text) { textGot = String(text); });
    for (int i = 0; i < 10000; ++i) {
        client->sendBinary(bytesOf("msg" + std::to_string(i)));
    }
    client->sendText("h\xC3\xA9llo");
    check(h.loop->runUntil([&] { return got.size() == 10000 && !textGot.empty(); }, 10s), "10,000 binary messages echoed");
    bool inOrder = got.size() == 10000;
    for (std::size_t i = 0; inOrder && i < got.size(); ++i) {
        inOrder = got[i] == "msg" + std::to_string(i);
    }
    check(inOrder, "in order, none lost");
    checkEqual(textGot, String("echo:h\xC3\xA9llo"), "text round-trips as UTF-8");

    // One 4 MB message (several TCP reads, 64-bit length).
    String big(4u * 1024u * 1024u, 'q');
    big[12345] = 'X';
    got.clear();
    client->sendBinary(bytesOf(big));
    check(h.loop->runUntil([&] { return !got.empty(); }, 10s) && got[0] == big, "4 MB message intact");
}

void pingIsAnsweredAutomatically() {
    Harness h;
    auto client = h.connect();
    String pong;
    client->setOnPong([&](Span<const std::byte> payload) { pong = textOf(payload); });
    client->ping(bytesOf("are you there"));
    check(h.loop->runUntil([&] { return !pong.empty(); }, 3s), "server answered the ping");
    checkEqual(pong, String("are you there"), "with the same payload");
}

void closeHandshakeCarriesCodes() {
    Harness h;
    auto client = h.connect();
    WebSocket &server = *h.serverSockets[0];
    std::uint16_t serverCode = 0;
    std::uint16_t clientCode = 0;
    String clientReason;
    server.setOnClosed([&](std::uint16_t code, StringView) { serverCode = code; });
    client->setOnClosed([&](std::uint16_t code, StringView reason) {
        clientCode = code;
        clientReason = String(reason);
    });
    // The runtime kicks a player with an application code (4000 + reason).
    server.close(4003, "kicked: spamming");
    check(h.loop->runUntil([&] { return serverCode != 0 && clientCode != 0; }, 5s), "both sides closed");
    checkEqual(clientCode, std::uint16_t(4003), "client sees the server's code");
    checkEqual(clientReason, String("kicked: spamming"), "and its reason");
    checkEqual(serverCode, std::uint16_t(4003), "server sees the echoed code");
    check(client->state() == WebSocket::State::Closed, "client state is Closed");
}

void droppedTcpIs1006() {
    Harness h;
    auto client = h.connect();
    std::uint16_t serverCode = 0;
    h.serverSockets[0]->setOnClosed([&](std::uint16_t code, StringView) { serverCode = code; });
    client.reset(); // gone without a close frame
    check(h.loop->runUntil([&] { return serverCode != 0; }, 5s), "server notices");
    checkEqual(serverCode, ws::kAbnormalClosure, "abnormal closure (1006)");
}

void oversizedMessagesAreRefused() {
    WebSocketServer::Options options;
    options.socket.limits.maxMessageBytes = 1024;
    options.socket.limits.maxFrameBytes = 1024;
    Harness h(options);
    auto client = h.connect();
    std::uint16_t serverCode = 0;
    std::uint16_t clientCode = 0;
    bool delivered = false;
    h.serverSockets[0]->setOnBinary([&](Span<const std::byte>) { delivered = true; });
    h.serverSockets[0]->setOnClosed([&](std::uint16_t code, StringView) { serverCode = code; });
    client->setOnClosed([&](std::uint16_t code, StringView) { clientCode = code; });
    client->sendBinary(bytesOf(String(5000, 'x')));
    check(h.loop->runUntil([&] { return serverCode != 0 && clientCode != 0; }, 5s), "connection closed");
    check(!delivered, "the oversized message was never delivered");
    checkEqual(serverCode, ws::kMessageTooBig, "server closes with 1009");
    checkEqual(clientCode, ws::kMessageTooBig, "client is told 1009");
}

void hostileClientsAreHandled() {
    WebSocketServer::Options options;
    options.handshakeTimeout = 200ms;
    Harness h(options);

    // Not a WebSocket request: a 400, then the connection ends.
    std::unique_ptr<TcpConnection> raw;
    String response;
    bool rawClosed = false;
    ConnectRequest r = TcpConnection::connect(*h.loop, h.resolver, "127.0.0.1", h.server->port(),
                                              [&](Result<std::unique_ptr<TcpConnection>> c) {
                                                  raw = std::move(c).value();
                                                  raw->setOnData([&](Span<const std::byte> d) { response += textOf(d); });
                                                  raw->setOnClosed([&](const std::optional<Error> &) { rawClosed = true; });
                                                  raw->send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
                                              });
    check(h.loop->runUntil([&] { return rawClosed; }, 3s), "plain HTTP request is answered and closed");
    check(response.starts_with("HTTP/1.1 400"), "with 400 Bad Request");
    check(h.serverSockets.empty(), "and no WebSocket was created");

    // A client that connects and says nothing is dropped after the timeout.
    std::unique_ptr<TcpConnection> silent;
    bool silentClosed = false;
    ConnectRequest s = TcpConnection::connect(*h.loop, h.resolver, "127.0.0.1", h.server->port(),
                                              [&](Result<std::unique_ptr<TcpConnection>> c) {
                                                  silent = std::move(c).value();
                                                  silent->setOnClosed([&](const std::optional<Error> &) { silentClosed = true; });
                                              });
    check(h.loop->runUntil([&] { return silentClosed; }, 3s), "silent client dropped after the handshake timeout");
    checkEqual(h.server->pendingHandshakes(), std::size_t(0), "no pending handshake left behind");
}

void clientErrors() {
    Harness h;
    const std::uint16_t deadPort = [&] {
        auto temp = TcpListener::listen(*h.loop, "127.0.0.1", 0).value();
        return temp->port();
    }();
    std::optional<Error> error;
    ConnectRequest refused = WebSocket::connect(*h.loop, h.resolver,
                                                Url::parse("ws://127.0.0.1:" + std::to_string(deadPort) + "/").value(),
                                                [&](Result<std::unique_ptr<WebSocket>> r) {
                                                    error = r ? std::nullopt : std::optional<Error>(r.error());
                                                });
    check(h.loop->runUntil([&] { return error.has_value(); }, 5s), "connecting to a closed port fails cleanly");
}

void destroyingInsideCallbacksIsSafe() {
    Harness h;
    auto client = h.connect();
    h.serverSockets[0]->setOnBinary([&](Span<const std::byte>) { h.serverSockets.clear(); }); // destroys itself
    std::uint16_t clientCode = 0;
    client->setOnClosed([&](std::uint16_t code, StringView) {
        clientCode = code;
        client.reset(); // and the client destroys itself in its closed callback
    });
    client->sendBinary(bytesOf("bye"));
    check(h.loop->runUntil([&] { return clientCode != 0; }, 5s), "client sees the server vanish");
    check(client == nullptr, "client destroyed itself from its own callback without crashing");
}

} // namespace

int main() {
    echoesMessagesInOrder();
    pingIsAnsweredAutomatically();
    closeHandshakeCarriesCodes();
    droppedTcpIs1006();
    oversizedMessagesAreRefused();
    hostileClientsAreHandled();
    clientErrors();
    destroyingInsideCallbacksIsSafe();
    return cfw::test::finish("WebSocketTest");
}
