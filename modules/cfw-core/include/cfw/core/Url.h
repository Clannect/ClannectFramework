#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw {

// An absolute URL (RFC 3986): scheme ":" ["//" authority] path ["?" query]
// ["#" fragment]. URLs reach CFW from creators, the network and the web view,
// so parsing is strict: no whitespace, no control characters, no malformed
// percent escapes, bounded length. Relative references are not URLs here;
// resolve them against a base before constructing one.
//
// Components are kept in their encoded (on-the-wire) form. The scheme and host
// are lowercased (they are case-insensitive), so equality and "is this https"
// checks cannot be fooled by case.
//
// Threads: a value type. Allocates: the component strings.
class Url {
public:
    static constexpr std::size_t kMaxLength = 8192;

    Url() = default;

    [[nodiscard]] static Result<Url> parse(StringView text);
    // file:/// URL for a local path. Backslashes become '/', and a Windows
    // drive path gets the leading '/' (file:///C:/Users/...).
    [[nodiscard]] static Url fromLocalPath(StringView path);

    [[nodiscard]] const String &scheme() const noexcept { return m_scheme; }
    [[nodiscard]] const String &userInfo() const noexcept { return m_userInfo; }
    [[nodiscard]] const String &host() const noexcept { return m_host; }
    [[nodiscard]] std::optional<std::uint16_t> port() const noexcept { return m_port; }
    [[nodiscard]] const String &path() const noexcept { return m_path; }
    [[nodiscard]] const String &query() const noexcept { return m_query; }
    [[nodiscard]] const String &fragment() const noexcept { return m_fragment; }
    [[nodiscard]] bool hasAuthority() const noexcept { return m_hasAuthority; }

    // The port, or the scheme's default (http 80, https 443, ws 80, wss 443).
    [[nodiscard]] std::optional<std::uint16_t> effectivePort() const noexcept;
    // For file: URLs, the decoded local path (C:/Users/... on Windows).
    [[nodiscard]] Result<String> toLocalPath() const;

    [[nodiscard]] String toString() const;

    // Resolves a reference (as found in an HTTP Location header or a link)
    // against this URL, per RFC 3986 §5.2: absolute URLs replace it,
    // "//host/..." keeps the scheme, "/path" keeps the authority, relative
    // paths merge with this path, and "." / ".." segments are removed. The
    // result is validated like parse().
    [[nodiscard]] Result<Url> resolve(StringView reference) const;

    // Scheme, host and effective port equal: same origin for security
    // decisions (e.g. whether credentials may follow a redirect).
    [[nodiscard]] bool sameOrigin(const Url &other) const noexcept;

    friend bool operator==(const Url &a, const Url &b) = default;
    friend std::ostream &operator<<(std::ostream &out, const Url &url) { return out << url.toString(); }

private:
    String m_scheme;
    String m_userInfo;
    String m_host;
    std::optional<std::uint16_t> m_port;
    String m_path;
    String m_query;
    String m_fragment;
    bool m_hasAuthority = false;
};

// Percent-encodes every byte except the RFC 3986 unreserved characters
// (A-Z a-z 0-9 - . _ ~) and any listed in `keep`. Hex digits are uppercase.
[[nodiscard]] String percentEncode(StringView text, StringView keep = {});
// Decodes %XX escapes. Fails on a malformed escape; does not treat '+' as space.
[[nodiscard]] Result<String> percentDecode(StringView text);

// application/x-www-form-urlencoded body or query: "a=1&b=two%20words".
[[nodiscard]] String encodeQuery(const std::vector<std::pair<String, String>> &items);
// The reverse: "a=1&b=two%20words&c" -> {a, 1}, {b, two words}, {c, ""}, in
// order. '+' is a space. Fails on a malformed percent escape.
[[nodiscard]] Result<std::vector<std::pair<String, String>>> decodeQuery(StringView query);
// The decoded value of the first item named `key`; nothing if there is none
// or the query is malformed. Replaces QUrlQuery::queryItemValue.
[[nodiscard]] std::optional<String> queryValue(StringView query, StringView key);

} // namespace cfw
