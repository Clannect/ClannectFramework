// EventLoop, Executor and TCP over real loopback sockets.

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TcpConnection.h"
#include "cfw/net/TcpListener.h"

#include <atomic>
#include <thread>

#include "cfw/test/Check.h"

using namespace cfw;
using namespace std::chrono_literals;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::unique_ptr<EventLoop> makeLoop() {
    auto loop = EventLoop::create();
    check(loop.ok(), "event loop created");
    return std::move(loop).value();
}

void timersFireInOrderAndCancel() {
    auto loop = makeLoop();
    std::vector<int> order;
    loop->startTimer(30ms, [&] { order.push_back(3); });
    loop->startTimer(10ms, [&] { order.push_back(1); });
    const TimerId cancelled = loop->startTimer(15ms, [&] { order.push_back(99); });
    loop->startTimer(20ms, [&] { order.push_back(2); });
    loop->cancelTimer(cancelled);
    check(loop->runUntil([&] { return order.size() == 3; }, 2s), "all timers fired");
    check(order == std::vector<int>{1, 2, 3}, "in deadline order, cancelled one skipped");

    int ticks = 0;
    TimerId repeating = 0;
    repeating = loop->startRepeatingTimer(5ms, [&] {
        if (++ticks == 3) {
            loop->cancelTimer(repeating); // a timer may cancel itself
        }
    });
    loop->runUntil([] { return false; }, 60ms);
    checkEqual(ticks, 3, "repeating timer stops when it cancels itself");
}

void postWorksFromOtherThreads() {
    auto loop = makeLoop();
    std::atomic<int> received{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 250; ++i) {
                loop->post([&] { ++received; });
            }
        });
    }
    for (std::thread &t : threads) {
        t.join();
    }
    check(loop->runUntil([&] { return received == 1000; }, 2s), "1000 posts from 4 threads all ran");

    // stop() from another thread wakes a loop blocked with nothing to do.
    std::thread stopper([&] {
        std::this_thread::sleep_for(20ms);
        loop->stop();
    });
    const Stopwatch watch;
    loop->run();
    stopper.join();
    check(watch.elapsedSeconds() < 2.0, "run() returned promptly after stop()");
}

void executorRunsAndJoins() {
    std::atomic<int> done{0};
    {
        Executor executor(3);
        for (int i = 0; i < 100; ++i) {
            executor.submit([&] { ++done; });
        }
        while (done < 100) {
            std::this_thread::sleep_for(1ms);
        }
    } // destructor joins
    checkEqual(done.load(), 100, "every task ran");
}

struct EchoServer {
    std::unique_ptr<TcpListener> listener;
    std::vector<std::unique_ptr<TcpConnection>> clients;
};

void tcpEchoLoopback() {
    auto loop = makeLoop();
    Executor resolver(1);
    EchoServer server;
    server.listener = TcpListener::listen(*loop, "127.0.0.1", 0).value();
    server.listener->setOnAccept([&](std::unique_ptr<TcpConnection> c) {
        TcpConnection *raw = c.get();
        raw->setOnData([raw](Span<const std::byte> data) { raw->send(data); });
        server.clients.push_back(std::move(c));
    });

    std::unique_ptr<TcpConnection> client;
    String echoed;
    std::optional<Error> connectError;
    ConnectRequest request = TcpConnection::connect(*loop, resolver, "127.0.0.1", server.listener->port(),
                                                    [&](Result<std::unique_ptr<TcpConnection>> r) {
                                                        if (!r) {
                                                            connectError = r.error();
                                                            return;
                                                        }
                                                        client = std::move(r).value();
                                                        client->setOnData([&](Span<const std::byte> d) {
                                                            echoed.append(reinterpret_cast<const char *>(d.data()), d.size());
                                                        });
                                                        client->send("hello over TCP");
                                                    });
    check(loop->runUntil([&] { return echoed.size() == 14 || connectError; }, 3s), "echo arrives");
    checkEqual(echoed, String("hello over TCP"), "echoed bytes");

    // Backpressure: the server stops reading, so the kernel buffers fill and
    // send() must start queuing. (How much the OS absorbs first varies: Windows
    // loopback takes many megabytes.) Then reading resumes and every byte must
    // arrive, in order, and the drained callback must fire.
    TcpConnection &serverSide = *server.clients.front();
    serverSide.setReadPaused(true);
    std::uint64_t sentBytes = 0;
    std::uint64_t sentHash = 1469598103934665603ull;
    std::uint64_t receivedBytes = 0;
    std::uint64_t receivedHash = 1469598103934665603ull;
    const auto mix = [](std::uint64_t hash, Span<const std::byte> data) {
        for (std::byte b : data) {
            hash = (hash ^ std::to_integer<std::uint64_t>(b)) * 1099511628211ull;
        }
        return hash;
    };
    serverSide.setOnData([&](Span<const std::byte> d) {
        receivedBytes += d.size();
        receivedHash = mix(receivedHash, d);
    });
    String chunk(1024u * 1024u, 'x');
    for (int i = 0; i < 1024 && client->queuedBytes() == 0; ++i) {
        for (std::size_t k = 0; k < chunk.size(); k += 997) {
            chunk[k] = static_cast<char>('a' + (i + static_cast<int>(k)) % 26);
        }
        const Span<const std::byte> bytes(reinterpret_cast<const std::byte *>(chunk.data()), chunk.size());
        client->send(bytes);
        sentBytes += chunk.size();
        sentHash = mix(sentHash, bytes);
    }
    check(client->queuedBytes() > 0, "with the reader paused, send() eventually queues instead of blocking");
    bool drained = false;
    client->setOnDrained([&] { drained = true; });
    serverSide.setReadPaused(false);
    check(loop->runUntil([&] { return receivedBytes == sentBytes && drained; }, 60s), "everything delivered once reading resumes");
    checkEqual(receivedBytes, sentBytes, "byte count");
    check(receivedHash == sentHash, "and intact, in order");
    check(drained, "drained callback fired when the queue emptied");
    std::printf("      backpressure: queued after %llu MB\n", static_cast<unsigned long long>(sentBytes >> 20));

    // Orderly close from the client reaches the server as a clean close.
    bool serverSawClose = false;
    server.clients.front()->setOnClosed([&](const std::optional<Error> &error) { serverSawClose = !error; });
    client->close();
    check(loop->runUntil([&] { return serverSawClose; }, 3s), "server sees an orderly close");
}

