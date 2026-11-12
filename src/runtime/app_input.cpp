// Keyboard, gamepads and steering wheels: the car's controls (moved from app.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app_internal.h"
#include "inputs.h"
#include "runtime.h"
#include "wheel.h"

bool g_menu_open = false;   // a menu has the controls
Gamepads g_pads;
int g_pad_rumble = 2;

static int adc(float v) { return std::clamp(int(std::lround(v)), 0, 255); }

// The car on the keyboard: arrows steer (the wheel turns over ~0.3 s and
// comes back by itself), Up / Down are the accelerator and the brake, A / Z
// shift up / down, V is VIEW CHANGE and Space the hand brake. The pads and
// wheels add theirs after this (poll_gamepads)
template <class Down>
static void poll_car_keys(Inputs& in, Down down) {
    static double last = qpc_seconds();
    static float steer = 0;
    static bool up_was = false, down_was = false;
    double now = qpc_seconds();
    float dt = std::clamp(float(now - last), 0.0f, 0.05f);
    last = now;
    int dir = down(VK_RIGHT) - down(VK_LEFT);
    float target = float(dir);
    float rate = dir == 0 || (target > 0) != (steer > 0) ? 6.0f : 3.5f;   // back to the centre faster
    steer += std::clamp(target - steer, -rate * dt, rate * dt);
    in.steer = adc(128.0f + 127.0f * steer);
    in.accel = down(VK_UP) ? 0xFF : 0;
    in.brake = down(VK_DOWN) ? 0xFF : 0;
    bool up = down('A'), dn = down('Z');
    if (up && !up_was) in.gear = std::min(int(in.gear) + 1, 4);
    if (dn && !down_was) in.gear = std::max(int(in.gear) - 1, 1);
    up_was = up, down_was = dn;
    in.view = down('V');
    in.handbrake = down(VK_SPACE);
}

// Keyboard (only while the window is active): 5 = coin, 1 = start (6 / 2 the
// second pair), F2 = test, F3 = service, and the car's keys
void poll_keyboard(HWND hwnd) {
    Inputs& in = g_inputs;
    // Scripted input (--auto) and the unseen run to SELECT GAME take over the controls
    bool scripted = !g_auto.events.empty() || g_arcade_entry;
    if (GetForegroundWindow() != hwnd || scripted) {
        if (!scripted) in.release_car();   // in the background: nothing stays held
        return;
    }
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (g_menu_open) {
        in.test = in.service = false;
        in.coin[0] = in.coin[1] = in.start[0] = in.start[1] = false;
        in.release_car();
        return;
    }
    in.coin[0] = down('5');
    in.start[0] = down('1');
    in.coin[1] = down('6');
    in.start[1] = down('2');
    in.test = down(VK_F2);
    in.service = down(VK_F3);
    poll_car_keys(in, down);
}

// Gamepads (gamepad.h) and steering wheels (wheel.h), after the keyboard:
// what they press is added to it, the stronger steering of the two. The first
// pad connected drives; on any pad BACK is the coin, and START starts the
// game after a coin (within 30 s, once per coin) and otherwise asks for the
// pause menu. Returns true when a pad asks for it
bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    g_pads.poll(g_pad_rumble, focused, g_menu_open);
    if (!focused || g_menu_open || !g_auto.events.empty()) return false;
    Inputs& in = g_inputs;
    auto keyboard_steer = [&] { return (float(in.steer) - 128.0f) / 127.0f; };
    if (const WheelState& w = wheel_state(); w.connected) {
        if (std::fabs(w.steer) >= std::fabs(keyboard_steer())) in.steer = adc(128.0f + 127.0f * w.steer);
        in.accel = std::max(int(in.accel), adc(255.0f * w.gas));
        in.brake = std::max(int(in.brake), adc(255.0f * w.brake));
        if (w.gear >= 0) in.gear = w.gear;   // H shifter: where the stick is (0 = neutral)
        if (w.shift_up) in.gear = std::min(std::max(int(in.gear), 1) + 1, 4);
        if (w.shift_down) in.gear = std::max(int(in.gear) - 1, 1);
        if (w.view) in.view = true;
        if (w.handbrake) in.handbrake = true;
        if (w.coin) press_coin();
        if (w.start) press_start();
    }
    bool pause = false, driver = true;   // the first connected pad is the driver's
    for (int i = 0; i < Gamepads::MAX; i++) {
        const PadState& pad = g_pads.pad(i);
        if (!pad.connected) continue;
        if (driver) {
            if (std::fabs(pad.steer) > std::fabs(keyboard_steer())) in.steer = adc(128.0f + 127.0f * pad.steer);
            in.accel = std::max(int(in.accel), adc(255.0f * pad.gas));
            in.brake = std::max(int(in.brake), adc(255.0f * pad.brake));
            if (pad.shift_up) in.gear = std::min(int(in.gear) + 1, 4);
            if (pad.shift_down) in.gear = std::max(int(in.gear) - 1, 1);
            if (pad.view) in.view = true;
            if (pad.handbrake) in.handbrake = true;
            static const bool no_ffb = getenv("SR2_NO_FFB_RUMBLE") != nullptr;
            if (!no_ffb) {   // the wheel's force feedback (runtime.cpp ffb_frame) as rumble
                float sh = std::min(1.0f, g_ffb_shake.exchange(0.0f) * 2.5f);
                if (sh > 0.08f) g_pads.rumble(i, sh, 0.6f * sh, 60);
            }
            driver = false;
        }
        static double coin_at = -1;
        if (pad.back) {
            press_coin();
            coin_at = qpc_seconds();
        }
        if (pad.start) {
            if (coin_at >= 0 && qpc_seconds() - coin_at < 30) {
                press_start();
                coin_at = -1;
            } else {
                pause = true;
            }
        }
    }
    return pause;
}
