// macOS backend: AppKit. An NSWindow per cfw::Window, whose content view
// takes the input (NSTextInputClient for text and input methods, dragging
// destination for file drops) and shows present()'s frame as a CGImage.
// processEvents() pumps NSApp's event queue itself (no [NSApp run]), so the
// application keeps its own loop as it does on Windows and X11.
//
// Modifiers follow the macOS convention every cross-platform toolkit uses:
// Command reports as Modifier::Control (so "Ctrl+S" is Command-S) and the
// Control key as Modifier::Meta.
//
// Built with ARC. Threads: AppKit is main-thread only; wakeUp() is the
// exception (postEvent:atStart: is safe from any thread).

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h> // kVK_ key codes

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>
#include <vector>

#include "PixelCopy.h"
#include "cfw/core/Utf8.h"
#include "cfw/image/Image.h"
#include "cfw/platform/Window.h"

namespace cfw {
namespace {
class WindowCocoa;
} // namespace
} // namespace cfw

// ---- Objective-C side ------------------------------------------------------------

@interface CFWContentView : NSView <NSTextInputClient>
@property(nonatomic, assign) cfw::WindowCocoa *owner;
@end

@interface CFWWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) cfw::WindowCocoa *owner;
@end

@interface CFWApplicationDelegate : NSObject <NSApplicationDelegate>
@end

