#include "cfw/io/JsonReader.h"
#include "cfw/core/CharConv.h"

#include <charconv>
#include <cmath>
#include <system_error>

#include "cfw/core/Utf8.h"

namespace cfw {

namespace {

class Parser {
public:
    Parser(StringView text, const JsonLimits &limits) : m_text(text), m_limits(limits) {}

    Result<JsonValue> run();

private:
    struct Frame {
        bool isObject = false;
        JsonArray array;
        std::vector<JsonObject::Member> members;
        String pendingKey;
    };

    enum class State { Value, ValueOrEnd, Key, KeyOrEnd, After };

    [[nodiscard]] Error error(ErrorCode code, const char *message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < m_pos && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        return Error(code, message)
            .with("offset", std::to_string(m_pos))
            .with("line", std::to_string(line))
            .with("column", std::to_string(column));
    }
    [[nodiscard]] Error syntax(const char *message) const { return error(ErrorCode::ParseError, message); }

    void skipWhitespace() noexcept {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_pos;
        }
    }
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_text.size(); }
    [[nodiscard]] char peek() const noexcept { return m_text[m_pos]; }
    [[nodiscard]] bool consumeLiteral(StringView word) noexcept {
        if (m_text.substr(m_pos, word.size()) == word) {
            m_pos += word.size();
            return true;
        }
        return false;
    }

    Result<void> pushFrame(bool isObject) {
        if (m_stack.size() >= m_limits.maxDepth) {
            return error(ErrorCode::LimitExceeded, "document nested too deeply");
        }
        m_stack.push_back(Frame{});
        m_stack.back().isObject = isObject;
        ++m_pos;
        return success();
    }

    // Places a finished value into the enclosing container (or as the root).
    void attach(JsonValue value) {
        if (m_stack.empty()) {
            m_root = std::move(value);
            m_haveRoot = true;
            return;
        }
        Frame &top = m_stack.back();
        if (top.isObject) {
            top.members.emplace_back(std::move(top.pendingKey), std::move(value));
            top.pendingKey = String();
        } else {
            top.array.push_back(std::move(value));
        }
    }

    void closeFrame() {
        ++m_pos;
        Frame frame = std::move(m_stack.back());
        m_stack.pop_back();
        if (frame.isObject) {
            attach(JsonValue(JsonObject::fromMembers(std::move(frame.members))));
        } else {
            attach(JsonValue(std::move(frame.array)));
        }
    }

    Result<String> parseString();
    Result<JsonValue> parseNumber();
    Result<JsonValue> parseScalar();

    StringView m_text;
    const JsonLimits &m_limits;
    std::size_t m_pos = 0;
    std::vector<Frame> m_stack;
    JsonValue m_root;
    bool m_haveRoot = false;
};

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Result<String> Parser::parseString() {
    // m_pos is on the opening quote.
    ++m_pos;
    String out;
    while (true) {
        if (atEnd()) {
            return syntax("unterminated string");
        }
        if (out.size() > m_limits.maxStringBytes) {
            return error(ErrorCode::LimitExceeded, "string too long");
        }
        const char c = peek();
        if (c == '"') {
            ++m_pos;
            return out;
        }
        if (c == '\\') {
            if (m_pos + 1 >= m_text.size()) {
                return syntax("unterminated escape");
            }
            const char e = m_text[m_pos + 1];
            m_pos += 2;
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                const auto readUnit = [this]() -> int {
                    if (m_pos + 4 > m_text.size()) {
                        return -1;
                    }
                    int unit = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int d = hexDigit(m_text[m_pos + static_cast<std::size_t>(i)]);
                        if (d < 0) {
                            return -1;
                        }
                        unit = unit * 16 + d;
                    }
                    m_pos += 4;
                    return unit;
                };
                const int unit = readUnit();
                if (unit < 0) {
                    return syntax("invalid \\u escape");
                }
                char32_t cp = static_cast<char32_t>(unit);
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // A high surrogate must be followed by an escaped low one.
                    const std::size_t save = m_pos;
                    if (m_text.substr(m_pos, 2) == "\\u") {
                        m_pos += 2;
                        const int low = readUnit();
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char32_t>(low) - 0xDC00);
                        } else {
                            m_pos = save; // not a pair: re-read it as its own escape
                            cp = kReplacementCharacter;
                        }
                    } else {
                        cp = kReplacementCharacter;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = kReplacementCharacter; // lone low surrogate
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                m_pos -= 1;
                return syntax("invalid escape");
            }
            continue;
        }
        // Raw bytes: copy a run up to the next quote or backslash, validating UTF-8.
        const std::size_t start = m_pos;
        while (m_pos < m_text.size() && m_text[m_pos] != '"' && m_text[m_pos] != '\\') {
            const auto byte = static_cast<unsigned char>(m_text[m_pos]);
            if (byte < 0x80u) {
                ++m_pos;
                continue;
            }
            const Utf8Char decoded = decodeUtf8At(m_text, m_pos);
            if (!decoded.valid) {
                return syntax("invalid UTF-8 in string");
            }
            m_pos += decoded.length;
        }
        out.append(m_text.substr(start, m_pos - start));
    }
}

