// HTTP/1.1: the body decoder in isolation (every framing, hostile inputs), then
// the client end to end against a scripted local server: redirects and their
// security policy, streaming, limits, timeouts, cancellation.

#include "cfw/net/HttpBodyDecoder.h"
#include "cfw/net/HttpClient.h"

#include <map>

#include "cfw/core/Strings.h"

#include "cfw/net/EventLoop.h"
#include "cfw/net/Executor.h"
#include "cfw/net/TcpListener.h"
#include "cfw/test/Check.h"

using namespace cfw;
using namespace std::chrono_literals;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Span<const std::byte> bytesOf(StringView s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }

// Parses `responseText` as a head, then decodes the rest; feeds byte by byte
// if asked. Returns the body or the error.
Result<String> decode(StringView responseText, bool byteByByte = false, std::size_t maxBody = 1 << 20,
                      bool closeAtEnd = true, StringView method = "GET") {
    const auto parsed = parseHttpResponseHead(responseText);
    if (!parsed || !parsed.value()) {
        return Error(ErrorCode::ParseError, "bad head in test");
    }
    Result<HttpBodyDecoder> decoder = HttpBodyDecoder::forResponse(parsed.value()->head, method, maxBody);
    if (!decoder) {
        return std::move(decoder).error();
    }
    String body;
    const auto sink = [&](Span<const std::byte> b) { body.append(reinterpret_cast<const char *>(b.data()), b.size()); };
    const StringView rest = responseText.substr(parsed.value()->consumed);
    bool complete = false;
    if (byteByByte) {
        for (std::size_t i = 0; i < rest.size() && !complete; ++i) {
            Result<bool> fed = decoder.value().feed(bytesOf(rest.substr(i, 1)), sink);
            if (!fed) {
                return std::move(fed).error();
            }
            complete = fed.value();
        }
    } else {
        Result<bool> fed = decoder.value().feed(bytesOf(rest), sink);
        if (!fed) {
            return std::move(fed).error();
        }
        complete = fed.value();
    }
    if (!complete && closeAtEnd) {
        if (Result<void> finished = decoder.value().finishOnClose(); !finished) {
            return std::move(finished).error();
        }
    } else if (!complete) {
        return Error(ErrorCode::Unknown, "incomplete");
    }
    return body;
}

void decoderFramings() {
    checkEqual(decode("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA").valueOr("ERR"), String("hello"),
               "Content-Length stops at the length");
    checkEqual(decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\nX-Trailer: y\r\n\r\n",
                      true)
                   .valueOr("ERR"),
               String("hello, world"), "chunked with an extension and a trailer, fed byte by byte");
    checkEqual(decode("HTTP/1.1 200 OK\r\n\r\nuntil the end").valueOr("ERR"), String("until the end"),
               "no length: read until close");
    checkEqual(decode("HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n").valueOr("ERR"), String(""),
               "204 has no body whatever the headers say");
    checkEqual(decode("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n", false, 1 << 20, true, "HEAD").valueOr("ERR"),
               String(""), "HEAD responses have no body");
    checkEqual(decode("HTTP/1.1 200 OK\r\nContent-Length: 3, 3\r\n\r\nabc").valueOr("ERR"), String("abc"),
               "repeated identical Content-Length is fine");
}

