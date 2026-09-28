// A window's surface through MSAA, as a screen reader reads it: the client
// object from AccessibleObjectFromWindow, its children with names, roles,
// values, states and locations, hit testing, the default action, setting a
// value, focus WinEvents resolved with AccessibleObjectFromEvent, and
// objects kept past the window's end reporting that they are gone.

#include <cstdio>
#include <string>
#include <vector>

#include "cfw/app/UiWindow.h"
#include "cfw/platform/Window.h"
#include "cfw/test/Check.h"
#include "cfw/ui/Controls.h"
#include "cfw/ui/Surface.h"

// After the framework's headers: windows.h defines macros (IN, OUT) that
// clash with names in them.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleacc.h>

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

// IAccessible's IID, spelled out: MinGW's IID_IAccessible is a data import
// that resolves to an import thunk without dllimport.
constexpr IID kIidAccessible = {0x618736e0, 0x3c3d, 0x11cf, {0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};


VARIANT self() {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = CHILDID_SELF;
    return v;
}

std::wstring nameOf(IAccessible *object) {
    BSTR name = nullptr;
    object->get_accName(self(), &name);
    std::wstring out = name ? std::wstring(name, SysStringLen(name)) : std::wstring();
    SysFreeString(name);
    return out;
}

std::wstring valueOf(IAccessible *object) {
    BSTR value = nullptr;
    object->get_accValue(self(), &value);
    std::wstring out = value ? std::wstring(value, SysStringLen(value)) : std::wstring();
    SysFreeString(value);
    return out;
}

LONG roleOf(IAccessible *object) {
    VARIANT role;
    VariantInit(&role);
    object->get_accRole(self(), &role);
    return role.vt == VT_I4 ? role.lVal : -1;
}

LONG stateOf(IAccessible *object) {
    VARIANT state;
    VariantInit(&state);
    object->get_accState(self(), &state);
    return state.vt == VT_I4 ? state.lVal : 0;
}

// The children of `parent` as objects (released by the caller).
std::vector<IAccessible *> childrenOf(IAccessible *parent) {
    std::vector<IAccessible *> out;
    long count = 0;
    parent->get_accChildCount(&count);
    for (long i = 1; i <= count; ++i) {
        VARIANT child;
        VariantInit(&child);
        child.vt = VT_I4;
        child.lVal = i;
        IDispatch *dispatch = nullptr;
        IAccessible *object = nullptr;
        if (SUCCEEDED(parent->get_accChild(child, &dispatch)) && dispatch &&
            SUCCEEDED(dispatch->QueryInterface(kIidAccessible, reinterpret_cast<void **>(&object)))) {
            out.push_back(object);
        }
        if (dispatch) {
            dispatch->Release();
        }
    }
    return out;
}

IAccessible *childNamed(IAccessible *parent, const std::wstring &name) {
    IAccessible *found = nullptr;
    for (IAccessible *child : childrenOf(parent)) {
        if (!found && nameOf(child) == name) {
            found = child;
        } else {
            child->Release();
        }
    }
    return found;
}

struct FocusEvent {
    HWND hwnd;
    LONG object;
    LONG child;
};
std::vector<FocusEvent> gFocusEvents;

void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
    if (event == EVENT_OBJECT_FOCUS) {
        gFocusEvents.push_back({hwnd, object, child});
    }
}

} // namespace

