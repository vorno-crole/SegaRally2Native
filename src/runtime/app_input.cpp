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
float g_trigger_share = 1.0f;

static int adc(float v) { return std::clamp(int(std::lround(v)), 0, 255); }

// SELECT CAR picks the car from where the wheel is (the game, 0x00003F30):
// six bands of the steering ADC (up to 0x44 the first car, 0x46-0x59,
// 0x5B-0x6F, 0x91-0xA4, 0xA6-0xB9, from 0xBB the sixth), the centre keeping
// the car there is; the hidden pair (0x00004028) up to 0x6F the Delta, past
// 0x90 the Celica. The arrows, turning the wheel over 0.3 s and back, went
// through the middle bands in a frame or two and only ever stopped on the
// outer cars (issue #105). There each press steps one car: the wheel is put in
// the next car's band for a moment, then straight back to the centre (no
// sweep back over the others); held, it repeats
static int car_select_steer(int car, int dir, double now) {
    static constexpr int BAND[6] = {0x20, 0x50, 0x65, 0x9A, 0xAF, 0xE0};
    static int last_dir = 0, held = 0x80, held_car = 0;
    static double held_until = 0, repeat_at = 0;
    if (dir != 0 && (dir != last_dir || now >= repeat_at)) {
        if (now < held_until) car = held_car;   // pressed again before the game took the last one
        held_car = car >= 6 ? car : std::clamp(car + dir, 0, 5);
        held = car >= 6 ? (dir < 0 ? 0x40 : 0xC0) : BAND[held_car];
        held_until = now + 0.1;   // the game reads the ADC at 57.5 Hz: a few frames
        repeat_at = now + (dir != last_dir ? 0.45 : 0.25);
    }
    last_dir = dir;
    return now < held_until ? held : 0x80;
}

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
    if (int car = g_car_select; car >= 0) {   // SELECT CAR: a car a press
        in.steer = car_select_steer(car, dir, now);
        steer = 0;
    } else {
        float target = float(dir);
        float rate = dir == 0 || (target > 0) != (steer > 0) ? 6.0f : 3.5f;   // back to the centre faster
        steer += std::clamp(target - steer, -rate * dt, rate * dt);
        in.steer = adc(128.0f + 127.0f * steer);
    }
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
// pause menu; the wheel's coin and start buttons do the same. Returns true
// when a pad or the wheel asks for it
bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    g_pads.poll(g_pad_rumble, focused, g_menu_open);
    if (!focused || g_menu_open || !g_auto.events.empty()) return false;
    Inputs& in = g_inputs;
    auto keyboard_steer = [&] { return (float(in.steer) - 128.0f) / 127.0f; };
    bool pause = false;
    static double coin_at = -1;   // START after a coin (any pad or the wheel) starts the game, else pauses
    auto coin = [&] {
        press_coin();
        coin_at = qpc_seconds();
    };
    // ARCADE ORIGINAL: START is the cabinet's while the game waits for it (the
    // attract, the credit screen), with a coin put in first when it takes coins
    // (START a moment after it, as a player would); in a game it pauses
    static double start_due = -1;
    if (start_due >= 0 && qpc_seconds() >= start_due) {
        press_start();
        start_due = -1;
    }
    auto start = [&] {
        if (g_opt.arcade) {
            if (!g_wants_start) pause = true;
            else if (g_opt.free_play != 1 && g_in_attract) {
                press_coin();
                start_due = qpc_seconds() + 0.5;
            } else press_start();
            return;
        }
        if (coin_at >= 0 && qpc_seconds() - coin_at < 30) {
            press_start();
            coin_at = -1;
        } else {
            pause = true;
        }
    };
    if (const WheelState& w = wheel_state(); w.connected) {
        if (std::fabs(w.steer) >= std::fabs(keyboard_steer())) in.steer = adc(128.0f + 127.0f * w.steer);
        in.accel = std::max(int(in.accel), adc(255.0f * w.gas));
        in.brake = std::max(int(in.brake), adc(255.0f * w.brake));
        if (w.gear >= 0) in.gear = w.gear;   // H shifter: where the stick is (0 = neutral)
        if (w.shift_up) in.gear = std::min(std::max(int(in.gear), 1) + 1, 4);
        if (w.shift_down) in.gear = std::max(int(in.gear) - 1, 1);
        if (w.view) in.view = true;
        if (w.handbrake) in.handbrake = true;
        if (w.coin) coin();
        if (w.start) start();
    }
    bool driver = true;   // the first connected pad is the driver's
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
                // and the triggers' own (runtime.cpp, trigger_frame)
                float tl = g_trigger_rumble[0].exchange(0.0f), tr = g_trigger_rumble[1].exchange(0.0f);
                tl *= g_trigger_share, tr *= g_trigger_share;
                if (tl > 0.05f || tr > 0.05f) g_pads.rumble_triggers(i, tl, tr, 60);
            }
            driver = false;
        }
        if (pad.back) coin();
        if (pad.start) start();
    }
    return pause;
}
