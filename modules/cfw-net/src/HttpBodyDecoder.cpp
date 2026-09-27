#include "cfw/net/HttpBodyDecoder.h"

#include <algorithm>

#include "cfw/core/Strings.h"

namespace cfw {

namespace {

constexpr std::size_t kMaxLineBytes = 4096;

Error malformed(const char *message) { return Error(ErrorCode::ParseError, message); }

} // namespace

Result<HttpBodyDecoder> HttpBodyDecoder::forResponse(const HttpHead &head, StringView requestMethod,
                                                     std::size_t maxBodyBytes) {
    // No body at all, whatever the headers say (RFC 9112 §6.3).
    if (requestMethod == "HEAD" || (head.status >= 100 && head.status < 200) || head.status == 204 ||
        head.status == 304) {
        return HttpBodyDecoder(Framing::None, 0, maxBodyBytes);
    }

    // Transfer-Encoding wins over Content-Length. Only "chunked" is supported:
    // requests say "Accept-Encoding: identity", so nothing else should come.
    if (const String *encoding = head.header("Transfer-Encoding")) {
        const std::vector<StringView> codings = split(*encoding, ',', SplitMode::SkipEmpty);
        if (codings.size() == 1 && equalsIgnoreCase(trim(codings[0]), "chunked")) {
            return HttpBodyDecoder(Framing::Chunked, 0, maxBodyBytes);
        }
        return Error(ErrorCode::Unsupported, "unsupported Transfer-Encoding").with("value", *encoding);
    }

    // Content-Length: every occurrence (and every list element) must agree.
    std::optional<std::uint64_t> length;
    for (const auto &[name, value] : head.headers) {
        if (!equalsIgnoreCase(name, "Content-Length")) {
            continue;
        }
        for (StringView part : split(value, ',')) {
            const StringView digits = trim(part);
            if (digits.empty() || digits.find_first_not_of("0123456789") != StringView::npos || digits.size() > 18) {
                return malformed("invalid Content-Length");
            }
            const auto parsed = static_cast<std::uint64_t>(parseInt(digits).value());
            if (length && *length != parsed) {
                return malformed("conflicting Content-Length values");
            }
            length = parsed;
        }
    }
    if (length) {
        if (*length > maxBodyBytes) {
            return Error(ErrorCode::LimitExceeded, "response body too large")
                .with("content-length", std::to_string(*length));
        }
        return HttpBodyDecoder(*length == 0 ? Framing::None : Framing::Length, *length, maxBodyBytes);
    }
    return HttpBodyDecoder(Framing::UntilClose, 0, maxBodyBytes);
}

Result<void> HttpBodyDecoder::deliver(Span<const std::byte> bytes, const Sink &sink) {
    if (bytes.empty()) {
        return success();
    }
    m_bodyBytes += bytes.size();
    if (m_bodyBytes > m_maxBodyBytes) {
        return Error(ErrorCode::LimitExceeded, "response body too large");
    }
    sink(bytes);
    return success();
}

Result<bool> HttpBodyDecoder::feed(Span<const std::byte> data, const Sink &sink) {
    if (m_complete) {
        return true;
    }
    switch (m_framing) {
    case Framing::None:
        m_complete = true;
        return true;
    case Framing::UntilClose:
        if (auto ok = deliver(data, sink); !ok) {
            return std::move(ok).error();
        }
        return false;
    case Framing::Length: {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(m_remaining, data.size()));
        if (auto ok = deliver(data.first(take), sink); !ok) {
            return std::move(ok).error();
        }
        m_remaining -= take;
        m_complete = m_remaining == 0;
        return m_complete;
    }
    case Framing::Chunked:
        break;
    }

    // Chunked: size line, data, CRLF, ..., "0" line, trailers, empty line.
    std::size_t pos = 0;
    while (pos < data.size()) {
        switch (m_chunkState) {
        case ChunkState::Size:
        case ChunkState::Trailer: {
            const char c = static_cast<char>(data[pos++]);
            if (c != '\n') {
                m_line += c;
                if (m_line.size() > kMaxLineBytes) {
                    return malformed("chunk-size or trailer line too long");
                }
                continue;
            }
            if (m_line.empty() || m_line.back() != '\r') {
                return malformed("chunked line not terminated by CRLF");
            }
            m_line.pop_back();
            if (m_chunkState == ChunkState::Trailer) {
                if (m_line.empty()) {
                    m_complete = true; // blank line after the trailers: done
                    return true;
                }
                m_line.clear(); // trailer fields are ignored
                continue;
            }
            // Size line: hex digits, optionally ";extension".
            const StringView sizeText = trim(StringView(m_line).substr(0, m_line.find(';')));
            if (sizeText.empty() || sizeText.size() > 15) {
                return malformed("invalid chunk size");
            }
            std::uint64_t size = 0;
            for (char h : sizeText) {
                int d = -1;
                if (h >= '0' && h <= '9') {
                    d = h - '0';
                } else if (h >= 'a' && h <= 'f') {
                    d = h - 'a' + 10;
                } else if (h >= 'A' && h <= 'F') {
                    d = h - 'A' + 10;
                }
                if (d < 0) {
                    return malformed("invalid chunk size");
                }
                size = size * 16 + static_cast<std::uint64_t>(d);
            }
            m_line.clear();
            if (size == 0) {
                m_chunkState = ChunkState::Trailer;
            } else {
                if (m_bodyBytes + size > m_maxBodyBytes) {
                    return Error(ErrorCode::LimitExceeded, "response body too large");
                }
                m_remaining = size;
                m_chunkState = ChunkState::Data;
            }
            break;
        }
        case ChunkState::Data: {
            const std::size_t take =
                static_cast<std::size_t>(std::min<std::uint64_t>(m_remaining, data.size() - pos));
            if (auto ok = deliver(data.subspan(pos, take), sink); !ok) {
                return std::move(ok).error();
            }
            pos += take;
            m_remaining -= take;
            if (m_remaining == 0) {
                m_chunkState = ChunkState::DataEnd;
            }
            break;
        }
        case ChunkState::DataEnd: {
            // Exactly CRLF after each chunk's data.
            const char c = static_cast<char>(data[pos++]);
            m_line += c;
            if (m_line == "\r") {
                break;
            }
            if (m_line != "\r\n") {
                return malformed("chunk data not followed by CRLF");
            }
            m_line.clear();
            m_chunkState = ChunkState::Size;
            break;
        }
        }
    }
    return false;
}

Result<void> HttpBodyDecoder::finishOnClose() {
    if (m_complete || m_framing == Framing::UntilClose) {
        m_complete = true;
        return success();
    }
    return Error(ErrorCode::NetworkError, "connection closed before the response body was complete")
        .with("received", std::to_string(m_bodyBytes));
}

} // namespace cfw
