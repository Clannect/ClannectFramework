#include "DBus.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace cfw::dbus {

namespace {

constexpr std::uint32_t kMaxMessage = 128u * 1024u * 1024u; // the specification's limit
constexpr std::uint32_t kMaxArray = 64u * 1024u * 1024u;

std::size_t alignmentOf(char type) {
    switch (type) {
    case 'y': case 'g': case 'v': return 1;
    case 'n': case 'q': return 2;
    case 'b': case 'i': case 'u': case 's': case 'o': case 'a': case 'h': return 4;
    case 'x': case 't': case 'd': case '(': case '{': return 8;
    default: return 1;
    }
}

// ---- Writing ------------------------------------------------------------------------------

class Writer {
public:
    std::vector<std::uint8_t> out;
    void align(std::size_t n) {
        while (out.size() % n != 0) {
            out.push_back(0);
        }
    }
    void raw(const void *data, std::size_t size) {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        out.insert(out.end(), bytes, bytes + size);
    }
    template <class T> void number(T value) {
        align(sizeof(T));
        std::uint8_t bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T)); // little-endian hosts only (x86, ARM)
        raw(bytes, sizeof(T));
    }
    void string(const String &text) {
        number(std::uint32_t(text.size()));
        raw(text.data(), text.size());
        out.push_back(0);
    }
    void signature(const String &text) {
        out.push_back(std::uint8_t(text.size()));
        raw(text.data(), text.size());
        out.push_back(0);
    }
    void value(const Value &v) {
        using K = Value::Kind;
        switch (v.kind) {
        case K::Byte: out.push_back(std::uint8_t(v.integer)); break;
        case K::Bool: number(std::uint32_t(v.integer != 0)); break;
        case K::Int16: number(std::int16_t(v.integer)); break;
        case K::UInt16: number(std::uint16_t(v.unsignedInteger)); break;
        case K::Int32: number(std::int32_t(v.integer)); break;
        case K::UInt32: number(std::uint32_t(v.unsignedInteger)); break;
        case K::Int64: number(std::int64_t(v.integer)); break;
        case K::UInt64: number(std::uint64_t(v.unsignedInteger)); break;
        case K::Double: number(v.real); break;
        case K::String: case K::ObjectPath: string(v.text); break;
        case K::Signature: signature(v.text); break;
        case K::Array: {
            align(4);
            const std::size_t lengthAt = out.size();
            number(std::uint32_t(0));
            align(alignmentOf(v.elementSignature.empty() ? 'y' : v.elementSignature[0]));
            const std::size_t start = out.size();
            for (const Value &item : v.items) {
                value(item);
            }
            const auto length = std::uint32_t(out.size() - start);
            std::memcpy(out.data() + lengthAt, &length, 4);
            break;
        }
        case K::Struct: case K::DictEntry:
            align(8);
            for (const Value &field : v.items) {
                value(field);
            }
            break;
        case K::Variant:
            signature(v.items.empty() ? String("s") : v.items[0].typeSignature());
            if (!v.items.empty()) {
                value(v.items[0]);
            }
            break;
        }
    }
};

// ---- Reading -------------------------------------------------------------------------------

class Reader {
public:
    Reader(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}
    std::size_t pos = 0;
    bool failed = false;