int main() {
    CoInitialize(nullptr);
    auto created = UiWindow::create({"Accessible window", {400, 200}, false, true}, Theme::dark().withSystemFonts());
    if (!created) {
        std::printf("AccessibilityWin32Test: skipped (%s)\n", created.error().message().c_str());
        return cfw::test::finish("AccessibilityWin32Test");
    }
    std::unique_ptr<UiWindow> ui = std::move(created).value();
    ui->surface().accessibleTitle = "Accessible window";
    auto &row = static_cast<Stack &>(ui->surface().root().add(std::make_unique<Stack>(Stack::Direction::Row, 8.0f, 8.0f)));
    int clicks = 0;
    Button &save = row.add<Button>("Save");
    static_cast<void>(save.clicked.connect([&] { ++clicks; }));
    CheckBox &snap = row.add<CheckBox>("Snap", true);
    TextField &name = row.add<TextField>("Part");
    name.setAccessibleName("Name");
    for (int i = 0; i < 5; ++i) {
        processEvents(std::chrono::milliseconds(10));
        ui->frame();
    }
    const auto hwnd = static_cast<HWND>(ui->window().nativeHandle());

    IAccessible *client = nullptr;
    const HRESULT got =
        AccessibleObjectFromWindow(hwnd, DWORD(OBJID_CLIENT), kIidAccessible, reinterpret_cast<void **>(&client));
    check(SUCCEEDED(got) && client, "the window answers WM_GETOBJECT with a client object");
    if (!client) {
        return cfw::test::finish("AccessibilityWin32Test");
    }
    checkEqual(roleOf(client), LONG(ROLE_SYSTEM_CLIENT), "a client");
    check(nameOf(client) == L"Accessible window", "named by the title");
    long count = 0;
    client->get_accChildCount(&count);
    checkEqual(count, 3L, "three controls (the stack is anonymous)");

    IAccessible *button = childNamed(client, L"Save");
    IAccessible *box = childNamed(client, L"Snap");
    IAccessible *field = childNamed(client, L"Name");
    check(button && box && field, "each control is a child, by name");
    if (button && box && field) {
        checkEqual(roleOf(button), LONG(ROLE_SYSTEM_PUSHBUTTON), "a push button");
        checkEqual(roleOf(box), LONG(ROLE_SYSTEM_CHECKBUTTON), "a check box");
        check((stateOf(box) & STATE_SYSTEM_CHECKED) != 0, "checked");
        checkEqual(roleOf(field), LONG(ROLE_SYSTEM_TEXT), "a text field");
        check(valueOf(field) == L"Part", "its value");

        // Where it is, on the screen.
        long x = 0, y = 0, w = 0, h = 0;
        button->accLocation(&x, &y, &w, &h, self());
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        check(w > 0 && h > 0 && x >= origin.x && y >= origin.y, "its screen rectangle");
        VARIANT hit;
        VariantInit(&hit);
        client->accHitTest(x + w / 2, y + h / 2, &hit);
        check(hit.vt == VT_DISPATCH, "hit testing its middle finds an object");
        if (hit.vt == VT_DISPATCH) {
            IAccessible *hitObject = nullptr;
            hit.pdispVal->QueryInterface(kIidAccessible, reinterpret_cast<void **>(&hitObject));
            check(hitObject && nameOf(hitObject) == L"Save", "the button");
            if (hitObject) {
                hitObject->Release();
            }
            hit.pdispVal->Release();
        }

        // Acting.
        check(SUCCEEDED(button->accDoDefaultAction(self())), "the default action");
        checkEqual(clicks, 1, "presses the button");
        box->accDoDefaultAction(self());
        check(!snap.isChecked() && (stateOf(box) & STATE_SYSTEM_CHECKED) == 0, "toggles the check box");
        BSTR text = SysAllocString(L"Wall é");
        check(SUCCEEDED(field->put_accValue(self(), text)), "setting a value");
        SysFreeString(text);
        checkEqual(name.text(), String("Wall é"), "changes the text");

        // Focus: a WinEvent whose child id leads back to the object.
        const HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, onWinEvent,
                                                   GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
        ui->surface().setFocus(&name);
        for (int i = 0; i < 20 && gFocusEvents.empty(); ++i) {
            processEvents(std::chrono::milliseconds(10));
        }
        check(!gFocusEvents.empty(), "focusing a field raises EVENT_OBJECT_FOCUS");
        if (!gFocusEvents.empty()) {
            const FocusEvent &event = gFocusEvents.back();
            IAccessible *focused = nullptr;
            VARIANT child;
            VariantInit(&child);
            const HRESULT fromEvent = AccessibleObjectFromEvent(event.hwnd, DWORD(event.object), DWORD(event.child),
                                                                &focused, &child);
            check(SUCCEEDED(fromEvent) && focused, "the event resolves to an object");
            if (focused) {
                if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
                    // A child of the returned object.
                    BSTR focusedName = nullptr;
                    focused->get_accName(child, &focusedName);
                    check(focusedName && std::wstring(focusedName) == L"Name", "the focused field");
                    SysFreeString(focusedName);
                } else {
                    check(nameOf(focused) == L"Name", "the focused field");
                }
                focused->Release();
            }
        }
        if (hook) {
            UnhookWinEvent(hook);
        }
        VARIANT focus;
        VariantInit(&focus);
        client->get_accFocus(&focus);
        check(focus.vt == VT_DISPATCH, "get_accFocus gives the focused object");
        if (focus.vt == VT_DISPATCH) {
            focus.pdispVal->Release();
        }
    }

    // Kept past the window: gone, not crashing.
    ui.reset();
    long after = 0;
    check(FAILED(client->get_accChildCount(&after)) || after == 0, "an object outliving its window reports it");
    for (IAccessible *object : {button, box, field}) {
        if (object) {
            object->Release();
        }
    }
    client->Release();
    CoUninitialize();
    return cfw::test::finish("AccessibilityWin32Test");
}
