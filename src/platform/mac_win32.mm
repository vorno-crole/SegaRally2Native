// macOS: files, clocks, threads, message boxes, text conversion, links and
// the achievement chime behind the Win32 names the shared code calls
// (mac_win32.h). Each function keeps the Windows behaviour the game relies on.
#define MAC_WIN32_NO_BOOL
#include "mac_win32.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <fnmatch.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string narrow(const wchar_t* w) {
    std::string s;
    if (!w) return s;
    for (; *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c < 0x80) s += char(c);
        else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
        else if (c < 0x10000) s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
        else s += char(0xF0 | (c >> 18)), s += char(0x80 | ((c >> 12) & 0x3F)), s += char(0x80 | ((c >> 6) & 0x3F)),
             s += char(0x80 | (c & 0x3F));
    }
    return s;
}

std::wstring widen(const std::string& s) {
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    w.resize(size_t(n > 0 ? n : 0));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Copies s into out (size chars with the terminator) and returns its length,
// or the size needed (with the terminator) when it does not fit: as Windows
template <typename C>
DWORD copy_out(const std::basic_string<C>& s, C* out, DWORD size) {
    if (s.size() + 1 > size) return DWORD(s.size() + 1);
    std::copy(s.begin(), s.end(), out);
    out[s.size()] = 0;
    return DWORD(s.size());
}

std::string exe_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string p(size, '\0');
    if (_NSGetExecutablePath(p.data(), &size) != 0) return std::string();
    p.resize(strlen(p.c_str()));
    char real[PATH_MAX];
    return realpath(p.c_str(), real) ? std::string(real) : p;
}

// "a/./b/../c" -> "a/c" on an absolute path (GetFullPathName does not need the file to exist)
std::string full_path(const std::string& path) {
    std::string abs = path;
    if (abs.empty() || abs[0] != '/') {
        char cwd[PATH_MAX];
        abs = std::string(getcwd(cwd, sizeof(cwd)) ? cwd : ".") + "/" + abs;
    }
    for (char& c : abs)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= abs.size()) {
        size_t end = abs.find('/', at);
        if (end == std::string::npos) end = abs.size();
        std::string part = abs.substr(at, end - at);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        at = end + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

DWORD attributes(const std::string& path) {
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

struct Thread {
    pthread_t id;
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
};

struct Find {
    DIR* dir = nullptr;
    std::string path, pattern;
};

bool find_next(Find* f, WIN32_FIND_DATAA* fd) {
    while (dirent* e = readdir(f->dir)) {
        if (fnmatch(f->pattern.c_str(), e->d_name, 0) != 0) continue;
        snprintf(fd->cFileName, sizeof(fd->cFileName), "%s", e->d_name);
        fd->dwFileAttributes = attributes(f->path + "/" + e->d_name);
        return true;
    }
    return false;
}

}  // namespace

// ---- Clocks and threads ----

WINBOOL QueryPerformanceFrequency(LARGE_INTEGER* f) {
    f->QuadPart = 1000000000;   // nanoseconds
    return TRUE;
}

WINBOOL QueryPerformanceCounter(LARGE_INTEGER* t) {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    t->QuadPart = int64_t(__uint128_t(mach_absolute_time()) * tb.numer / tb.denom);
    return TRUE;
}

ULONGLONG GetTickCount64() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return ULONGLONG(t.QuadPart / 1000000);
}

void Sleep(DWORD ms) {
    if (ms == INFINITE) {
        for (;;) pause();
    }
    usleep(useconds_t(ms) * 1000);
}

HANDLE CreateThread(void*, size_t stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD* id) {
    auto* t = new Thread{nullptr, fn, arg};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack) {
        size_t page = size_t(getpagesize());
        pthread_attr_setstacksize(&attr, (stack + page - 1) / page * page);
    }
    int r = pthread_create(&t->id, &attr, [](void* p) -> void* {
        auto* t = static_cast<Thread*>(p);
        return reinterpret_cast<void*>(uintptr_t(t->fn(t->arg)));
    }, t);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        fprintf(stderr, "[mac] CreateThread: pthread_create failed (%d)\n", r);
        delete t;
        return nullptr;
    }
    if (id) *id = 0;
    return t;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    auto* t = static_cast<Thread*>(h);
    if (!t) return WAIT_TIMEOUT;
    if (ms != INFINITE) {
        fprintf(stderr, "[mac] WaitForSingleObject: only INFINITE waits on threads are supported\n");
        return WAIT_TIMEOUT;
    }
    pthread_join(t->id, nullptr);
    delete t;
    return WAIT_OBJECT_0;
}

WINBOOL CloseHandle(HANDLE) { return TRUE; }   // threads are released by the wait

void ExitProcess(UINT code) {
    fflush(stdout);
    fflush(stderr);
    _exit(int(code));   // as on Windows: other threads end with it, nothing else runs
}

// ---- Files ----

WINBOOL CreateDirectoryA(LPCSTR path, void*) {
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    return mkdir(p.c_str(), 0755) == 0 ? TRUE : FALSE;
}

DWORD GetFileAttributesA(LPCSTR path) { return attributes(path); }
DWORD GetFileAttributesW(LPCWSTR path) { return attributes(narrow(path)); }

