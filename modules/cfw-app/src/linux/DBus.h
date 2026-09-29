#pragma once

// A small D-Bus client (the wire protocol, version 1): connects to a bus by
// address over a Unix socket, authenticates with EXTERNAL, calls methods
// (blocking, with a timeout), serves method calls on object paths, and
// sends signals. Enough for AT-SPI; no third-party code.
//
// Threads: one connection is used from one thread.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "cfw/core/Result.h"
#include "cfw/core/String.h"

namespace cfw::dbus {

// A value of any D-Bus type, with its type signature.
struct Value {
    enum class Kind : std::uint8_t {
        Byte, Bool, Int16, UInt16, Int32, UInt32, Int64, UInt64, Double,
        String, ObjectPath, Signature, Array, Struct, Variant, DictEntry,
    };
    Kind kind = Kind::String;
    std::int64_t integer = 0; // every integer kind, and bool
    std::uint64_t unsignedInteger = 0;
    double real = 0.0;
    String text;               // string, object path, signature
    String elementSignature;   // arrays: the element type (also when empty)
    std::vector<Value> items;  // array elements, struct fields, the variant's value, key and value

    static Value byte(std::uint8_t v);
    static Value boolean(bool v);
    static Value int16(std::int16_t v);
    static Value uint16(std::uint16_t v);
    static Value int32(std::int32_t v);
    static Value uint32(std::uint32_t v);
    static Value int64(std::int64_t v);
    static Value uint64(std::uint64_t v);
    static Value dbl(double v);
    static Value string(String v);
    static Value objectPath(String v);
    static Value signature(String v);
    static Value array(String elementSignature, std::vector<Value> items = {});
    static Value structure(std::vector<Value> fields);
    static Value variant(Value inner);
    static Value dictEntry(Value key, Value value);

    [[nodiscard]] String typeSignature() const;
    // Convenience readers (0 / empty when the kind differs).
    [[nodiscard]] std::int64_t asInt() const;
    [[nodiscard]] double asDouble() const;
    [[nodiscard]] const String &asString() const { return text; }
    [[nodiscard]] bool asBool() const { return integer != 0; }
};

struct Message {
    enum class Type : std::uint8_t { MethodCall = 1, MethodReturn = 2, Error = 3, Signal = 4 };
    Type type = Type::MethodCall;
    std::uint8_t flags = 0; // 1: no reply expected
    std::uint32_t serial = 0;
    std::uint32_t replySerial = 0;
    String path;
    String interface;
    String member;
    String errorName;
    String destination;
    String sender;
    std::vector<Value> body;

    [[nodiscard]] String bodySignature() const;
    static Message methodCall(String destination, String path, String interface, String member,
                              std::vector<Value> args = {});
    static Message signal(String path, String interface, String member, std::vector<Value> args = {});
    // A reply to `call` carrying `values`.
    static Message methodReturn(const Message &call, std::vector<Value> values = {});
    static Message error(const Message &call, String name, String text);
};

// Encoding and decoding (little-endian), for tests and the connection.
[[nodiscard]] std::vector<std::uint8_t> encode(const Message &message);
// Decodes one message from the front of `data`; how many bytes it took, or
// nothing if the data is incomplete. Fails on malformed data.
[[nodiscard]] Result<std::optional<std::pair<Message, std::size_t>>> decode(const std::uint8_t *data,
                                                                            std::size_t size);

class Connection {
public:
    // "unix:path=/run/user/1000/bus" or "unix:abstract=/tmp/dbus-x,guid=...",
    // alternatives separated by ';'. Authenticates and says Hello.
    [[nodiscard]] static Result<std::unique_ptr<Connection>> open(StringView address,
                                                                  std::chrono::milliseconds timeout);
    // The session bus ($DBUS_SESSION_BUS_ADDRESS).
    [[nodiscard]] static Result<std::unique_ptr<Connection>> session(std::chrono::milliseconds timeout);
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    [[nodiscard]] const String &uniqueName() const noexcept { return m_uniqueName; }
    [[nodiscard]] int fd() const noexcept { return m_fd; }

    // Sends a call and waits for its reply (other messages that arrive
    // meanwhile are kept for dispatch()). An error reply is an Error.
    Result<Message> call(Message message, std::chrono::milliseconds timeout);
    // Sends without waiting (replies, signals, calls whose reply is ignored).
    bool send(Message message);
    // Method calls to this connection go here (unhandled: an error reply is
    // sent unless the handler replied).
    std::function<void(const Message &call)> onMethodCall;
    std::function<void(const Message &signal)> onSignal;
    // Reads what is waiting without blocking and dispatches it; false when
    // the connection is gone.
    bool dispatch();

private:
    Connection() = default;
    bool readAvailable(int timeoutMs);
    void handle(Message message);

    int m_fd = -1;
    std::uint32_t m_nextSerial = 1;
    String m_uniqueName;
    std::vector<std::uint8_t> m_input;
    std::deque<Message> m_pending;
    bool m_closed = false;
};

} // namespace cfw::dbus