namespace cfw {

namespace {

std::atomic<bool> gAppReady{false};

// Every live window, for processEvents' repaints and Quit.
std::vector<WindowCocoa *> &windows() {
    static std::vector<WindowCocoa *> list;
    return list;
}

NSString *toNs(StringView text) {
    return [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] ?: @"";
}

String fromNs(NSString *text) {
    if (!text) {
        return {};
    }
    const char *utf8 = [text UTF8String];
    return utf8 ? String(utf8) : String();
}

// NSApplication, set up once: a regular (Dock) application with the usual
// application menu, so Command-Q and the menu bar behave.
void ensureApplication() {
    if (gAppReady.load()) {
        return;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        static CFWApplicationDelegate *delegate = [[CFWApplicationDelegate alloc] init];
        [NSApp setDelegate:delegate];

        NSMenu *bar = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [bar addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        NSString *name = [[NSProcessInfo processInfo] processName];
        [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name] action:@selector(hide:) keyEquivalent:@"h"];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];
        [NSApp setMainMenu:bar];

        [NSApp finishLaunching];
    }
    gAppReady.store(true);
}

Key keyOf(unsigned short code) {
    switch (code) {
    case kVK_ANSI_A: return Key::A;
    case kVK_ANSI_B: return Key::B;
    case kVK_ANSI_C: return Key::C;
    case kVK_ANSI_D: return Key::D;
    case kVK_ANSI_E: return Key::E;
    case kVK_ANSI_F: return Key::F;
    case kVK_ANSI_G: return Key::G;
    case kVK_ANSI_H: return Key::H;
    case kVK_ANSI_I: return Key::I;
    case kVK_ANSI_J: return Key::J;
    case kVK_ANSI_K: return Key::K;
    case kVK_ANSI_L: return Key::L;
    case kVK_ANSI_M: return Key::M;
    case kVK_ANSI_N: return Key::N;
    case kVK_ANSI_O: return Key::O;
    case kVK_ANSI_P: return Key::P;
    case kVK_ANSI_Q: return Key::Q;
    case kVK_ANSI_R: return Key::R;
    case kVK_ANSI_S: return Key::S;
    case kVK_ANSI_T: return Key::T;
    case kVK_ANSI_U: return Key::U;
    case kVK_ANSI_V: return Key::V;
    case kVK_ANSI_W: return Key::W;
    case kVK_ANSI_X: return Key::X;
    case kVK_ANSI_Y: return Key::Y;
    case kVK_ANSI_Z: return Key::Z;
    case kVK_ANSI_0: case kVK_ANSI_Keypad0: return Key::Digit0;
    case kVK_ANSI_1: case kVK_ANSI_Keypad1: return Key::Digit1;
    case kVK_ANSI_2: case kVK_ANSI_Keypad2: return Key::Digit2;
    case kVK_ANSI_3: case kVK_ANSI_Keypad3: return Key::Digit3;
    case kVK_ANSI_4: case kVK_ANSI_Keypad4: return Key::Digit4;
    case kVK_ANSI_5: case kVK_ANSI_Keypad5: return Key::Digit5;
    case kVK_ANSI_6: case kVK_ANSI_Keypad6: return Key::Digit6;
    case kVK_ANSI_7: case kVK_ANSI_Keypad7: return Key::Digit7;
    case kVK_ANSI_8: case kVK_ANSI_Keypad8: return Key::Digit8;
    case kVK_ANSI_9: case kVK_ANSI_Keypad9: return Key::Digit9;
    case kVK_F1: return Key::F1;
    case kVK_F2: return Key::F2;
    case kVK_F3: return Key::F3;
    case kVK_F4: return Key::F4;
    case kVK_F5: return Key::F5;
    case kVK_F6: return Key::F6;
    case kVK_F7: return Key::F7;
    case kVK_F8: return Key::F8;
    case kVK_F9: return Key::F9;
    case kVK_F10: return Key::F10;
    case kVK_F11: return Key::F11;
    case kVK_F12: return Key::F12;
    case kVK_Tab: return Key::Tab;
    case kVK_Return: case kVK_ANSI_KeypadEnter: return Key::Enter;
    case kVK_Escape: return Key::Escape;
    case kVK_Space: return Key::Space;
    case kVK_Delete: return Key::Backspace; // the key labelled "delete" erases backwards
    case kVK_ForwardDelete: return Key::Delete;
    case kVK_LeftArrow: return Key::Left;
    case kVK_RightArrow: return Key::Right;
    case kVK_UpArrow: return Key::Up;
    case kVK_DownArrow: return Key::Down;
    case kVK_Home: return Key::Home;
    case kVK_End: return Key::End;
    case kVK_PageUp: return Key::PageUp;
    case kVK_PageDown: return Key::PageDown;
    case kVK_Help: return Key::Insert; // where Insert is on a PC keyboard
    case kVK_ANSI_Grave: return Key::Backquote;
    case kVK_ANSI_Minus: return Key::Minus;
    case kVK_ANSI_Equal: return Key::Equal;
    case kVK_ANSI_LeftBracket: return Key::BracketLeft;
    case kVK_ANSI_RightBracket: return Key::BracketRight;
    case kVK_ANSI_Backslash: return Key::Backslash;
    case kVK_ANSI_Semicolon: return Key::Semicolon;
    case kVK_ANSI_Quote: return Key::Quote;
    case kVK_ANSI_Comma: return Key::Comma;
    case kVK_ANSI_Period: return Key::Period;
    case kVK_ANSI_Slash: return Key::Slash;
    case kVK_Shift: case kVK_RightShift: return Key::Shift;
    case kVK_Command: case kVK_RightCommand: return Key::Control;
    case kVK_Option: case kVK_RightOption: return Key::Alt;
    case kVK_Control: case kVK_RightControl: return Key::Meta;
    default: return Key::Unknown;
    }
}

Modifier modifiersOf(NSEventModifierFlags flags) {
    Modifier m = Modifier::None;
    if (flags & NSEventModifierFlagShift) m = m | Modifier::Shift;
    if (flags & NSEventModifierFlagCommand) m = m | Modifier::Control;
    if (flags & NSEventModifierFlagOption) m = m | Modifier::Alt;
    if (flags & NSEventModifierFlagControl) m = m | Modifier::Meta;
    return m;
}

// The flag a modifier key's own press or release changes.
NSEventModifierFlags flagOf(unsigned short code) {
    switch (code) {
    case kVK_Shift: case kVK_RightShift: return NSEventModifierFlagShift;
    case kVK_Command: case kVK_RightCommand: return NSEventModifierFlagCommand;
    case kVK_Option: case kVK_RightOption: return NSEventModifierFlagOption;
    case kVK_Control: case kVK_RightControl: return NSEventModifierFlagControl;
    default: return 0;
    }
}

NSCursor *cursorFor(Cursor cursor) {
    switch (cursor) {
    case Cursor::IBeam: return [NSCursor IBeamCursor];
    case Cursor::Hand: return [NSCursor pointingHandCursor];
    case Cursor::Crosshair: return [NSCursor crosshairCursor];
    case Cursor::SizeHorizontal: return [NSCursor resizeLeftRightCursor];
    case Cursor::SizeVertical: return [NSCursor resizeUpDownCursor];
    case Cursor::SizeAll: return [NSCursor openHandCursor];
    case Cursor::NotAllowed: return [NSCursor operationNotAllowedCursor];
    // AppKit has no public diagonal-resize or busy cursor.
    case Cursor::SizeDiagonal:
    case Cursor::SizeAntiDiagonal:
    case Cursor::Wait:
    case Cursor::Arrow:
    case Cursor::Hidden: return [NSCursor arrowCursor];
    }
    return [NSCursor arrowCursor];
}

// Byte offset in UTF-8 of UTF-16 index `index` of `text`.
std::size_t utf8Offset(NSString *text, NSUInteger index) {
    index = std::min<NSUInteger>(index, [text length]);
    return [[text substringToIndex:index] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

std::vector<String> filePaths(id<NSDraggingInfo> info) {
    std::vector<String> paths;
    NSArray<NSURL *> *urls =
        [[info draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                 options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL *url in urls) {
        if ([url isFileURL] && [url path]) {
            paths.push_back(fromNs([url path]));
        }
    }
    return paths;
}

class WindowCocoa final : public Window {
public:
    WindowCocoa(const WindowOptions &options) {
        NSWindowStyleMask style =
            NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (options.resizable) {
            style |= NSWindowStyleMaskResizable;
        }
        const NSRect frame = NSMakeRect(0, 0, std::max(1, options.size.x), std::max(1, options.size.y));
        m_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        [m_window setReleasedWhenClosed:NO];
        [m_window setAcceptsMouseMovedEvents:YES];
        [m_window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        m_delegate = [[CFWWindowDelegate alloc] init];
        m_delegate.owner = this;
        [m_window setDelegate:m_delegate];
        m_view = [[CFWContentView alloc] initWithFrame:frame];
        m_view.owner = this;
        [m_window setContentView:m_view];
        [m_window makeFirstResponder:m_view];
        [m_view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [m_window center];
        setTitle(options.title);
        m_size = currentPixelSize();
        windows().push_back(this);
    }

    ~WindowCocoa() override {
        auto &list = windows();
        list.erase(std::remove(list.begin(), list.end(), this), list.end());
        if (m_cursorHidden) {
            [NSCursor unhide];
        }
        m_view.owner = nullptr;
        m_delegate.owner = nullptr;
        [m_window setDelegate:nil];
        [m_window orderOut:nil];
        [m_window close];
        if (m_image) {
            CGImageRelease(m_image);
        }
    }

    void show() override {
        [m_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    void hide() override { [m_window orderOut:nil]; }
    void setTitle(StringView title) override { [m_window setTitle:toNs(title)]; }
    void setSize(Vec2i size) override {
        [m_window setContentSize:NSMakeSize(std::max(1, size.x), std::max(1, size.y))];
        sizeChanged();
    }
    Vec2i pixelSize() const override { return m_size; }
    float devicePixelRatio() const override { return float([m_window backingScaleFactor]); }

    void present(const Image &image) override {
        detail::toBgrx(image, m_frame);
        m_frameSize = {int(image.width()), int(image.height())};
        if (m_image) {
            CGImageRelease(m_image);
            m_image = nullptr;
        }
        if (!m_frame.empty()) {
            CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(m_frame.data()),
                                          CFIndex(m_frame.size() * sizeof(std::uint32_t)));
            CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            m_image = CGImageCreate(std::size_t(m_frameSize.x), std::size_t(m_frameSize.y), 8, 32,
                                    std::size_t(m_frameSize.x) * 4, space,
                                    CGBitmapInfo(std::uint32_t(kCGBitmapByteOrder32Little) | std::uint32_t(kCGImageAlphaNoneSkipFirst)),
                                    provider, nullptr,
                                    false, kCGRenderingIntentDefault);
            CGColorSpaceRelease(space);
            CGDataProviderRelease(provider);
            CFRelease(data);
        }
        [m_view setNeedsDisplay:YES];
        [m_view displayIfNeeded];
    }

    // What the window shows: the last frame presented (the window server
    // would need the screen-recording permission to be asked).
    Result<Image> capture() const override {
        auto created = Image::create(std::uint32_t(std::max(1, m_size.x)), std::uint32_t(std::max(1, m_size.y)));
        if (!created) {
            return created;
        }
        Image image = std::move(created).value();
        for (int y = 0; y < m_size.y; ++y) {
            std::uint8_t *row = image.row(std::uint32_t(y)).data();
            for (int x = 0; x < m_size.x; ++x) {
                const std::uint32_t p = x < m_frameSize.x && y < m_frameSize.y
                                            ? m_frame[std::size_t(y) * std::size_t(m_frameSize.x) + std::size_t(x)]
                                            : 0;
                row[x * 4] = std::uint8_t(p >> 16);
                row[x * 4 + 1] = std::uint8_t(p >> 8);
                row[x * 4 + 2] = std::uint8_t(p);
                row[x * 4 + 3] = 255;
            }
        }
        return image;
    }

    void requestRepaint() override { m_repaint = true; }

    void setCursor(Cursor cursor) override {
        m_cursor = cursor;
        applyCursor();
    }

    void applyCursor() {
        const bool hide = m_cursor == Cursor::Hidden;
        if (hide != m_cursorHidden) {
            if (hide) {
                [NSCursor hide];
            } else {
                [NSCursor unhide];
            }
            m_cursorHidden = hide;
        }
        [cursorFor(m_cursor) set];
    }

    void setPointerPosition(Vec2 position) override {
        const NSPoint inWindow = [m_view convertPoint:NSMakePoint(position.x, position.y) toView:nil];
        const NSPoint onScreen = [m_window convertPointToScreen:inWindow];
        // Quartz's global space has its origin at the top-left of the main
        // display; AppKit's at the bottom-left.
        const CGFloat mainHeight = [[[NSScreen screens] firstObject] frame].size.height;
        CGWarpMouseCursorPosition(CGPointMake(onScreen.x, mainHeight - onScreen.y));
        CGAssociateMouseAndMouseCursorPosition(true);
        // Warping makes no event: one arrives from the next processEvents().
        PointerEvent e;
        e.type = PointerEvent::Type::Move;
        e.position = position;
        e.modifiers = modifiersOf([NSEvent modifierFlags]);
        m_pendingPointer.push_back(e);
    }

    // AppKit animates into and out of full screen, and ignores a toggle made
    // during the animation ("not in fullscreen state"). So m_fullScreen is
    // the state asked for, and a request made mid-transition is applied when
    // the transition ends.
    void setFullScreen(bool fullScreen) override {
        m_fullScreen = fullScreen;
        applyFullScreen();
    }
    bool isFullScreen() const override { return m_fullScreen; }

    void applyFullScreen() {
        if (!m_fullScreenTransition && m_fullScreen != m_inFullScreen) {
            m_fullScreenTransition = true;
            [m_window toggleFullScreen:nil];
        }
    }
    // A transition begins: ours, or the user's (the green button), which
    // then becomes the state asked for.
    void fullScreenWillChange(bool fullScreen) {
        if (!m_fullScreenTransition) {
            m_fullScreen = fullScreen;
        }
        m_fullScreenTransition = true;
    }
    void fullScreenDidChange(bool fullScreen, bool failed) {
        m_fullScreenTransition = false;
        if (failed) {
            m_fullScreen = m_inFullScreen; // do not retry forever
        } else {
            m_inFullScreen = fullScreen;
        }
        applyFullScreen();
    }

    void setTextInputArea(const std::optional<RectF> &caret) override {
        m_caret = caret;
        if (!caret && [m_markedText length] > 0) {
            [[m_view inputContext] discardMarkedText];
            m_markedText = nil;
            composition.emit(CompositionEvent{});
        }
    }

    Vec2i screenPosition() const override {
        const NSRect inWindow = [m_view convertRect:[m_view bounds] toView:nil];
        const NSRect onScreen = [m_window convertRectToScreen:inWindow];
        const CGFloat mainHeight = [[[NSScreen screens] firstObject] frame].size.height;
        const CGFloat scale = [m_window backingScaleFactor];
        return {int(std::lround(onScreen.origin.x * scale)),
                int(std::lround((mainHeight - onScreen.origin.y - onScreen.size.height) * scale))};
    }

    // The content view (an NSView *), which a GL context draws into.
    void *nativeHandle() const override { return (__bridge void *)m_view; }

    // ---- Called by the view and the delegate ----

    Vec2i currentPixelSize() const {
        const NSRect bounds = [m_view bounds];
        const CGFloat scale = [m_window backingScaleFactor];
        return {std::max(1, int(std::lround(bounds.size.width * scale))),
                std::max(1, int(std::lround(bounds.size.height * scale)))};
    }

    void sizeChanged() {
        const Vec2i size = currentPixelSize();
        if (!(size == m_size)) {
            m_size = size;
            resized.emit(size);
        }
        m_repaint = true;
        // A live resize runs AppKit's own loop: repaint now, or the window
        // would stay stale until the mouse is released.
        if ([m_window inLiveResize]) {
            m_repaint = false;
            repaintRequested.emit();
        }
    }

    void scaleChanged() {
        dpiChanged.emit(devicePixelRatio());
        sizeChanged();
    }

    void drawFrame() {
        if (!m_image) {
            return;
        }
        CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
        const CGFloat scale = [m_window backingScaleFactor];
        const CGFloat width = CGFloat(m_frameSize.x) / scale;
        const CGFloat height = CGFloat(m_frameSize.y) / scale;
        const CGFloat viewHeight = [m_view bounds].size.height;
        CGContextSaveGState(context);
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
        // The view is flipped (y down); CGContextDrawImage draws upright only
        // in a y-up space.
        CGContextTranslateCTM(context, 0, viewHeight);
        CGContextScaleCTM(context, 1, -1);
        CGContextDrawImage(context, CGRectMake(0, viewHeight - height, width, height), m_image);
        CGContextRestoreGState(context);
    }

    Vec2 logical(NSEvent *event) const {
        const NSPoint p = [m_view convertPoint:[event locationInWindow] fromView:nil];
        return {float(p.x), float(p.y)};
    }

    void mouse(NSEvent *event, PointerEvent::Type type, PointerButton button) {
        PointerEvent e;
        e.type = type;
        e.position = logical(event);
        e.button = button;
        e.modifiers = modifiersOf([event modifierFlags]);
        if (type == PointerEvent::Type::Press) {
            e.clickCount = int(std::max<NSInteger>(1, [event clickCount]));
        }
        pointer.emit(e);
    }

    void wheel(NSEvent *event) {
        PointerEvent e;
        e.type = PointerEvent::Type::Wheel;
        e.position = logical(event);
        e.modifiers = modifiersOf([event modifierFlags]);
        const float dx = float([event scrollingDeltaX]);
        const float dy = float([event scrollingDeltaY]);
        if ([event hasPreciseScrollingDeltas]) {
            e.wheelDelta = {dx, dy}; // trackpads: already points
        } else {
            constexpr float kNotch = 48.0f; // three lines of 16 px, as on X11 and Windows
            e.wheelDelta = {dx * kNotch, dy * kNotch};
        }
        if (e.wheelDelta.x != 0 || e.wheelDelta.y != 0) {
            pointer.emit(e);
        }
    }

    void leave(NSEvent *event) {
        PointerEvent e;
        e.type = PointerEvent::Type::Leave;
        e.position = logical(event);
        pointer.emit(e);
    }

    void keyDown(NSEvent *event) {
        KeyEvent e;
        e.type = KeyEvent::Type::Press;
        e.key = keyOf([event keyCode]);
        e.modifiers = modifiersOf([event modifierFlags]);
        e.repeat = [event isARepeat];
        key.emit(e);
        // Shortcuts (Command, Control) are keys only.
        if (hasModifier(e.modifiers, Modifier::Control) || hasModifier(e.modifiers, Modifier::Meta)) {
            return;
        }
        if (m_caret) {
            // Through the input method: insertText / setMarkedText come back.
            [m_view interpretKeyEvents:@[ event ]];
        } else {
            emitText([event characters]);
        }
    }

    void keyUp(NSEvent *event) {
        KeyEvent e;
        e.type = KeyEvent::Type::Release;
        e.key = keyOf([event keyCode]);
        e.modifiers = modifiersOf([event modifierFlags]);
        key.emit(e);
    }

    void flagsChanged(NSEvent *event) {
        const NSEventModifierFlags flag = flagOf([event keyCode]);
        if (flag == 0) {
            return; // Caps Lock, Fn
        }
        KeyEvent e;
        e.type = ([event modifierFlags] & flag) ? KeyEvent::Type::Press : KeyEvent::Type::Release;
        e.key = keyOf([event keyCode]);
        e.modifiers = modifiersOf([event modifierFlags]);
        key.emit(e);
    }

    // Printable text only: AppKit reports arrows and function keys as
    // characters in the private-use area, and Return, Tab, Escape and
    // Backspace as control characters.
    void emitText(NSString *characters) {
        const String all = fromNs(characters);
        const StringView view(all);
        String printable;
        for (std::size_t i = 0; i < view.size();) {
            const Utf8Char ch = decodeUtf8At(view, i);
            if (ch.codepoint >= 0x20 && ch.codepoint != 0x7F && !(ch.codepoint >= 0xE000 && ch.codepoint <= 0xF8FF)) {
                printable += String(view.substr(i, ch.length));
            }
            i += ch.length;
        }
        if (!printable.empty()) {
            text.emit(TextEvent{printable});
        }
    }

    // NSTextInputClient
    void insertText(NSString *string) {
        const bool composing = [m_markedText length] > 0;
        m_markedText = nil;
        if (composing) {
            composition.emit(CompositionEvent{});
        }
        emitText(string);
    }
    void setMarkedText(NSString *string, NSRange selected) {
        m_markedText = [string copy];
        CompositionEvent e;
        e.text = fromNs(string);
        e.cursor = utf8Offset(string, selected.location);
        composition.emit(e);
    }
    void unmarkText() {
        if ([m_markedText length] > 0) {
            // Commit what was being composed, as the input method intends.
            NSString *committed = m_markedText;
            m_markedText = nil;
            composition.emit(CompositionEvent{});
            emitText(committed);
        }
    }
    [[nodiscard]] NSString *markedText() const { return m_markedText; }
    // The caret in screen coordinates (where the candidate window goes).
    NSRect caretOnScreen() const {
        const RectF caret = m_caret.value_or(RectF{0, 0, 1, 16});
        const NSRect inView = NSMakeRect(caret.x, caret.y, std::max(1.0f, caret.width), std::max(1.0f, caret.height));
        return [m_window convertRectToScreen:[m_view convertRect:inView toView:nil]];
    }

    bool drop(DropEvent::Type type, id<NSDraggingInfo> info) {
        DropEvent e;
        e.type = type;
        const NSPoint p = [m_view convertPoint:[info draggingLocation] fromView:nil];
        e.position = {float(p.x), float(p.y)};
        if (type != DropEvent::Type::Leave) {
            e.paths = filePaths(info);
        }
        return handleDrop(e);
    }

    void focus(bool focused) { focusChanged.emit(focused); }
    void close() { closeRequested.emit(); }

    // processEvents(), after the queue is drained.
    void flush() {
        std::vector<PointerEvent> pending;
        pending.swap(m_pendingPointer);
        for (const PointerEvent &e : pending) {
            pointer.emit(e);
        }
    }
    [[nodiscard]] bool repaintPending() const noexcept { return m_repaint || !m_pendingPointer.empty(); }
    bool takeRepaint() {
        const bool repaint = m_repaint;
        m_repaint = false;
        return repaint;
    }
    [[nodiscard]] NSWindow *nsWindow() const { return m_window; }

private:
    NSWindow *m_window = nil;
    CFWContentView *m_view = nil;
    CFWWindowDelegate *m_delegate = nil;
    NSString *m_markedText = nil;
    std::optional<RectF> m_caret;
    Vec2i m_size;
    std::vector<std::uint32_t> m_frame;
    Vec2i m_frameSize;
    CGImageRef m_image = nullptr;
    Cursor m_cursor = Cursor::Arrow;
    bool m_cursorHidden = false;
    bool m_repaint = true;
    bool m_fullScreen = false;           // asked for
    bool m_inFullScreen = false;         // what AppKit last finished
    bool m_fullScreenTransition = false; // an animation is running
    std::vector<PointerEvent> m_pendingPointer;
};

} // namespace
} // namespace cfw

using cfw::PointerButton;
using cfw::PointerEvent;

@implementation CFWContentView {
    NSTrackingArea *_tracking;
}

- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}

- (void)updateTrackingAreas {
    if (_tracking) {
        [self removeTrackingArea:_tracking];
    }
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect | NSTrackingCursorUpdate
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirty {
    [[NSColor blackColor] setFill];
    NSRectFill(dirty);
    if (self.owner) {
        self.owner->drawFrame();
    }
}

- (void)cursorUpdate:(NSEvent *)event {
    if (self.owner) {
        self.owner->applyCursor();
    }
}

// Pointer
- (void)mouseMoved:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Move, PointerButton::None); }
- (void)mouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)mouseDown:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Press, PointerButton::Left); }
- (void)mouseUp:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Release, PointerButton::Left); }
- (void)rightMouseDown:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Press, PointerButton::Right); }
- (void)rightMouseUp:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Release, PointerButton::Right); }
- (void)otherMouseDown:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Press, PointerButton::Middle); }
- (void)otherMouseUp:(NSEvent *)e { if (self.owner) self.owner->mouse(e, PointerEvent::Type::Release, PointerButton::Middle); }
- (void)scrollWheel:(NSEvent *)e { if (self.owner) self.owner->wheel(e); }
- (void)mouseExited:(NSEvent *)e { if (self.owner) self.owner->leave(e); }

