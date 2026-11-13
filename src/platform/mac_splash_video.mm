// macOS: the splash video (ui/splash_video.h) through AVFoundation, in place
// of ui/splash_video.cpp (Media Foundation). As there: the sound goes to the
// default output by itself, the pictures come back as BGRA at the size the
// menu asks for, the whole picture fitted in with black around it. Any format
// macOS plays (the MP4 with H.264 and AAC). The video output is made at that
// size, so the decoder scales the pictures: scaling the 2560x1440 video on the
// CPU took 43 ms a picture, too long for the frame that draws the menu.
#include "splash_video.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

struct SplashVideo::Impl {
    AVPlayer* player = nil;
    AVPlayerItem* item = nil;
    AVPlayerItemVideoOutput* output = nil;
    id ended_observer = nil;
    std::atomic<bool> ended{false};
    CVPixelBufferRef picture = nullptr;   // the latest picture (retained)
    int out_w = 0, out_h = 0;             // the size the output delivers
};

namespace {
std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? char(c) : '?';   // the project's paths are ASCII
    return s;
}
}  // namespace

bool SplashVideo::open(const std::wstring& path) {
    close();
    std::string p = narrow(path);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    @autoreleasepool {
        impl_ = new Impl;
        Impl& m = *impl_;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
        m.item = [AVPlayerItem playerItemWithURL:url];
        m.player = [AVPlayer playerWithPlayerItem:m.item];   // the output comes with the first frame(): its size
        m.player.actionAtItemEnd = AVPlayerActionAtItemEndPause;   // the last picture stays
        Impl* self = impl_;
        m.ended_observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                        object:m.item
                         queue:nil
                    usingBlock:^(NSNotification*) { self->ended = true; }];
        if (!m.player || !m.item) {
            fprintf(stderr, "[menu] splash video: AVFoundation could not open it\n");
            close();
            return false;
        }
        [m.player pause];   // loaded paused, as on Windows
    }
    return true;
}

void SplashVideo::close() {
    if (!impl_) return;
    @autoreleasepool {
        Impl& m = *impl_;
        if (m.ended_observer) [[NSNotificationCenter defaultCenter] removeObserver:m.ended_observer];
        [m.player pause];
        [m.player replaceCurrentItemWithPlayerItem:nil];
        if (m.picture) CVPixelBufferRelease(m.picture);
        m.ended_observer = nil;
        m.output = nil;
        m.item = nil;
        m.player = nil;
    }
    delete impl_;
    impl_ = nullptr;
}

void SplashVideo::play() {
    if (impl_) [impl_->player play];
}

void SplashVideo::set_volume(float v) {
    if (impl_) impl_->player.volume = std::clamp(v, 0.0f, 1.0f);
}

bool SplashVideo::failed() const {
    if (!impl_) return false;
    if (impl_->item.status != AVPlayerItemStatusFailed) return false;
    static bool said = false;
    if (!said) {
        said = true;
        fprintf(stderr, "[menu] splash video: macOS could not play it (%s)\n",
                impl_->item.error.localizedDescription.UTF8String ?: "no reason given");
    }
    return true;
}

bool SplashVideo::ended() const {
    if (!impl_) return false;
    if (impl_->ended) return true;
    CMTime d = impl_->item.duration;
    return CMTIME_IS_NUMERIC(d) && CMTimeCompare(impl_->player.currentTime, d) >= 0;
}

double SplashVideo::position() const {
    if (!impl_) return 0;
    double t = CMTimeGetSeconds(impl_->player.currentTime);
    return std::isfinite(t) ? t : 0;
}

bool SplashVideo::size(int& w, int& h) const {
    if (!impl_ || impl_->item.status != AVPlayerItemStatusReadyToPlay) return false;
    CGSize s = impl_->item.presentationSize;
    if (s.width <= 0 || s.height <= 0) return false;
    w = int(s.width + 0.5), h = int(s.height + 0.5);
    return true;
}

bool SplashVideo::frame(int w, int h, std::vector<uint32_t>& px, bool always) {
    if (!impl_ || w <= 0 || h <= 0) return false;
    Impl& m = *impl_;
    bool fresh = false;
    @autoreleasepool {
        if (w != m.out_w || h != m.out_h) {   // pictures at this size from now on, scaled by the decoder
            if (m.output) [m.item removeOutput:m.output];
            NSDictionary* attrs = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                                    (id)kCVPixelBufferWidthKey : @(w), (id)kCVPixelBufferHeightKey : @(h)};
            m.output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
            [m.item addOutput:m.output];
            m.out_w = w, m.out_h = h;
        }
        CMTime t = [m.output itemTimeForHostTime:CACurrentMediaTime()];
        if ([m.output hasNewPixelBufferForItemTime:t]) {
            if (CVPixelBufferRef b = [m.output copyPixelBufferForItemTime:t itemTimeForDisplay:nullptr]) {
                if (m.picture) CVPixelBufferRelease(m.picture);
                m.picture = b;   // copy... returns it retained
                fresh = true;
            }
        }
    }
    if (!m.picture || (!fresh && !always)) return false;

    CVPixelBufferLockBaseAddress(m.picture, kCVPixelBufferLock_ReadOnly);
    size_t sw = CVPixelBufferGetWidth(m.picture), sh = CVPixelBufferGetHeight(m.picture);
    if (sw == size_t(w) && sh == size_t(h)) {   // already the size asked: the rows, made opaque
        px.resize(size_t(w) * h);
        const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(m.picture));
        size_t stride = CVPixelBufferGetBytesPerRow(m.picture);
        for (int y = 0; y < h; y++) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(base + size_t(y) * stride);
            for (int x = 0; x < w; x++) px[size_t(y) * w + x] = row[x] | 0xFF000000u;
        }
        CVPixelBufferUnlockBaseAddress(m.picture, kCVPixelBufferLock_ReadOnly);
        return true;
    }
    // A picture of another size (just after the size changed): fitted into
    // w x h on the CPU, black around it, opaque BGRA
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    const CGBitmapInfo bgra = CGBitmapInfo(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little;
    CGContextRef src = CGBitmapContextCreate(CVPixelBufferGetBaseAddress(m.picture), sw, sh, 8,
                                             CVPixelBufferGetBytesPerRow(m.picture), cs, bgra);
    CGImageRef img = src ? CGBitmapContextCreateImage(src) : nullptr;
    CVPixelBufferUnlockBaseAddress(m.picture, kCVPixelBufferLock_ReadOnly);
    px.assign(size_t(w) * h, 0xFF000000u);
    CGContextRef dst = CGBitmapContextCreate(px.data(), size_t(w), size_t(h), 8, size_t(w) * 4, cs, bgra);
    if (img && dst) {
        double k = std::min(double(w) / double(sw), double(h) / double(sh));
        double dw = sw * k, dh = sh * k;
        CGContextSetInterpolationQuality(dst, kCGInterpolationHigh);
        CGContextDrawImage(dst, CGRectMake((w - dw) / 2, (h - dh) / 2, dw, dh), img);
    }
    if (dst) CGContextRelease(dst);
    if (img) CGImageRelease(img);
    if (src) CGContextRelease(src);
    CGColorSpaceRelease(cs);
    for (uint32_t& p : px) p |= 0xFF000000u;   // opaque
    return true;
}
