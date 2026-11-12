// macOS: the game's window (mac_window.h). AppKit runs on the main thread,
// which is the one that draws (app.cpp's main): the game itself is on a thread
// of its own, so the window only turns events into the Win32 messages the
// game handles and keeps the key state GetAsyncKeyState reads.
#define MAC_WIN32_NO_BOOL
#include "mac_window.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach-o/dyld.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>

@class SR2View;

struct HWND__ {
    NSWindow* window = nil;
    SR2View* view = nil;
    id delegate = nil;
    WNDPROC proc = nullptr;
    LONG_PTR user = 0;
    bool fullscreen = false;
    bool show_cursor = false;
    double wheel = 0;   // trackpad scrolling, gathered into whole notches
};

namespace {

std::atomic<uint8_t> g_keys[256];   // held keys, by Windows key code
std::atomic<bool> g_focused{false};
HWND g_window = nullptr;

// macOS key codes (Carbon's kVK_*) to Windows virtual keys
int vk_from_keycode(unsigned short code) {
    static const struct {
        unsigned short code;
        uint8_t vk;
    } map[] = {
        {0x00, 'A'}, {0x0B, 'B'}, {0x08, 'C'}, {0x02, 'D'}, {0x0E, 'E'}, {0x03, 'F'}, {0x05, 'G'}, {0x04, 'H'},
        {0x22, 'I'}, {0x26, 'J'}, {0x28, 'K'}, {0x25, 'L'}, {0x2E, 'M'}, {0x2D, 'N'}, {0x1F, 'O'}, {0x23, 'P'},
        {0x0C, 'Q'}, {0x0F, 'R'}, {0x01, 'S'}, {0x11, 'T'}, {0x20, 'U'}, {0x09, 'V'}, {0x0D, 'W'}, {0x07, 'X'},
        {0x10, 'Y'}, {0x06, 'Z'},
        {0x1D, '0'}, {0x12, '1'}, {0x13, '2'}, {0x14, '3'}, {0x15, '4'}, {0x17, '5'}, {0x16, '6'}, {0x1A, '7'},
        {0x1C, '8'}, {0x19, '9'},
        {0x24, VK_RETURN}, {0x4C, VK_RETURN}, {0x35, VK_ESCAPE}, {0x30, VK_TAB}, {0x31, VK_SPACE},
        {0x33, VK_BACK}, {0x75, VK_DELETE}, {0x73, VK_HOME}, {0x77, VK_END}, {0x74, VK_PRIOR}, {0x79, VK_NEXT},
        {0x7B, VK_LEFT}, {0x7C, VK_RIGHT}, {0x7D, VK_DOWN}, {0x7E, VK_UP},
        {0x7A, VK_F1}, {0x78, VK_F2}, {0x63, VK_F3}, {0x76, VK_F4}, {0x60, VK_F5}, {0x61, VK_F6},
        {0x62, VK_F7}, {0x64, VK_F8}, {0x65, VK_F9}, {0x6D, VK_F10}, {0x67, VK_F11}, {0x6F, VK_F12},
        {0x38, VK_LSHIFT}, {0x3C, VK_RSHIFT}, {0x3B, VK_LCONTROL}, {0x3E, VK_RCONTROL},
        {0x3A, VK_LMENU}, {0x3D, VK_RMENU},
        {0x52, VK_NUMPAD0}, {0x53, VK_NUMPAD0 + 1}, {0x54, VK_NUMPAD0 + 2}, {0x55, VK_NUMPAD0 + 3},
        {0x56, VK_NUMPAD0 + 4}, {0x57, VK_NUMPAD0 + 5}, {0x58, VK_NUMPAD0 + 6}, {0x59, VK_NUMPAD0 + 7},
        {0x5B, VK_NUMPAD0 + 8}, {0x5C, VK_NUMPAD0 + 9}, {0x45, VK_ADD}, {0x4E, VK_SUBTRACT},
        {0x43, VK_MULTIPLY}, {0x4B, VK_DIVIDE}, {0x41, VK_DECIMAL},
        {0x18, VK_OEM_PLUS}, {0x1B, VK_OEM_MINUS}, {0x2B, VK_OEM_COMMA}, {0x2F, VK_OEM_PERIOD},
        {0x29, VK_OEM_1}, {0x2C, VK_OEM_2}, {0x32, VK_OEM_3}, {0x21, VK_OEM_4}, {0x2A, VK_OEM_5},
        {0x1E, VK_OEM_6}, {0x27, VK_OEM_7},
    };
    for (const auto& m : map)
        if (m.code == code) return m.vk;
    return 0;
}

// The generic key (VK_SHIFT...) is held while either side is
void set_key(int vk, bool down) {
    g_keys[vk] = down;
    auto either = [](int a, int b) { return g_keys[a].load() || g_keys[b].load(); };
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_keys[VK_SHIFT] = either(VK_LSHIFT, VK_RSHIFT);
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_keys[VK_CONTROL] = either(VK_LCONTROL, VK_RCONTROL);
    if (vk == VK_LMENU || vk == VK_RMENU) g_keys[VK_MENU] = either(VK_LMENU, VK_RMENU);
}

void release_all_keys() {
    for (auto& k : g_keys) k = 0;
}

LRESULT send(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return w && w->proc ? w->proc(w, msg, wp, lp) : 0; }

}  // namespace

