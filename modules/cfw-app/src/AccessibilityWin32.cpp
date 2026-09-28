// MSAA (IAccessible) for a window's surface. WM_GETOBJECT with OBJID_CLIENT
// gets the window node; every node of the surface's accessibility tree is
// its own IAccessible (full objects, children by 1-based index), found again
// by id against a fresh snapshot on every call, so an object for a node that
// has gone reports so. Focus changes are EVENT_OBJECT_FOCUS WinEvents whose
// child ids are negative, unique per node and resolvable through the window
// node's get_accChild, as screen readers expect.

#include "AccessibilityBridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleacc.h>

#include <chrono>
#include <cmath>
#include <map>
#include <string>

#include "cfw/core/Utf8.h"
#include "cfw/platform/Window.h"
#include "cfw/ui/Surface.h"

namespace cfw::detail {

namespace {

// IAccessible's IID, spelled out: MinGW's IID_IAccessible is a data import
// that resolves to an import thunk without dllimport.
constexpr IID kIidAccessible = {0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};


// Shared by the bridge and every IAccessible it handed out (screen readers
// keep them): the surface goes away with the window, the objects may not.
struct State {
    Window *window = nullptr;
    Surface *surface = nullptr;
    HWND hwnd = nullptr;
    AccessibleNode tree;
    std::chrono::steady_clock::time_point built;
    bool valid = false;
    std::map<std::uint64_t, LONG> eventIds;
    std::map<LONG, std::uint64_t> byEventId;
    LONG nextEventId = -1;