void decoderRejectsHostileResponses() {
    check(!decode("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd").ok(),
          "conflicting Content-Length");
    check(!decode("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n").ok(), "negative Content-Length");
    check(!decode("HTTP/1.1 200 OK\r\nContent-Length: 1e3\r\n\r\n").ok(), "non-numeric Content-Length");
    const auto gzip = decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");
    check(!gzip.ok() && gzip.error().code() == ErrorCode::Unsupported, "unsupported transfer coding");
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n").ok(), "non-hex chunk size");
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFFF\r\n").ok(),
          "chunk size overflow");
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n0\r\n\r\n").ok(),
          "chunk data not followed by CRLF");
    // Only the CRLF rule catches this one: without it, "1" would be skipped and
    // "2\r\nde" would decode as a valid chunk.
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc12\r\nde\r\n0\r\n\r\n").ok(),
          "a stray byte where CRLF belongs is an error, not skipped");
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + String(5000, '1')).ok(),
          "overlong chunk-size line");
    check(!decode("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort").ok(), "truncated Content-Length body");
    check(!decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel").ok(), "truncated chunked body");
    const auto tooBig = decode("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", false, 50);
    check(!tooBig.ok() && tooBig.error().code() == ErrorCode::LimitExceeded, "Content-Length over the limit, from the header");
    const auto tooBigChunks =
        decode("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n20\r\n" + String(32, 'a') + "\r\n20\r\n", false, 50);
    check(!tooBigChunks.ok() && tooBigChunks.error().code() == ErrorCode::LimitExceeded, "chunks over the limit");
    const auto tooBigStream = decode("HTTP/1.1 200 OK\r\n\r\n" + String(100, 'a'), false, 50);
    check(!tooBigStream.ok() && tooBigStream.error().code() == ErrorCode::LimitExceeded, "close-delimited over the limit");
}

// A scripted server: each request gets the response its path selects.
struct Server {
    std::unique_ptr<TcpListener> listener;
    std::vector<std::unique_ptr<TcpConnection>> connections;
    std::map<String, String> routes; // path -> raw response ("" = never answer)
    std::vector<String> requests;    // raw request heads received
    std::vector<String> bodies;

    Server(EventLoop &loop) {
        listener = TcpListener::listen(loop, "127.0.0.1", 0).value();
        listener->setOnAccept([this](std::unique_ptr<TcpConnection> c) {
            TcpConnection *raw = c.get();
            auto buffer = std::make_shared<String>();
            raw->setOnData([this, raw, buffer](Span<const std::byte> d) {
                buffer->append(reinterpret_cast<const char *>(d.data()), d.size());
                const auto parsed = parseHttpRequestHead(*buffer);
                if (!parsed || !parsed.value()) {
                    return;
                }
                const HttpHead &head = parsed.value()->head;
                std::size_t length = 0;
                if (const String *cl = head.header("Content-Length")) {
                    length = static_cast<std::size_t>(parseInt(*cl).valueOr(0));
                }
                if (buffer->size() < parsed.value()->consumed + length) {
                    return; // wait for the body
                }
                requests.push_back(buffer->substr(0, parsed.value()->consumed));
                bodies.push_back(buffer->substr(parsed.value()->consumed, length));
                const String path = head.target.substr(0, head.target.find('?'));
                const auto it = routes.find(path);
                const String response = it == routes.end() ? "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n" : it->second;
                buffer->clear();
                if (!response.empty()) {
                    raw->send(response);
                    raw->close();
                }
            });
            connections.push_back(std::move(c));
        });
    }
    [[nodiscard]] String url(StringView path) const { return "http://127.0.0.1:" + std::to_string(listener->port()) + String(path); }
};

struct Fixture {
    std::unique_ptr<EventLoop> loop = EventLoop::create().value();
    Executor resolver{1};
    HttpClient client{*loop, resolver};
    Server server{*loop};

    Result<HttpResponse> run(HttpRequest request, HttpOptions options = {}) {
        std::optional<Result<HttpResponse>> result;
        ConnectRequest handle = client.send(std::move(request), [&](Result<HttpResponse> r) { result = std::move(r); },
                                            std::move(options));
        loop->runUntil([&] { return result.has_value(); }, 10s);
        if (!result) {
            return Error(ErrorCode::Timeout, "test: no callback");
        }
        return std::move(*result);
    }
    Result<HttpResponse> get(StringView path, HttpOptions options = {}) {
        HttpRequest request;
        request.url = Url::parse(server.url(path)).value();
        return run(std::move(request), std::move(options));
    }
};