void connectFailuresAreReported() {
    auto loop = makeLoop();
    Executor resolver(1);
    // A port with no listener: grab a free port, then close the listener.
    std::uint16_t port = 0;
    {
        auto temp = TcpListener::listen(*loop, "127.0.0.1", 0).value();
        port = temp->port();
    }
    std::optional<Error> error;
    ConnectRequest request = TcpConnection::connect(*loop, resolver, "127.0.0.1", port,
                                                    [&](Result<std::unique_ptr<TcpConnection>> r) {
                                                        error = r ? std::nullopt : std::optional<Error>(r.error());
                                                    });
    check(loop->runUntil([&] { return error.has_value(); }, 5s), "refused connection reports an error");

    // A name that needs DNS resolves on the executor.
    std::optional<Error> resolveError;
    bool resolvedAndConnected = false;
    auto listener = TcpListener::listen(*loop, "127.0.0.1", 0).value();
    listener->setOnAccept([](std::unique_ptr<TcpConnection>) {});
    ConnectRequest byName = TcpConnection::connect(*loop, resolver, "localhost.", listener->port(),
                                                   [&](Result<std::unique_ptr<TcpConnection>> r) {
                                                       if (r) {
                                                           resolvedAndConnected = true;
                                                       } else {
                                                           resolveError = r.error();
                                                       }
                                                   });
    loop->runUntil([&] { return resolvedAndConnected || resolveError; }, 5s);
    check(resolvedAndConnected || resolveError, "a host name resolves off the loop thread and completes");

    // Cancelling means the callback never runs.
    bool called = false;
    {
        ConnectRequest cancelled = TcpConnection::connect(*loop, resolver, "127.0.0.1", listener->port(),
                                                          [&](Result<std::unique_ptr<TcpConnection>>) { called = true; });
    }
    loop->runUntil([] { return false; }, 100ms);
    check(!called, "destroying the request cancels it");
}

void callbacksMayDestroyTheirConnection() {
    auto loop = makeLoop();
    Executor resolver(1);
    auto listener = TcpListener::listen(*loop, "127.0.0.1", 0).value();
    std::unique_ptr<TcpConnection> accepted;
    listener->setOnAccept([&](std::unique_ptr<TcpConnection> c) {
        accepted = std::move(c);
        accepted->setOnData([&](Span<const std::byte>) { accepted.reset(); }); // destroys itself mid-callback
    });
    std::unique_ptr<TcpConnection> client;
    ConnectRequest r = TcpConnection::connect(*loop, resolver, "127.0.0.1", listener->port(),
                                              [&](Result<std::unique_ptr<TcpConnection>> c) {
                                                  client = std::move(c).value();
                                                  client->send("boom");
                                              });
    check(loop->runUntil([&] { return client && !accepted; }, 3s) || (client && !accepted),
          "a connection destroyed inside its own data callback does not crash");
    bool clientClosed = false;
    if (client) {
        client->setOnClosed([&](const std::optional<Error> &) { clientClosed = true; });
    }
    check(loop->runUntil([&] { return clientClosed; }, 3s), "and the peer sees the connection end");
}

} // namespace

int main() {
    timersFireInOrderAndCancel();
    postWorksFromOtherThreads();
    executorRunsAndJoins();
    tcpEchoLoopback();
    connectFailuresAreReported();
    callbacksMayDestroyTheirConnection();
    return cfw::test::finish("EventLoopTest");
}
