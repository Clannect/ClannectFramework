// AT-SPI 2 for a process's windows (screen readers on Linux: Orca, and any
// client of libatspi). One connection to the accessibility bus per process;
// the application object at /org/a11y/atspi/accessible/root has the
// windows as children, and every node of a window's surface is an object at
// /org/a11y/atspi/accessible/w<window>/<node id>, found again by id in a
// fresh snapshot on each call. Objects implement Accessible, Component and,
// as they apply, Action, Value, Text and EditableText; focus changes and
// window activation are signalled as AT-SPI events.
//
// The bridge only connects when assistive technology is on (the session's
// org.a11y.Status IsEnabled), when an accessibility bus is named in
// AT_SPI_BUS_ADDRESS, or when CFW_ACCESSIBILITY=1; CFW_ACCESSIBILITY=0
// turns it off. A helper thread only waits on the socket and wakes the
// window's loop, which does all the work on the window's thread.

#include "../AccessibilityBridge.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "DBus.h"
#include "cfw/core/Utf8.h"
#include "cfw/platform/Window.h"
#include "cfw/ui/Surface.h"

namespace cfw::detail {

namespace {

using dbus::Message;
using dbus::Value;

constexpr const char *kRootPath = "/org/a11y/atspi/accessible/root";
constexpr const char *kNullPath = "/org/a11y/atspi/null";
constexpr const char *kPathPrefix = "/org/a11y/atspi/accessible/w";
constexpr const char *kAccessible = "org.a11y.atspi.Accessible";
constexpr const char *kApplication = "org.a11y.atspi.Application";
constexpr const char *kComponent = "org.a11y.atspi.Component";
constexpr const char *kAction = "org.a11y.atspi.Action";
constexpr const char *kValue = "org.a11y.atspi.Value";
constexpr const char *kText = "org.a11y.atspi.Text";
constexpr const char *kEditableText = "org.a11y.atspi.EditableText";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr auto kTimeout = std::chrono::milliseconds(1500);

// AtspiRole values (atspi-constants.h): part of the protocol.
struct RoleInfo {
    std::uint32_t number;
    const char *name;
};

RoleInfo atspiRole(const AccessibleNode &node) {
    switch (node.role) {
    case Role::Window: return {23, "frame"};
    case Role::Group: return {99, "grouping"};
    case Role::Pane: return {39, "panel"};
    case Role::Label: return {29, "label"};
    case Role::Button: return node.states.checkable ? RoleInfo{62, "toggle button"} : RoleInfo{43, "push button"};
    case Role::CheckBox: return {7, "check box"};
    case Role::TextField: return node.states.protectedText ? RoleInfo{40, "password text"} : RoleInfo{61, "text"};
    case Role::SpinButton: return {52, "spin button"};
    case Role::ComboBox: return {11, "combo box"};
    case Role::List: return {31, "list"};
    case Role::ListItem: return {32, "list item"};
    case Role::Tree: return {65, "tree"};
    case Role::TreeItem: return {91, "tree item"};
    case Role::Table: return {66, "tree table"};
    case Role::Menu: return {33, "menu"};
    case Role::MenuBar: return {34, "menu bar"};
    case Role::MenuItem: return node.states.checkable ? RoleInfo{8, "check menu item"} : RoleInfo{35, "menu item"};
    case Role::ToolBar: return {63, "tool bar"};
    case Role::TabList: return {38, "page tab list"};
    case Role::Tab: return {37, "page tab"};
    case Role::Dialog: return {16, "dialog"};
    case Role::Slider: return {51, "slider"};
    case Role::ProgressBar: return {42, "progress bar"};
    case Role::ScrollArea: return {49, "scroll pane"};
    case Role::Separator: return {50, "separator"};
    case Role::Image: return {27, "image"};
    case Role::None: break;
    }
    return {20, "filler"};
}

// AtspiStateType values.
enum State : std::uint32_t {
    kActive = 1, kChecked = 4, kCollapsed = 5, kEditable = 7, kEnabled = 8, kExpandable = 9, kExpanded = 10,
    kFocusable = 11, kFocused = 12, kModal = 16, kSelectable = 22, kSelected = 23, kSensitive = 24, kShowing = 25,
    kSingleLine = 26, kVisible = 30, kManagesDescendants = 31, kIndeterminate = 32, kIsDefault = 39,
    kCheckable = 41, kHasPopup = 42, kReadOnly = 43,
};

Value stateSet(const std::vector<std::uint32_t> &states) {
    std::uint32_t words[2] = {0, 0};
    for (const std::uint32_t s : states) {
        words[s / 32] |= 1u << (s % 32);
    }
    return Value::array("u", {Value::uint32(words[0]), Value::uint32(words[1])});
}

std::vector<std::uint32_t> statesOf(const AccessibleNode &node, bool windowActive) {
    const AccessibleStates &s = node.states;
    std::vector<std::uint32_t> out;
    if (!s.disabled) {
        out.push_back(kEnabled);
        out.push_back(kSensitive);
    }
    if (node.bounds.width > 0.0f && node.bounds.height > 0.0f) {
        out.push_back(kVisible);
        out.push_back(kShowing);
    }
    if (node.role == Role::Window && windowActive) {
        out.push_back(kActive);
    }
    if (s.focusable) out.push_back(kFocusable);
    if (s.focused) out.push_back(kFocused);
    if (s.checkable) out.push_back(kCheckable);
    if (s.checked) out.push_back(kChecked);
    if (s.mixed) out.push_back(kIndeterminate);
    if (s.expandable) {
        out.push_back(kExpandable);
        out.push_back(s.expanded ? kExpanded : kCollapsed);
    }
    if (s.selectable) out.push_back(kSelectable);
    if (s.selected) out.push_back(kSelected);
    if (s.editable) out.push_back(kEditable);
    if (s.readOnly) out.push_back(kReadOnly);
    if (s.modal) out.push_back(kModal);
    if (s.hasPopup) out.push_back(kHasPopup);
    if (s.isDefault) out.push_back(kIsDefault);
    if (node.role == Role::TextField || node.role == Role::SpinButton) out.push_back(kSingleLine);
    if (node.role == Role::Tree || node.role == Role::Table || node.role == Role::List) {
        out.push_back(kManagesDescendants); // rows come and go as they scroll
    }
    return out;
}

// The actions a node offers, in order: its default, then expanding.
std::vector<std::pair<String, AccessibleAction>> actionsOf(const AccessibleNode &node) {
    std::vector<std::pair<String, AccessibleAction>> actions;
    const char *name = nullptr;
    switch (node.role) {
    case Role::Button: name = node.states.checkable ? "toggle" : "click"; break;
    case Role::CheckBox: name = "toggle"; break;
    case Role::ComboBox: name = "press"; break;
    case Role::MenuItem: name = "click"; break;
    case Role::Tab: name = "switch"; break;
    case Role::TreeItem:
    case Role::ListItem: name = "activate"; break;
    default: break;
    }
    if (name) {
        actions.emplace_back(name, AccessibleAction::Default);
    }
    if (node.states.expandable) {
        actions.emplace_back("expand or contract",
                             node.states.expanded ? AccessibleAction::Collapse : AccessibleAction::Expand);
    }
    return actions;
}

bool hasText(const AccessibleNode &node) {
    return node.role == Role::TextField || node.role == Role::SpinButton;
}

std::u32string codepoints(StringView utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const Utf8Char c = decodeUtf8At(utf8, i);
        out.push_back(c.codepoint);
        i += c.length;
    }
    return out;
}

String toUtf8(std::u32string_view text) {
    String out;
    for (const char32_t c : text) {
        appendUtf8(out, c);
    }
    return out;
}

Value emptyProperties() { return Value::array("{sv}"); }

// A window registered with the application object.
struct WindowEntry {
    int serial = 0;
    Window *window = nullptr;
    Surface *surface = nullptr;
    AccessibleNode tree;
    std::chrono::steady_clock::time_point built;
    bool valid = false;
    bool active = false;
    std::uint64_t lastFocus = 0;