Result<JsonValue> Parser::parseNumber() {
    const std::size_t start = m_pos;
    bool integral = true;
    if (!atEnd() && peek() == '-') {
        ++m_pos;
    }
    const auto digits = [this]() {
        const std::size_t from = m_pos;
        while (!atEnd() && peek() >= '0' && peek() <= '9') {
            ++m_pos;
        }
        return m_pos - from;
    };
    if (atEnd() || !(peek() >= '0' && peek() <= '9')) {
        return syntax("invalid number");
    }
    if (peek() == '0') {
        ++m_pos; // no leading zeros: "01" is two tokens, rejected by the caller
    } else {
        (void)digits();
    }
    if (!atEnd() && peek() == '.') {
        integral = false;
        ++m_pos;
        if (digits() == 0) {
            return syntax("invalid number: digits expected after '.'");
        }
    }
    bool negativeExponent = false;
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
        integral = false;
        ++m_pos;
        if (!atEnd() && (peek() == '+' || peek() == '-')) {
            negativeExponent = peek() == '-';
            ++m_pos;
        }
        if (digits() == 0) {
            return syntax("invalid number: digits expected in exponent");
        }
    }

    const StringView literal = m_text.substr(start, m_pos - start);
    const char *first = literal.data();
    const char *last = literal.data() + literal.size();
    if (integral) {
        std::int64_t value = 0;
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec == std::errc() && ptr == last) {
            return JsonValue(value);
        }
        // Too big for int64: fall through to double.
    }
    double value = 0.0;
    const auto [ptr, ec] = fromChars(first, last, value);
    if (ec == std::errc::result_out_of_range) {
        if (negativeExponent) {
            // Underflow: the nearest double is (signed) zero.
            return JsonValue(literal.front() == '-' ? -0.0 : 0.0);
        }
        m_pos = start;
        return syntax("number out of range");
    }
    if (ec != std::errc() || ptr != last || !std::isfinite(value)) {
        m_pos = start;
        return syntax("invalid number");
    }
    return JsonValue(value);
}

Result<JsonValue> Parser::parseScalar() {
    const char c = peek();
    if (c == '"') {
        Result<String> text = parseString();
        if (!text) {
            return std::move(text).error();
        }
        return JsonValue(std::move(text).value());
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        return parseNumber();
    }
    if (consumeLiteral("true")) {
        return JsonValue(true);
    }
    if (consumeLiteral("false")) {
        return JsonValue(false);
    }
    if (consumeLiteral("null")) {
        return JsonValue(nullptr);
    }
    return syntax("illegal value");
}

Result<JsonValue> Parser::run() {
    if (m_text.size() > m_limits.maxBytes) {
        return Error(ErrorCode::LimitExceeded, "JSON document too large")
            .with("bytes", std::to_string(m_text.size()));
    }
    if (m_text.starts_with("\xEF\xBB\xBF")) {
        m_pos = 3;
    }

    State state = State::Value;
    while (true) {
        skipWhitespace();
        if (state == State::After && m_stack.empty()) {
            break;
        }
        if (atEnd()) {
            return syntax(m_stack.empty() && !m_haveRoot ? "empty document" : "unexpected end of document");
        }
        const char c = peek();
        switch (state) {
        case State::ValueOrEnd:
            if (c == ']') {
                closeFrame();
                state = State::After;
                break;
            }
            [[fallthrough]];
        case State::Value:
            if (c == '{') {
                if (auto ok = pushFrame(true); !ok) {
                    return std::move(ok).error();
                }
                state = State::KeyOrEnd;
            } else if (c == '[') {
                if (auto ok = pushFrame(false); !ok) {
                    return std::move(ok).error();
                }
                state = State::ValueOrEnd;
            } else {
                Result<JsonValue> scalar = parseScalar();
                if (!scalar) {
                    return std::move(scalar).error();
                }
                attach(std::move(scalar).value());
                state = State::After;
            }
            break;
        case State::KeyOrEnd:
            if (c == '}') {
                closeFrame();
                state = State::After;
                break;
            }
            [[fallthrough]];
        case State::Key: {
            if (c != '"') {
                return syntax("object key (a string) expected");
            }
            Result<String> key = parseString();
            if (!key) {
                return std::move(key).error();
            }
            skipWhitespace();
            if (atEnd() || peek() != ':') {
                return syntax("':' expected after object key");
            }
            ++m_pos;
            m_stack.back().pendingKey = std::move(key).value();
            state = State::Value;
            break;
        }
        case State::After: {
            const bool inObject = m_stack.back().isObject;
            if (c == ',') {
                ++m_pos;
                state = inObject ? State::Key : State::Value;
            } else if (c == (inObject ? '}' : ']')) {
                closeFrame();
            } else {
                return syntax(inObject ? "',' or '}' expected" : "',' or ']' expected");
            }
            break;
        }
        }
    }

    if (!atEnd()) {
        return syntax("unexpected content after the document");
    }
    return std::move(m_root);
}

} // namespace

Result<JsonValue> parseJson(StringView text, const JsonLimits &limits) {
    Parser parser(text, limits);
    return parser.run();
}

} // namespace cfw
