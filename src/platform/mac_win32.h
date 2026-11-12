// macOS: the part of <windows.h> this project uses, so the shared code builds
// unchanged where a Win32 call has a plain macOS equivalent. Included instead
// of <windows.h> under __APPLE__. Nothing here is a general Win32 layer: only
// what the game calls, with the behaviour it relies on (docs/MACOS.md).
//
//   mac_win32.mm  files, clocks, threads, message boxes, text conversion,
//                 links (ShellExecuteW), the achievement chime (PlaySoundW)
//   mac_window.mm the window: key state, focus, client size, title
//   mac_gdi.mm    the GDI text drawing of the menus and the achievement
//                 notice (fonts from memory, measure, draw into a DIB)
#pragma once
#if !defined(__APPLE__)
#error "mac_win32.h is for macOS only"
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <strings.h>
#include <wchar.h>

// ---- Types ----
typedef int WINBOOL;
// Objective-C has a BOOL of its own: the .mm files define MAC_WIN32_NO_BOOL
#if !defined(MAC_WIN32_NO_BOOL)
typedef WINBOOL BOOL;
#endif
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t UINT;
typedef uint32_t UINT32;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef intptr_t INT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int32_t HRESULT;
typedef void* LPVOID;
typedef void* HANDLE;
typedef const char* LPCSTR;
typedef const wchar_t* LPCWSTR;
struct HWND__;
typedef HWND__* HWND;
struct HINSTANCE__;
typedef HINSTANCE__* HINSTANCE;
struct HFONT__;
typedef HFONT__* HFONT;
struct HDC__;
typedef HDC__* HDC;
struct HBITMAP__;
typedef HBITMAP__* HBITMAP;
typedef void* HGDIOBJ;
typedef uint32_t COLORREF;

#define WINAPI
#define CALLBACK
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(intptr_t(-1)))
#define MAX_PATH 1024
#define INFINITE 0xFFFFFFFFu
#define S_OK 0
#define E_FAIL int32_t(0x80004005u)
#define SUCCEEDED(hr) (HRESULT(hr) >= 0)
#define FAILED(hr) (HRESULT(hr) < 0)

union LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
};
struct POINT {
    LONG x, y;
};
struct RECT {
    LONG left, top, right, bottom;
};
struct SIZE {
    LONG cx, cy;
};

#define LOWORD(l) (WORD(uintptr_t(l) & 0xFFFF))
#define HIWORD(l) (WORD((uintptr_t(l) >> 16) & 0xFFFF))
#define MAKELPARAM(lo, hi) (LPARAM(uint32_t(WORD(lo)) | (uint32_t(WORD(hi)) << 16)))
#define GET_X_LPARAM(lp) (int(short(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (int(short(HIWORD(lp))))
#define GET_WHEEL_DELTA_WPARAM(wp) (short(HIWORD(wp)))
#define WHEEL_DELTA 120
#define RGB(r, g, b) (COLORREF(uint8_t(r) | (uint16_t(uint8_t(g)) << 8) | (uint32_t(uint8_t(b)) << 16)))

inline WINBOOL PtInRect(const RECT* r, POINT p) {
    return p.x >= r->left && p.x < r->right && p.y >= r->top && p.y < r->bottom;
}

// ---- Window messages and keys (mac_window.mm sends the same ones) ----
#define WM_SIZE 0x0005
#define WM_CLOSE 0x0010
#define WM_SETCURSOR 0x0020
#define WM_NCHITTEST 0x0084
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_MOUSEWHEEL 0x020A
#define HTCLIENT 1

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE

// mac_window.mm: the game's window (the one VkPresenter made). Coordinates are
// the window's pixels (its drawable), as on Windows
typedef int16_t SHORT;
SHORT GetAsyncKeyState(int vk);   // 0x8000 while held (keys pressed in this window only)
SHORT GetKeyState(int vk);
HWND GetForegroundWindow();       // the game's window while it has the keyboard, else nullptr
WINBOOL GetClientRect(HWND, RECT*);
WINBOOL ScreenToClient(HWND, POINT*);   // WM_MOUSEWHEEL already carries window coordinates here
WINBOOL SetWindowTextA(HWND, LPCSTR);

// ---- Clocks and threads (mac_win32.mm) ----
WINBOOL QueryPerformanceFrequency(LARGE_INTEGER*);
WINBOOL QueryPerformanceCounter(LARGE_INTEGER*);
ULONGLONG GetTickCount64();
void Sleep(DWORD ms);
inline UINT timeBeginPeriod(UINT) { return 0; }   // macOS timers are fine-grained already
inline UINT timeEndPeriod(UINT) { return 0; }