    const AccessibleNode &snapshot() {
        const auto now = std::chrono::steady_clock::now();
        if (!valid || now - built > std::chrono::milliseconds(100)) {
            tree = surface->accessibilityTree();
            if (tree.name.empty()) {
                tree.name = "Window";
            }
            built = now;
            valid = true;
        }
        return tree;
    }
};

class Application {
public:
    // The process's application object; null when assistive technology is
    // off or the bus cannot be reached.
    static std::shared_ptr<Application> acquire();
    ~Application();

    void add(WindowEntry &entry);
    void remove(WindowEntry &entry);
    void dispatch();
    void focusChanged(WindowEntry &entry, std::uint64_t id);
    void activeChanged(WindowEntry &entry, bool active);
    [[nodiscard]] int nextSerial() { return ++m_serials; }

private:
    Application() = default;
    bool connect();

    String pathOf(const WindowEntry &entry, std::uint64_t id) const {
        return kPathPrefix + std::to_string(entry.serial) + "/" + std::to_string(id);
    }
    Value ref(const String &path) const {
        return Value::structure({Value::string(m_connection->uniqueName()), Value::objectPath(path)});
    }
    Value nullRef() const { return Value::structure({Value::string(""), Value::objectPath(kNullPath)}); }
    Value refOf(const WindowEntry &entry, std::uint64_t id) const { return ref(pathOf(entry, id)); }

