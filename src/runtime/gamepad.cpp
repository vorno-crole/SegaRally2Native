#include "gamepad.h"

#include "wheel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <SDL3/SDL.h>

namespace {

constexpr float STICK_DEAD = 0.15f;
constexpr float TRIGGER_DOWN = 0.35f;
constexpr float RUMBLE_STRENGTH[4] = {0.0f, 0.45f, 0.75f, 1.0f};

float axis(SDL_Gamepad* p, SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(p, a)) / 32767.0f; }

}  // namespace

double g_vibration_seconds = 0;

bool Gamepads::init() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // The focus is the window's business (poll's `focused`); SDL has no window
    // of its own to judge it by
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // XInput has no way to the motors in an Xbox One / Series pad's triggers;
    // SDL's raw input driver reaches them through Windows.Gaming.Input (its
    // default correlation). Bluetooth pads go through SDL's HIDAPI driver,
    // which drives them on its own when their firmware allows
    if (trigger_rumble) SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("[gamepad] SDL could not initialize gamepads: %s\n", SDL_GetError());
        return false;
    }
    ok_ = true;
    wheels_init();   // steering wheels and other joysticks that are not gamepads (wheel.cpp)
    return true;   // the pads already plugged in arrive as SDL_EVENT_GAMEPAD_ADDED
}

void Gamepads::shutdown() {
    if (!ok_) return;
    for (Slot& s : slot_)
        if (s.pad) SDL_CloseGamepad(s.pad);
    wheels_shutdown();
    SDL_Quit();
    ok_ = false;
}

int Gamepads::slot_of(uint32_t id) const {
    for (int i = 0; i < MAX; i++)
        if (slot_[i].pad && slot_[i].id == id) return i;
    return -1;
}