    bool align(std::size_t n) {
        while (pos % n != 0) {
            if (pos >= m_size) {
                return fail();
            }
            ++pos;
        }
        return true;
    }
    template <class T> T number() {
        T value{};
        if (!align(sizeof(T)) || pos + sizeof(T) > m_size) {
            fail();
            return value;
        }
        std::memcpy(&value, m_data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }
    String string() {
        const auto length = number<std::uint32_t>();
        if (failed || pos + length + 1 > m_size) {
            fail();
            return {};
        }
        String text(reinterpret_cast<const char *>(m_data + pos), length);
        pos += length + 1;
        return text;
    }
    String signature() {
        if (pos >= m_size) {
            fail();
            return {};
        }
        const std::size_t length = m_data[pos++];
        if (pos + length + 1 > m_size) {
            fail();
            return {};
        }
        String text(reinterpret_cast<const char *>(m_data + pos), length);
        pos += length + 1;
        return text;
    }
    // One complete type from `sig` at `at`, which moves past it.
    Value value(const String &sig, std::size_t &at, int depth = 0) {
        Value v;
        if (at >= sig.size() || depth > 32) {
            fail();
            return v;
        }
        using K = Value::Kind;
        const char type = sig[at++];
        switch (type) {
        case 'y':
            v.kind = K::Byte;
            if (pos >= m_size) {
                fail();
                break;
            }
            v.integer = m_data[pos++];
            v.unsignedInteger = std::uint64_t(v.integer);
            break;
        case 'b': v.kind = K::Bool; v.integer = number<std::uint32_t>() != 0; break;
        case 'n': v.kind = K::Int16; v.integer = number<std::int16_t>(); break;
        case 'q': v.kind = K::UInt16; v.unsignedInteger = number<std::uint16_t>(); v.integer = std::int64_t(v.unsignedInteger); break;
        case 'i': v.kind = K::Int32; v.integer = number<std::int32_t>(); break;
        case 'u': v.kind = K::UInt32; v.unsignedInteger = number<std::uint32_t>(); v.integer = std::int64_t(v.unsignedInteger); break;
        case 'x': v.kind = K::Int64; v.integer = number<std::int64_t>(); break;
        case 't': v.kind = K::UInt64; v.unsignedInteger = number<std::uint64_t>(); v.integer = std::int64_t(v.unsignedInteger); break;
        case 'd': v.kind = K::Double; v.real = number<double>(); break;
        case 's': v.kind = K::String; v.text = string(); break;
        case 'o': v.kind = K::ObjectPath; v.text = string(); break;
        case 'g': v.kind = K::Signature; v.text = signature(); break;
        case 'h': v.kind = K::UInt32; v.unsignedInteger = number<std::uint32_t>(); break;
        case 'a': {
            v.kind = K::Array;
            const std::size_t elementStart = at;
            skipType(sig, at);
            v.elementSignature = sig.substr(elementStart, at - elementStart);
            const auto length = number<std::uint32_t>();
            if (failed || length > kMaxArray) {
                fail();
                break;
            }
            align(alignmentOf(v.elementSignature.empty() ? 'y' : v.elementSignature[0]));
            const std::size_t end = pos + length;
            if (end > m_size) {
                fail();
                break;
            }
            while (pos < end && !failed) {
                std::size_t e = 0;
                v.items.push_back(value(v.elementSignature, e, depth + 1));
            }
            break;
        }
        case '(':
        case '{': {
            v.kind = type == '(' ? K::Struct : K::DictEntry;
            align(8);
            const char close = type == '(' ? ')' : '}';
            while (at < sig.size() && sig[at] != close && !failed) {
                v.items.push_back(value(sig, at, depth + 1));
            }
            ++at; // the closing bracket
            break;
        }
        case 'v': {
            v.kind = K::Variant;
            const String inner = signature();
            std::size_t e = 0;
            if (!failed && !inner.empty()) {
                v.items.push_back(value(inner, e, depth + 1));
            }
            break;
        }
        default:
            fail();
            break;
        }
        return v;
    }

private:
    bool fail() {
        failed = true;
        return false;
    }
    void skipType(const String &sig, std::size_t &at) {
        if (at >= sig.size()) {
            fail();
            return;
        }
        const char type = sig[at++];
        if (type == 'a') {
            skipType(sig, at);
        } else if (type == '(' || type == '{') {
            const char close = type == '(' ? ')' : '}';
            while (at < sig.size() && sig[at] != close) {
                skipType(sig, at);
            }
            ++at;
        }
    }
    const std::uint8_t *m_data;
    std::size_t m_size;
};

} // namespace

// ---- Values ---------------------------------------------------------------------------------

