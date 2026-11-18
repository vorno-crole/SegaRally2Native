#include "panel.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t BOX_COLOR = 0x15181D;
constexpr uint8_t BOX_ALPHA = 232;
}  // namespace

Panel::~Panel() { release(); }

void Panel::release() {
    if (dc_) {
        if (old_font_) SelectObject(dc_, old_font_);
        if (old_bitmap_) SelectObject(dc_, old_bitmap_);
        DeleteDC(dc_);
    }
    if (font_) DeleteObject(font_);
    if (bitmap_) DeleteObject(bitmap_);
    dc_ = nullptr, bitmap_ = old_bitmap_ = nullptr, font_ = old_font_ = nullptr, bits_ = nullptr;
    w_ = h_ = font_px_ = 0;
}

bool Panel::begin(int w, int h, int font_px) {
    if (w <= 0 || h <= 0) return false;
    if (w != w_ || h != h_ || font_px != font_px_) {
        release();
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap_ || !bits) {
            release();
            return false;
        }
        bits_ = static_cast<uint32_t*>(bits);
        old_bitmap_ = static_cast<HBITMAP>(SelectObject(dc_, bitmap_));
        font_ = CreateFontA(-font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        if (font_) old_font_ = static_cast<HFONT>(SelectObject(dc_, font_));
        SetBkMode(dc_, TRANSPARENT);
        TEXTMETRICA tm{};
        line_height_ = GetTextMetricsA(dc_, &tm) ? int(tm.tmHeight + tm.tmExternalLeading) + 2 : font_px + 4;
        w_ = w, h_ = h, font_px_ = font_px;
        alpha_.assign(size_t(w) * h, 0);
        out_.assign(size_t(w) * h, 0);
        used_w_ = w, used_h_ = h;   // (a new bitmap is cleared whole below)
    }
    GdiFlush();
    // Only the corner the last picture used needs clearing
    for (int y = 0; y < used_h_; y++) {
        memset(bits_ + size_t(y) * w_, 0, size_t(used_w_) * 4);
        memset(alpha_.data() + size_t(y) * w_, 0, size_t(used_w_));
        memset(out_.data() + size_t(y) * w_, 0, size_t(used_w_) * 4);
    }
    used_w_ = used_h_ = 0;
    return true;
}

void Panel::box(int x, int y, int w, int h) {
    if (!bits_) return;
    GdiFlush();
    int x0 = std::clamp(x, 0, w_), x1 = std::clamp(x + w, 0, w_), y0 = std::clamp(y, 0, h_), y1 = std::clamp(y + h, 0, h_);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            bits_[size_t(yy) * w_ + xx] = BOX_COLOR;
            alpha_[size_t(yy) * w_ + xx] = BOX_ALPHA;
        }
    used_w_ = std::max(used_w_, x1), used_h_ = std::max(used_h_, y1);
}

void Panel::text(int x, int y, const std::string& s, uint32_t rgb) {
    if (!dc_ || s.empty()) return;
    SetTextColor(dc_, RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
    TextOutA(dc_, x, y, s.c_str(), int(s.size()));
}

int Panel::text_width(const std::string& s) {
    SIZE size{};
    if (!dc_ || s.empty() || !GetTextExtentPoint32A(dc_, s.c_str(), int(s.size()), &size)) return 0;
    return int(size.cx);
}

void Panel::image(int x, int y, int size, const uint8_t* rgba, int w, int h) {
    if (!bits_ || !rgba || w <= 0 || h <= 0 || size <= 0) return;
    GdiFlush();
    // The whole picture inside the square, its shape kept
    int dw = w >= h ? size : std::max(1, size * w / h), dh = h >= w ? size : std::max(1, size * h / w);
    int left = x + (size - dw) / 2, top = y + (size - dh) / 2;
    for (int yy = 0; yy < dh; yy++) {
        int py = top + yy;
        if (py < 0 || py >= h_) continue;
        const uint8_t* row = rgba + size_t(yy * h / dh) * w * 4;
        for (int xx = 0; xx < dw; xx++) {
            int px = left + xx;
            if (px < 0 || px >= w_) continue;
            const uint8_t* t = row + size_t(xx * w / dw) * 4;
            uint32_t check = ((xx / 6 + yy / 6) & 1) ? 0x60 : 0x90, a = t[3];
            uint32_t r = (t[0] * a + check * (255 - a)) / 255, g = (t[1] * a + check * (255 - a)) / 255,
                     b = (t[2] * a + check * (255 - a)) / 255;
            bits_[size_t(py) * w_ + px] = r << 16 | g << 8 | b;
            alpha_[size_t(py) * w_ + px] = 255;
        }
    }
    used_w_ = std::max(used_w_, std::clamp(left + dw, 0, w_)), used_h_ = std::max(used_h_, std::clamp(top + dh, 0, h_));
}

const uint32_t* Panel::end() {
    if (!bits_) return nullptr;
    GdiFlush();
    for (int y = 0; y < used_h_; y++)
        for (int x = 0; x < used_w_; x++) {
            size_t i = size_t(y) * w_ + x;
            uint32_t a = alpha_[i], c = bits_[i];
            if (!a) continue;
            uint32_t r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
            out_[i] = a << 24 | r << 16 | g << 8 | b;
        }
    return out_.data();
}
