// ScreenCast and takeScreenshot over the freedesktop desktop portal
// (org.freedesktop.portal.ScreenCast / Screenshot), with CFW's D-Bus client.
//
// Every portal call that needs the user returns a Request object whose
// Response signal carries the answer; the request's path is predictable from
// our unique name and a token we choose, so we listen before calling.

#include "cfw/app/ScreenCast.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <map>

#include "DBus.h"

namespace cfw {

using dbus::Connection;
using dbus::Message;
using dbus::Value;

namespace {

constexpr const char *kPortal = "org.freedesktop.portal.Desktop";
constexpr const char *kPath = "/org/freedesktop/portal/desktop";
constexpr auto kCallTimeout = std::chrono::seconds(10);

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
                                        std::vector<Value> args, const String &token, std::chrono::seconds wait) {
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

// file:///home/x/My%20Shot.png -> /home/x/My Shot.png
Path pathFromUri(StringView uri) {
    StringView rest = uri;
    if (rest.substr(0, 7) == "file://") {
        rest = rest.substr(7);
    }
    String out;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const String hex(rest.substr(i + 1, 2));
            out += char(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else {
            out += rest[i];
        }
    }
    return Path(out);
}

} // namespace

struct ScreenCast::Impl {
    std::unique_ptr<Connection> bus;
    String session;
    int fd = -1;
    std::uint32_t node = 0;
    Vec2i size;
    std::vector<ScreenCastStream> streams;
    String restoreToken;
    ~Impl() {
        if (fd >= 0) {
            ::close(fd);
        }
        if (bus && !session.empty()) {
            static_cast<void>(bus->call(Message::methodCall(kPortal, session, "org.freedesktop.portal.Session", "Close"),
                                        std::chrono::seconds(2)));
        }
    }
};

ScreenCast::ScreenCast(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
ScreenCast::~ScreenCast() = default;
int ScreenCast::pipeWireFd() const noexcept { return m_impl->fd; }
std::uint32_t ScreenCast::nodeId() const noexcept { return m_impl->node; }
Vec2i ScreenCast::size() const noexcept { return m_impl->size; }
const std::vector<ScreenCastStream> &ScreenCast::streams() const noexcept { return m_impl->streams; }
const String &ScreenCast::restoreToken() const noexcept { return m_impl->restoreToken; }

bool ScreenCast::available() {
    auto bus = Connection::session(std::chrono::seconds(2));
    if (!bus) {
        return false;
    }
    return property(*bus.value(), "org.freedesktop.portal.ScreenCast", "version").value_or(0) > 0;
}

Result<std::unique_ptr<ScreenCast>> ScreenCast::start(const ScreenCastOptions &o) {
    auto connected = Connection::session(kCallTimeout);
    if (!connected) {
        return connected.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->bus = std::move(connected).value();
    Connection &bus = *impl->bus;
    if (!bus.passesDescriptors()) {
        return Error(ErrorCode::Unsupported, "the session bus does not pass file descriptors");
    }
    const char *iface = "org.freedesktop.portal.ScreenCast";
    const std::uint32_t version = property(bus, iface, "version").value_or(0);
    if (version == 0) {
        return Error(ErrorCode::Unsupported, "this desktop offers no screen casting portal");
    }
    // 1. A session.
    {
        const String token = newToken();
        auto r = request(bus, iface, "CreateSession",
                         {options({entry("handle_token", Value::string(token)),
                                   entry("session_handle_token", Value::string(newToken()))})},
                         token, std::chrono::seconds(30));
        if (!r) {
            return r.error();
        }
        impl->session = r.value()["session_handle"].text;
        if (impl->session.empty()) {
            return Error(ErrorCode::Corrupt, "the portal gave no session");
        }
    }
    // 2. What to share.
    {
        const String token = newToken();
        std::uint32_t types = (o.monitors ? 1u : 0u) | (o.windows ? 2u : 0u);
        const std::uint32_t offered = property(bus, iface, "AvailableSourceTypes").value_or(3);
        types &= offered;
        if (types == 0) {
            types = offered;
        }
        std::vector<Value> opts{entry("handle_token", Value::string(token)), entry("types", Value::uint32(types)),
                                entry("multiple", Value::boolean(o.multiple))};
        const std::uint32_t cursors = property(bus, iface, "AvailableCursorModes").value_or(1);
        if (o.cursor && (cursors & 2u)) {
            opts.push_back(entry("cursor_mode", Value::uint32(2))); // embedded
        } else if (!o.cursor && (cursors & 1u)) {
            opts.push_back(entry("cursor_mode", Value::uint32(1))); // hidden
        }
        if (version >= 4) {
            opts.push_back(entry("persist_mode", Value::uint32(2))); // until revoked
            if (!o.restoreToken.empty()) {
                opts.push_back(entry("restore_token", Value::string(o.restoreToken)));
            }
        }
        auto r = request(bus, iface, "SelectSources", {Value::objectPath(impl->session), options(std::move(opts))}, token,
                         std::chrono::seconds(30));
        if (!r) {
            return r.error();
        }
    }
    // 3. Start: the desktop shows its picker here (unless the token restores).
    {
        const String token = newToken();
        auto r = request(bus, iface, "Start",
                         {Value::objectPath(impl->session), Value::string(""),
                          options({entry("handle_token", Value::string(token))})},
                         token, o.timeout);
        if (!r) {
            return r.error();
        }
        auto &results = r.value();
        const Value &streams = results["streams"];
        if (streams.items.empty() || streams.items[0].items.empty()) {
            return Error(ErrorCode::Corrupt, "the portal gave no stream");
        }
        auto number = [](const Value &v) {
            const bool isUnsigned = v.kind == Value::Kind::UInt16 || v.kind == Value::Kind::UInt32 ||
                                    v.kind == Value::Kind::UInt64 || v.kind == Value::Kind::Byte;
            return isUnsigned ? std::int64_t(v.unsignedInteger) : v.integer; // positions may be negative
        };
        for (const Value &stream : streams.items) {
            if (stream.items.empty()) {
                continue;
            }
            ScreenCastStream info;
            info.nodeId = std::uint32_t(number(stream.items[0]));
            if (stream.items.size() > 1) {
                auto props = dict(stream.items[1]);
                if (auto it = props.find("size"); it != props.end() && it->second.items.size() == 2) {
                    info.size = {int(number(it->second.items[0])), int(number(it->second.items[1]))};
                }
                if (auto it = props.find("position"); it != props.end() && it->second.items.size() == 2) {
                    info.position = {int(number(it->second.items[0])), int(number(it->second.items[1]))};
                    info.hasPosition = true;
                }
                if (auto it = props.find("source_type"); it != props.end()) {
                    info.sourceType = std::uint32_t(number(it->second));
                }
            }
            impl->streams.push_back(info);
        }
        if (impl->streams.empty()) {
            return Error(ErrorCode::Corrupt, "the portal gave no stream");
        }
        impl->node = impl->streams.front().nodeId;
        impl->size = impl->streams.front().size;
        if (auto it = results.find("restore_token"); it != results.end()) {
            impl->restoreToken = it->second.text;
        }
    }
    // 4. The PipeWire connection for it.
    {
        auto r = bus.call(Message::methodCall(kPortal, kPath, iface, "OpenPipeWireRemote",
                                              {Value::objectPath(impl->session), options({})}),
                          kCallTimeout);
        if (!r) {
            return r.error();
        }
        Message &m = r.value();
        if (m.body.empty() || m.fds.empty()) {
            for (int fd : m.fds) {
                ::close(fd);
            }
            return Error(ErrorCode::Corrupt, "the portal gave no PipeWire connection");
        }
        const std::size_t index = std::size_t(m.body[0].unsignedInteger);
        for (std::size_t i = 0; i < m.fds.size(); ++i) {
            if (i == index) {
                impl->fd = m.fds[i];
            } else {
                ::close(m.fds[i]);
            }
        }
        if (impl->fd < 0) {
            return Error(ErrorCode::Corrupt, "the portal gave no PipeWire connection");
        }
    }
    return std::unique_ptr<ScreenCast>(new ScreenCast(std::move(impl)));
}

Result<Path> takeScreenshot(std::chrono::seconds timeout) {
    auto connected = Connection::session(kCallTimeout);
    if (!connected) {
        return connected.error();
    }
    Connection &bus = *connected.value();
    const String token = newToken();
    auto r = request(bus, "org.freedesktop.portal.Screenshot", "Screenshot",
                     {Value::string(""), options({entry("handle_token", Value::string(token)),
                                                  entry("interactive", Value::boolean(false))})},
                     token, timeout);
    if (!r) {
        return r.error();
    }
    const String uri = r.value()["uri"].text;
    if (uri.empty()) {
        return Error(ErrorCode::Corrupt, "the portal gave no image");
    }
    return pathFromUri(uri);
}

} // namespace cfw
