#include "cfw/net/HttpClient.h"

#include <algorithm>
#include <memory>
#include <optional>

#include "cfw/core/Strings.h"
#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/HttpBodyDecoder.h"
#include "cfw/net/TlsStream.h"
#include "cfw/net/WebSocketHandshake.h"

namespace cfw {

const String *HttpResponse::header(StringView name) const noexcept {
    for (const auto &[key, value] : headers) {
        if (equalsIgnoreCase(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

namespace {

bool isTokenChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           StringView("!#$%&'*+-.^_`|~").find(c) != StringView::npos;
}

bool isToken(StringView text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), isTokenChar);
}

// Field values may not contain CR, LF or NUL: that would inject headers.
bool isSafeValue(StringView text) noexcept { return text.find_first_of(StringView("\r\n\0", 3)) == StringView::npos; }

Result<void> validate(const HttpRequest &request) {
    if (!isToken(request.method)) {
        return Error(ErrorCode::InvalidArgument, "invalid HTTP method").with("method", request.method);
    }
    for (const auto &[name, value] : request.headers) {
        if (!isToken(name) || !isSafeValue(value)) {
            return Error(ErrorCode::InvalidArgument, "invalid HTTP header (bad name or CR/LF in the value)")
                .with("header", name);
        }
        for (StringView reserved : {"Host", "Content-Length", "Transfer-Encoding", "Connection"}) {
            if (equalsIgnoreCase(name, reserved)) {
                return Error(ErrorCode::InvalidArgument, "this header is set by the client").with("header", name);
            }
        }
    }
    return success();
}

String serialize(const HttpRequest &request) {
    const Url &url = request.url;
    String target = url.path().empty() ? String("/") : url.path();
    if (!url.query().empty()) {
        target += "?" + url.query();
    }
    String host = url.host();
    if (url.port()) {
        host += ":" + std::to_string(*url.port());
    }
    String out = request.method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    bool hasAcceptEncoding = false;
    for (const auto &[name, value] : request.headers) {
        hasAcceptEncoding = hasAcceptEncoding || equalsIgnoreCase(name, "Accept-Encoding");
        out += name + ": " + value + "\r\n";
    }
    if (!hasAcceptEncoding) {
        out += "Accept-Encoding: identity\r\n"; // no compression support yet
    }
    const bool bodyMethod = request.method == "POST" || request.method == "PUT" || request.method == "PATCH";
    if (!request.body.empty() || bodyMethod) {
        out += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
    }
    out += "\r\n";
    return out;
}

bool isRedirect(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

struct HttpCall final : detail::Cancellable, std::enable_shared_from_this<HttpCall> {
    EventLoop *loop = nullptr;
    Executor *resolver = nullptr;
    HttpClient::Callback callback;
    HttpOptions options;
    HttpRequest request;
    Url originalUrl;
    int redirects = 0;

    ConnectRequest connectRequest;
    ConnectRequest tlsRequest;
    std::unique_ptr<ByteStream> connection; // TCP, or TLS over TCP for https
    String head;
    bool inBody = false;
    bool deliverBody = true; // false while reading the body of a redirect we will follow
    std::optional<HttpBodyDecoder> decoder;
    HttpResponse response;

    TimerId totalTimer = 0;
    TimerId idleTimer = 0;
    bool done = false;

    [[nodiscard]] bool isDone() const override { return done; }

    void releaseConnection() {
        connectRequest.cancel();
        tlsRequest.cancel();
        if (connection) {
            connection->abort();
            connection.reset();
        }
    }

    void cancel() override {
        if (done) {
            return;
        }
        done = true;
        callback = nullptr;
        releaseConnection();
        if (totalTimer != 0) {
            loop->cancelTimer(totalTimer);
            totalTimer = 0;
        }
        if (idleTimer != 0) {
            loop->cancelTimer(idleTimer);
            idleTimer = 0;
        }
    }

    void complete(Result<HttpResponse> result) {
        if (done) {
            return;
        }
        auto cb = std::move(callback);
        cancel();
        if (cb) {
            cb(std::move(result));
        }
    }

    void touch() {
        if (idleTimer != 0) {
            loop->cancelTimer(idleTimer);
        }
        std::weak_ptr<HttpCall> weak = shared_from_this();
        idleTimer = loop->startTimer(options.idleTimeout, [weak] {
            if (auto self = weak.lock()) {
                self->idleTimer = 0;
                self->complete(Error(ErrorCode::Timeout, "HTTP response stalled (idle timeout)"));
            }
        });
    }

    void start() {
        releaseConnection();
        head.clear();
        inBody = false;
        decoder.reset();
        response = HttpResponse{};

        const Url &url = request.url;
        const bool secure = url.scheme() == "https";
        if ((!secure && url.scheme() != "http") || url.host().empty()) {
            complete(Error(ErrorCode::InvalidArgument, "not an http:// or https:// URL").with("url", url.toString()));
            return;
        }
        std::weak_ptr<HttpCall> weak = shared_from_this();
        TcpConnection::ConnectOptions connectOptions;
        connectOptions.timeout = options.connectTimeout;
        touch();
        connectRequest = TcpConnection::connect(
            *loop, *resolver, url.host(), url.effectivePort().value_or(secure ? 443 : 80),
            [weak, secure](Result<std::unique_ptr<TcpConnection>> connected) {
                auto self = weak.lock();
                if (!self || self->done) {
                    return;
                }
                if (!connected || !secure) {
                    self->onConnected(connected ? Result<std::unique_ptr<ByteStream>>(std::move(connected).value())
                                                : Result<std::unique_ptr<ByteStream>>(std::move(connected).error()));
                    return;
                }
                TlsOptions tls = self->options.tls;
                tls.handshakeTimeout = std::min(tls.handshakeTimeout, self->options.connectTimeout);
                self->tlsRequest = TlsStream::startClient(*self->loop, *self->resolver, std::move(connected).value(),
                                                          self->request.url.host(), std::move(tls),
                                                          [weak](Result<std::unique_ptr<ByteStream>> secured) {
                                                              if (auto s = weak.lock()) {
                                                                  s->onConnected(std::move(secured));
                                                              }
                                                          });
            },
            connectOptions);
    }

    void onConnected(Result<std::unique_ptr<ByteStream>> connected) {
        if (done) {
            return;
        }
        if (!connected) {
            complete(std::move(connected).error().with("url", request.url.toString()));
            return;
        }
        connection = std::move(connected).value();
        std::weak_ptr<HttpCall> weak = shared_from_this();
        connection->setOnData([weak](Span<const std::byte> data) {
            if (auto self = weak.lock()) {
                self->onData(data);
            }
        });
        connection->setOnClosed([weak](const std::optional<Error> &error) {
            if (auto self = weak.lock()) {
                self->onClosed(error);
            }
        });
        touch();
        connection->send(serialize(request));
        if (!request.body.empty()) {
            connection->send(request.body);
        }
    }

    void onData(Span<const std::byte> data) {
        if (done) {
            return;
        }
        touch();
        if (!inBody) {
            head.append(reinterpret_cast<const char *>(data.data()), data.size());
            while (true) {
                Result<std::optional<ParsedHead>> parsed = parseHttpResponseHead(head, options.maxHeadBytes);
                if (!parsed) {
                    complete(std::move(parsed).error());
                    return;
                }
                if (!parsed.value()) {
                    return; // head incomplete
                }
                ParsedHead &ph = *parsed.value();
                if (ph.head.status >= 100 && ph.head.status < 200) {
                    head.erase(0, ph.consumed); // interim response (100 Continue): skip it
                    continue;
                }
                response.status = ph.head.status;
                response.reason = ph.head.reason;
                response.headers = std::move(ph.head.headers);
                response.url = request.url;
                response.redirects = redirects;
                HttpHead headCopy;
                headCopy.status = response.status;
                headCopy.headers = response.headers;
                Result<HttpBodyDecoder> body = HttpBodyDecoder::forResponse(headCopy, request.method, options.maxBodyBytes);
                if (!body) {
                    complete(std::move(body).error());
                    return;
                }
                decoder = std::move(body).value();
                deliverBody = !(options.maxRedirects > 0 && isRedirect(response.status) &&
                                response.header("Location") != nullptr);
                inBody = true;
                const String rest = head.substr(ph.consumed);
                head.clear();
                feedBody(Span<const std::byte>(reinterpret_cast<const std::byte *>(rest.data()), rest.size()));
                return;
            }
        }
        feedBody(data);
    }

    void feedBody(Span<const std::byte> data) {
        if (done) {
            return;
        }
        bool cancelled = false;
        Result<bool> fed = decoder->feed(data, [&](Span<const std::byte> bytes) {
            if (!deliverBody || cancelled) {
                return;
            }
            if (options.onBodyData) {
                cancelled = !options.onBodyData(bytes);
            } else {
                response.body.insert(response.body.end(), bytes.begin(), bytes.end());
            }
        });
        if (done) {
            return; // the body callback cancelled the request
        }
        if (cancelled) {
            complete(Error(ErrorCode::Cancelled, "body stream cancelled by the receiver"));
            return;
        }
        if (!fed) {
            complete(std::move(fed).error());
            return;
        }
        if (fed.value()) {
            finishResponse();
        }
    }

    void onClosed(const std::optional<Error> &error) {
        connection.reset();
        if (done) {
            return;
        }
        if (!inBody) {
            complete(error ? *error : Error(ErrorCode::NetworkError, "connection closed before a response"));
            return;
        }
        if (Result<void> finished = decoder->finishOnClose(); !finished) {
            complete(error ? *error : std::move(finished).error());
            return;
        }
        finishResponse();
    }

    void finishResponse() {
        releaseConnection();
        const String *location = response.header("Location");
        if (options.maxRedirects <= 0 || !isRedirect(response.status) || location == nullptr) {
            complete(std::move(response));
            return;
        }
        if (redirects >= options.maxRedirects) {
            complete(Error(ErrorCode::LimitExceeded, "too many redirects").with("url", request.url.toString()));
            return;
        }
        Result<Url> next = request.url.resolve(*location);
        if (!next) {
            complete(std::move(next).error().with("location", *location));
            return;
        }
        if (request.url.scheme() == "https" && next.value().scheme() == "http") {
            complete(Error(ErrorCode::PermissionDenied, "refusing to follow a redirect from https to http")
                         .with("location", next.value().toString()));
            return;
        }
        if (!next.value().sameOrigin(originalUrl)) {
            std::erase_if(request.headers, [](const auto &header) {
                return equalsIgnoreCase(header.first, "Authorization") || equalsIgnoreCase(header.first, "Cookie") ||
                       equalsIgnoreCase(header.first, "Proxy-Authorization");
            });
        }
        const bool toGet = response.status == 303 ? request.method != "HEAD"
                                                  : (response.status == 301 || response.status == 302) &&
                                                        request.method == "POST";
        if (toGet) {
            request.method = "GET";
            request.body.clear();
            std::erase_if(request.headers, [](const auto &header) {
                return equalsIgnoreCase(header.first, "Content-Type");
            });
        }
        request.url = std::move(next).value();
        ++redirects;
        start();
    }
};

} // namespace

ConnectRequest HttpClient::send(HttpRequest request, Callback callback, HttpOptions options) {
    auto call = std::make_shared<HttpCall>();
    call->loop = &m_loop;
    call->resolver = &m_resolver;
    call->callback = std::move(callback);
    call->options = std::move(options);
    call->originalUrl = request.url;
    call->request = std::move(request);

    std::weak_ptr<HttpCall> weak = call;
    if (Result<void> valid = validate(call->request); !valid) {
        // Report on the next turn: callbacks never run inside send().
        m_loop.post([weak, error = valid.error()] {
            if (auto self = weak.lock()) {
                self->complete(error);
            }
        });
        return ConnectRequest(std::move(call));
    }
    call->totalTimer = m_loop.startTimer(call->options.totalTimeout, [weak] {
        if (auto self = weak.lock()) {
            self->totalTimer = 0;
            self->complete(Error(ErrorCode::Timeout, "HTTP request timed out"));
        }
    });
    // Start on the next turn too, so the caller holds the handle first.
    m_loop.post([weak] {
        if (auto self = weak.lock(); self && !self->done) {
            self->start();
        }
    });
    return ConnectRequest(std::move(call));
}

} // namespace cfw
