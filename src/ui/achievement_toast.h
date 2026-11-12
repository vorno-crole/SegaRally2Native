// The notice of a new achievement: a small panel that slides in from the
// right edge to the bottom right corner with a short chime, stays a few
// seconds and slides out, over the game and the menus, one achievement after
// another. Drawn on the CPU like the menus (ui/frontend.cpp) and
// put on screen by video/ui_overlay.h.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

class AchievementToast {
public:
    bool init(const std::string& font_dir);
    void shutdown();
    void push(const std::wstring& name, const std::wstring& text);
    void set_volume(float v) { volume_ = v; }   // the game's gain, 0..10 (5 default); 0 = no chime
    // The author's credit, two small lines in the bottom right corner, faded
    // in while `shown` (the attract and its INSERT COIN, with the menu off)
    void set_credit(const std::wstring& line1, const std::wstring& line2, bool shown) {
        credit_[0] = line1, credit_[1] = line2, credit_on_ = shown;
    }
    // Redraws into `out` when the picture changes; true if it did. `now` in seconds
    bool draw(int w, int h, double now, std::vector<uint32_t>& out);

private:
    struct Mask {
        int w = 0, h = 0, pad = 0, text_w = 0, ascent = 0;
        std::vector<uint8_t> a;
    };
    enum Family { TITLE, TEXT };
    HFONT font(Family f, int px);
    const Mask& mask(Family f, int px, const std::wstring& s);
    int text(Family f, int px, const std::wstring& s, int x, int y, int align, uint32_t top, uint32_t bottom,
             float alpha);
    void fill(int x0, int y0, int x1, int y1, uint32_t rgb, float alpha);
    void shine(int x0, int y0, int x1, int y1, float pos);
    void chime();
    RECT draw_credit(float alpha);

    struct Notice { std::wstring name, text; };
    std::deque<Notice> queue_;
    double shown_at_ = -1;   // when the front notice appeared
    bool drawn_ = false;     // the last picture has a panel
    RECT dirty_{};           // where it was
    float volume_ = 5;
    std::vector<uint8_t> wav_;   // the chime, played from memory
    std::wstring credit_[2];
    bool credit_on_ = false;
    float credit_alpha_ = 0;     // fades towards credit_on_
    double credit_at_ = -1;      // the last draw's time, for the fade
    RECT credit_dirty_{};        // where the credit was drawn

    std::vector<HANDLE> font_handles_;
    std::vector<std::vector<char>> font_data_;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::wstring, Mask> masks_;
    HDC dc_ = nullptr;
    bool jurassic_ = false;
    int w_ = 0, h_ = 0;
    uint32_t* px_ = nullptr;
};