Value Value::byte(std::uint8_t v) { Value x; x.kind = Kind::Byte; x.integer = v; x.unsignedInteger = v; return x; }
Value Value::boolean(bool v) { Value x; x.kind = Kind::Bool; x.integer = v ? 1 : 0; return x; }
Value Value::int16(std::int16_t v) { Value x; x.kind = Kind::Int16; x.integer = v; return x; }
Value Value::uint16(std::uint16_t v) { Value x; x.kind = Kind::UInt16; x.unsignedInteger = v; x.integer = v; return x; }
Value Value::int32(std::int32_t v) { Value x; x.kind = Kind::Int32; x.integer = v; return x; }
Value Value::uint32(std::uint32_t v) { Value x; x.kind = Kind::UInt32; x.unsignedInteger = v; x.integer = v; return x; }
Value Value::int64(std::int64_t v) { Value x; x.kind = Kind::Int64; x.integer = v; return x; }
Value Value::uint64(std::uint64_t v) { Value x; x.kind = Kind::UInt64; x.unsignedInteger = v; x.integer = std::int64_t(v); return x; }
Value Value::dbl(double v) { Value x; x.kind = Kind::Double; x.real = v; return x; }
Value Value::string(String v) { Value x; x.kind = Kind::String; x.text = std::move(v); return x; }
Value Value::objectPath(String v) { Value x; x.kind = Kind::ObjectPath; x.text = std::move(v); return x; }
Value Value::signature(String v) { Value x; x.kind = Kind::Signature; x.text = std::move(v); return x; }
Value Value::array(String elementSignature, std::vector<Value> items) {
    Value x;
    x.kind = Kind::Array;
    x.elementSignature = std::move(elementSignature);
    x.items = std::move(items);
    return x;
}
Value Value::structure(std::vector<Value> fields) { Value x; x.kind = Kind::Struct; x.items = std::move(fields); return x; }
Value Value::variant(Value inner) { Value x; x.kind = Kind::Variant; x.items.push_back(std::move(inner)); return x; }
Value Value::dictEntry(Value key, Value value) {
    Value x;
    x.kind = Kind::DictEntry;
    x.items.push_back(std::move(key));
    x.items.push_back(std::move(value));
    return x;
}

String Value::typeSignature() const {
    switch (kind) {
    case Kind::Byte: return "y";
    case Kind::Bool: return "b";
    case Kind::Int16: return "n";
    case Kind::UInt16: return "q";
    case Kind::Int32: return "i";
    case Kind::UInt32: return "u";
    case Kind::Int64: return "x";
    case Kind::UInt64: return "t";
    case Kind::Double: return "d";
    case Kind::String: return "s";
    case Kind::ObjectPath: return "o";
    case Kind::Signature: return "g";
    case Kind::Array: return "a" + elementSignature;
    case Kind::Variant: return "v";
    case Kind::Struct:
    case Kind::DictEntry: {
        String sig(1, kind == Kind::Struct ? '(' : '{');
        for (const Value &item : items) {
            sig += item.typeSignature();
        }
        sig += kind == Kind::Struct ? ')' : '}';
        return sig;
    }
    }
    return "s";
}

std::int64_t Value::asInt() const {
    if (kind == Kind::Double) {
        return std::int64_t(real);
    }
    if (kind == Kind::Variant && !items.empty()) {
        return items[0].asInt();
    }
    return integer;
}

double Value::asDouble() const {
    if (kind == Kind::Double) {
        return real;
    }
    if (kind == Kind::Variant && !items.empty()) {
        return items[0].asDouble();
    }
    return double(integer);
}

// ---- Messages -------------------------------------------------------------------------------

String Message::bodySignature() const {
    String sig;
    for (const Value &v : body) {
        sig += v.typeSignature();
    }
    return sig;
}

Message Message::methodCall(String destination, String path, String interface, String member,
                            std::vector<Value> args) {
    Message m;
    m.type = Type::MethodCall;
    m.destination = std::move(destination);
    m.path = std::move(path);
    m.interface = std::move(interface);
    m.member = std::move(member);
    m.body = std::move(args);
    return m;
}