DWORD GetFullPathNameA(LPCSTR path, DWORD size, char* out, char** file_part) {
    if (file_part) *file_part = nullptr;
    return copy_out(full_path(path), out, size);
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, wchar_t* out, wchar_t** file_part) {
    if (file_part) *file_part = nullptr;
    return copy_out(widen(full_path(narrow(path))), out, size);
}

DWORD GetModuleFileNameA(HINSTANCE, char* out, DWORD size) {
    std::string p = exe_path();
    DWORD n = copy_out(p, out, size);
    return n > p.size() ? 0 : n;
}

DWORD GetModuleFileNameW(HINSTANCE, wchar_t* out, DWORD size) {
    std::wstring p = widen(exe_path());
    DWORD n = copy_out(p, out, size);
    return n > p.size() ? 0 : n;
}

WINBOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD) {
    return rename(from, to) == 0 ? TRUE : FALSE;   // rename() always replaces, atomically
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd) {
    std::string p = pattern;
    for (char& c : p)
        if (c == '\\') c = '/';
    size_t slash = p.find_last_of('/');
    auto* f = new Find;
    f->path = slash == std::string::npos ? "." : p.substr(0, slash);
    f->pattern = slash == std::string::npos ? p : p.substr(slash + 1);
    f->dir = opendir(f->path.c_str());
    if (!f->dir || !find_next(f, fd)) {
        if (f->dir) closedir(f->dir);
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

WINBOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) {
    return h && h != INVALID_HANDLE_VALUE && find_next(static_cast<Find*>(h), fd) ? TRUE : FALSE;
}

WINBOOL FindClose(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    auto* f = static_cast<Find*>(h);
    closedir(f->dir);
    delete f;
    return TRUE;
}

// ---- Messages to the user ----

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    fprintf(stderr, "%s: %s\n", caption ? caption : "", text ? text : "");
    // CFUserNotification: a modal alert from any thread, without the AppKit loop
    CFStringRef title = CFStringCreateWithCString(nullptr, caption ? caption : "", kCFStringEncodingUTF8);
    CFStringRef msg = CFStringCreateWithCString(nullptr, text ? text : "", kCFStringEncodingUTF8);
    CFOptionFlags level = (type & 0xF0) == MB_ICONERROR ? kCFUserNotificationStopAlertLevel
                          : (type & 0xF0) == MB_ICONWARNING ? kCFUserNotificationCautionAlertLevel
                                                            : kCFUserNotificationNoteAlertLevel;
    CFOptionFlags answer = 0;
    CFUserNotificationDisplayAlert(0, level, nullptr, nullptr, nullptr, title, msg, nullptr, nullptr, nullptr, &answer);
    if (title) CFRelease(title);
    if (msg) CFRelease(msg);
    return 1;   // IDOK
}

// ---- Text ----

int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* out, int out_n) {
    size_t len = n < 0 ? strlen(s) + 1 : size_t(n);   // -1: with the terminator, as Windows counts it
    int count = 0;
    for (size_t i = 0; i < len;) {
        uint8_t b = uint8_t(s[i++]);
        uint32_t c;
        int extra;
        if (b < 0x80) c = b, extra = 0;
        else if (b < 0xE0) c = b & 0x1F, extra = 1;
        else if (b < 0xF0) c = b & 0x0F, extra = 2;
        else c = b & 0x07, extra = 3;
        for (int k = 0; k < extra && i < len; k++) c = (c << 6) | (uint8_t(s[i++]) & 0x3F);
        if (out) {
            if (count >= out_n) return 0;
            out[count] = wchar_t(c);
        }
        count++;
    }
    return count;
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* s, int n, char* out, int out_n, const char*, WINBOOL*) {
    std::wstring w = n < 0 ? std::wstring(s) + L'\0' : std::wstring(s, size_t(n));
    std::string u;
    for (wchar_t c : w) {
        if (c == 0) u += '\0';
        else {
            wchar_t one[2] = {c, 0};
            u += narrow(one);
        }
    }
    if (!out) return int(u.size());
    if (int(u.size()) > out_n) return 0;
    memcpy(out, u.data(), u.size());
    return int(u.size());
}

// ---- Links ----

HINSTANCE ShellExecuteW(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR, int) {
    std::string target = narrow(file);
    if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0) {
        @autoreleasepool {
            NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:target.c_str()]];
            if (url && [[NSWorkspace sharedWorkspace] openURL:url]) return reinterpret_cast<HINSTANCE>(intptr_t(42));
        }
    }
    fprintf(stderr, "[mac] not opened on macOS: %s\n", target.c_str());
    return reinterpret_cast<HINSTANCE>(intptr_t(2));   // SE_ERR_FNF: "not started"
}

// ---- The achievement chime ----

WINBOOL PlaySoundW(LPCWSTR sound, HINSTANCE, DWORD flags) {
    static NSSound* playing = nil;
    @autoreleasepool {
        if (playing) {
            [playing stop];
            playing = nil;
        }
        if (!sound || !(flags & SND_MEMORY)) return sound ? FALSE : TRUE;
        // A RIFF WAV in memory: its size is in the header
        const uint8_t* p = reinterpret_cast<const uint8_t*>(sound);
        uint32_t riff = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
        NSData* data = [NSData dataWithBytes:p length:NSUInteger(riff) + 8];
        playing = [[NSSound alloc] initWithData:data];
        return playing && [playing play] ? TRUE : FALSE;
    }
}
