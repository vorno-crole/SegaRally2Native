// macOS: the GDI text drawing of the menus (ui/frontend.cpp) and the
// achievement notice (ui/achievement_toast.cpp) on Core Text. Both add their
// fonts from memory, pick them by family name, measure a string and draw it
// in white into a 32-bit top-down DIB, then read the coverage back. To lay
// out as on Windows:
//   - ascent and descent are the font's OS/2 win metrics, as GDI takes them;
//   - no kerning and no ligatures (TextOutW applies neither);
//   - grey antialiasing, no font smoothing (ANTIALIASED_QUALITY).
#define MAC_WIN32_NO_BOOL
#include "mac_win32.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum class Kind { Font, Bitmap };

struct Face {
    CGFontRef cg = nullptr;
    std::vector<std::string> names;   // family, full and PostScript names, simplified
    int units_per_em = 1000;
    int win_ascent = 0, win_descent = 0;   // OS/2, in font units
};

struct Font {
    Kind kind = Kind::Font;
    CTFontRef ct = nullptr;
    LONG ascent = 0, descent = 0, em = 0;
};

struct Bitmap {
    Kind kind = Kind::Bitmap;
    int w = 0, h = 0;
    std::vector<uint8_t> bits;   // BGRA, top row first
};

struct DC {
    Font* font = nullptr;
    Bitmap* bitmap = nullptr;
    COLORREF color = 0;
};

std::vector<Face*>& faces() {
    static std::vector<Face*> list;
    return list;
}

// "Saira ExtraCondensed ExCond" -> "sairaextracondensedexcond"
std::string simple(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum(uint8_t(c))) out += char(std::tolower(uint8_t(c)));
    return out;
}

std::string utf8(CFStringRef s) {
    if (!s) return std::string();
    char buf[512];
    return CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';   // face names are ASCII
    return s;
}

CFStringRef cfstring(const wchar_t* text, int n) {
    if (n < 0) n = int(wcslen(text));
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(text), CFIndex(n) * CFIndex(sizeof(wchar_t)),
                                   kCFStringEncodingUTF32LE, false);
}

uint16_t be16(CFDataRef table, size_t at) {
    if (!table || CFDataGetLength(table) < CFIndex(at + 2)) return 0;
    const uint8_t* p = CFDataGetBytePtr(table) + at;
    return uint16_t(p[0] << 8 | p[1]);
}

// A line of text in white with GDI's spacing (no kerning, no ligatures)
CTLineRef make_line(Font* f, const wchar_t* text, int n) {
    CFStringRef str = cfstring(text, n);
    if (!str) return nullptr;
    int zero = 0;
    CFNumberRef no = CFNumberCreate(nullptr, kCFNumberIntType, &zero);
    CGFloat white[4] = {1, 1, 1, 1};
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGColorRef color = CGColorCreate(rgb, white);
    const void* keys[] = {kCTFontAttributeName, kCTKernAttributeName, kCTLigatureAttributeName,
                          kCTForegroundColorAttributeName};
    const void* values[] = {f->ct, no, no, color};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(color);
    CGColorSpaceRelease(rgb);
    CFRelease(no);
    CFRelease(str);
    return line;
}

Font* fallback_font(int px) {
    auto* f = new Font;
    f->ct = CTFontCreateWithName(CFSTR("Helvetica-Bold"), CGFloat(px), nullptr);
    f->em = px;
    f->ascent = LONG(std::lround(CTFontGetAscent(f->ct)));
    f->descent = LONG(std::lround(CTFontGetDescent(f->ct)));
    return f;
}

}  // namespace