    void handle(const Message &call);
    void handleRoot(const Message &call);
    void handleNode(const Message &call, WindowEntry &entry, std::uint64_t id);
    std::optional<Value> nodeProperty(WindowEntry &entry, const AccessibleNode &node, StringView interface,
                                      StringView name);
    std::vector<String> interfacesOf(const AccessibleNode &node) const;
    void reply(const Message &call, std::vector<Value> values) {
        if (!(call.flags & 1)) {
            m_connection->send(Message::methodReturn(call, std::move(values)));
        }
    }
    void fail(const Message &call, const char *name, String text) {
        if (!(call.flags & 1)) {
            m_connection->send(Message::error(call, name, std::move(text)));
        }
    }
    void emit(const String &path, const char *interface, const char *member, String detail, std::int32_t detail1,
              std::int32_t detail2, Value data) {
        m_connection->send(Message::signal(path, interface, member,
                                           {Value::string(std::move(detail)), Value::int32(detail1),
                                            Value::int32(detail2), Value::variant(std::move(data)),
                                            emptyProperties()}));
    }
    bool act(WindowEntry &entry, std::uint64_t id, AccessibleAction action, StringView value = {}) {
        const bool done = entry.surface->performAccessibleAction(id, action, value);
        entry.valid = false;
        return done;
    }
    Vec2i toScreen(const WindowEntry &entry, const RectF &bounds, std::uint32_t coordType,
                   const AccessibleNode *parent) const;

    std::unique_ptr<dbus::Connection> m_connection;
    Value m_desktop; // the registry's root: the application's parent
    std::vector<WindowEntry *> m_entries;
    int m_serials = 0;
    std::int32_t m_id = 0;

    // The waker: waits on the socket and wakes the window's loop.
    std::thread m_waker;
    int m_stop[2] = {-1, -1};
    std::mutex m_mutex;
    std::condition_variable m_drained;
    bool m_waiting = false;

