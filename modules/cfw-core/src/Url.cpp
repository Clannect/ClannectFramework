#include "cfw/core/Url.h"

#include "cfw/core/Strings.h"

namespace cfw {

namespace {

constexpr bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isHexDigit(char c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
constexpr bool isUnreserved(char c) noexcept {
    return isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
constexpr bool isSubDelim(char c) noexcept {
    return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' ||
           c == ',' || c == ';' || c == '=';
}

int hexValue(char c) noexcept {
    if (isDigit(c)) {
        return c - '0';
    }
    return toLowerAscii(c) - 'a' + 10;
}

Error urlError(const char *message, std::size_t offset) {
    return Error(ErrorCode::ParseError, message).with("offset", std::to_string(offset));
}

// Checks one component: every byte is unreserved, a sub-delimiter, one of
// `extra`, or a well-formed %XX escape. Non-ASCII bytes (raw UTF-8, as typed
// into an address bar) are accepted; spaces and control characters never are.
Result<void> validateComponent(StringView text, StringView extra, std::size_t base) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size()) {
                return urlError("truncated percent escape", base + i);
            }
            if (!isHexDigit(text[i + 1]) || !isHexDigit(text[i + 2])) {
                return urlError("malformed percent escape", base + i);
            }
            i += 2;
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x80u) {
            continue;
        }
        if (isUnreserved(c) || isSubDelim(c) || extra.find(c) != StringView::npos) {
            continue;
        }
        return urlError("character not allowed in URL", base + i);
    }
    return success();
}

std::optional<std::uint16_t> defaultPort(StringView scheme) noexcept {
    if (scheme == "http" || scheme == "ws") {
        return 80;
    }
    if (scheme == "https" || scheme == "wss") {
        return 443;
    }
    return std::nullopt;
}

} // namespace

Result<Url> Url::parse(StringView text) {
    if (text.empty()) {
        return Error(ErrorCode::ParseError, "empty URL");
    }
    if (text.size() > kMaxLength) {
        return Error(ErrorCode::LimitExceeded, "URL too long").with("length", std::to_string(text.size()));
    }

    Url url;

    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    const std::size_t colon = text.find(':');
    if (colon == StringView::npos || colon == 0 || !isAlpha(text[0])) {
        return urlError("URL has no scheme", 0);
    }
    for (std::size_t i = 0; i < colon; ++i) {
        const char c = text[i];
        if (!isAlpha(c) && !isDigit(c) && c != '+' && c != '-' && c != '.') {
            return urlError("invalid character in scheme", i);
        }
    }
    url.m_scheme = toLowerAscii(text.substr(0, colon));
    std::size_t pos = colon + 1;

    // Fragment and query split off from the end first.
    StringView rest = text.substr(pos);
    if (const std::size_t hash = rest.find('#'); hash != StringView::npos) {
        const StringView fragment = rest.substr(hash + 1);
        if (auto ok = validateComponent(fragment, ":@/?", pos + hash + 1); !ok) {
            return std::move(ok).error();
        }
        url.m_fragment = String(fragment);
        rest = rest.substr(0, hash);
    }
    if (const std::size_t question = rest.find('?'); question != StringView::npos) {
        const StringView query = rest.substr(question + 1);
        if (auto ok = validateComponent(query, ":@/?", pos + question + 1); !ok) {
            return std::move(ok).error();
        }
        url.m_query = String(query);
        rest = rest.substr(0, question);
    }

    // Authority.
    if (rest.starts_with("//")) {
        url.m_hasAuthority = true;
        rest = rest.substr(2);
        pos += 2;
        const std::size_t slash = rest.find('/');
        StringView authority = rest.substr(0, slash);
        const std::size_t authorityStart = pos;
        rest = slash == StringView::npos ? StringView() : rest.substr(slash);
        pos += authority.size();

        if (const std::size_t at = authority.rfind('@'); at != StringView::npos) {
            const StringView userInfo = authority.substr(0, at);
            if (auto ok = validateComponent(userInfo, ":", authorityStart); !ok) {
                return std::move(ok).error();
            }
            url.m_userInfo = String(userInfo);
            authority = authority.substr(at + 1);
        }

        StringView host = authority;
        StringView portText;
        if (authority.starts_with('[')) {
            // IPv6 literal: [hex:hex...]
            const std::size_t close = authority.find(']');
            if (close == StringView::npos) {
                return urlError("unterminated IPv6 address", authorityStart);
            }
            host = authority.substr(0, close + 1);
            for (char c : host.substr(1, host.size() - 2)) {
                if (!isHexDigit(c) && c != ':' && c != '.') {
                    return urlError("invalid IPv6 address", authorityStart);
                }
            }
            const StringView after = authority.substr(close + 1);
            if (!after.empty()) {
                if (after[0] != ':') {
                    return urlError("junk after IPv6 address", authorityStart);
                }
                portText = after.substr(1);
            }
        } else if (const std::size_t portColon = authority.rfind(':'); portColon != StringView::npos) {
            host = authority.substr(0, portColon);
            portText = authority.substr(portColon + 1);
            if (auto ok = validateComponent(host, "", authorityStart); !ok) {
                return std::move(ok).error();
            }
        } else if (auto ok = validateComponent(host, "", authorityStart); !ok) {
            return std::move(ok).error();
        }

        if (!portText.empty()) {
            const Result<std::int64_t> port = parseInt(portText);
            if (!port || port.value() < 0 || port.value() > 65535) {
                return urlError("invalid port", authorityStart);
            }
            url.m_port = static_cast<std::uint16_t>(port.value());
        }
        url.m_host = toLowerAscii(host);
    }

    if (auto ok = validateComponent(rest, ":@/", pos); !ok) {
        return std::move(ok).error();
    }
    url.m_path = String(rest);
    return url;
}