Message Message::signal(String path, String interface, String member, std::vector<Value> args) {
    Message m;
    m.type = Type::Signal;
    m.path = std::move(path);
    m.interface = std::move(interface);
    m.member = std::move(member);
    m.body = std::move(args);
    return m;
}

Message Message::methodReturn(const Message &call, std::vector<Value> values) {
    Message m;
    m.type = Type::MethodReturn;
    m.replySerial = call.serial;
    m.destination = call.sender;
    m.body = std::move(values);
    return m;
}

Message Message::error(const Message &call, String name, String text) {
    Message m;
    m.type = Type::Error;
    m.replySerial = call.serial;
    m.destination = call.sender;
    m.errorName = std::move(name);
    m.body.push_back(Value::string(std::move(text)));
    return m;
}

std::vector<std::uint8_t> encode(const Message &message) {
    Writer body;
    for (const Value &v : message.body) {
        body.value(v);
    }
    std::vector<Value> fields;
    const auto field = [&fields](std::uint8_t code, Value v) {
        fields.push_back(Value::structure({Value::byte(code), Value::variant(std::move(v))}));
    };
    if (!message.path.empty()) field(1, Value::objectPath(message.path));
    if (!message.interface.empty()) field(2, Value::string(message.interface));
    if (!message.member.empty()) field(3, Value::string(message.member));
    if (!message.errorName.empty()) field(4, Value::string(message.errorName));
    if (message.replySerial) field(5, Value::uint32(message.replySerial));
    if (!message.destination.empty()) field(6, Value::string(message.destination));
    if (!message.sender.empty()) field(7, Value::string(message.sender));
    if (!message.body.empty()) field(8, Value::signature(message.bodySignature()));

    Writer header;
    header.out.push_back('l');
    header.out.push_back(std::uint8_t(message.type));
    header.out.push_back(message.flags);
    header.out.push_back(1);
    header.number(std::uint32_t(body.out.size()));
    header.number(message.serial);
    header.value(Value::array("(yv)", std::move(fields)));
    header.align(8);
    header.out.insert(header.out.end(), body.out.begin(), body.out.end());
    return std::move(header.out);
}

Result<std::optional<std::pair<Message, std::size_t>>> decode(const std::uint8_t *data, std::size_t size) {
    if (size < 16) {
        return std::optional<std::pair<Message, std::size_t>>();
    }
    if (data[0] != 'l') {
        return Error(ErrorCode::Corrupt, "D-Bus: only little-endian messages are read");
    }
    std::uint32_t bodyLength = 0;
    std::uint32_t fieldsLength = 0;
    std::memcpy(&bodyLength, data + 4, 4);
    std::memcpy(&fieldsLength, data + 12, 4);
    const std::size_t headerEnd = (16 + std::size_t(fieldsLength) + 7) & ~std::size_t(7);
    const std::size_t total = headerEnd + bodyLength;
    if (fieldsLength > kMaxMessage || total > kMaxMessage) {
        return Error(ErrorCode::Corrupt, "D-Bus: message too large");
    }
    if (size < total) {
        return std::optional<std::pair<Message, std::size_t>>();
    }
    Message m;
    m.type = static_cast<Message::Type>(data[1]);
    m.flags = data[2];
    std::memcpy(&m.serial, data + 8, 4);
    Reader header(data, total);
    header.pos = 12;
    std::size_t at = 0;
    const Value fields = header.value("a(yv)", at);
    if (header.failed) {
        return Error(ErrorCode::Corrupt, "D-Bus: malformed header");
    }
    String signature;
    for (const Value &f : fields.items) {
        if (f.items.size() != 2 || f.items[1].items.empty()) {
            continue;
        }
        const Value &v = f.items[1].items[0];
        switch (f.items[0].integer) {
        case 1: m.path = v.text; break;
        case 2: m.interface = v.text; break;
        case 3: m.member = v.text; break;
        case 4: m.errorName = v.text; break;
        case 5: m.replySerial = std::uint32_t(v.unsignedInteger); break;
        case 6: m.destination = v.text; break;
        case 7: m.sender = v.text; break;
        case 8: signature = v.text; break;
        default: break;
        }
    }
    Reader body(data + headerEnd, bodyLength);
    for (std::size_t i = 0; i < signature.size() && !body.failed;) {
        m.body.push_back(body.value(signature, i));
    }
    if (body.failed) {
        return Error(ErrorCode::Corrupt, "D-Bus: malformed body");
    }
    return std::optional<std::pair<Message, std::size_t>>(std::pair{std::move(m), total});
}