    static std::weak_ptr<Application> s_instance;
};

std::weak_ptr<Application> Application::s_instance;

std::optional<bool> sessionSaysEnabled() {
    auto session = dbus::Connection::session(std::chrono::milliseconds(500));
    if (!session) {
        return std::nullopt;
    }
    for (const char *property : {"IsEnabled", "ScreenReaderEnabled"}) {
        Result<Message> reply = session.value()->call(
            Message::methodCall("org.a11y.Bus", "/org/a11y/bus", kProperties, "Get",
                                {Value::string("org.a11y.Status"), Value::string(property)}),
            std::chrono::milliseconds(500));
        if (reply && !reply.value().body.empty() && reply.value().body[0].asBool()) {
            return true;
        }
        if (reply && !reply.value().body.empty() && !reply.value().body[0].items.empty() &&
            reply.value().body[0].items[0].asBool()) {
            return true;
        }
    }
    return false;
}

std::optional<String> accessibilityBusAddress() {
    if (const char *address = std::getenv("AT_SPI_BUS_ADDRESS"); address && *address) {
        return String(address);
    }
    auto session = dbus::Connection::session(std::chrono::milliseconds(500));
    if (!session) {
        return std::nullopt;
    }
    Result<Message> reply = session.value()->call(
        Message::methodCall("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress"), kTimeout);
    if (!reply || reply.value().body.empty()) {
        return std::nullopt;
    }
    return reply.value().body[0].text;
}

std::shared_ptr<Application> Application::acquire() {
    if (auto existing = s_instance.lock()) {
        return existing;
    }
    const char *setting = std::getenv("CFW_ACCESSIBILITY");
    if (setting && std::strcmp(setting, "0") == 0) {
        return nullptr;
    }
    const bool forced = (setting && std::strcmp(setting, "1") == 0) || std::getenv("AT_SPI_BUS_ADDRESS");
    if (!forced && sessionSaysEnabled() != true) {
        return nullptr;
    }
    std::shared_ptr<Application> application(new Application());
    if (!application->connect()) {
        return nullptr;
    }
    s_instance = application;
    return application;
}

bool Application::connect() {
    const std::optional<String> address = accessibilityBusAddress();
    if (!address) {
        return false;
    }
    auto connection = dbus::Connection::open(*address, kTimeout);
    if (!connection) {
        return false;
    }
    m_connection = std::move(connection).value();
    m_connection->onMethodCall = [this](const Message &call) { handle(call); };
    // Tell the registry the application is here; it answers with its root.
    Result<Message> embedded = m_connection->call(
        Message::methodCall("org.a11y.atspi.Registry", kRootPath, "org.a11y.atspi.Socket", "Embed",
                            {ref(kRootPath)}),
        kTimeout);
    m_desktop = embedded && !embedded.value().body.empty() ? embedded.value().body[0] : nullRef();

    if (::pipe(m_stop) != 0) {
        m_stop[0] = m_stop[1] = -1;
    }
    const int fd = m_connection->fd();
    m_waker = std::thread([this, fd] {
        for (;;) {
            pollfd fds[2] = {{fd, POLLIN, 0}, {m_stop[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                continue;
            }
            if (fds[1].revents) {
                return;
            }
            if (fds[0].revents & (POLLHUP | POLLERR)) {
                return;
            }
            std::unique_lock lock(m_mutex);
            m_waiting = true;
            wakeUp();
            // The loop reads the socket in dispatch(); until then the socket
            // stays readable, so wait for it rather than spinning.
            m_drained.wait_for(lock, std::chrono::milliseconds(200), [this] { return !m_waiting; });
        }
    });
    return true;
}

Application::~Application() {
    if (m_stop[1] >= 0) {
        const char byte = 1;
        [[maybe_unused]] const auto n = ::write(m_stop[1], &byte, 1);
    }
    if (m_waker.joinable()) {
        m_waker.join();
    }
    for (int fd : m_stop) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

void Application::add(WindowEntry &entry) {
    m_entries.push_back(&entry);
    emit(kRootPath, "org.a11y.atspi.Event.Object", "ChildrenChanged", "add", std::int32_t(m_entries.size() - 1), 0,
         refOf(entry, 1));
}

void Application::remove(WindowEntry &entry) {
    const auto it = std::find(m_entries.begin(), m_entries.end(), &entry);
    if (it == m_entries.end()) {
        return;
    }
    const auto index = std::int32_t(it - m_entries.begin());
    m_entries.erase(it);
    emit(kRootPath, "org.a11y.atspi.Event.Object", "ChildrenChanged", "remove", index, 0, refOf(entry, 1));
}

void Application::dispatch() {
    m_connection->dispatch();
    std::lock_guard lock(m_mutex);
    m_waiting = false;
    m_drained.notify_all();
}

void Application::focusChanged(WindowEntry &entry, std::uint64_t id) {
    entry.valid = false;
    if (entry.lastFocus != 0) {
        emit(pathOf(entry, entry.lastFocus), "org.a11y.atspi.Event.Object", "StateChanged", "focused", 0, 0,
             Value::int32(0));
    }
    entry.lastFocus = id;
    if (id != 0) {
        emit(pathOf(entry, id), "org.a11y.atspi.Event.Object", "StateChanged", "focused", 1, 0, Value::int32(0));
    }
}

void Application::activeChanged(WindowEntry &entry, bool active) {
    entry.active = active;
    entry.valid = false;
    const String path = pathOf(entry, 1);
    const String title = entry.snapshot().name;
    emit(path, "org.a11y.atspi.Event.Window", active ? "Activate" : "Deactivate", "", 0, 0, Value::string(title));
    emit(path, "org.a11y.atspi.Event.Object", "StateChanged", "active", active ? 1 : 0, 0, Value::int32(0));
}

// ---- Method calls ------------------------------------------------------------------------------

void Application::handle(const Message &call) {
    if (call.path == "/org/a11y/atspi/cache" && call.member == "GetItems") {
        // No cache is offered: clients ask for objects as they need them.
        reply(call, {Value::array("((so)(so)(so)iiassusau)")});
        return;
    }
    if (call.path == kRootPath) {
        handleRoot(call);
        return;
    }
    // /org/a11y/atspi/accessible/w<serial>/<id>
    const String prefix = kPathPrefix;
    if (call.path.rfind(prefix, 0) == 0) {
        const String rest = call.path.substr(prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash != String::npos) {
            const int serial = std::atoi(rest.substr(0, slash).c_str());
            const std::uint64_t id = std::strtoull(rest.c_str() + slash + 1, nullptr, 10);
            for (WindowEntry *entry : m_entries) {
                if (entry->serial == serial) {
                    handleNode(call, *entry, id);
                    return;
                }
            }
        }
    }
    fail(call, "org.freedesktop.DBus.Error.UnknownObject", "no such object: " + call.path);
}

void Application::handleRoot(const Message &call) {
    const auto name = [this] {
        return m_entries.empty() ? String("Clannect") : m_entries.front()->snapshot().name;
    };
    const auto property = [&](StringView interface, StringView prop) -> std::optional<Value> {
        if (interface == kAccessible) {
            if (prop == "Name") return Value::string(name());
            if (prop == "Description") return Value::string("");
            if (prop == "Parent") return m_desktop;
            if (prop == "ChildCount") return Value::int32(std::int32_t(m_entries.size()));
            if (prop == "Locale") return Value::string("");
            if (prop == "AccessibleId") return Value::string("");
            if (prop == "HelpText") return Value::string("");
        } else if (interface == kApplication) {
            if (prop == "ToolkitName") return Value::string("Clannect Framework");
            if (prop == "Version") return Value::string("1");
            if (prop == "AtspiVersion") return Value::string("2.1");
            if (prop == "Id") return Value::int32(m_id);
        }
        return std::nullopt;
    };
    if (call.interface == kProperties) {
        if (call.member == "Get" && call.body.size() == 2) {
            if (auto v = property(call.body[0].text, call.body[1].text)) {
                reply(call, {Value::variant(*v)});
                return;
            }
        } else if (call.member == "GetAll" && call.body.size() == 1) {
            std::vector<Value> all;
            for (const char *prop : {"Name", "Description", "Parent", "ChildCount", "Locale", "ToolkitName", "Version",
                                     "AtspiVersion", "Id"}) {
                if (auto v = property(call.body[0].text, prop)) {
                    all.push_back(Value::dictEntry(Value::string(prop), Value::variant(*v)));
                }
            }
            reply(call, {Value::array("{sv}", std::move(all))});
            return;
        } else if (call.member == "Set" && call.body.size() == 3 && call.body[1].text == "Id") {
            m_id = std::int32_t(call.body[2].asInt());
            reply(call, {});
            return;
        }
        fail(call, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
        return;
    }
    const StringView member = call.member;
    if (member == "GetChildAtIndex" && !call.body.empty()) {
        const auto index = call.body[0].asInt();
        reply(call, {index >= 0 && std::size_t(index) < m_entries.size() ? refOf(*m_entries[std::size_t(index)], 1)
                                                                         : nullRef()});
    } else if (member == "GetChildren") {
        std::vector<Value> children;
        for (WindowEntry *entry : m_entries) {
            children.push_back(refOf(*entry, 1));
        }
        reply(call, {Value::array("(so)", std::move(children))});
    } else if (member == "GetIndexInParent") {
        reply(call, {Value::int32(-1)});
    } else if (member == "GetRelationSet") {
        reply(call, {Value::array("(ua(so))")});
    } else if (member == "GetRole") {
        reply(call, {Value::uint32(75)}); // application
    } else if (member == "GetRoleName" || member == "GetLocalizedRoleName") {
        reply(call, {Value::string("application")});
    } else if (member == "GetState") {
        reply(call, {stateSet({})});
    } else if (member == "GetAttributes") {
        reply(call, {Value::array("{ss}", {Value::dictEntry(Value::string("toolkit"), Value::string("cfw"))})});
    } else if (member == "GetApplication") {
        reply(call, {ref(kRootPath)});
    } else if (member == "GetInterfaces") {
        reply(call, {Value::array("s", {Value::string(kAccessible), Value::string(kApplication)})});
    } else if (member == "GetLocale") {
        reply(call, {Value::string("")});
    } else {
        fail(call, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method " + call.member);
    }
}

std::vector<String> Application::interfacesOf(const AccessibleNode &node) const {
    std::vector<String> out{kAccessible, kComponent};
    if (!actionsOf(node).empty()) {
        out.push_back(kAction);
    }
    if (node.current) {
        out.push_back(kValue);
    }
    if (hasText(node)) {
        out.push_back(kText);
        if (node.states.editable) {
            out.push_back(kEditableText);
        }
    }
    return out;
}

Vec2i Application::toScreen(const WindowEntry &entry, const RectF &bounds, std::uint32_t coordType,
                            const AccessibleNode *parent) const {
    const float scale = entry.window->devicePixelRatio();
    Vec2i at{int(std::lround(bounds.x * scale)), int(std::lround(bounds.y * scale))};
    if (coordType == 0) { // screen
        const Vec2i origin = entry.window->screenPosition();
        at = {at.x + origin.x, at.y + origin.y};
    } else if (coordType == 2 && parent) { // parent
        at = {at.x - int(std::lround(parent->bounds.x * scale)), at.y - int(std::lround(parent->bounds.y * scale))};
    }
    return at;
}

std::optional<Value> Application::nodeProperty(WindowEntry &entry, const AccessibleNode &node, StringView interface,
                                               StringView name) {
    const AccessibleNode &root = entry.snapshot();
    if (interface == kAccessible) {
        if (name == "Name") return Value::string(node.name);
        if (name == "Description") return Value::string(node.description);
        if (name == "Parent") {
            if (node.id == 1) return ref(kRootPath);
            const AccessibleNode *up = root.parentOf(node.id);
            return up ? refOf(entry, up->id) : nullRef();
        }
        if (name == "ChildCount") return Value::int32(std::int32_t(node.children.size()));
        if (name == "Locale") return Value::string("");
        if (name == "AccessibleId") return Value::string(std::to_string(node.id));
        if (name == "HelpText") return Value::string("");
    } else if (interface == kAction && name == "NActions") {
        return Value::int32(std::int32_t(actionsOf(node).size()));
    } else if (interface == kValue) {
        if (name == "MinimumValue") return Value::dbl(node.minimum.value_or(0.0));
        if (name == "MaximumValue") return Value::dbl(node.maximum.value_or(0.0));
        if (name == "MinimumIncrement") return Value::dbl(0.0);
        if (name == "CurrentValue") return Value::dbl(node.current.value_or(0.0));
        if (name == "Text") return Value::string(node.value);
    } else if (interface == kText) {
        if (name == "CharacterCount") return Value::int32(std::int32_t(countCodepoints(node.value)));
        if (name == "CaretOffset") return Value::int32(std::int32_t(countCodepoints(node.value)));
    }
    return std::nullopt;
}

void Application::handleNode(const Message &call, WindowEntry &entry, std::uint64_t id) {
    const AccessibleNode &root = entry.snapshot();
    const AccessibleNode *found = root.find(id);
    if (!found) {
        fail(call, "org.freedesktop.DBus.Error.UnknownObject", "the object is gone");
        return;
    }
    const AccessibleNode node = *found; // actions may rebuild the snapshot
    const StringView member = call.member;
    const StringView interface = call.interface;

    if (interface == kProperties) {
        if (member == "Get" && call.body.size() == 2) {
            if (auto v = nodeProperty(entry, node, call.body[0].text, call.body[1].text)) {
                reply(call, {Value::variant(*v)});
                return;
            }
        } else if (member == "GetAll" && call.body.size() == 1) {
            std::vector<Value> all;
            for (const char *prop : {"Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId",
                                     "NActions", "MinimumValue", "MaximumValue", "MinimumIncrement", "CurrentValue",
                                     "Text", "CharacterCount", "CaretOffset"}) {
                if (auto v = nodeProperty(entry, node, call.body[0].text, prop)) {
                    all.push_back(Value::dictEntry(Value::string(prop), Value::variant(*v)));
                }
            }
            reply(call, {Value::array("{sv}", std::move(all))});
            return;
        } else if (member == "Set" && call.body.size() == 3 && call.body[0].text == kValue &&
                   call.body[1].text == "CurrentValue") {
            char text[32];
            std::snprintf(text, sizeof text, "%.17g", call.body[2].asDouble());
            act(entry, id, AccessibleAction::SetValue, text);
            reply(call, {});
            return;
        }
        fail(call, "org.freedesktop.DBus.Error.UnknownProperty", "no such property");
        return;
    }

    if (interface == kAccessible || interface.empty()) {
        if (member == "GetChildAtIndex" && !call.body.empty()) {
            const auto index = call.body[0].asInt();
            reply(call, {index >= 0 && std::size_t(index) < node.children.size()
                             ? refOf(entry, node.children[std::size_t(index)].id)
                             : nullRef()});
            return;
        }
        if (member == "GetChildren") {
            std::vector<Value> children;
            for (const AccessibleNode &child : node.children) {
                children.push_back(refOf(entry, child.id));
            }
            reply(call, {Value::array("(so)", std::move(children))});
            return;
        }
        if (member == "GetIndexInParent") {
            std::int32_t index = -1;
            if (node.id == 1) {
                const auto it = std::find(m_entries.begin(), m_entries.end(), &entry);
                index = std::int32_t(it - m_entries.begin());
            } else if (const AccessibleNode *up = root.parentOf(id)) {
                for (std::size_t i = 0; i < up->children.size(); ++i) {
                    index = up->children[i].id == id ? std::int32_t(i) : index;
                }
            }
            reply(call, {Value::int32(index)});
            return;
        }
        if (member == "GetRelationSet") {
            reply(call, {Value::array("(ua(so))")});
            return;
        }
        if (member == "GetRole") {
            reply(call, {Value::uint32(atspiRole(node).number)});
            return;
        }
        if (member == "GetRoleName" || member == "GetLocalizedRoleName") {
            reply(call, {Value::string(atspiRole(node).name)});
            return;
        }
        if (member == "GetState") {
            reply(call, {stateSet(statesOf(node, entry.active))});
            return;
        }
        if (member == "GetAttributes") {
            std::vector<Value> attributes{Value::dictEntry(Value::string("toolkit"), Value::string("cfw"))};
            if (node.level > 0) {
                attributes.push_back(Value::dictEntry(Value::string("level"), Value::string(std::to_string(node.level))));
            }
            if (!node.shortcut.empty()) {
                attributes.push_back(Value::dictEntry(Value::string("keyshortcuts"), Value::string(node.shortcut)));
            }
            reply(call, {Value::array("{ss}", std::move(attributes))});
            return;
        }
        if (member == "GetApplication") {
            reply(call, {ref(kRootPath)});
            return;
        }
        if (member == "GetInterfaces") {
            std::vector<Value> names;
            for (const String &name : interfacesOf(node)) {
                names.push_back(Value::string(name));
            }
            reply(call, {Value::array("s", std::move(names))});
            return;
        }
    }

    if (interface == kComponent) {
        const AccessibleNode *parent = root.parentOf(id);
        const auto extents = [&](std::uint32_t coordType) {
            const Vec2i at = toScreen(entry, node.bounds, coordType, parent);
            const float scale = entry.window->devicePixelRatio();
            return std::array<std::int32_t, 4>{at.x, at.y, std::int32_t(std::lround(node.bounds.width * scale)),
                                               std::int32_t(std::lround(node.bounds.height * scale))};
        };
        // A point in `coordType` coordinates, in the surface's logical pixels.
        const auto toSurface = [&](std::int64_t x, std::int64_t y, std::uint32_t coordType) {
            const float scale = entry.window->devicePixelRatio();
            Vec2i origin{0, 0};
            if (coordType == 0) {
                origin = entry.window->screenPosition();
            }
            return Vec2{float(x - origin.x) / scale, float(y - origin.y) / scale};
        };
        if (member == "GetExtents" && !call.body.empty()) {
            const auto e = extents(std::uint32_t(call.body[0].asInt()));
            reply(call, {Value::structure({Value::int32(e[0]), Value::int32(e[1]), Value::int32(e[2]), Value::int32(e[3])})});
            return;
        }
        if (member == "GetPosition" && !call.body.empty()) {
            const auto e = extents(std::uint32_t(call.body[0].asInt()));
            reply(call, {Value::int32(e[0]), Value::int32(e[1])});
            return;
        }
        if (member == "GetSize") {
            const auto e = extents(1);
            reply(call, {Value::int32(e[2]), Value::int32(e[3])});
            return;
        }
        if (member == "Contains" && call.body.size() == 3) {
            const Vec2 p = toSurface(call.body[0].asInt(), call.body[1].asInt(), std::uint32_t(call.body[2].asInt()));
            reply(call, {Value::boolean(node.bounds.contains(p))});
            return;
        }
        if (member == "GetAccessibleAtPoint" && call.body.size() == 3) {
            const Vec2 p = toSurface(call.body[0].asInt(), call.body[1].asInt(), std::uint32_t(call.body[2].asInt()));
            const AccessibleNode *deepest = node.bounds.contains(p) ? &node : nullptr;
            for (bool descended = deepest != nullptr; descended;) {
                descended = false;
                for (auto it = deepest->children.rbegin(); it != deepest->children.rend(); ++it) {
                    if (it->bounds.contains(p)) {
                        deepest = &*it;
                        descended = true;
                        break;
                    }
                }
            }
            reply(call, {deepest && deepest->id != node.id ? refOf(entry, deepest->id) : nullRef()});
            return;
        }
        if (member == "GetLayer") {
            reply(call, {Value::uint32(node.id == 1 ? 7 : 3)}); // window, widget
            return;
        }
        if (member == "GetMDIZOrder") {
            reply(call, {Value::int16(0)});
            return;
        }
        if (member == "GrabFocus") {
            reply(call, {Value::boolean(act(entry, id, AccessibleAction::Focus))});
            return;
        }
        if (member == "GetAlpha") {
            reply(call, {Value::dbl(1.0)});
            return;
        }
        if (member == "ScrollTo" || member == "ScrollToPoint" || member == "SetExtents" ||
            member == "SetPosition" || member == "SetSize") {
            reply(call, {Value::boolean(false)});
            return;
        }
    }

    if (interface == kAction) {
        const auto actions = actionsOf(node);
        const auto index = call.body.empty() ? std::int64_t(-1) : call.body[0].asInt();
        const bool valid = index >= 0 && std::size_t(index) < actions.size();
        if (member == "GetActions") {
            std::vector<Value> list;
            for (const auto &[name, action] : actions) {
                list.push_back(Value::structure({Value::string(name), Value::string(name), Value::string("")}));
            }
            reply(call, {Value::array("(sss)", std::move(list))});
            return;
        }
        if (member == "GetName" || member == "GetLocalizedName") {
            reply(call, {Value::string(valid ? actions[std::size_t(index)].first : String())});
            return;
        }
        if (member == "GetDescription") {
            reply(call, {Value::string("")});
            return;
        }
        if (member == "GetKeyBinding") {
            reply(call, {Value::string(valid && index == 0 ? node.shortcut : String())});
            return;
        }
        if (member == "DoAction") {
            reply(call, {Value::boolean(valid && act(entry, id, actions[std::size_t(index)].second))});
            return;
        }
    }

    if (interface == kText && hasText(node)) {
        const std::u32string text = codepoints(node.value);
        const auto length = std::int64_t(text.size());
        const auto clamp = [length](std::int64_t offset) { return std::clamp<std::int64_t>(offset, 0, length); };
        const auto slice = [&](std::int64_t from, std::int64_t to) {
            from = clamp(from);
            to = to < 0 ? length : clamp(to);
            return toUtf8(std::u32string_view(text).substr(std::size_t(from), std::size_t(std::max(from, to) - from)));
        };
        // The unit around `offset`: a character, a word, or the whole line.
        const auto unitAt = [&](std::int64_t offset, std::uint32_t granularity) -> std::pair<std::int64_t, std::int64_t> {
            offset = clamp(offset);
            if (granularity == 0) { // character
                return {offset, std::min(offset + 1, length)};
            }
            if (granularity == 1) { // word
                std::int64_t start = offset;
                while (start > 0 && text[std::size_t(start - 1)] != U' ') --start;
                std::int64_t end = offset;
                while (end < length && text[std::size_t(end)] != U' ') ++end;
                while (end < length && text[std::size_t(end)] == U' ') ++end;
                return {start, end};
            }
            return {0, length}; // sentence, line, paragraph: one line
        };
        if (member == "GetText" && call.body.size() == 2) {
            reply(call, {Value::string(slice(call.body[0].asInt(), call.body[1].asInt()))});
            return;
        }
        if (member == "GetCharacterAtOffset" && !call.body.empty()) {
            const auto offset = call.body[0].asInt();
            reply(call, {Value::int32(offset >= 0 && offset < length ? std::int32_t(text[std::size_t(offset)]) : 0)});
            return;
        }
        if ((member == "GetStringAtOffset" || member == "GetTextAtOffset") && call.body.size() == 2) {
            // GetTextAtOffset's boundary types pair up (start, end): halve them.
            auto kind = std::uint32_t(call.body[1].asInt());
            if (member == "GetTextAtOffset") {
                kind = kind == 0 ? 0 : kind <= 2 ? 1 : kind <= 4 ? 2 : 3;
            }
            const auto [start, end] = unitAt(call.body[0].asInt(), kind);
            reply(call, {Value::string(slice(start, end)), Value::int32(std::int32_t(start)),
                         Value::int32(std::int32_t(end))});
            return;
        }
        if ((member == "GetTextBeforeOffset" || member == "GetTextAfterOffset") && call.body.size() == 2) {
            const auto kind = std::uint32_t(call.body[1].asInt());
            const std::uint32_t granularity = kind == 0 ? 0 : kind <= 2 ? 1 : kind <= 4 ? 2 : 3;
            const auto [start, end] = unitAt(call.body[0].asInt(), granularity);
            std::pair<std::int64_t, std::int64_t> other{0, 0};
            if (granularity < 2) {
                other = member == "GetTextBeforeOffset" ? (start > 0 ? unitAt(start - 1, granularity) : other)
                                                        : (end < length ? unitAt(end, granularity) : other);
            }
            reply(call, {Value::string(slice(other.first, other.second)), Value::int32(std::int32_t(other.first)),
                         Value::int32(std::int32_t(other.second))});
            return;
        }
        if (member == "GetNSelections") {
            reply(call, {Value::int32(0)});
            return;
        }
        if (member == "GetSelection") {
            reply(call, {Value::int32(0), Value::int32(0)});
            return;
        }
        if (member == "SetCaretOffset" || member == "AddSelection" || member == "RemoveSelection" ||
            member == "SetSelection") {
            reply(call, {Value::boolean(false)});
            return;
        }
        if (member == "GetDefaultAttributes" || member == "GetDefaultAttributeSet") {
            reply(call, {Value::array("{ss}")});
            return;
        }
        if (member == "GetAttributes" || member == "GetAttributeRun") {
            reply(call, {Value::array("{ss}"), Value::int32(0), Value::int32(std::int32_t(length))});
            return;
        }
        if (member == "GetCharacterExtents" || member == "GetRangeExtents") {
            const auto coordType = std::uint32_t(call.body.empty() ? 0 : call.body.back().asInt());
            const Vec2i at = toScreen(entry, node.bounds, coordType, root.parentOf(id));
            const float scale = entry.window->devicePixelRatio();
            reply(call, {Value::int32(at.x), Value::int32(at.y), Value::int32(std::int32_t(std::lround(node.bounds.width * scale))),
                         Value::int32(std::int32_t(std::lround(node.bounds.height * scale)))});
            return;
        }
        if (member == "GetOffsetAtPoint") {
            reply(call, {Value::int32(-1)});
            return;
        }
    }

    if (interface == kEditableText && hasText(node)) {
        if (member == "SetTextContents" && !call.body.empty()) {
            reply(call, {Value::boolean(act(entry, id, AccessibleAction::SetValue, call.body[0].text))});
            return;
        }
        if (member == "InsertText" || member == "DeleteText" || member == "CopyText" || member == "CutText" ||
            member == "PasteText") {
            reply(call, {Value::boolean(false)});
            return;
        }
    }

    fail(call, "org.freedesktop.DBus.Error.UnknownMethod",
         "unknown method " + call.interface + "." + call.member);
}

// ---- The bridge ----------------------------------------------------------------------------------

class AtspiBridge final : public AccessibilityBridge {
public:
    AtspiBridge(std::shared_ptr<Application> application, Window &window, Surface &surface)
        : m_application(std::move(application)) {
        m_entry.serial = m_application->nextSerial();
        m_entry.window = &window;
        m_entry.surface = &surface;
        m_application->add(m_entry);
        m_focus = surface.accessibleFocusChanged.connect(
            [this](std::uint64_t id) { m_application->focusChanged(m_entry, id); });
        m_active = window.focusChanged.connect([this](bool active) { m_application->activeChanged(m_entry, active); });
    }
    ~AtspiBridge() override { m_application->remove(m_entry); }
    void update() override {
        m_entry.valid = false;
        m_application->dispatch();
    }

private:
    std::shared_ptr<Application> m_application;
    WindowEntry m_entry;
    ScopedConnection m_focus;
    ScopedConnection m_active;
};

} // namespace

std::unique_ptr<AccessibilityBridge> createAccessibilityBridge(Window &window, Surface &surface) {
    std::shared_ptr<Application> application = Application::acquire();
    if (!application) {
        return nullptr;
    }
    return std::make_unique<AtspiBridge>(std::move(application), window, surface);
}

} // namespace cfw::detail
