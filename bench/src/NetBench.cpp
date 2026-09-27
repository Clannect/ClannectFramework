// cfw-bench-net: the Runtime's networking budget (spec §4.3 / §7): "hundreds
// of connections per process, tens of thousands of small binary messages per
// second."
//
// 200 WebSocket clients connect to one server over loopback; each sends small
// binary messages that the server echoes. Clients and server share one event
// loop on one thread, so the figures are the CPU cost of both ends together:
// the server alone has at least this much headroom.

#include <cstdio>

#include "cfw/core/Clock.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/WebSocket.h"
#include "cfw/net/WebSocketServer.h"

using namespace cfw;
using namespace std::chrono_literals;

int main() {
    constexpr int kClients = 200;
    constexpr int kMessagesPerClient = 500;
    constexpr std::size_t kMessageBytes = 48; // a typical input/state message

    auto loop = EventLoop::create().value();
    Executor resolver(1);
    auto server = WebSocketServer::listen(*loop, "127.0.0.1", 0).value();
    std::vector<std::unique_ptr<WebSocket>> serverSockets;
    std::uint64_t serverMessages = 0;
    server->setOnConnection([&](std::unique_ptr<WebSocket> socket, const String &) {
        WebSocket *raw = socket.get();
        raw->setOnBinary([raw, &serverMessages](Span<const std::byte> data) {
            ++serverMessages;
            raw->sendBinary(data);
        });
        serverSockets.push_back(std::move(socket));
    });

    const Url url = Url::parse("ws://127.0.0.1:" + std::to_string(server->port()) + "/").value();
    std::vector<std::unique_ptr<WebSocket>> clients;
    std::vector<ConnectRequest> requests;
    int failed = 0;
    const Stopwatch connectTimer;
    for (int i = 0; i < kClients; ++i) {
        requests.push_back(WebSocket::connect(*loop, resolver, url, [&](Result<std::unique_ptr<WebSocket>> r) {
            if (r) {
                clients.push_back(std::move(r).value());
            } else {
                ++failed;
            }
        }));
    }
    loop->runUntil([&] { return clients.size() + static_cast<std::size_t>(failed) == kClients &&
                                serverSockets.size() == clients.size(); },
                   30s);
    const double connectMs = connectTimer.elapsedMilliseconds();
    std::printf("connections: %zu open, %d failed, in %.1f ms\n", clients.size(), failed, connectMs);

    std::uint64_t echoed = 0;
    const std::vector<std::byte> message(kMessageBytes, std::byte{0x5A});
    for (auto &client : clients) {
        client->setOnBinary([&](Span<const std::byte>) { ++echoed; });
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(clients.size()) * kMessagesPerClient;
    const Stopwatch timer;
    // Send in waves, as clients would: every client sends a batch, then the loop runs.
    for (int wave = 0; wave < kMessagesPerClient / 10; ++wave) {
        for (auto &client : clients) {
            for (int k = 0; k < 10; ++k) {
                client->sendBinary(message);
            }
        }
        loop->runOnce(0ms);
    }
    const bool complete = loop->runUntil([&] { return echoed == expected; }, 60s);
    const double seconds = timer.elapsedSeconds();

    std::printf("messages: %llu echoed of %llu (%s) in %.3f s\n", static_cast<unsigned long long>(echoed),
                static_cast<unsigned long long>(expected), complete ? "complete" : "INCOMPLETE", seconds);
    const double serverRate = static_cast<double>(serverMessages) / seconds;
    std::printf("server: %.0f messages/s received and echoed (%.0f frames/s through the server)\n", serverRate,
                serverRate * 2.0);
    std::printf("budget: >= 20,000 messages/s with 200 connections -> %s\n",
                complete && serverRate >= 20000.0 ? "MET" : "NOT MET");
    return complete && failed == 0 && serverRate >= 20000.0 ? 0 : 4;
}