// Keys
- (void)keyDown:(NSEvent *)e { if (self.owner) self.owner->keyDown(e); }
- (void)keyUp:(NSEvent *)e { if (self.owner) self.owner->keyUp(e); }
- (void)flagsChanged:(NSEvent *)e { if (self.owner) self.owner->flagsChanged(e); }
// Command-key shortcuts reach keyDown: instead of AppKit's key equivalents
// (except those the menu bar claims first, such as Command-Q).
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if ([e type] != NSEventTypeKeyDown) {
        return NO;
    }
    if ([[NSApp mainMenu] performKeyEquivalent:e]) {
        return YES; // the menu bar's (Command-Q, Command-H)
    }
    if ([[self window] firstResponder] == self && self.owner) {
        self.owner->keyDown(e);
        return YES;
    }
    return NO;
}

// NSTextInputClient
- (void)insertText:(id)string replacementRange:(NSRange)range {
    NSString *text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    if (self.owner) self.owner->insertText(text);
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)range {
    NSString *text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    if (self.owner) self.owner->setMarkedText(text, selected);
}
- (void)unmarkText {
    if (self.owner) self.owner->unmarkText();
}
- (BOOL)hasMarkedText {
    return self.owner && [self.owner->markedText() length] > 0;
}
- (NSRange)markedRange {
    NSString *marked = self.owner ? self.owner->markedText() : nil;
    return [marked length] > 0 ? NSMakeRange(0, [marked length]) : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual {
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return NSNotFound;
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual {
    return self.owner ? self.owner->caretOnScreen() : NSZeroRect;
}
- (void)doCommandBySelector:(SEL)selector {
    // Return, arrows, Backspace... already went out as KeyEvents.
}

// File drops
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    return self.owner && self.owner->drop(cfw::DropEvent::Type::Enter, info) ? NSDragOperationCopy
                                                                               : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
    return self.owner && self.owner->drop(cfw::DropEvent::Type::Move, info) ? NSDragOperationCopy
                                                                              : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)info {
    if (self.owner) self.owner->drop(cfw::DropEvent::Type::Leave, info);
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    return self.owner && self.owner->drop(cfw::DropEvent::Type::Drop, info);
}
@end

@implementation CFWWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    if (self.owner) self.owner->close();
    return NO; // the window stays until it is destroyed
}
- (void)windowDidResize:(NSNotification *)n {
    if (self.owner) self.owner->sizeChanged();
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n {
    if (self.owner) self.owner->scaleChanged();
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    if (self.owner) self.owner->focus(true);
}
- (void)windowDidResignKey:(NSNotification *)n {
    if (self.owner) self.owner->focus(false);
}
- (void)windowWillEnterFullScreen:(NSNotification *)n {
    if (self.owner) self.owner->fullScreenWillChange(true);
}
- (void)windowWillExitFullScreen:(NSNotification *)n {
    if (self.owner) self.owner->fullScreenWillChange(false);
}
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    if (self.owner) self.owner->fullScreenDidChange(true, false);
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    if (self.owner) self.owner->fullScreenDidChange(false, false);
}
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window {
    if (self.owner) self.owner->fullScreenDidChange(true, true);
}
- (void)windowDidFailToExitFullScreen:(NSWindow *)window {
    if (self.owner) self.owner->fullScreenDidChange(false, true);
}
@end

