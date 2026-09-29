// Soak: WebSocket clients come and go against one server for a while
// (CFW_SOAK_SECONDS, 3 s by default). Clients connect, send text and
// binary messages of random sizes, and leave by closing, by dropping the
// connection, or by being closed by the server; every message sent on an
// open connection comes back in order. At the end nothing is left: no
// server sockets, no requests, and no file descriptors beyond where it
// started.

#include <deque>
#include <list>
#include <memory>
#include <random>

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/WebSocket.h"
#include "cfw/net/WebSocketServer.h"
#include "cfw/test/Check.h"
#include "cfw/test/Soak.h"

using namespace cfw;
using namespace std::chrono_literals;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

struct ServerSide {
    std::unique_ptr<WebSocket> socket;
    bool closed = false;
};

struct Client {
    std::unique_ptr<WebSocket> socket;
    std::deque<String> expected; // echoes still owed, in order
    bool closed = false;
    bool mismatch = false;
    std::size_t received = 0;
};

} // namespace

int main() {
    const auto duration = cfw::test::soakDuration(3.0);
    std::mt19937_64 random(cfw::test::soakSeed("SoakNetTest"));
    const int fdsBefore = cfw::test::openFileDescriptors();
    {
        auto loop = EventLoop::create().value();
        Executor resolver(1);
        auto server = WebSocketServer::listen(*loop, "127.0.0.1", 0).value();
        std::list<ServerSide> serverSockets;
        server->setOnConnection([&](std::unique_ptr<WebSocket> socket, const String &) {
            ServerSide &side = serverSockets.emplace_back();
            side.socket = std::move(socket);
            WebSocket *raw = side.socket.get();
            ServerSide *sidePtr = &side;
            raw->setOnText([raw](StringView text) { raw->sendText(text); });
            raw->setOnBinary([raw](Span<const std::byte> data) { raw->sendBinary(data); });
            raw->setOnClosed([sidePtr](std::uint16_t, StringView) { sidePtr->closed = true; });
        });
        const Url url = Url::parse("ws://127.0.0.1:" + std::to_string(server->port()) + "/soak").value();

        std::list<Client> clients;
        std::list<std::pair<ConnectRequest, std::shared_ptr<bool>>> connecting;
        std::size_t connections = 0, messages = 0, echoes = 0, drops = 0, serverCloses = 0, failures = 0;
        bool anyMismatch = false;

        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < duration) {
            loop->runOnce(1ms);
            const auto choice = random() % 100;
            if (choice < 6 && clients.size() + connecting.size() < 24) {
                auto done = std::make_shared<bool>(false);
                ConnectRequest request = WebSocket::connect(*loop, resolver, url, [&, done](Result<std::unique_ptr<WebSocket>> r) {
                    *done = true;
                    if (!r) {
                        ++failures;
                        return;
                    }
                    Client &client = clients.emplace_back();
                    client.socket = std::move(r).value();
                    Client *c = &client;
                    const auto echo = [c, &echoes](String got) {
                        if (c->expected.empty() || c->expected.front() != got) {
                            c->mismatch = true;
                        } else {
                            c->expected.pop_front();
                            ++echoes;
                        }
                        ++c->received;
                    };
                    client.socket->setOnText([echo](StringView text) { echo("t" + String(text)); });
                    client.socket->setOnBinary([echo](Span<const std::byte> data) {
                        echo("b" + String(reinterpret_cast<const char *>(data.data()), data.size()));
                    });
                    client.socket->setOnClosed([c](std::uint16_t, StringView) { c->closed = true; });
                    ++connections;
                });
                connecting.emplace_back(std::move(request), done);
            } else if (choice < 70 && !clients.empty()) {
                // A message from a random open client.
                auto it = clients.begin();
                std::advance(it, std::ptrdiff_t(random() % clients.size()));
                if (!it->closed && it->socket->state() == WebSocket::State::Open) {
                    const std::size_t size = random() % 4 == 0 ? random() % 65536 : random() % 64;
                    String payload(size, 'a');
                    for (char &ch : payload) {
                        ch = char('a' + random() % 26);
                    }
                    if (random() % 2) {
                        it->socket->sendText(payload);
                        it->expected.push_back("t" + payload);
                    } else {
                        it->socket->sendBinary({reinterpret_cast<const std::byte *>(payload.data()), payload.size()});
                        it->expected.push_back("b" + payload);
                    }
                    ++messages;
                }
            } else if (choice < 74 && !clients.empty()) {
                // A client leaves: politely, or by dropping the connection.
                auto it = clients.begin();
                std::advance(it, std::ptrdiff_t(random() % clients.size()));
                if (random() % 2) {
                    it->socket->close();
                } else {
                    ++drops;
                    anyMismatch = anyMismatch || it->mismatch;
                    clients.erase(it);
                }
            } else if (choice < 76 && !serverSockets.empty()) {
                auto it = serverSockets.begin();
                std::advance(it, std::ptrdiff_t(random() % serverSockets.size()));
                if (!it->closed) {
                    it->socket->close(4001, "soak");
                    ++serverCloses;
                }
            }
            // Finished requests and closed sockets go, outside their callbacks.
            connecting.remove_if([](const auto &entry) { return *entry.second; });
            for (auto it = clients.begin(); it != clients.end();) {
                if (it->closed) {
                    anyMismatch = anyMismatch || it->mismatch;
                    it = clients.erase(it);
                } else {
                    ++it;
                }
            }
            serverSockets.remove_if([](const ServerSide &side) { return side.closed; });
        }

        // Wind down: let echoes arrive, then everyone closes.
        loop->runUntil(
            [&] {
                for (const Client &c : clients) {
                    if (!c.expected.empty() && !c.closed) {
                        return false;
                    }
                }
                return true;
            },
            10s);
        for (Client &c : clients) {
            anyMismatch = anyMismatch || c.mismatch;
            c.socket->close();
        }
        loop->runUntil(
            [&] {
                serverSockets.remove_if([](const ServerSide &side) { return side.closed; });
                return serverSockets.empty();
            },
            10s);
        clients.clear();
        connecting.clear();
        loop->runOnce(10ms);

        std::printf("SoakNetTest: %zu connections, %zu messages, %zu echoes, %zu dropped, %zu closed by the server\n",
                    connections, messages, echoes, drops, serverCloses);
        check(connections > 10 && messages > 100, "the soak did real work");
        checkEqual(failures, std::size_t(0), "every connection attempt succeeded");
        check(!anyMismatch, "every echo came back in order, none altered");
        check(serverSockets.empty(), "the server has no sockets left");
    }
    const int fdsAfter = cfw::test::openFileDescriptors();
    if (fdsBefore >= 0) {
        check(fdsAfter <= fdsBefore + 2, "no file descriptors leaked");
        if (fdsAfter > fdsBefore + 2) {
            std::printf("  file descriptors: %d before, %d after\n", fdsBefore, fdsAfter);
        }
    }
    return cfw::test::finish("SoakNetTest");
}
