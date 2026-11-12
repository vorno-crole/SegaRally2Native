// macOS: the splash video (ui/splash_video.h), in place of
// ui/splash_video.cpp (Media Foundation). Not done yet: open() says there is
// no video, so the menu shows the splash picture instead, as on a Windows
// without the media engine. AVFoundation is the plan (docs/MACOS.md).
#include "splash_video.h"

#include <cstdio>

struct SplashVideo::Impl {};

bool SplashVideo::open(const std::wstring&) {
    static bool said = false;
    if (!said) {
        said = true;
        printf("[menu] splash video: not on macOS yet, the picture instead\n");
    }
    return false;
}

void SplashVideo::close() {
    delete impl_;
    impl_ = nullptr;
}

void SplashVideo::play() {}
void SplashVideo::set_volume(float) {}
bool SplashVideo::failed() const { return true; }
bool SplashVideo::ended() const { return true; }
double SplashVideo::position() const { return 0; }
bool SplashVideo::size(int&, int&) const { return false; }
bool SplashVideo::frame(int, int, std::vector<uint32_t>&, bool) { return false; }