typedef DWORD(WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
// A pthread with that stack size (the game thread asks for 512 MB)
HANDLE CreateThread(void* security, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD* id);
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
DWORD WaitForSingleObject(HANDLE thread, DWORD ms);   // threads from CreateThread only
WINBOOL CloseHandle(HANDLE);
[[noreturn]] void ExitProcess(UINT code);

// ---- Files ----
#define INVALID_FILE_ATTRIBUTES DWORD(0xFFFFFFFF)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define MOVEFILE_REPLACE_EXISTING 0x1
WINBOOL CreateDirectoryA(LPCSTR path, void* security);
DWORD GetFileAttributesA(LPCSTR path);
DWORD GetFileAttributesW(LPCWSTR path);
DWORD GetFullPathNameA(LPCSTR path, DWORD size, char* out, char** file_part);
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, wchar_t* out, wchar_t** file_part);
DWORD GetModuleFileNameA(HINSTANCE, char* out, DWORD size);
DWORD GetModuleFileNameW(HINSTANCE, wchar_t* out, DWORD size);
WINBOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    char cFileName[MAX_PATH];
};
// Patterns of the form "dir/*.ext", "dir/*" or "dir/name*"
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd);
WINBOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA* fd);
WINBOOL FindClose(HANDLE);

// The console: on macOS stdout and stderr are always where they were started from
#define STD_OUTPUT_HANDLE DWORD(-11)
#define STD_ERROR_HANDLE DWORD(-12)
#define FILE_TYPE_UNKNOWN 0
#define FILE_TYPE_CHAR 2
#define ATTACH_PARENT_PROCESS DWORD(-1)
inline HANDLE GetStdHandle(DWORD) { return reinterpret_cast<HANDLE>(intptr_t(1)); }
inline DWORD GetFileType(HANDLE) { return FILE_TYPE_CHAR; }
inline WINBOOL AttachConsole(DWORD) { return FALSE; }

// ---- Messages to the user (a native alert; from any thread) ----
#define MB_OK 0x0
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type);

// ---- Text ----
#define CP_UTF8 65001
// UTF-8 to wchar_t (UTF-32 here); the other code pages are not used
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int n, wchar_t* out, int out_n);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* s, int n, char* out, int out_n, const char*, WINBOOL*);
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
// "NAME=" clears it, as on Windows
inline int _putenv(const char* s) {
    const char* eq = strchr(s, '=');
    if (!eq) return -1;
    char name[256];
    size_t n = size_t(eq - s) < sizeof(name) - 1 ? size_t(eq - s) : sizeof(name) - 1;
    memcpy(name, s, n);
    name[n] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 1; }
// The CRT's environment block
#include <crt_externs.h>
#define _environ (*_NSGetEnviron())

// ---- Opening links and programs ----
#define SW_SHOWNORMAL 1
// http(s) links open in the default browser. Anything else (update.bat, a
// Windows .exe) fails with 0, as a missing file does on Windows
HINSTANCE ShellExecuteW(HWND, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show);

// ---- The achievement chime: a WAV in memory ----
#define SND_ASYNC 0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY 0x0004
WINBOOL PlaySoundW(LPCWSTR sound, HINSTANCE, DWORD flags);   // nullptr stops it

// ---- Display timing (DWM). Not available yet on macOS: app.cpp's clock
// then follows the presents, as on Windows when DWM does not answer ----
struct DWM_TIMING_INFO {
    UINT32 cbSize;
    ULONGLONG qpcRefreshPeriod;
    ULONGLONG qpcVBlank;
};
inline HRESULT DwmGetCompositionTimingInfo(HWND, DWM_TIMING_INFO*) { return E_FAIL; }

// ---- BMP files (screenshots, dumps): Windows' exact layout ----
#pragma pack(push, 2)
struct BITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1, bfReserved2;
    DWORD bfOffBits;
};
#pragma pack(pop)
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
struct RGBQUAD {
    BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
};
struct BITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
};
#define BI_RGB 0
static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40, "BMP layout");

// ---- GDI text (mac_gdi.mm, on Core Text) ----
// Enough for: fonts added from memory and chosen by family name, white text
// drawn antialiased into a 32-bit top-down DIB, its extent and metrics
struct TEXTMETRICW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    LONG tmAveCharWidth, tmMaxCharWidth, tmWeight;
};
#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_TT_ONLY_PRECIS 7
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_PITCH 0
#define TRANSPARENT 1
#define DIB_RGB_COLORS 0
HANDLE AddFontMemResourceEx(void* data, DWORD size, void* reserved, DWORD* count);
WINBOOL RemoveFontMemResourceEx(HANDLE);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch, LPCWSTR face);
HDC CreateCompatibleDC(HDC);
WINBOOL DeleteDC(HDC);
HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT usage, void** bits, HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
WINBOOL DeleteObject(HGDIOBJ);
int SetBkMode(HDC, int mode);
COLORREF SetTextColor(HDC, COLORREF);
WINBOOL TextOutW(HDC, int x, int y, LPCWSTR text, int n);
WINBOOL GetTextExtentPoint32W(HDC, LPCWSTR text, int n, SIZE* out);
WINBOOL GetTextMetricsW(HDC, TEXTMETRICW* out);
inline WINBOOL GdiFlush() { return TRUE; }