void basicRequests() {
    Fixture f;
    f.server.routes["/hello"] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
    f.server.routes["/chunked"] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n";
    f.server.routes["/eof"] = "HTTP/1.1 200 OK\r\n\r\nuntil close";
    f.server.routes["/continue"] = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";

    const auto hello = f.get("/hello?x=1");
    check(hello.ok() && hello.value().status == 200 && hello.value().bodyText() == "hello", "Content-Length response");
    check(hello.ok() && hello.value().header("content-type") && *hello.value().header("content-type") == "text/plain",
          "headers are case-insensitive");
    const String &request = f.server.requests.back();
    check(request.starts_with("GET /hello?x=1 HTTP/1.1\r\nHost: 127.0.0.1:"), "request line and Host");
    check(request.find("Accept-Encoding: identity") != String::npos, "asks for no compression");

    const auto chunked = f.get("/chunked");
    check(chunked.ok() && chunked.value().bodyText() == "abcdef", "chunked response");
    const auto eof = f.get("/eof");
    check(eof.ok() && eof.value().bodyText() == "until close", "close-delimited response");
    const auto created = f.get("/continue");
    check(created.ok() && created.value().status == 201 && created.value().bodyText() == "ok", "100 Continue is skipped");
    const auto missing = f.get("/nope");
    check(missing.ok() && missing.value().status == 404, "a 404 is a successful exchange, not an error");

    HttpRequest post;
    post.method = "POST";
    post.url = Url::parse(f.server.url("/hello")).value();
    post.headers = {{"Content-Type", "application/json"}};
    const String json = R"({"figma":"file"})";
    post.body.assign(bytesOf(json).begin(), bytesOf(json).end());
    check(f.run(post).ok(), "POST succeeds");
    checkEqual(f.server.bodies.back(), json, "server received the POST body");
    check(f.server.requests.back().find("Content-Length: 16\r\n") != String::npos, "with its Content-Length");
}

void redirectsAndTheirPolicy() {
    Fixture f;
    Server other(*f.loop); // a different origin (different port)
    f.server.routes["/start"] = "HTTP/1.1 302 Found\r\nLocation: /middle\r\nContent-Length: 4\r\n\r\njunk";
    f.server.routes["/middle"] = "HTTP/1.1 301 Moved\r\nLocation: ../end?final=1\r\nContent-Length: 0\r\n\r\n";
    f.server.routes["/end"] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone";
    f.server.routes["/loop"] = "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n";
    f.server.routes["/away"] = "HTTP/1.1 307 Temporary\r\nLocation: " + other.url("/landing") + "\r\nContent-Length: 0\r\n\r\n";
    f.server.routes["/see-other"] = "HTTP/1.1 303 See Other\r\nLocation: /end\r\nContent-Length: 0\r\n\r\n";
    other.routes["/landing"] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

    const auto chain = f.get("/start");
    check(chain.ok() && chain.value().status == 200 && chain.value().bodyText() == "done", "redirect chain followed");
    check(chain.ok() && chain.value().redirects == 2, "two hops counted");
    check(chain.ok() && chain.value().url.toString().ends_with("/end?final=1"), "relative Location resolved");

    const std::size_t before = f.server.requests.size();
    const auto loopResult = f.get("/loop");
    check(!loopResult.ok() && loopResult.error().code() == ErrorCode::LimitExceeded, "redirect loop stops at the limit");
    checkEqual(f.server.requests.size() - before, std::size_t(6), "exactly the original request plus 5 hops");

    HttpOptions noFollow;
    noFollow.maxRedirects = 0;
    const auto raw = f.get("/start", noFollow);
    check(raw.ok() && raw.value().status == 302 && raw.value().bodyText() == "junk", "maxRedirects 0 returns the 3xx");

    HttpRequest withAuth;
    withAuth.url = Url::parse(f.server.url("/away")).value();
    withAuth.headers = {{"Authorization", "Bearer secret-figma-token"}, {"X-Keep", "yes"}};
    const auto away = f.run(withAuth);
    check(away.ok() && away.value().bodyText() == "hi", "cross-origin redirect followed");
    check(f.server.requests.back().find("secret-figma-token") != String::npos, "the original origin got the token");
    check(other.requests.back().find("secret-figma-token") == String::npos,
          "the token is NOT sent to a different origin");
    check(other.requests.back().find("X-Keep: yes") != String::npos, "other headers still follow");

    HttpRequest post;
    post.method = "POST";
    post.url = Url::parse(f.server.url("/see-other")).value();
    post.body = std::vector<std::byte>(bytesOf("payload").begin(), bytesOf("payload").end());
    const auto seeOther = f.run(post);
    check(seeOther.ok() && f.server.requests.back().starts_with("GET /end "), "303 continues as GET");
    check(f.server.bodies.back().empty(), "without the body");
}