void Gamepads::open(uint32_t id) {
    if (slot_of(id) >= 0) return;
    // A wheel that SDL also maps as a gamepad is wheel.cpp's
    if (SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || wheel_is_ours(id)) return;
    int i = 0;
    while (i < MAX && slot_[i].pad) i++;
    if (i == MAX) return;
    SDL_Gamepad* p = SDL_OpenGamepad(id);
    if (!p) return;
    Slot& s = slot_[i];
    s = Slot{};
    s.pad = p;
    s.id = id;
    s.nintendo = SDL_GetGamepadButtonLabel(p, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
    PadState& st = state_[i];
    st = PadState{};
    st.connected = true;
    const char* name = SDL_GetGamepadName(p);
    st.name = name ? name : "Gamepad";
    s.trigger_motors = SDL_GetBooleanProperty(SDL_GetGamepadProperties(p), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    changed_ = true;
    printf("[gamepad] %d: %s%s\n", i + 1, st.name.c_str(), s.trigger_motors ? " (trigger rumble)" : "");
}

void Gamepads::close(uint32_t id) {
    int i = slot_of(id);
    if (i < 0) return;
    SDL_CloseGamepad(slot_[i].pad);
    slot_[i] = Slot{};
    state_[i].connected = false;
    changed_ = true;
    printf("[gamepad] %d disconnected\n", i + 1);
}

void Gamepads::poll(int rumble, bool focused, bool menu) {
    if (!ok_) return;
    rumble_ = rumble;
    double now = double(SDL_GetTicksNS()) * 1e-9;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: open(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close(e.gdevice.which); break;
        case SDL_EVENT_JOYSTICK_ADDED: wheels_added(e.jdevice.which); break;
        case SDL_EVENT_JOYSTICK_REMOVED: wheels_removed(e.jdevice.which); break;
        default: break;
        }
    }
    wheels_poll(focused, menu, keys_);
    for (int i = 0; i < MAX; i++) {
        Slot& s = slot_[i];
        PadState& st = state_[i];
        st.start = st.back = st.shift_up = st.shift_down = false;
        if (!s.pad) continue;
        SDL_Gamepad* p = s.pad;
        // Buttons and triggers, with what changed since the last poll
        bool now_down[32] = {};
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++) now_down[b] = SDL_GetGamepadButton(p, SDL_GamepadButton(b));
        // A button still held from the menu does nothing in the game until it
        // is let go: A, which chose PLAY, is also the accelerator, and the
        // pedal confirmed SELECT GAME's first choice (CHAMPIONSHIP) at once
        for (int b = 0; b < 32; b++) {
            if (menu) s.menu_held[b] = now_down[b];
            else if (!now_down[b]) s.menu_held[b] = false;
            else if (s.menu_held[b]) now_down[b] = false;
        }
        bool lt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > int(TRIGGER_DOWN * 32767);
        bool rt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > int(TRIGGER_DOWN * 32767);
        auto down = [&](SDL_GamepadButton b) { return now_down[b]; };
        auto pressed = [&](SDL_GamepadButton b) { return now_down[b] && !s.buttons[b]; };

        if (!focused || menu) {
            st.view = st.handbrake = false;
            st.steer = st.gas = st.brake = 0;
        }
        if (focused && menu) {
            SDL_GamepadButton accept = s.nintendo ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
            SDL_GamepadButton back = s.nintendo ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
            if (pressed(accept)) keys_.push_back(PadKey::Accept);
            if (pressed(back) || pressed(SDL_GAMEPAD_BUTTON_BACK)) keys_.push_back(PadKey::Back);
            if (pressed(SDL_GAMEPAD_BUTTON_START)) keys_.push_back(PadKey::Start);
            if (pressed(SDL_GAMEPAD_BUTTON_NORTH)) keys_.push_back(PadKey::Page);
            if (pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) || (lt && !s.left_trigger)) keys_.push_back(PadKey::TabPrev);
            if (pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) || (rt && !s.right_trigger)) keys_.push_back(PadKey::TabNext);
            // Direction: the d-pad or the left stick (past its round dead zone
            // and half way along its response curve), repeated while held
            int dir = down(SDL_GAMEPAD_BUTTON_DPAD_UP) ? 0 : down(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1
                    : down(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 2 : down(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 3 : -1;
            float sx = axis(p, SDL_GAMEPAD_AXIS_LEFTX), sy = axis(p, SDL_GAMEPAD_AXIS_LEFTY);
            float m = std::sqrt(sx * sx + sy * sy);
            if (dir < 0 && m >= STICK_DEAD) {
                float t = std::min((m - STICK_DEAD) / (0.92f - STICK_DEAD), 1.0f);
                float c = 0.2f * t + 0.8f * t * t * t;
                float lx = sx / m * c, ly = sy / m * c;
                if (std::max(std::fabs(lx), std::fabs(ly)) > 0.35f)
                    dir = std::fabs(ly) >= std::fabs(lx) ? (ly < 0 ? 0 : 1) : (lx < 0 ? 2 : 3);
            }
            if (dir != s.dir || (dir >= 0 && now >= s.next_repeat)) {
                if (dir >= 0) {
                    static const PadKey DIRS[4] = {PadKey::Up, PadKey::Down, PadKey::Left, PadKey::Right};
                    keys_.push_back(DIRS[dir]);
                    s.next_repeat = now + (dir != s.dir ? 0.4 : 0.11);
                }
                s.dir = dir;
            }
        } else if (focused) {
            s.dir = -1;
            st.start = pressed(SDL_GAMEPAD_BUTTON_START);
            st.back = pressed(SDL_GAMEPAD_BUTTON_BACK);
            // Steering: the left stick's x past a small dead zone, a
            // gentler response near the centre, the whole lock at the edge
            float sx = axis(p, SDL_GAMEPAD_AXIS_LEFTX);
            float m = std::fabs(sx);
            float t = m < STICK_DEAD ? 0.0f : std::min((m - STICK_DEAD) / (0.95f - STICK_DEAD), 1.0f);
            t *= 0.35f + 0.65f * t;
            st.steer = sx < 0 ? -t : t;
            auto pedal = [&](SDL_GamepadAxis a, SDL_GamepadButton b) {
                float v = std::clamp(axis(p, a), 0.0f, 1.0f);
                return down(b) ? 1.0f : v < 0.04f ? 0.0f : v;
            };
            st.gas = pedal(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_BUTTON_SOUTH);
            st.brake = pedal(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_BUTTON_EAST);
            st.shift_up = pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
            st.shift_down = pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            st.view = down(SDL_GAMEPAD_BUTTON_NORTH);
            st.handbrake = down(SDL_GAMEPAD_BUTTON_WEST);
        }
        std::copy(std::begin(now_down), std::end(now_down), std::begin(s.buttons));
        s.left_trigger = lt, s.right_trigger = rt;
    }
}

std::vector<PadKey> Gamepads::take_keys() {
    std::vector<PadKey> k;
    k.swap(keys_);
    return k;
}

void Gamepads::rumble(int i, float low, float high, int ms) {
    if (!slot_[i].pad || rumble_ <= 0) return;   // (VIBRATION OFF: nothing to send)
    // The call waits for the pad (on Xbox pads SDL sends it at once, with
    // XInput): timed, so a slow one shows in out/slow.log
    float k = RUMBLE_STRENGTH[std::clamp(rumble_, 0, 3)] * 65535.0f;
    Uint64 from = SDL_GetTicksNS();
    SDL_RumbleGamepad(slot_[i].pad, Uint16(std::clamp(low, 0.0f, 1.0f) * k), Uint16(std::clamp(high, 0.0f, 1.0f) * k),
                      Uint32(ms));
    g_vibration_seconds += double(SDL_GetTicksNS() - from) * 1e-9;
}

void Gamepads::rumble_triggers(int i, float left, float right, int ms) {
    Slot& s = slot_[i];
    if (!s.pad || rumble_ <= 0) return;
    // (raw input knows a pad's trigger motors only once it has matched the
    // pad with Windows.Gaming.Input, after its first input: asked again then)
    if (!s.trigger_motors) {
        s.trigger_motors = SDL_GetBooleanProperty(SDL_GetGamepadProperties(s.pad), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        if (!s.trigger_motors) return;
        printf("[gamepad] %d: trigger rumble\n", i + 1);
    }
    float k = RUMBLE_STRENGTH[std::clamp(rumble_, 0, 3)] * 65535.0f;
    Uint64 from = SDL_GetTicksNS();
    SDL_RumbleGamepadTriggers(s.pad, Uint16(std::clamp(left, 0.0f, 1.0f) * k), Uint16(std::clamp(right, 0.0f, 1.0f) * k),
                              Uint32(ms));
    g_vibration_seconds += double(SDL_GetTicksNS() - from) * 1e-9;
}
