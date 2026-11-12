// macOS: the game's window (AppKit, on the main thread) with a CAMetalLayer
// for MoltenVK. It sends the window procedure the Win32 messages the game
// handles (WM_KEYDOWN with Windows key codes, WM_SYSKEYDOWN with Option held,
// mouse messages in the window's pixels, WM_SIZE, WM_CLOSE), so
// VkPresenter::wndproc and the menus' input hook work unchanged.
#pragma once
#include "mac_win32.h"

typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// background: behind the other windows and never taking the keyboard (LW_BACKGROUND)
HWND mac_window_create(const char* title, int width, int height, bool background, WNDPROC proc);
void mac_window_destroy(HWND);
// Handles the waiting events (the message loop); main thread only
void mac_window_pump();
// The window's CAMetalLayer, for vkCreateMetalSurfaceEXT
void* mac_window_layer(HWND);
// macOS full screen (its own space); WM_SIZE follows when it settles
void mac_window_toggle_fullscreen(HWND);
bool mac_window_fullscreen(HWND);
// The arrow over the window (menus), else no pointer
void mac_window_set_cursor_visible(HWND, bool visible);
// The shorter side, in real pixels, of the display mode of the screen the
// window is on (0: not known), as app.cpp's screen_height reads it on Windows
int mac_window_screen_height(HWND);
// The folder of the executable, with a trailing slash
const char* mac_exe_dir();

// What the window procedure calls
#define GWLP_USERDATA (-21)
LONG_PTR GetWindowLongPtrA(HWND, int index);
LONG_PTR SetWindowLongPtrA(HWND, int index, LONG_PTR value);
inline LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
// WM_SETCURSOR is never sent on macOS (mac_window_set_cursor_visible does it)
typedef void* HCURSOR;
#define IDC_ARROW (reinterpret_cast<LPCSTR>(intptr_t(32512)))
#define IDC_CROSS (reinterpret_cast<LPCSTR>(intptr_t(32515)))
inline HCURSOR LoadCursor(HINSTANCE, LPCSTR) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