// The content view: its backing layer is the CAMetalLayer MoltenVK draws to
@interface SR2View : NSView
@property(nonatomic, assign) HWND owner;
@end

@implementation SR2View
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.opaque = YES;
    return layer;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    self.layer.contentsScale = self.window.backingScaleFactor;
    send(self.owner, WM_SIZE, 0, 0);
}
- (void)resetCursorRects {
    static NSCursor* none = [] {
        NSImage* empty = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        return [[NSCursor alloc] initWithImage:empty hotSpot:NSZeroPoint];
    }();
    [self addCursorRect:self.bounds cursor:self.owner && self.owner->show_cursor ? [NSCursor arrowCursor] : none];
}
// Keys are handled in mac_window_pump; these only stop AppKit's beep
- (void)keyDown:(NSEvent*)event {
}
- (void)keyUp:(NSEvent*)event {
}
@end

@interface SR2WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) HWND owner;
@end

@implementation SR2WindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    send(self.owner, WM_CLOSE, 0, 0);
    return NO;   // the game closes it when it is ready
}
- (void)windowDidResize:(NSNotification*)n {
    send(self.owner, WM_SIZE, 0, 0);
}
- (void)windowDidBecomeKey:(NSNotification*)n {
    g_focused = true;
}
- (void)windowDidResignKey:(NSNotification*)n {
    g_focused = false;
    release_all_keys();   // no key stays held while another window has the keyboard
}
- (void)windowDidEnterFullScreen:(NSNotification*)n {
    self.owner->fullscreen = true;
    send(self.owner, WM_SIZE, 0, 0);
}
- (void)windowDidExitFullScreen:(NSNotification*)n {
    self.owner->fullscreen = false;
    send(self.owner, WM_SIZE, 0, 0);
}
@end