// ---- Connection -----------------------------------------------------------------------------

namespace {

int connectUnix(StringView address) {
    // "unix:path=...,guid=..." or "unix:abstract=..."
    if (address.substr(0, 5) != "unix:") {
        return -1;
    }
    String path;
    bool abstract = false;
    StringView rest = address.substr(5);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const StringView part = rest.substr(0, comma);
        if (part.substr(0, 5) == "path=") {
            path = String(part.substr(5));
        } else if (part.substr(0, 9) == "abstract=") {
            path = String(part.substr(9));
            abstract = true;
        }
        rest = comma == StringView::npos ? StringView() : rest.substr(comma + 1);
    }
    if (path.empty()) {
        return -1;
    }
    sockaddr_un where{};
    where.sun_family = AF_UNIX;
    const std::size_t offset = abstract ? 1 : 0;
    if (path.size() + offset >= sizeof where.sun_path) {
        return -1;
    }
    std::memcpy(where.sun_path + offset, path.data(), path.size());
    const auto length = socklen_t(offsetof(sockaddr_un, sun_path) + offset + path.size() + (abstract ? 0 : 1));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&where), length) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const void *data, std::size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    while (size > 0) {
        const ssize_t n = ::send(fd, bytes, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                pollfd p{fd, POLLOUT, 0};
                ::poll(&p, 1, 1000);
                continue;
            }
            return false;
        }
        bytes += n;
        size -= std::size_t(n);
    }
    return true;
}

// One "\r\n"-terminated line of the authentication exchange.
std::optional<String> readLine(int fd, std::chrono::milliseconds timeout) {
    String line;
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (line.size() < 4096) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        pollfd p{fd, POLLIN, 0};
        if (left.count() <= 0 || ::poll(&p, 1, int(left.count())) <= 0) {
            return std::nullopt;
        }
        char c = 0;
        if (::recv(fd, &c, 1, 0) != 1) {
            return std::nullopt;
        }
        line += c;
        if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) {
            line.resize(line.size() - 2);
            return line;
        }
    }
    return std::nullopt;
}

} // namespace

Result<std::unique_ptr<Connection>> Connection::open(StringView address, std::chrono::milliseconds timeout) {
    int fd = -1;
    StringView rest = address;
    while (fd < 0 && !rest.empty()) {
        const std::size_t semicolon = rest.find(';');
        fd = connectUnix(rest.substr(0, semicolon));
        rest = semicolon == StringView::npos ? StringView() : rest.substr(semicolon + 1);
    }
    if (fd < 0) {
        return Error(ErrorCode::NotFound, "D-Bus: cannot connect to " + String(address));
    }
    // EXTERNAL: the server checks our credentials on the socket.
    char uid[32];
    std::snprintf(uid, sizeof uid, "%u", unsigned(::getuid()));
    String hex;
    for (const char *c = uid; *c; ++c) {
        char pair[3];
        std::snprintf(pair, sizeof pair, "%02x", unsigned(static_cast<unsigned char>(*c)));
        hex += pair;
    }
    const String auth = String(1, '\0') + "AUTH EXTERNAL " + hex + "\r\n";
    std::unique_ptr<Connection> connection(new Connection());
    connection->m_fd = fd;
    if (!writeAll(fd, auth.data(), auth.size())) {
        return Error(ErrorCode::IoError, "D-Bus: authentication failed");
    }
    const std::optional<String> answer = readLine(fd, timeout);
    if (!answer || answer->substr(0, 3) != "OK ") {
        return Error(ErrorCode::PermissionDenied, "D-Bus: the bus refused EXTERNAL authentication");
    }
    const String begin = "BEGIN\r\n";
    if (!writeAll(fd, begin.data(), begin.size())) {
        return Error(ErrorCode::IoError, "D-Bus: authentication failed");
    }
    Result<Message> hello = connection->call(
        Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello"), timeout);
    if (!hello) {
        return hello.error();
    }
    if (hello.value().body.empty()) {
        return Error(ErrorCode::Corrupt, "D-Bus: Hello gave no name");
    }
    connection->m_uniqueName = hello.value().body[0].text;
    return connection;
}

Result<std::unique_ptr<Connection>> Connection::session(std::chrono::milliseconds timeout) {
    const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address || !*address) {
        return Error(ErrorCode::NotFound, "D-Bus: no session bus address");
    }
    return open(address, timeout);
}

