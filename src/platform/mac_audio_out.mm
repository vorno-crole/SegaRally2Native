// macOS: the sound output (audio/audio_out.h) through Core Audio's default
// output unit, in place of audio/audio_out.cpp (WASAPI). As on Windows the
// emulation fills an AudioStream at 44.1 kHz and the output side pulls from
// it at the device's own rate, resampled by AudioStream, not by the system.
// The default output unit follows the system's choice of device by itself;
// this thread reopens it when the speaker layout or the device rate changes.
#include "audio_out.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

// The default output device's own rate (0 if there is none)
double device_rate() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &dev) != noErr ||
        dev == kAudioObjectUnknown)
        return 0;
    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    Float64 rate = 0;
    size = sizeof(rate);
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &rate) != noErr) return 0;
    return rate;
}

}  // namespace

bool AudioOut::start() {
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void AudioOut::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioOut::run() {
    bool warned = false;
    while (running_) {
        bool opened = play_default_device();
        if (!opened && !warned) {
            fprintf(stderr, "[audio] could not open the audio output; retrying in the background\n");
            warned = true;
        }
        for (int i = 0; i < (opened ? 1 : 10) && running_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// Plays until the output stops, the speaker layout changes or the device's
// rate does. Returns false if the output could not be opened at all.
bool AudioOut::play_default_device() {
    const double rate = device_rate();
    if (rate <= 0) return false;
    const auto mode = AudioStream::Speakers(speakers_.load());
    const int channels = AudioStream::channels(mode);

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    AudioUnit unit = nullptr;
    if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return false;

    // What we hand Core Audio: interleaved float at the device's rate
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = rate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = UInt32(channels);
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = UInt32(channels * 4);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;
    bool ok = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt,
                                   sizeof(fmt)) == noErr;
    if (ok && channels == 4) {   // front pair, then the rear pair on the device's surround speakers
        AudioChannelLayout layout{};
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_Quadraphonic;
        AudioUnitSetProperty(unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &layout,
                             sizeof(layout));
    }

    // The render callback runs on Core Audio's thread: the reading side of the stream
    struct Ctx {
        AudioOut* self;
        int channels, rate;
        std::atomic<bool> layout_changed{false};
    } ctx{this, channels, int(rate)};
    AURenderCallbackStruct cb{};
    cb.inputProcRefCon = &ctx;
    cb.inputProc = [](void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames,
                      AudioBufferList* io) -> OSStatus {
        auto* c = static_cast<Ctx*>(ref);
        auto* out = static_cast<float*>(io->mBuffers[0].mData);
        auto now_mode = AudioStream::Speakers(c->self->speakers_.load());
        if (AudioStream::channels(now_mode) != c->channels) {   // another layout: silence until reopened
            memset(out, 0, size_t(frames) * size_t(c->channels) * 4);
            c->layout_changed = true;
            return noErr;
        }
        c->self->stream_.pull(out, frames, now_mode);
        if (Extra extra = c->self->extra_.load()) extra(c->self->extra_user_.load(), out, frames, c->channels, c->rate);
        return noErr;
    };
    if (ok) ok = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                      sizeof(cb)) == noErr;
    if (ok) {
        stream_.set_output_rate(int(rate));   // before the first pull
        ok = AudioUnitInitialize(unit) == noErr && AudioOutputUnitStart(unit) == noErr;
    }
    const bool opened = ok;
    if (ok) printf("[audio] output at %d Hz, %d channels (Core Audio)\n", int(rate), channels);

    while (ok && running_ && !ctx.layout_changed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (device_rate() != rate) break;   // another device or rate: reopen at its rate
    }
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    return opened;
}