    [[nodiscard]] bool alive() const noexcept { return surface != nullptr; }
    const AccessibleNode &snapshot() {
        const auto now = std::chrono::steady_clock::now();
        if (!valid || now - built > std::chrono::milliseconds(100)) {
            tree = surface->accessibilityTree();
            tree.name = tree.name.empty() ? String("Window") : tree.name;
            built = now;
            valid = true;
        }
        return tree;
    }
    void invalidate() noexcept { valid = false; }
    LONG eventId(std::uint64_t id) {
        const auto [it, inserted] = eventIds.try_emplace(id, 0);
        if (inserted) {
            it->second = nextEventId--;
            byEventId[it->second] = id;
        }
        return it->second;
    }
};

BSTR bstr(StringView utf8) {
    const Result<std::u16string> wide = utf8ToUtf16(utf8);
    if (!wide || wide.value().empty()) {
        return nullptr;
    }
    return SysAllocStringLen(reinterpret_cast<const OLECHAR *>(wide.value().data()), UINT(wide.value().size()));
}

String utf8(BSTR text) {
    if (!text) {
        return {};
    }
    return utf16ToUtf8Lossy(std::u16string_view(reinterpret_cast<const char16_t *>(text), SysStringLen(text)));
}

LONG roleOf(Role role) {
    switch (role) {
    case Role::Window: return ROLE_SYSTEM_CLIENT;
    case Role::Group: return ROLE_SYSTEM_GROUPING;
    case Role::Pane: return ROLE_SYSTEM_PANE;
    case Role::Label: return ROLE_SYSTEM_STATICTEXT;
    case Role::Button: return ROLE_SYSTEM_PUSHBUTTON;
    case Role::CheckBox: return ROLE_SYSTEM_CHECKBUTTON;
    case Role::TextField: return ROLE_SYSTEM_TEXT;
    case Role::SpinButton: return ROLE_SYSTEM_SPINBUTTON;
    case Role::ComboBox: return ROLE_SYSTEM_COMBOBOX;
    case Role::List: return ROLE_SYSTEM_LIST;
    case Role::ListItem: return ROLE_SYSTEM_LISTITEM;
    case Role::Tree: return ROLE_SYSTEM_OUTLINE;
    case Role::TreeItem: return ROLE_SYSTEM_OUTLINEITEM;
    case Role::Table: return ROLE_SYSTEM_OUTLINE;
    case Role::Menu: return ROLE_SYSTEM_MENUPOPUP;
    case Role::MenuBar: return ROLE_SYSTEM_MENUBAR;
    case Role::MenuItem: return ROLE_SYSTEM_MENUITEM;
    case Role::ToolBar: return ROLE_SYSTEM_TOOLBAR;
    case Role::TabList: return ROLE_SYSTEM_PAGETABLIST;
    case Role::Tab: return ROLE_SYSTEM_PAGETAB;
    case Role::Dialog: return ROLE_SYSTEM_DIALOG;
    case Role::Slider: return ROLE_SYSTEM_SLIDER;
    case Role::ProgressBar: return ROLE_SYSTEM_PROGRESSBAR;
    case Role::ScrollArea: return ROLE_SYSTEM_PANE;
    case Role::Separator: return ROLE_SYSTEM_SEPARATOR;
    case Role::Image: return ROLE_SYSTEM_GRAPHIC;
    case Role::None: break;
    }
    return ROLE_SYSTEM_CLIENT;
}

LONG statesOf(const AccessibleNode &node) {
    const AccessibleStates &s = node.states;
    LONG state = 0;
    state |= s.focusable ? STATE_SYSTEM_FOCUSABLE : 0;
    state |= s.focused ? STATE_SYSTEM_FOCUSED : 0;
    state |= s.disabled ? STATE_SYSTEM_UNAVAILABLE : 0;
    state |= s.checked ? STATE_SYSTEM_CHECKED : 0;
    state |= s.mixed ? STATE_SYSTEM_MIXED : 0;
    if (s.expandable) {
        state |= s.expanded ? STATE_SYSTEM_EXPANDED : STATE_SYSTEM_COLLAPSED;
    }
    state |= s.selectable ? STATE_SYSTEM_SELECTABLE : 0;
    state |= s.selected ? STATE_SYSTEM_SELECTED : 0;
    state |= s.readOnly ? STATE_SYSTEM_READONLY : 0;
    state |= s.protectedText ? STATE_SYSTEM_PROTECTED : 0;
    state |= s.hasPopup ? STATE_SYSTEM_HASPOPUP : 0;
    state |= s.isDefault ? STATE_SYSTEM_DEFAULT : 0;
    if (node.bounds.width <= 0.0f || node.bounds.height <= 0.0f) {
        state |= STATE_SYSTEM_INVISIBLE;
    }
    if (node.role == Role::Label) {
        state |= STATE_SYSTEM_READONLY;
    }
    return state;
}

// What the default action does, for the role ("Press").
const char *defaultActionOf(const AccessibleNode &node) {
    switch (node.role) {
    case Role::Button: return node.states.checkable ? "Toggle" : "Press";
    case Role::CheckBox: return node.states.checked ? "Uncheck" : "Check";
    case Role::ComboBox: return "Open";
    case Role::MenuItem: return node.states.hasPopup ? "Open" : "Execute";
    case Role::Tab: return "Switch";
    case Role::TreeItem:
    case Role::ListItem: return "Activate";
    case Role::TextField:
    case Role::SpinButton: return "Focus";
    default: return nullptr;
    }
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor" // COM: Release deletes
#endif
class Node final : public IAccessible {
public:
    Node(std::shared_ptr<State> state, std::uint64_t id) : m_state(std::move(state)), m_id(id) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
        if (!object) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, kIidAccessible)) {
            *object = static_cast<IAccessible *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // IDispatch: not scriptable.
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *count) override {
        if (count) {
            *count = 0;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) override {
        return E_NOTIMPL;
    }

    // IAccessible
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch **parent) override {
        if (!parent) {
            return E_INVALIDARG;
        }
        *parent = nullptr;
        if (!m_state->alive()) {
            return CO_E_OBJNOTCONNECTED;
        }
        if (m_id == 1) {
            return AccessibleObjectFromWindow(m_state->hwnd, DWORD(OBJID_WINDOW), IID_IDispatch,
                                              reinterpret_cast<void **>(parent));
        }
        const AccessibleNode *up = m_state->snapshot().parentOf(m_id);
        if (!up) {
            return S_FALSE;
        }
        *parent = new Node(m_state, up->id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long *count) override {
        const AccessibleNode *node = self();
        if (!count) {
            return E_INVALIDARG;
        }
        *count = node ? long(node->children.size()) : 0;
        return node ? S_OK : CO_E_OBJNOTCONNECTED;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch **dispatch) override {
        if (!dispatch) {
            return E_INVALIDARG;
        }
        *dispatch = nullptr;
        const AccessibleNode *node = resolve(child);
        if (!node) {
            return E_INVALIDARG;
        }
        if (node->id == m_id) {
            AddRef();
            *dispatch = this;
            return S_OK;
        }
        *dispatch = new Node(m_state, node->id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR *name) override {
        return text(child, name, [](const AccessibleNode &n) { return n.name; });
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR *value) override {
        return text(child, value, [](const AccessibleNode &n) {
            if (!n.value.empty() || !n.current) {
                return n.value;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%g", *n.current);
            return String(buffer);
        });
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR *description) override {
        return text(child, description, [](const AccessibleNode &n) { return n.description; });
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT *role) override {
        const AccessibleNode *node = resolve(child);
        if (!role || !node) {
            return E_INVALIDARG;
        }
        role->vt = VT_I4;
        role->lVal = roleOf(node->role);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT *state) override {
        const AccessibleNode *node = resolve(child);
        if (!state || !node) {
            return E_INVALIDARG;
        }
        state->vt = VT_I4;
        state->lVal = statesOf(*node);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR *help) override {
        if (help) {
            *help = nullptr;
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR *, VARIANT, long *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR *shortcut) override {
        return text(child, shortcut, [](const AccessibleNode &n) { return n.shortcut; });
    }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT *focus) override {
        if (!focus) {
            return E_INVALIDARG;
        }
        focus->vt = VT_EMPTY;
        const AccessibleNode *node = self();
        if (!node) {
            return CO_E_OBJNOTCONNECTED;
        }
        const std::uint64_t id = m_state->surface->accessibleFocus();
        if (id == 0 || !node->find(id)) {
            return S_FALSE;
        }
        if (id == m_id) {
            focus->vt = VT_I4;
            focus->lVal = CHILDID_SELF;
        } else {
            focus->vt = VT_DISPATCH;
            focus->pdispVal = new Node(m_state, id);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT *selection) override {
        if (selection) {
            selection->vt = VT_EMPTY;
        }
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR *action) override {
        return text(child, action, [](const AccessibleNode &n) {
            const char *name = defaultActionOf(n);
            return name ? String(name) : String();
        });
    }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        const AccessibleNode *node = resolve(child);
        if (!node) {
            return E_INVALIDARG;
        }
        const std::uint64_t id = node->id;
        bool done = false;
        if (flags & SELFLAG_TAKEFOCUS) {
            done = act(id, AccessibleAction::Focus) || done;
        }
        if (flags & (SELFLAG_TAKESELECTION | SELFLAG_ADDSELECTION)) {
            done = act(id, AccessibleAction::Select) || done;
        }
        return done ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long *x, long *y, long *width, long *height, VARIANT child) override {
        const AccessibleNode *node = resolve(child);
        if (!x || !y || !width || !height || !node) {
            return E_INVALIDARG;
        }
        const float scale = m_state->window->devicePixelRatio();
        POINT origin{0, 0};
        ClientToScreen(m_state->hwnd, &origin);
        *x = origin.x + long(std::lround(node->bounds.x * scale));
        *y = origin.y + long(std::lround(node->bounds.y * scale));
        *width = long(std::lround(node->bounds.width * scale));
        *height = long(std::lround(node->bounds.height * scale));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT *end) override {
        if (!end) {
            return E_INVALIDARG;
        }
        end->vt = VT_EMPTY;
        const AccessibleNode *node = resolve(start);
        if (!node) {
            return E_INVALIDARG;
        }
        const AccessibleNode *target = nullptr;
        if (direction == NAVDIR_FIRSTCHILD || direction == NAVDIR_LASTCHILD) {
            if (!node->children.empty()) {
                target = direction == NAVDIR_FIRSTCHILD ? &node->children.front() : &node->children.back();
            }
        } else if (direction == NAVDIR_NEXT || direction == NAVDIR_PREVIOUS) {
            if (const AccessibleNode *up = m_state->snapshot().parentOf(node->id)) {
                for (std::size_t i = 0; i < up->children.size(); ++i) {
                    if (up->children[i].id != node->id) {
                        continue;
                    }
                    if (direction == NAVDIR_NEXT && i + 1 < up->children.size()) {
                        target = &up->children[i + 1];
                    } else if (direction == NAVDIR_PREVIOUS && i > 0) {
                        target = &up->children[i - 1];
                    }
                }
            }
        } else {
            return E_NOTIMPL; // spatial navigation
        }
        if (!target) {
            return S_FALSE;
        }
        end->vt = VT_DISPATCH;
        end->pdispVal = new Node(m_state, target->id);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT *hit) override {
        if (!hit) {
            return E_INVALIDARG;
        }
        hit->vt = VT_EMPTY;
        const AccessibleNode *node = self();
        if (!node) {
            return CO_E_OBJNOTCONNECTED;
        }
        POINT client{x, y};
        ScreenToClient(m_state->hwnd, &client);
        const float scale = m_state->window->devicePixelRatio();
        const Vec2 point{float(client.x) / scale, float(client.y) / scale};
        if (!node->bounds.contains(point)) {
            return S_FALSE;
        }
        // The deepest node there, later siblings (drawn over) first.
        const AccessibleNode *deepest = node;
        for (bool descended = true; descended;) {
            descended = false;
            for (auto it = deepest->children.rbegin(); it != deepest->children.rend(); ++it) {
                if (it->bounds.contains(point)) {
                    deepest = &*it;
                    descended = true;
                    break;
                }
            }
        }
        if (deepest == node) {
            hit->vt = VT_I4;
            hit->lVal = CHILDID_SELF;
        } else {
            hit->vt = VT_DISPATCH;
            hit->pdispVal = new Node(m_state, deepest->id);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        const AccessibleNode *node = resolve(child);
        if (!node) {
            return E_INVALIDARG;
        }
        return act(node->id, AccessibleAction::Default) ? S_OK : DISP_E_MEMBERNOTFOUND;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override {
        const AccessibleNode *node = resolve(child);
        if (!node) {
            return E_INVALIDARG;
        }
        return act(node->id, AccessibleAction::SetValue, utf8(value)) ? S_OK : E_FAIL;
    }

private:
    ~Node() = default;

    const AccessibleNode *self() {
        return m_state->alive() ? m_state->snapshot().find(m_id) : nullptr;
    }
    // CHILDID_SELF, a 1-based child index, or a (negative) event id.
    const AccessibleNode *resolve(const VARIANT &child) {
        const AccessibleNode *node = self();
        if (!node) {
            return nullptr;
        }
        if (child.vt == VT_EMPTY || (child.vt == VT_I4 && child.lVal == CHILDID_SELF)) {
            return node;
        }
        if (child.vt != VT_I4) {
            return nullptr;
        }
        if (child.lVal > 0) {
            return std::size_t(child.lVal) <= node->children.size() ? &node->children[std::size_t(child.lVal) - 1]
                                                                    : nullptr;
        }
        const auto it = m_state->byEventId.find(child.lVal);
        return it == m_state->byEventId.end() ? nullptr : node->find(it->second);
    }
    template <class Get> HRESULT text(const VARIANT &child, BSTR *out, Get get) {
        if (!out) {
            return E_INVALIDARG;
        }
        *out = nullptr;
        const AccessibleNode *node = resolve(child);
        if (!node) {
            return m_state->alive() ? E_INVALIDARG : CO_E_OBJNOTCONNECTED;
        }
        *out = bstr(get(*node));
        return *out ? S_OK : S_FALSE;
    }
    bool act(std::uint64_t id, AccessibleAction action, StringView value = {}) {
        if (!m_state->alive()) {
            return false;
        }
        const bool done = m_state->surface->performAccessibleAction(id, action, value);
        m_state->invalidate();
        return done;
    }

    std::shared_ptr<State> m_state;
    std::uint64_t m_id;
    ULONG m_refs = 1;
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class Win32Bridge final : public AccessibilityBridge {
public:
    Win32Bridge(Window &window, Surface &surface) : m_state(std::make_shared<State>()) {
        m_state->window = &window;
        m_state->surface = &surface;
        m_state->hwnd = static_cast<HWND>(window.nativeHandle());
        m_window = &window;
        window.nativeMessageFilter = [state = m_state](unsigned message, std::uintptr_t wParam,
                                                       std::intptr_t lParam) -> std::optional<std::intptr_t> {
            if (message != WM_GETOBJECT || LONG(lParam) != LONG(OBJID_CLIENT) || !state->alive()) {
                return std::nullopt;
            }
            state->invalidate();
            Node *root = new Node(state, 1);
            const LRESULT result = LresultFromObject(kIidAccessible, WPARAM(wParam), root);
            root->Release();
            return std::intptr_t(result);
        };
        m_focus = surface.accessibleFocusChanged.connect([state = m_state](std::uint64_t id) {
            state->invalidate();
            if (id != 0 && state->alive()) {
                NotifyWinEvent(EVENT_OBJECT_FOCUS, state->hwnd, OBJID_CLIENT, state->eventId(id));
            }
        });
    }
    ~Win32Bridge() override {
        m_window->nativeMessageFilter = nullptr;
        m_state->surface = nullptr; // objects still held by clients now report CO_E_OBJNOTCONNECTED
        m_state->window = nullptr;
    }
    void update() override { m_state->invalidate(); }

private:
    std::shared_ptr<State> m_state;
    Window *m_window = nullptr;
    ScopedConnection m_focus;
};

} // namespace

std::unique_ptr<AccessibilityBridge> createAccessibilityBridge(Window &window, Surface &surface) {
    return std::make_unique<Win32Bridge>(window, surface);
}

} // namespace cfw::detail
