// The viewer's side panel: text and texture previews drawn on the CPU with
// GDI into a picture of the window's size, which UiOverlay (the game's menu
// layer) blends over the frame. No new dependency and nothing new on the
// GPU: the editor's own interface comes with a later milestone.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class Panel {
public:
    ~Panel();
    // Starts an empty (transparent) picture of w x h pixels with text font_px tall
    bool begin(int w, int h, int font_px);
    // A dark, slightly see-through box: whatever is drawn afterwards goes over it
    void box(int x, int y, int w, int h);
    void text(int x, int y, const std::string& s, uint32_t rgb);   // 0xRRGGBB
    int text_width(const std::string& s);
    int line_height() const { return line_height_; }
    // An RGBA picture fitted into a square of `size` pixels (nearest texel),
    // over a checkerboard where it is see-through
    void image(int x, int y, int size, const uint8_t* rgba, int w, int h);
    // The finished picture: BGRA with premultiplied alpha (UiOverlay::set_image)
    const uint32_t* end();
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void release();
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr, old_bitmap_ = nullptr;
    HFONT font_ = nullptr, old_font_ = nullptr;
    uint32_t* bits_ = nullptr;     // the bitmap's pixels, 0x00RRGGBB, top row first
    std::vector<uint8_t> alpha_;   // how opaque each pixel of the panel is
    std::vector<uint32_t> out_;
    int w_ = 0, h_ = 0, font_px_ = 0, line_height_ = 16;
    int used_w_ = 0, used_h_ = 0;   // the corner of the picture that has anything in it
};