HANDLE AddFontMemResourceEx(void* data, DWORD size, void*, DWORD* count) {
    if (count) *count = 0;
    CFDataRef bytes = CFDataCreate(nullptr, static_cast<const UInt8*>(data), CFIndex(size));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
    CGFontRef cg = provider ? CGFontCreateWithDataProvider(provider) : nullptr;
    if (provider) CGDataProviderRelease(provider);
    CFRelease(bytes);
    if (!cg) return nullptr;
    auto* face = new Face;
    face->cg = cg;
    face->units_per_em = std::max(1, int(CGFontGetUnitsPerEm(cg)));
    CFDataRef os2 = CGFontCopyTableForTag(cg, 'OS/2');
    face->win_ascent = be16(os2, 74);
    face->win_descent = be16(os2, 76);
    if (os2) CFRelease(os2);
    if (!face->win_ascent) {   // no OS/2 table: the font's own ascent and descent
        face->win_ascent = std::abs(CGFontGetAscent(cg));
        face->win_descent = std::abs(CGFontGetDescent(cg));
    }
    CTFontRef ct = CTFontCreateWithGraphicsFont(cg, 12, nullptr, nullptr);
    for (CFStringRef key : {kCTFontFamilyNameKey, kCTFontFullNameKey, kCTFontPostScriptNameKey}) {
        CFStringRef name = CTFontCopyName(ct, key);
        if (name) {
            face->names.push_back(simple(utf8(name)));
            CFRelease(name);
        }
    }
    CFRelease(ct);
    faces().push_back(face);
    if (count) *count = 1;
    return face;
}

WINBOOL RemoveFontMemResourceEx(HANDLE h) {
    auto& list = faces();
    auto it = std::find(list.begin(), list.end(), static_cast<Face*>(h));
    if (it == list.end()) return FALSE;
    CGFontRelease((*it)->cg);
    delete *it;
    list.erase(it);
    return TRUE;
}

// GDI's face names are the fonts' legacy family names ("Saira ExtraCondensed
// ExCond"), Core Text's the typographic ones ("Saira ExtraCondensed"): the
// longest name of a font that is the face name or starts it wins
HFONT CreateFontW(int height, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR face) {
    std::string want = simple(narrow(face));
    Face* best = nullptr;
    size_t best_len = 0;
    for (Face* f : faces())
        for (const std::string& n : f->names)
            if (!n.empty() && want.compare(0, n.size(), n) == 0 && n.size() > best_len) best = f, best_len = n.size();
    int px = std::abs(height);
    if (!best) {
        static std::vector<std::string> warned;
        if (std::find(warned.begin(), warned.end(), want) == warned.end()) {
            warned.push_back(want);
            fprintf(stderr, "[mac] font \"%s\" not loaded: Helvetica Bold instead\n", narrow(face).c_str());
        }
        return reinterpret_cast<HFONT>(fallback_font(px));
    }
    // GDI: a negative height is the em size; a positive one the cell (ascent + descent)
    double scale = height < 0 ? double(px) / best->units_per_em
                              : double(px) / std::max(1, best->win_ascent + best->win_descent);
    auto* f = new Font;
    f->em = LONG(std::lround(best->units_per_em * scale));
    f->ct = CTFontCreateWithGraphicsFont(best->cg, CGFloat(f->em), nullptr, nullptr);
    f->ascent = LONG(std::lround(best->win_ascent * scale));
    f->descent = LONG(std::lround(best->win_descent * scale));
    return reinterpret_cast<HFONT>(f);
}

HDC CreateCompatibleDC(HDC) { return reinterpret_cast<HDC>(new DC); }