@implementation CFWApplicationDelegate
// Command-Q (or Quit from the Dock) asks every window to close, as their
// close buttons do; the application decides.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    const std::vector<cfw::WindowCocoa *> open = cfw::windows();
    for (cfw::WindowCocoa *window : open) {
        window->close();
    }
    return open.empty() ? NSTerminateNow : NSTerminateCancel;
}
@end

namespace cfw {

Result<std::unique_ptr<Window>> Window::create(const WindowOptions &options) {
    if (![NSThread isMainThread]) {
        return Error(ErrorCode::Unsupported, "AppKit windows can only be created on the main thread");
    }
    ensureApplication();
    @autoreleasepool {
        auto window = std::make_unique<WindowCocoa>(options);
        if (options.visible) {
            window->show();
        }
        return std::unique_ptr<Window>(std::move(window));
    }
}

bool processEvents(Duration maxWait) {
    ensureApplication();
    @autoreleasepool {
        const bool pending = std::any_of(windows().begin(), windows().end(),
                                         [](const WindowCocoa *w) { return w->repaintPending(); });
        const double seconds = pending ? 0.0 : std::max(0.0, std::chrono::duration<double>(maxWait).count());
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
        for (;;) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:until
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) {
                break;
            }
            until = [NSDate distantPast]; // then drain what is queued without waiting
            if ([event type] == NSEventTypeApplicationDefined) {
                continue; // wakeUp()
            }
            // NSApplication swallows key-up while Command is held: deliver it.
            if ([event type] == NSEventTypeKeyUp && ([event modifierFlags] & NSEventModifierFlagCommand)) {
                [[NSApp keyWindow] sendEvent:event];
                continue;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
        // Warped-pointer moves, then repaints, once per window; handlers may
        // close windows, so each is looked up again.
        const std::vector<WindowCocoa *> open = windows();
        for (WindowCocoa *w : open) {
            if (std::find(windows().begin(), windows().end(), w) != windows().end()) {
                w->flush();
            }
        }
        const std::vector<WindowCocoa *> still = windows();
        for (WindowCocoa *w : still) {
            if (std::find(windows().begin(), windows().end(), w) != windows().end() && w->takeRepaint()) {
                w->repaintRequested.emit();
            }
        }
    }
    return true;
}

void wakeUp() {
    if (!gAppReady.load()) {
        return;
    }
    @autoreleasepool {
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:event atStart:NO];
    }
}

String clipboardText() {
    @autoreleasepool {
        return fromNs([[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString]);
    }
}

void setClipboardText(StringView text) {
    @autoreleasepool {
        NSPasteboard *board = [NSPasteboard generalPasteboard];
        [board clearContents];
        [board setString:toNs(text) forType:NSPasteboardTypeString];
    }
}

} // namespace cfw