namespace {

void start_app() {
    static bool started = false;
    if (started) return;
    started = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    // The application menu, for Cmd+Q (as closing the window) and Cmd+H
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* app_item = [[NSMenuItem alloc] init];
    [bar addItem:app_item];
    NSMenu* app_menu = [[NSMenu alloc] init];
    [app_menu addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
    [app_menu addItemWithTitle:@"Quit" action:@selector(performClose:) keyEquivalent:@"q"];
    [app_item setSubmenu:app_menu];
    [NSApp setMainMenu:bar];
    [NSApp finishLaunching];
}

// Where the pointer is, in the window's pixels from the top left (as Win32's client coordinates)
LPARAM mouse_lparam(HWND w, NSEvent* e) {
    NSPoint p = [w->view convertPoint:e.locationInWindow fromView:nil];
    double scale = w->window.backingScaleFactor;
    int x = int(std::floor(p.x * scale)), y = int(std::floor((w->view.bounds.size.height - p.y) * scale));
    return MAKELPARAM(x, y);
}

bool in_view(HWND w, NSEvent* e) {
    NSPoint p = [w->view convertPoint:e.locationInWindow fromView:nil];
    return NSPointInRect(p, w->view.bounds);
}

// true: handled here (not passed on to AppKit)
bool handle(HWND w, NSEvent* e) {
    if (!w || e.window != w->window) return false;
    switch (e.type) {
    case NSEventTypeKeyDown: {
        if (e.modifierFlags & NSEventModifierFlagCommand) return false;   // menu shortcuts
        int vk = vk_from_keycode(e.keyCode);
        if (!vk) return true;
        set_key(vk, true);
        // Option is Alt: Option+Return toggles full screen as Alt+Enter does
        UINT msg = (e.modifierFlags & NSEventModifierFlagOption) ? WM_SYSKEYDOWN : WM_KEYDOWN;
        send(w, msg, WPARAM(vk), e.isARepeat ? LPARAM(1) << 30 : 0);
        return true;
    }
    case NSEventTypeKeyUp: {
        if (int vk = vk_from_keycode(e.keyCode)) {
            set_key(vk, false);
            send(w, WM_KEYUP, WPARAM(vk), 0);
        }
        return !(e.modifierFlags & NSEventModifierFlagCommand);
    }
    case NSEventTypeFlagsChanged: {
        int vk = vk_from_keycode(e.keyCode);
        if (!vk) return false;
        NSEventModifierFlags flag = vk == VK_LSHIFT || vk == VK_RSHIFT       ? NSEventModifierFlagShift
                                    : vk == VK_LCONTROL || vk == VK_RCONTROL ? NSEventModifierFlagControl
                                                                             : NSEventModifierFlagOption;
        bool down = (e.modifierFlags & flag) != 0 && !g_keys[vk];
        set_key(vk, down);
        if (down) send(w, WM_KEYDOWN, WPARAM(vk == VK_LSHIFT || vk == VK_RSHIFT ? VK_SHIFT
                                              : vk == VK_LCONTROL || vk == VK_RCONTROL ? VK_CONTROL
                                                                                       : VK_MENU), 0);
        return false;
    }
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
        if (in_view(w, e)) send(w, WM_MOUSEMOVE, 0, mouse_lparam(w, e));
        return false;
    case NSEventTypeLeftMouseDown:
        if (in_view(w, e)) send(w, WM_LBUTTONDOWN, 0, mouse_lparam(w, e));
        return false;
    case NSEventTypeLeftMouseUp:
        if (in_view(w, e)) send(w, WM_LBUTTONUP, 0, mouse_lparam(w, e));
        return false;
    case NSEventTypeRightMouseDown:
        if (in_view(w, e)) send(w, WM_RBUTTONDOWN, 0, mouse_lparam(w, e));
        return false;
    case NSEventTypeScrollWheel: {
        if (!in_view(w, e)) return false;
        // A mouse wheel gives about 1 per notch; a trackpad many small steps
        w->wheel += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 10.0 : e.scrollingDeltaY;
        int notches = int(w->wheel);
        if (notches) {
            w->wheel -= notches;
            send(w, WM_MOUSEWHEEL, WPARAM(uint32_t(uint16_t(int16_t(notches * WHEEL_DELTA))) << 16),
                 mouse_lparam(w, e));
        }
        return false;
    }
    default:
        return false;
    }
}

}  // namespace

HWND mac_window_create(const char* title, int width, int height, bool background, WNDPROC proc) {
    @autoreleasepool {
        start_app();
        HWND w = new HWND__;
        w->proc = proc;
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        NSScreen* screen = [NSScreen mainScreen];
        double scale = screen ? screen.backingScaleFactor : 1.0;
        // width x height are pixels, as on Windows
        NSRect frame = NSMakeRect(0, 0, width / scale, height / scale);
        w->window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
        w->window.releasedWhenClosed = NO;
        w->window.title = [NSString stringWithUTF8String:title ? title : ""];
        w->window.acceptsMouseMovedEvents = YES;
        w->window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        w->view = [[SR2View alloc] initWithFrame:frame];
        w->view.owner = w;
        w->view.wantsLayer = YES;
        w->view.layer.contentsScale = w->window.backingScaleFactor;
        w->window.contentView = w->view;
        SR2WindowDelegate* delegate = [[SR2WindowDelegate alloc] init];
        delegate.owner = w;
        w->delegate = delegate;
        w->window.delegate = delegate;
        [w->window center];
        if (background) {
            [w->window orderBack:nil];
        } else {
            [w->window makeKeyAndOrderFront:nil];
            [w->window makeFirstResponder:w->view];
            [NSApp activateIgnoringOtherApps:YES];
        }
        g_window = w;
        mac_window_pump();
        return w;
    }
}

void mac_window_destroy(HWND w) {
    if (!w) return;
    @autoreleasepool {
        w->window.delegate = nil;
        [w->window close];
        w->window = nil;
        w->view = nil;
        w->delegate = nil;
    }
    if (g_window == w) g_window = nullptr;
    delete w;
}

void mac_window_pump() {
    @autoreleasepool {
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            if (!handle(g_window, e)) [NSApp sendEvent:e];
        }
        [NSApp updateWindows];
    }
}