WINBOOL DeleteDC(HDC dc) {
    delete reinterpret_cast<DC*>(dc);
    return TRUE;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, HANDLE, DWORD) {
    const BITMAPINFOHEADER& h = bi->bmiHeader;
    if (h.biBitCount != 32 || h.biWidth <= 0 || h.biHeight == 0) {
        fprintf(stderr, "[mac] CreateDIBSection: only 32-bit DIBs are supported\n");
        return nullptr;
    }
    if (h.biHeight > 0) fprintf(stderr, "[mac] CreateDIBSection: bottom-up DIB drawn top-down\n");
    auto* b = new Bitmap;
    b->w = h.biWidth;
    b->h = std::abs(h.biHeight);
    b->bits.assign(size_t(b->w) * b->h * 4, 0);
    if (bits) *bits = b->bits.data();
    return reinterpret_cast<HBITMAP>(b);
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj) {
    auto* dc = reinterpret_cast<DC*>(hdc);
    if (!dc || !obj) return nullptr;
    if (*static_cast<Kind*>(obj) == Kind::Font) {
        Font* old = dc->font;
        dc->font = static_cast<Font*>(obj);
        return old;
    }
    Bitmap* old = dc->bitmap;
    dc->bitmap = static_cast<Bitmap*>(obj);
    return old;
}

WINBOOL DeleteObject(HGDIOBJ obj) {
    if (!obj) return FALSE;
    if (*static_cast<Kind*>(obj) == Kind::Font) {
        auto* f = static_cast<Font*>(obj);
        if (f->ct) CFRelease(f->ct);
        delete f;
    } else {
        delete static_cast<Bitmap*>(obj);
    }
    return TRUE;
}

int SetBkMode(HDC, int) { return TRANSPARENT; }   // drawing is always transparent here

COLORREF SetTextColor(HDC hdc, COLORREF c) {
    auto* dc = reinterpret_cast<DC*>(hdc);
    COLORREF old = dc->color;
    dc->color = c;
    if (c != RGB(255, 255, 255)) fprintf(stderr, "[mac] SetTextColor: text is drawn in white only\n");
    return old;
}

WINBOOL GetTextMetricsW(HDC hdc, TEXTMETRICW* tm) {
    auto* dc = reinterpret_cast<DC*>(hdc);
    if (!dc || !dc->font) return FALSE;
    Font* f = dc->font;
    *tm = TEXTMETRICW{};
    tm->tmAscent = f->ascent;
    tm->tmDescent = f->descent;
    tm->tmHeight = f->ascent + f->descent;
    tm->tmInternalLeading = std::max<LONG>(0, tm->tmHeight - f->em);
    tm->tmAveCharWidth = LONG(std::lround(f->em * 0.5));
    tm->tmMaxCharWidth = f->em;
    tm->tmWeight = FW_BOLD;
    return TRUE;
}

WINBOOL GetTextExtentPoint32W(HDC hdc, LPCWSTR text, int n, SIZE* out) {
    auto* dc = reinterpret_cast<DC*>(hdc);
    if (!dc || !dc->font) return FALSE;
    CTLineRef line = make_line(dc->font, text, n);
    double w = line ? CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) : 0;
    if (line) CFRelease(line);
    out->cx = LONG(std::lround(w));
    out->cy = dc->font->ascent + dc->font->descent;
    return TRUE;
}

// (x, y): the top left of the text cell, as GDI's default TA_TOP | TA_LEFT
WINBOOL TextOutW(HDC hdc, int x, int y, LPCWSTR text, int n) {
    auto* dc = reinterpret_cast<DC*>(hdc);
    if (!dc || !dc->font || !dc->bitmap) return FALSE;
    Bitmap* b = dc->bitmap;
    CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(b->bits.data(), size_t(b->w), size_t(b->h), 8, size_t(b->w) * 4, rgb,
                                             CGBitmapInfo(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(rgb);
    if (!ctx) return FALSE;
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, false);
    CGContextSetAllowsFontSubpixelPositioning(ctx, true);
    CTLineRef line = make_line(dc->font, text, n);
    if (line) {
        // Core Graphics counts from the bottom row up
        CGContextSetTextPosition(ctx, CGFloat(x), CGFloat(b->h - (y + dc->font->ascent)));
        CTLineDraw(line, ctx);
        CFRelease(line);
    }
    CGContextFlush(ctx);
    CGContextRelease(ctx);
    return TRUE;
}
