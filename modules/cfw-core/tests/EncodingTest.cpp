// Sha256 (NIST vectors), Uuid, and Url parsing/encoding. All three handle
// untrusted input or must agree bit-for-bit with other implementations.

#include "cfw/core/Base64.h"
#include "cfw/core/Sha1.h"
#include "cfw/core/Sha256.h"
#include "cfw/core/Url.h"
#include "cfw/core/Uuid.h"

#include <set>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void sha256MatchesNistVectors() {
    checkEqual(Sha256::toHex(Sha256::hash("")),
               String("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty input");
    checkEqual(Sha256::toHex(Sha256::hash("abc")),
               String("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc");
    checkEqual(Sha256::toHex(Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
               String("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "448-bit message");

    Sha256 million;
    const String chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) {
        million.update(chunk);
    }
    checkEqual(Sha256::toHex(million.finish()),
               String("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "one million 'a'");
}

void sha256IsIndependentOfChunking() {
    const String text = "The quick brown fox jumps over the lazy dog, repeatedly, across many blocks of input.";
    const auto whole = Sha256::hash(text);
    for (std::size_t step : {1u, 3u, 63u, 64u, 65u}) {
        Sha256 sha;
        for (std::size_t i = 0; i < text.size(); i += step) {
            sha.update(StringView(text).substr(i, step));
        }
        check(sha.finish() == whole, "same digest however the input is split");
    }
    // Padding edge: 55 and 56 bytes straddle the length field.
    checkEqual(Sha256::toHex(Sha256::hash(String(56, 'x'))).size(), std::size_t(64), "56-byte input");
}

void uuidsAreRandomVersion4() {
    std::set<Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const Uuid id = Uuid::generate();
        check(id.version() == 4, "version 4");
        check((id.bytes()[8] & 0xC0) == 0x80, "RFC variant");
        seen.insert(id);
    }
    checkEqual(seen.size(), std::size_t(1000), "1000 generated ids are distinct");
}

void uuidTextRoundTrips() {
    const Result<Uuid> parsed = Uuid::parse("{123E4567-E89B-12D3-A456-426614174000}");
    check(parsed.ok(), "braces and uppercase accepted");
    checkEqual(parsed.value().toString(), String("123e4567-e89b-12d3-a456-426614174000"), "canonical form");
    check(!Uuid::parse("123e4567e89b12d3a456426614174000").ok(), "missing hyphens rejected");
    check(!Uuid::parse("123e4567-e89b-12d3-a456-42661417400g").ok(), "non-hex rejected");
    check(Uuid().isNil(), "default is nil");
}

void parsesUrls() {
    const Result<Url> url = Url::parse("HTTPS://User:pw@Assets.Clannect.com:8443/a/b%20c?x=1&y=2#frag");
    check(url.ok(), "full URL parses");
    const Url &u = url.value();
    checkEqual(u.scheme(), String("https"), "scheme lowercased");
    checkEqual(u.host(), String("assets.clannect.com"), "host lowercased");
    checkEqual(u.userInfo(), String("User:pw"), "user info");
    check(u.port() == std::uint16_t(8443), "explicit port");
    checkEqual(u.path(), String("/a/b%20c"), "path stays encoded");
    checkEqual(u.query(), String("x=1&y=2"), "query");
    checkEqual(u.fragment(), String("frag"), "fragment");
    checkEqual(u.toString(), String("https://User:pw@assets.clannect.com:8443/a/b%20c?x=1&y=2#frag"), "round-trip");

    check(Url::parse("https://example.com").value().effectivePort() == std::uint16_t(443), "default https port");
    checkEqual(Url::parse("clannect://asset/123").value().host(), String("asset"), "custom scheme");
    checkEqual(Url::parse("mailto:someone@example.com").value().path(), String("someone@example.com"),
               "URL without authority");
    checkEqual(Url::parse("http://[::1]:8080/").value().host(), String("[::1]"), "IPv6 host");
}

void rejectsHostileUrls() {
    check(!Url::parse("").ok(), "empty");
    check(!Url::parse("no-scheme").ok(), "no scheme");
    check(!Url::parse("https://exa mple.com").ok(), "space in host");
    check(!Url::parse("https://example.com/a\nb").ok(), "newline in path");
    check(!Url::parse("https://example.com/%zz").ok(), "malformed escape");
    check(!Url::parse("https://example.com/%4").ok(), "truncated escape");
    check(!Url::parse("https://example.com:99999/").ok(), "port out of range");
    check(!Url::parse("https://example.com:-1/").ok(), "negative port");
    check(!Url::parse("1http://x").ok(), "scheme must start with a letter");
    check(!Url::parse("https://" + String(Url::kMaxLength, 'a')).ok(), "over-long URL");
}

void localPathsRoundTrip() {
    const Url url = Url::fromLocalPath("C:\\Users\\Creator\\My Game\\scene.cescene");
    checkEqual(url.toString(), String("file:///C:/Users/Creator/My%20Game/scene.cescene"), "file URL");
    checkEqual(url.toLocalPath().value(), String("C:/Users/Creator/My Game/scene.cescene"), "back to a path");
    check(!Url::parse("https://x/y").value().toLocalPath().ok(), "only file: URLs have local paths");
}

void percentEncoding() {
    checkEqual(percentEncode("a b/c?d=é"), String("a%20b%2Fc%3Fd%3D%C3%A9"), "encode reserved and UTF-8 bytes");
    checkEqual(percentEncode("a/b", "/"), String("a/b"), "kept characters");
    checkEqual(percentDecode("a%20b%2fc").value(), String("a b/c"), "decode either case");
    check(!percentDecode("100%").ok(), "lone percent rejected");

    checkEqual(fileUrlToPath("file:///home/me/a%20b.png").value_or(""), String("/home/me/a b.png"), "a file URL");
    checkEqual(fileUrlToPath("file://localhost/tmp/x").value_or(""), String("/tmp/x"), "localhost is local");
    checkEqual(fileUrlToPath("FILE:///C:/Users/x%C3%A9.txt").value_or(""), String("C:/Users/xé.txt"),
               "a Windows drive, UTF-8 escapes");
    check(!fileUrlToPath("file://server/share/x").has_value(), "another host is not local");
    check(!fileUrlToPath("https://example.com/x").has_value(), "not a file URL");
    const std::vector<String> dropped =
        pathsFromUriList("# from a file manager\r\nfile:///a/one.png\r\nhttp://x/y\r\nfile:///b/two%23.txt\r\n");
    check(dropped == std::vector<String>{"/a/one.png", "/b/two#.txt"}, "a uri-list gives its local files");
    checkEqual(encodeQuery({{"grant type", "client_credentials"}, {"id", "a&b"}}),
               String("grant%20type=client_credentials&id=a%26b"), "form encoding");
}

} // namespace

namespace {

String hex(const Sha1::Digest &d) {
    constexpr char kHex[] = "0123456789abcdef";
    String out;
    for (std::uint8_t b : d) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

String text(const std::vector<std::byte> &bytes) {
    return String(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

void sha1MatchesKnownVectors() {
    checkEqual(hex(Sha1::hash("")), String("da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");
    checkEqual(hex(Sha1::hash("abc")), String("a9993e364706816aba3e25717850c26c9cd0d89d"), "abc");
    checkEqual(hex(Sha1::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
               String("84983e441c3bd26ebaae4aa1f95129e5e54670f1"), "448-bit message");
    // The worked example from RFC 6455 section 1.3.
    const String accept = base64Encode(Span<const std::byte>(
        reinterpret_cast<const std::byte *>(Sha1::hash("dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11").data()),
        20));
    checkEqual(accept, String("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "RFC 6455 Sec-WebSocket-Accept example");
}

void base64MatchesRfc4648() {
    const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char *encoded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        checkEqual(base64Encode(plain[i]), String(encoded[i]), "RFC 4648 encode vector");
        const auto decoded = base64Decode(encoded[i]);
        check(decoded.ok() && text(decoded.value()) == plain[i], "RFC 4648 decode vector");
    }
}

void base64RejectsMalformedInput() {
    check(!base64Decode("Zg=").ok(), "length not a multiple of 4");
    check(!base64Decode("Z===").ok(), "too much padding");
    check(!base64Decode("Zg=a").ok(), "data after padding");
    check(!base64Decode("Zg==Zm9v").ok(), "padding in the middle");
    check(!base64Decode("Zm9v!A==").ok(), "invalid character");
    check(!base64Decode("Zh==").ok(), "non-canonical: unused bits set");
    check(!base64Decode("Zm9 v").ok(), "whitespace is not allowed");
}

} // namespace

namespace {

void resolvesPerRfc3986() {
    // Every example from RFC 3986 section 5.4 (normal and abnormal).
    const Url base = Url::parse("http://a/b/c/d;p?q").value();
    const char *cases[][2] = {
        {"g:h", "g:h"},
        {"g", "http://a/b/c/g"},
        {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"},
        {"//g", "http://g"},
        {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q#s"},
        {"g#s", "http://a/b/c/g#s"},
        {"g?y#s", "http://a/b/c/g?y#s"},
        {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"},
        {"g;x?y#s", "http://a/b/c/g;x?y#s"},
        {"", "http://a/b/c/d;p?q"},
        {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"},
        {"..", "http://a/b/"},
        {"../", "http://a/b/"},
        {"../g", "http://a/b/g"},
        {"../..", "http://a/"},
        {"../../", "http://a/"},
        {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"},
        {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"},
        {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."},
        {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."},
        {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"},
        {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"},
        {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"},
        {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g#s/./x"},
        {"g#s/../x", "http://a/b/c/g#s/../x"},
    };
    int wrong = 0;
    for (const auto &c : cases) {
        const Result<Url> resolved = base.resolve(c[0]);
        if (!resolved || resolved.value().toString() != c[1]) {
            ++wrong;
            std::printf("      resolve(\"%s\") = %s, expected %s\n", c[0],
                        resolved ? resolved.value().toString().c_str() : resolved.error().describe().c_str(), c[1]);
        }
    }
    checkEqual(wrong, 0, "all RFC 3986 section 5.4 examples resolve correctly");
    check(!base.resolve("http://bad host/").ok(), "a resolved URL is validated like parse()");

    const Url a = Url::parse("https://assets.clannect.com/a/1").value();
    check(a.sameOrigin(Url::parse("https://assets.clannect.com:443/b").value()), "default port is the same origin");
    check(!a.sameOrigin(Url::parse("http://assets.clannect.com/a/1").value()), "scheme differs");
    check(!a.sameOrigin(Url::parse("https://evil.example/a/1").value()), "host differs");
}

} // namespace

void hmacMatchesRfc4231() {
    const auto hex = [](const Sha256::Digest &d) { return Sha256::toHex(d); };
    checkEqual(hex(hmacSha256(String(20, '\x0b'), "Hi There")),
               String("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "RFC 4231 case 1");
    checkEqual(hex(hmacSha256("Jefe", "what do ya want for nothing?")),
               String("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231 case 2");
    checkEqual(hex(hmacSha256(String(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First")),
               String("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "RFC 4231 case 6 (long key)");
}

void decodesQueries() {
    const auto items = decodeQuery("a=1&b=two%20words&c&&d=x+y&=e");
    check(items.ok(), "query decodes");
    if (items.ok()) {
        const auto &v = items.value();
        check(v.size() == 5, "empty items skipped");
        check(v.size() == 5 && v[1].second == "two words" && v[2].first == "c" && v[2].second.empty() &&
                  v[3].second == "x y" && v[4].first.empty() && v[4].second == "e",
              "values decoded, '+' is a space");
    }
    check(!decodeQuery("a=%zz").ok(), "malformed escape rejected");
    check(queryValue("node-id=12%3A34&x=1", "node-id") == String("12:34"), "queryValue finds and decodes");
    check(!queryValue("x=1", "node-id"), "queryValue: missing key");
    const std::vector<std::pair<String, String>> round{{"k y", "v&=%"}, {"n", ""}};
    check(decodeQuery(encodeQuery(round)).value() == round, "encodeQuery round-trips");
}

int main() {
    hmacMatchesRfc4231();
    decodesQueries();
    resolvesPerRfc3986();
    sha1MatchesKnownVectors();
    base64MatchesRfc4648();
    base64RejectsMalformedInput();
    sha256MatchesNistVectors();
    sha256IsIndependentOfChunking();
    uuidsAreRandomVersion4();
    uuidTextRoundTrips();
    parsesUrls();
    rejectsHostileUrls();
    localPathsRoundTrip();
    percentEncoding();
    return cfw::test::finish("EncodingTest");
}