void streamingLimitsAndTimeouts() {
    Fixture f;
    f.server.routes["/big"] = "HTTP/1.1 200 OK\r\nContent-Length: 300000\r\n\r\n" + String(300000, 'B');
    f.server.routes["/silent"] = ""; // accepts and never answers

    std::size_t streamed = 0;
    HttpOptions streaming;
    streaming.onBodyData = [&](Span<const std::byte> chunk) {
        streamed += chunk.size();
        return true;
    };
    const auto big = f.get("/big", streaming);
    check(big.ok() && big.value().body.empty(), "streamed body is not also buffered");
    checkEqual(streamed, std::size_t(300000), "every byte reached the stream callback");

    // Streaming through a redirect: only the final response's body is streamed.
    String streamedText;
    HttpOptions streamRedirect;
    streamRedirect.onBodyData = [&](Span<const std::byte> chunk) {
        streamedText.append(reinterpret_cast<const char *>(chunk.data()), chunk.size());
        return true;
    };
    f.server.routes["/start"] = "HTTP/1.1 302 Found\r\nLocation: /end\r\nContent-Length: 4\r\n\r\njunk";
    f.server.routes["/end"] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone";
    check(f.get("/start", streamRedirect).ok(), "streamed request through a redirect");
    checkEqual(streamedText, String("done"), "the redirect's own body is never streamed");

    HttpOptions stopEarly;
    stopEarly.onBodyData = [](Span<const std::byte>) { return false; };
    const auto stopped = f.get("/big", stopEarly);
    check(!stopped.ok() && stopped.error().code() == ErrorCode::Cancelled, "stream callback can cancel");

    HttpOptions small;
    small.maxBodyBytes = 1000;
    const auto limited = f.get("/big", small);
    check(!limited.ok() && limited.error().code() == ErrorCode::LimitExceeded, "body over the limit is refused");

    HttpOptions quick;
    quick.idleTimeout = 150ms;
    const Stopwatch watch;
    const auto silent = f.get("/silent", quick);
    check(!silent.ok() && silent.error().code() == ErrorCode::Timeout, "a silent server times out");
    check(watch.elapsedSeconds() < 2.0, "promptly");
}

void rejectsBadRequestsAndCancels() {
    Fixture f;
    HttpRequest injected;
    injected.url = Url::parse(f.server.url("/hello")).value();
    injected.headers = {{"X-Evil", "a\r\nInjected: yes"}};
    const auto bad = f.run(injected);
    check(!bad.ok() && bad.error().code() == ErrorCode::InvalidArgument, "CR/LF in a header value is refused");
    check(f.server.requests.empty(), "nothing was sent");

    HttpRequest smuggle;
    smuggle.url = injected.url;
    smuggle.headers = {{"Content-Length", "0"}};
    check(!f.run(smuggle).ok(), "framing headers are the client's to set");

    HttpRequest tls;
    tls.url = Url::parse("https://api.figma.com/v1/files/x").value();
    const auto https = f.run(tls);
    check(!https.ok() && https.error().code() == ErrorCode::Unsupported, "https reports Unsupported until TLS exists");

    f.server.routes["/silent"] = "";
    bool called = false;
    {
        ConnectRequest handle = f.client.get(Url::parse(f.server.url("/silent")).value(),
                                             [&](Result<HttpResponse>) { called = true; });
        f.loop->runUntil([&] { return !f.server.requests.empty(); }, 3s);
    } // handle destroyed: cancelled
    f.loop->runUntil([] { return false; }, 100ms);
    check(!called, "a cancelled request never calls back");
}

} // namespace

int main() {
    decoderFramings();
    decoderRejectsHostileResponses();
    basicRequests();
    redirectsAndTheirPolicy();
    streamingLimitsAndTimeouts();
    rejectsBadRequestsAndCancels();
    return cfw::test::finish("HttpTest");
}
