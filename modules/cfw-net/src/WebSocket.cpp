#include "cfw/net/WebSocket.h"

#include <random>

#include "cfw/core/Base64.h"
#include "cfw/core/Contract.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TlsStream.h"
#include "cfw/net/WebSocketHandshake.h"
#include "cfw/net/WebSocketServer.h"

namespace cfw {

namespace {

Span<const std::byte> bytesOf(StringView text) {
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

StringView textOf(Span<const std::byte> bytes) { return {reinterpret_cast<const char *>(bytes.data()), bytes.size()}; }

} // namespace

// --- WebSocket --------------------------------------------------------------

std::unique_ptr<WebSocket> WebSocket::adopt(EventLoop &loop, std::unique_ptr<ByteStream> connection, WsRole role,
                                            Options options, Span<const std::byte> leftover) {
    std::unique_ptr<WebSocket> socket(new WebSocket(loop, std::move(connection), role, options));
    if (!leftover.empty()) {
        // Frames that arrived with the handshake are processed on the next
        // turn, after the owner has had a chance to set its callbacks.
        std::vector<std::byte> copy(leftover.begin(), leftover.end());
        WebSocket *raw = socket.get();
        const std::shared_ptr<bool> alive = socket->m_alive;
        loop.post([raw, alive, data = std::move(copy)] {
            if (*alive) {
                raw->onData(data);
            }
        });
    }
    return socket;
}

WebSocket::WebSocket(EventLoop &loop, std::unique_ptr<ByteStream> connection, WsRole role, Options options)
    : m_loop(loop), m_connection(std::move(connection)), m_role(role), m_options(options),
      m_decoder(role, options.limits), m_peer(m_connection->peerAddress()), m_maskRandom(std::random_device{}()) {
    m_connection->setOnData([this](Span<const std::byte> data) { onData(data); });
    m_connection->setOnClosed([this](const std::optional<Error> &) {
        // Whatever code was agreed (or 1006 if TCP simply went away).
        finish(m_closeCode, m_closeReason);
    });
}

WebSocket::~WebSocket() {
    *m_alive = false;
    if (m_closeTimer != 0) {
        m_loop.cancelTimer(m_closeTimer);
    }
}

void WebSocket::setOnDrained(std::function<void()> callback) {
    if (m_connection) {
        m_connection->setOnDrained(std::move(callback));
    }
}

std::size_t WebSocket::bufferedAmount() const noexcept { return m_connection ? m_connection->queuedBytes() : 0; }

void WebSocket::sendFrame(WsOpcode opcode, Span<const std::byte> payload) {
    std::optional<std::array<std::byte, 4>> mask;
    if (m_role == WsRole::Client) {
        const std::uint32_t bits = static_cast<std::uint32_t>(m_maskRandom());
        mask = std::array<std::byte, 4>{static_cast<std::byte>(bits), static_cast<std::byte>(bits >> 8),
                                        static_cast<std::byte>(bits >> 16), static_cast<std::byte>(bits >> 24)};
    }
    m_frame.clear();
    appendWsFrame(m_frame, opcode, payload, true, mask);
    m_connection->send(m_frame);
}

void WebSocket::sendBinary(Span<const std::byte> data) {
    if (m_state == State::Open) {
        sendFrame(WsOpcode::Binary, data);
    }
}

void WebSocket::sendText(StringView text) {
    if (m_state == State::Open) {
        sendFrame(WsOpcode::Text, bytesOf(text));
    }
}

void WebSocket::ping(Span<const std::byte> payload) {
    if (m_state == State::Open) {
        sendFrame(WsOpcode::Ping, payload.first(std::min<std::size_t>(payload.size(), 125)));
    }
}

void WebSocket::close(std::uint16_t code, StringView reason) {
    if (m_state != State::Open) {
        return;
    }
    require(ws::isSendableCloseCode(code), "WebSocket::close: code may not be sent in a close frame");
    sendFrame(WsOpcode::Close, encodeWsClosePayload(code, reason));
    m_state = State::Closing;
    // If the peer never answers, give up after the timeout.
    const std::shared_ptr<bool> alive = m_alive;
    m_closeTimer = m_loop.startTimer(m_options.closeTimeout, [this, alive] {
        if (*alive) {
            m_closeTimer = 0;
            m_connection->abort();
            finish(ws::kAbnormalClosure, "close handshake timed out");
        }
    });
}

void WebSocket::failConnection(std::uint16_t code, StringView reason) {
    if (m_state == State::Open) {
        sendFrame(WsOpcode::Close, encodeWsClosePayload(code, reason));
    }
    m_state = State::Closing;
    m_closeCode = code;
    m_closeReason = String(reason);
    m_connection->close(); // flush the close frame, then end TCP; finish() follows
}

void WebSocket::onData(Span<const std::byte> data) {
    if (m_state == State::Closed) {
        return;
    }
    const std::shared_ptr<bool> alive = m_alive;
    m_decoder.feed(data);
    while (true) {
        Result<std::optional<WsMessage>> next = m_decoder.next();
        if (!next) {
            failConnection(wsCloseCodeOf(next.error()), next.error().message());
            return;
        }
        if (!next.value()) {
            return; // need more bytes
        }
        WsMessage &message = *next.value();
        switch (message.kind) {
        case WsMessage::Kind::Binary:
            if (m_state == State::Open && m_onBinary) {
                const auto callback = m_onBinary;
                callback(message.payload);
            }
            break;
        case WsMessage::Kind::Text:
            if (m_state == State::Open && m_onText) {
                const auto callback = m_onText;
                callback(message.text());
            }
            break;
        case WsMessage::Kind::Ping:
            if (m_state == State::Open) {
                sendFrame(WsOpcode::Pong, message.payload);
            }
            break;
        case WsMessage::Kind::Pong:
            if (m_onPong) {
                const auto callback = m_onPong;
                callback(message.payload);
            }
            break;
        case WsMessage::Kind::Close:
            m_closeCode = message.closeCode;
            m_closeReason = String(message.text());
            if (m_state == State::Open) {
                // Echo the close (with its code; none if the peer sent none).
                const std::vector<std::byte> echo =
                    message.closeCode == ws::kNoStatus ? std::vector<std::byte>{}
                                                       : encodeWsClosePayload(message.closeCode, {});
                sendFrame(WsOpcode::Close, echo);
                m_state = State::Closing;
            }
            // Handshake complete either way: end TCP once the echo is out.
            m_connection->close();
            return;
        }
        if (!*alive) {
            return;
        }
    }
}

void WebSocket::finish(std::uint16_t code, StringView reason) {
    if (m_state == State::Closed) {
        return;
    }
    m_state = State::Closed;
    if (m_closeTimer != 0) {
        m_loop.cancelTimer(m_closeTimer);
        m_closeTimer = 0;
    }
    if (m_onClosed) {
        auto callback = std::move(m_onClosed);
        m_onClosed = nullptr;
        const String reasonCopy(reason); // `reason` may point into this object
        callback(code, reasonCopy);
    }
}

// --- Client connect ---------------------------------------------------------

namespace {

struct ClientHandshake final : detail::Cancellable, std::enable_shared_from_this<ClientHandshake> {
    EventLoop *loop = nullptr;
    WebSocket::ConnectCallback callback;
    WebSocket::Options options;
    ConnectRequest tcpRequest;
    ConnectRequest tlsRequest;
    std::unique_ptr<ByteStream> tcp; // TCP, or TLS over TCP for wss
    String key;
    String received;
    TimerId timer = 0;
    bool done = false;