Connection::~Connection() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

bool Connection::send(Message message) {
    if (m_closed) {
        return false;
    }
    message.serial = m_nextSerial++;
    const std::vector<std::uint8_t> bytes = encode(message);
    if (!writeAll(m_fd, bytes.data(), bytes.size())) {
        m_closed = true;
        return false;
    }
    return true;
}

bool Connection::readAvailable(int timeoutMs) {
    pollfd p{m_fd, POLLIN, 0};
    if (::poll(&p, 1, timeoutMs) <= 0) {
        return !m_closed;
    }
    std::uint8_t buffer[65536];
    for (;;) {
        const ssize_t n = ::recv(m_fd, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0) {
            m_input.insert(m_input.end(), buffer, buffer + n);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            m_closed = n == 0 || errno != EAGAIN;
        }
        break;
    }
    // Complete messages go to the queue.
    std::size_t used = 0;
    for (;;) {
        auto decoded = decode(m_input.data() + used, m_input.size() - used);
        if (!decoded) {
            m_closed = true; // the stream cannot be resynchronised
            break;
        }
        if (!decoded.value()) {
            break;
        }
        used += decoded.value()->second;
        m_pending.push_back(std::move(decoded.value()->first));
    }
    m_input.erase(m_input.begin(), m_input.begin() + std::ptrdiff_t(used));
    return !m_closed;
}

Result<Message> Connection::call(Message message, std::chrono::milliseconds timeout) {
    if (m_closed) {
        return Error(ErrorCode::IoError, "D-Bus: the connection is closed");
    }
    const std::uint32_t serial = m_nextSerial;
    if (!send(std::move(message))) {
        return Error(ErrorCode::IoError, "D-Bus: send failed");
    }
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if ((it->type == Message::Type::MethodReturn || it->type == Message::Type::Error) &&
                it->replySerial == serial) {
                Message reply = std::move(*it);
                m_pending.erase(it);
                if (reply.type == Message::Type::Error) {
                    const String text = reply.body.empty() ? String() : reply.body[0].text;
                    return Error(ErrorCode::IoError, reply.errorName + ": " + text);
                }
                return reply;
            }
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return Error(ErrorCode::Timeout, "D-Bus: no reply in time");
        }
        if (!readAvailable(int(std::min<long long>(left.count(), 1000)))) {
            return Error(ErrorCode::IoError, "D-Bus: the connection closed");
        }
    }
}

bool Connection::dispatch() {
    readAvailable(0);
    while (!m_pending.empty()) {
        Message message = std::move(m_pending.front());
        m_pending.pop_front();
        handle(std::move(message));
    }
    return !m_closed;
}

void Connection::handle(Message message) {
    if (message.type == Message::Type::MethodCall) {
        if (onMethodCall) {
            onMethodCall(message);
        } else if (!(message.flags & 1)) {
            send(Message::error(message, "org.freedesktop.DBus.Error.UnknownMethod", "no handler"));
        }
    } else if (message.type == Message::Type::Signal && onSignal) {
        onSignal(message);
    }
}

} // namespace cfw::dbus