void* mac_window_layer(HWND w) { return w ? (__bridge void*)w->view.layer : nullptr; }

void mac_window_toggle_fullscreen(HWND w) {
    if (w) [w->window toggleFullScreen:nil];
}

bool mac_window_fullscreen(HWND w) { return w && w->fullscreen; }

void mac_window_set_cursor_visible(HWND w, bool visible) {
    if (!w || w->show_cursor == visible) return;
    w->show_cursor = visible;
    [w->window invalidateCursorRectsForView:w->view];
}

const char* mac_exe_dir() {
    static std::string dir = [] {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string p(size, '\0');
        if (_NSGetExecutablePath(p.data(), &size) != 0) return std::string("./");
        char real[PATH_MAX];
        std::string path = realpath(p.c_str(), real) ? std::string(real) : std::string(p.c_str());
        return path.substr(0, path.find_last_of('/') + 1);
    }();
    return dir.c_str();
}

LONG_PTR GetWindowLongPtrA(HWND w, int index) { return w && index == GWLP_USERDATA ? w->user : 0; }

LONG_PTR SetWindowLongPtrA(HWND w, int index, LONG_PTR value) {
    if (!w || index != GWLP_USERDATA) return 0;
    LONG_PTR old = w->user;
    w->user = value;
    return old;
}

// ---- What mac_win32.h promises about the window ----

SHORT GetAsyncKeyState(int vk) { return vk >= 0 && vk < 256 && g_keys[vk] ? SHORT(0x8000) : 0; }
SHORT GetKeyState(int vk) { return GetAsyncKeyState(vk); }

HWND GetForegroundWindow() { return g_focused && g_window ? g_window : nullptr; }

WINBOOL GetClientRect(HWND w, RECT* r) {
    *r = RECT{0, 0, 0, 0};
    if (!w) return FALSE;
    NSSize s = w->view.bounds.size;
    double scale = w->window.backingScaleFactor;
    r->right = LONG(std::lround(s.width * scale));
    r->bottom = LONG(std::lround(s.height * scale));
    return TRUE;
}

WINBOOL ScreenToClient(HWND, POINT*) { return TRUE; }

WINBOOL SetWindowTextA(HWND w, LPCSTR text) {
    if (!w) return FALSE;
    std::string t = text ? text : "";
    NSWindow* window = w->window;
    dispatch_async(dispatch_get_main_queue(), ^{
        window.title = [NSString stringWithUTF8String:t.c_str()];
    });
    return TRUE;
}