    [[nodiscard]] bool isDone() const override { return done; }

    void cancel() override {
        if (done) {
            return;
        }
        done = true;
        callback = nullptr;
        tcpRequest.cancel();
        tlsRequest.cancel();
        tcp.reset();
        if (timer != 0) {
            loop->cancelTimer(timer);
            timer = 0;
        }
    }

    void complete(Result<std::unique_ptr<WebSocket>> result) {
        if (done) {
            return;
        }
        auto cb = std::move(callback);
        cancel(); // releases the TCP attempt, timer and any leftover connection
        if (cb) {
            cb(std::move(result));
        }
    }
};

} // namespace

ConnectRequest WebSocket::connect(EventLoop &loop, Executor &resolver, const Url &url, ConnectCallback callback,
                                  ConnectOptions options) {
    auto state = std::make_shared<ClientHandshake>();
    state->loop = &loop;
    state->callback = std::move(callback);
    state->options = options.socket;

    const bool secure = url.scheme() == "wss";
    if (!secure && url.scheme() != "ws") {
        const Error error = Error(ErrorCode::InvalidArgument, "not a ws:// or wss:// URL").with("url", url.toString());
        std::weak_ptr<ClientHandshake> weak = state;
        loop.post([weak, error] {
            if (auto s = weak.lock()) {
                s->complete(error);
            }
        });
        return ConnectRequest(std::move(state));
    }

    std::random_device entropy;
    std::array<std::byte, 16> nonce{};
    for (std::byte &b : nonce) {
        b = static_cast<std::byte>(entropy() & 0xFF);
    }
    state->key = base64Encode(nonce);
    const String host = url.host();
    const std::uint16_t port = url.effectivePort().value_or(secure ? 443 : 80);
    const String target = (url.path().empty() ? String("/") : url.path()) + (url.query().empty() ? "" : "?" + url.query());

    std::weak_ptr<ClientHandshake> weak = state;
    state->timer = loop.startTimer(options.timeout, [weak] {
        if (auto s = weak.lock()) {
            s->timer = 0;
            s->complete(Error(ErrorCode::Timeout, "WebSocket connect timed out"));
        }
    });

    // The HTTP upgrade, over TCP or over TLS.
    const auto upgrade = [weak, host, port, target](std::unique_ptr<ByteStream> stream) {
        auto s = weak.lock();
        if (!s || s->done) {
            return;
        }
        s->tcp = std::move(stream);
        s->tcp->setOnClosed([weak](const std::optional<Error> &error) {
            if (auto st = weak.lock()) {
                st->complete(error ? *error : Error(ErrorCode::NetworkError, "connection closed during handshake"));
            }
        });
        s->tcp->setOnData([weak](Span<const std::byte> data) {
            auto st = weak.lock();
            if (!st || st->done) {
                return;
            }
            st->received.append(textOf(data));
            Result<std::optional<ParsedHead>> head = parseHttpResponseHead(st->received);
            if (!head) {
                st->complete(std::move(head).error());
                return;
            }
            if (!head.value()) {
                return; // the head is not complete yet
            }
            if (Result<void> accepted = wsCheckServerResponse(head.value()->head, st->key); !accepted) {
                st->complete(std::move(accepted).error());
                return;
            }
            const StringView leftover = StringView(st->received).substr(head.value()->consumed);
            std::unique_ptr<WebSocket> socket =
                WebSocket::adopt(*st->loop, std::move(st->tcp), WsRole::Client, st->options, bytesOf(leftover));
            st->complete(std::move(socket));
        });
        s->tcp->send(wsClientRequest(host, port, target, s->key));
    };

    TcpConnection::ConnectOptions tcpOptions;
    tcpOptions.timeout = options.timeout;
    state->tcpRequest = TcpConnection::connect(
        loop, resolver, host, port,
        [weak, secure, host, upgrade, &resolver, tls = options.tls](Result<std::unique_ptr<TcpConnection>> connected) {
            auto s = weak.lock();
            if (!s || s->done) {
                return;
            }
            if (!connected) {
                s->complete(std::move(connected).error());
                return;
            }
            if (!secure) {
                upgrade(std::move(connected).value());
                return;
            }
            s->tlsRequest = TlsStream::startClient(*s->loop, resolver, std::move(connected).value(), host, tls,
                                                   [weak, upgrade](Result<std::unique_ptr<ByteStream>> secured) {
                                                       auto st = weak.lock();
                                                       if (!st || st->done) {
                                                           return;
                                                       }
                                                       if (!secured) {
                                                           st->complete(std::move(secured).error());
                                                           return;
                                                       }
                                                       upgrade(std::move(secured).value());
                                                   });
        },
        tcpOptions);
    return ConnectRequest(std::move(state));
}

// --- Server -----------------------------------------------------------------

struct WebSocketServer::Pending {
    std::unique_ptr<TcpConnection> connection;
    String received;
    TimerId timer = 0;
    bool rejecting = false; // a 400 is being flushed; drop once TCP closes
};

Result<std::unique_ptr<WebSocketServer>> WebSocketServer::listen(EventLoop &loop, StringView address,
                                                                  std::uint16_t port, Options options) {
    Result<std::unique_ptr<TcpListener>> listener = TcpListener::listen(loop, address, port);
    if (!listener) {
        return std::move(listener).error();
    }
    return std::unique_ptr<WebSocketServer>(new WebSocketServer(loop, std::move(listener).value(), options));
}

WebSocketServer::WebSocketServer(EventLoop &loop, std::unique_ptr<TcpListener> listener, Options options)
    : m_loop(loop), m_listener(std::move(listener)), m_options(options) {
    m_listener->setOnAccept([this](std::unique_ptr<TcpConnection> connection) { accept(std::move(connection)); });
}

WebSocketServer::~WebSocketServer() {
    *m_alive = false;
    for (Pending &pending : m_pending) {
        if (pending.timer != 0) {
            m_loop.cancelTimer(pending.timer);
        }
    }
}

void WebSocketServer::accept(std::unique_ptr<TcpConnection> connection) {
    if (m_pending.size() >= m_options.maxPendingHandshakes) {
        return; // over the limit: the connection is dropped as it goes out of scope
    }
    m_pending.push_back(Pending{std::move(connection), String(), 0, false});
    const auto it = std::prev(m_pending.end());
    const std::shared_ptr<bool> alive = m_alive;
    it->timer = m_loop.startTimer(m_options.handshakeTimeout, [this, alive, it] {
        if (*alive) {
            it->timer = 0;
            drop(it);
        }
    });
    it->connection->setOnData([this, it](Span<const std::byte> data) { onHandshakeData(it, data); });
    it->connection->setOnClosed([this, it](const std::optional<Error> &) { drop(it); });
}

void WebSocketServer::drop(std::list<Pending>::iterator it) {
    if (it->timer != 0) {
        m_loop.cancelTimer(it->timer);
    }
    m_pending.erase(it);
}

void WebSocketServer::onHandshakeData(std::list<Pending>::iterator it, Span<const std::byte> data) {
    if (it->rejecting) {
        return;
    }
    it->received.append(textOf(data));
    Result<std::optional<ParsedHead>> head = parseHttpRequestHead(it->received);
    if (head && !head.value()) {
        return; // wait for the rest of the head
    }
    Result<String> response = head ? wsServerResponse(head.value()->head) : Result<String>(head.error());
    if (!response) {
        it->rejecting = true;
        it->connection->send(httpErrorResponse(400, response.error().message()));
        it->connection->close(); // the closed callback drops the entry once flushed
        return;
    }
    it->connection->send(response.value());
    const String target = head.value()->head.target;
    const StringView leftover = StringView(it->received).substr(head.value()->consumed);
    std::unique_ptr<WebSocket> socket =
        WebSocket::adopt(m_loop, std::move(it->connection), WsRole::Server, m_options.socket, bytesOf(leftover));
    drop(it);
    if (m_onConnection) {
        m_onConnection(std::move(socket), target);
    }
}

} // namespace cfw
