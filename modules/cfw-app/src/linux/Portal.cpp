#include "Portal.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>

namespace cfw::portal {

using dbus::Connection;
using dbus::Message;
using dbus::Value;


String newToken() {
    static std::atomic<unsigned> counter{0};
    return "cfw" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
}

Value entry(const char *key, Value value) { return Value::dictEntry(Value::string(key), Value::variant(std::move(value))); }

Value options(std::vector<Value> entries) { return Value::array("{sv}", std::move(entries)); }

// a{sv} as key -> value (the variants' contents).
std::map<String, Value> dict(const Value &v) {
    std::map<String, Value> out;
    for (const Value &e : v.items) {
        if (e.items.size() == 2) {
            const Value &inner = e.items[1].kind == Value::Kind::Variant && !e.items[1].items.empty() ? e.items[1].items[0]
                                                                                                      : e.items[1];
            out.emplace(e.items[0].text, inner);
        }
    }
    return out;
}

// Calls a portal method that answers through a Request and waits for the
// answer's results.
Result<std::map<String, Value>> request(Connection &bus, const char *interface, const char *method,
                                        std::vector<Value> args, const String &token, std::chrono::seconds wait,
                                        const std::atomic<bool> *cancel) {
    String sender = bus.uniqueName().substr(1); // ":1.42" -> "1_42"
    for (char &c : sender) {
        if (c == '.') {
            c = '_';
        }
    }
    const String expected = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    auto match = [&](const String &path) {
        return bus.call(Message::methodCall("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                            "AddMatch",
                                            {Value::string("type='signal',interface='org.freedesktop.portal.Request',"
                                                           "member='Response',path='" +
                                                           path + "'")}),
                        kCallTimeout);
    };
    if (auto m = match(expected); !m) {
        return m.error();
    }
    std::optional<Message> response;
    String handle = expected;
    bus.onSignal = [&](const Message &signal) {
        if (signal.member == "Response" && signal.path == handle) {
            response = signal;
        }
    };
    auto reply = bus.call(Message::methodCall(kPortal, kPath, interface, method, std::move(args)), kCallTimeout);
    if (!reply) {
        bus.onSignal = nullptr;
        return reply.error();
    }
    if (!reply.value().body.empty() && reply.value().body[0].text != expected) {
        handle = reply.value().body[0].text; // an older portal chose its own path
        static_cast<void>(match(handle));
    }
    const auto end = std::chrono::steady_clock::now() + wait;
    while (!response) {
        bus.dispatch();
        if (response) {
            break;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (cancel && *cancel) {
            bus.onSignal = nullptr;
            return Error(ErrorCode::Cancelled, "cancelled");
        }
        if (left <= 0) {
            bus.onSignal = nullptr;
            return Error(ErrorCode::Timeout, String("the desktop did not answer ") + method);
        }
        pollfd p{bus.fd(), POLLIN, 0};
        ::poll(&p, 1, int(std::min<long long>(left, 500)));
    }
    bus.onSignal = nullptr;
    if (response->body.size() < 2) {
        return Error(ErrorCode::Corrupt, String("a malformed answer to ") + method);
    }
    const std::int64_t code = response->body[0].asInt();
    if (code == 1) {
        return Error(ErrorCode::Cancelled, "cancelled");
    }
    if (code != 0) {
        return Error(ErrorCode::IoError, String("the desktop refused ") + method);
    }
    return dict(response->body[1]);
}

std::optional<std::uint32_t> property(Connection &bus, const char *interface, const char *name) {
    auto r = bus.call(Message::methodCall(kPortal, kPath, "org.freedesktop.DBus.Properties", "Get",
                                          {Value::string(interface), Value::string(name)}),
                      kCallTimeout);
    if (!r || r.value().body.empty()) {
        return std::nullopt;
    }
    const Value &v = r.value().body[0];
    const Value &inner = v.kind == Value::Kind::Variant && !v.items.empty() ? v.items[0] : v;
    return std::uint32_t(inner.unsignedInteger ? inner.unsignedInteger : std::uint64_t(inner.integer));
}

} // namespace cfw::portal
