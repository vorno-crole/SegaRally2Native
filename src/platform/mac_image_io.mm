// macOS: PNG files for the texture, 2D, backdrop and text packs
// (video/image_io.h) and the menus' pictures (mac_platform.h), through
// ImageIO, in place of video/image_io.cpp (Windows Imaging Component).
// As WIC gives them: straight (not premultiplied) alpha and the file's own
// values, with no colour management.
#define MAC_WIN32_NO_BOOL
#include "image_io.h"
#include "mac_platform.h"
#include "mac_win32.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstdio>
#include <utility>

namespace {

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? char(c) : '?';   // the project's paths are ASCII
    return s;
}

// RGBA, 8 bits, straight alpha, rows top to bottom
bool decode(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) {
    CFStringRef cfpath = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfpath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    w = int(CGImageGetWidth(img));
    h = int(CGImageGetHeight(img));
    rgba.assign(size_t(w) * h * 4, 0);
    bool ok = w > 0 && h > 0;
    CGImageAlphaInfo alpha = CGImageGetAlphaInfo(img);
    CGBitmapInfo order = CGImageGetBitmapInfo(img) & kCGBitmapByteOrderMask;
    CGColorSpaceRef cs = CGImageGetColorSpace(img);
    bool rgb = cs && CGColorSpaceGetModel(cs) == kCGColorSpaceModelRGB;
    if (ok && rgb && CGImageGetBitsPerComponent(img) == 8 && CGImageGetBitsPerPixel(img) == 32 &&
        (alpha == kCGImageAlphaLast || alpha == kCGImageAlphaNoneSkipLast) &&
        (order == kCGBitmapByteOrderDefault || order == kCGBitmapByteOrder32Big)) {
        // Already RGBA with straight alpha: the file's bytes as they are
        CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(img));
        const uint8_t* p = CFDataGetBytePtr(data);
        size_t stride = CGImageGetBytesPerRow(img);
        for (int y = 0; y < h; y++) memcpy(&rgba[size_t(y) * w * 4], p + size_t(y) * stride, size_t(w) * 4);
        if (alpha == kCGImageAlphaNoneSkipLast)
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        CFRelease(data);
    } else if (ok) {
        // Anything else (RGB, grey, palette, 16 bits): drawn in its own colour
        // space so the values are not converted, then the alpha divided out
        CGColorSpaceRef draw_cs = rgb ? CGColorSpaceRetain(cs) : CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(rgba.data(), size_t(w), size_t(h), 8, size_t(w) * 4, draw_cs,
                                                 CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(draw_cs);
        ok = ctx != nullptr;
        if (ok) {
            CGContextSetBlendMode(ctx, kCGBlendModeCopy);
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
            CGContextRelease(ctx);
            if (alpha != kCGImageAlphaNone && alpha != kCGImageAlphaNoneSkipLast && alpha != kCGImageAlphaNoneSkipFirst)
                for (size_t i = 0; i < rgba.size(); i += 4) {
                    uint8_t a = rgba[i + 3];
                    if (a && a < 255)
                        for (int c = 0; c < 3; c++) rgba[i + size_t(c)] = uint8_t(std::min(255, (rgba[i + size_t(c)] * 255 + a / 2) / a));
                }
        }
    }
    CGImageRelease(img);
    if (!ok) rgba.clear();
    return ok;
}

}  // namespace

bool png_load(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba) { return decode(path, w, h, rgba); }

bool png_save(const std::string& path, int w, int h, const uint8_t* rgba) {
    CFDataRef data = CFDataCreate(nullptr, rgba, CFIndex(size_t(w) * h * 4));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4, cs,
                                   CGBitmapInfo(kCGImageAlphaLast) | kCGBitmapByteOrder32Big, provider, nullptr, false,
                                   kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    CFRelease(data);
    if (!img) return false;
    CFStringRef cfpath = CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfpath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    bool ok = false;
    if (dst) {
        CGImageDestinationAddImage(dst, img, nullptr);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    CGImageRelease(img);
    return ok;
}

std::vector<std::string> list_files(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA((dir + "/" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        out.push_back(fd.cFileName);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    return out;
}

void list_hashes(const std::string& dir, std::unordered_set<uint64_t>& out) {
    for (const std::string& name : list_files(dir, "*.png")) {
        unsigned long long hash = 0;
        if (sscanf(name.c_str(), "%16llx", &hash) == 1) out.insert(hash);
    }
}

bool mac_load_image_bgra(const std::wstring& path, std::vector<uint32_t>& px, int& w, int& h) {
    std::vector<uint8_t> rgba;
    if (!decode(narrow(path), w, h, rgba)) {
        px.clear();
        w = h = 0;
        return false;
    }
    px.resize(size_t(w) * h);
    for (size_t i = 0; i < px.size(); i++) {
        const uint8_t* p = &rgba[i * 4];
        px[i] = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];   // BGRA in memory
    }
    return true;
}