Url Url::fromLocalPath(StringView path) {
    String normalized(path);
    for (char &c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    // Windows drive path (C:/...) needs a leading slash: file:///C:/...
    if (normalized.size() >= 2 && isAlpha(normalized[0]) && normalized[1] == ':') {
        normalized.insert(normalized.begin(), '/');
    }
    Url url;
    url.m_scheme = "file";
    url.m_hasAuthority = true;
    url.m_path = percentEncode(normalized, "/:");
    return url;
}

Result<String> Url::toLocalPath() const {
    if (m_scheme != "file") {
        return Error(ErrorCode::InvalidArgument, "not a file: URL").with("scheme", m_scheme);
    }
    Result<String> decoded = percentDecode(m_path);
    if (!decoded) {
        return decoded;
    }
    String path = std::move(decoded).value();
    // "/C:/x" -> "C:/x"
    if (path.size() >= 3 && path[0] == '/' && isAlpha(path[1]) && path[2] == ':') {
        path.erase(0, 1);
    }
    return path;
}

std::optional<std::uint16_t> Url::effectivePort() const noexcept {
    return m_port ? m_port : defaultPort(m_scheme);
}

namespace {

// RFC 3986 §5.2.4.
String removeDotSegments(StringView input) {
    String output;
    while (!input.empty()) {
        if (input.starts_with("../")) {
            input.remove_prefix(3);
        } else if (input.starts_with("./")) {
            input.remove_prefix(2);
        } else if (input.starts_with("/./")) {
            input.remove_prefix(2); // "/./x" -> "/x"
        } else if (input == "/.") {
            input = "/";
        } else if (input.starts_with("/../") || input == "/..") {
            input = input == "/.." ? StringView("/") : input.substr(3);
            const std::size_t slash = output.rfind('/');
            output.erase(slash == String::npos ? 0 : slash);
        } else if (input == "." || input == "..") {
            input = {};
        } else {
            // Move the first segment (with its leading '/', if any) to the output.
            const std::size_t next = input.find('/', input.front() == '/' ? 1 : 0);
            const std::size_t length = next == StringView::npos ? input.size() : next;
            output.append(input.substr(0, length));
            input.remove_prefix(length);
        }
    }
    return output;
}

bool hasScheme(StringView reference) noexcept {
    const std::size_t colon = reference.find(':');
    if (colon == StringView::npos || colon == 0 || !isAlpha(reference[0])) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        const char c = reference[i];
        if (!isAlpha(c) && !isDigit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

} // namespace

Result<Url> Url::resolve(StringView reference) const {
    if (hasScheme(reference)) {
        Result<Url> absolute = parse(reference);
        if (absolute) {
            absolute.value().m_path = removeDotSegments(absolute.value().m_path);
        }
        return absolute;
    }

    StringView rest = reference;
    String fragment;
    if (const std::size_t hash = rest.find('#'); hash != StringView::npos) {
        fragment = String(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    bool hasQuery = false;
    String query;
    if (const std::size_t question = rest.find('?'); question != StringView::npos) {
        hasQuery = true;
        query = String(rest.substr(question + 1));
        rest = rest.substr(0, question);
    }

    String authority;
    bool withAuthority = m_hasAuthority;
    if (m_hasAuthority) {
        if (!m_userInfo.empty()) {
            authority = m_userInfo + "@";
        }
        authority += m_host;
        if (m_port) {
            authority += ":" + std::to_string(*m_port);
        }
    }

    String path;
    if (rest.starts_with("//")) {
        // Network-path reference: new authority, keep only the scheme.
        const StringView afterSlashes = rest.substr(2);
        const std::size_t slash = afterSlashes.find('/');
        authority = String(afterSlashes.substr(0, slash));
        withAuthority = true;
        path = slash == StringView::npos ? String() : removeDotSegments(afterSlashes.substr(slash));
    } else if (rest.empty()) {
        path = m_path;
        if (!hasQuery) {
            query = m_query;
            hasQuery = !m_query.empty();
        }
    } else if (rest.front() == '/') {
        path = removeDotSegments(rest);
    } else {
        // Merge with the base path (RFC 3986 §5.2.3).
        String merged;
        if (m_hasAuthority && m_path.empty()) {
            merged = "/" + String(rest);
        } else {
            const std::size_t slash = m_path.rfind('/');
            merged = (slash == String::npos ? String() : m_path.substr(0, slash + 1)) + String(rest);
        }
        path = removeDotSegments(merged);
    }

    String text = m_scheme + ":";
    if (withAuthority) {
        text += "//" + authority;
    }
    text += path;
    if (hasQuery) {
        text += "?" + query;
    }
    if (!fragment.empty()) {
        text += "#" + fragment;
    }
    return parse(text);
}

bool Url::sameOrigin(const Url &other) const noexcept {
    return m_scheme == other.m_scheme && m_host == other.m_host && effectivePort() == other.effectivePort();
}

String Url::toString() const {
    String out = m_scheme;
    out += ':';
    if (m_hasAuthority) {
        out += "//";
        if (!m_userInfo.empty()) {
            out += m_userInfo;
            out += '@';
        }
        out += m_host;
        if (m_port) {
            out += ':';
            out += std::to_string(*m_port);
        }
    }
    out += m_path;
    if (!m_query.empty()) {
        out += '?';
        out += m_query;
    }
    if (!m_fragment.empty()) {
        out += '#';
        out += m_fragment;
    }
    return out;
}

String percentEncode(StringView text, StringView keep) {
    constexpr char kHex[] = "0123456789ABCDEF";
    String out;
    out.reserve(text.size());
    for (char c : text) {
        if (isUnreserved(c) || keep.find(c) != StringView::npos) {
            out += c;
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        }
    }
    return out;
}

Result<String> percentDecode(StringView text) {
    String out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) {
            return urlError("truncated percent escape", i);
        }
        if (!isHexDigit(text[i + 1]) || !isHexDigit(text[i + 2])) {
            return urlError("malformed percent escape", i);
        }
        out += static_cast<char>(hexValue(text[i + 1]) << 4 | hexValue(text[i + 2]));
        i += 2;
    }
    return out;
}

String encodeQuery(const std::vector<std::pair<String, String>> &items) {
    String out;
    for (const auto &[key, value] : items) {
        if (!out.empty()) {
            out += '&';
        }
        out += percentEncode(key);
        out += '=';
        out += percentEncode(value);
    }
    return out;
}

} // namespace cfw
