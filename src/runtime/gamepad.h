// Gamepads through SDL3's gamepad subsystem (no SDL window): Xbox,
// DualSense / DualShock 4, Switch Pro, Joy-Con and the rest SDL knows.
// The window thread polls them. In the game a pad drives the car and
// vibrates with the wheel's force feedback; in the menus it sends PadKeys.
// app_input.cpp decides which pad drives.
//
// Buttons (positions, so the same on every pad):
//   steering  left stick             accelerator / brake  RT / LT, or A / B
//   shifter   RB up, LB down         VIEW CHANGE  Y       hand brake  X
//   BACK      coin (Share, View, -)  START  start after a coin, else pause
//   menus     d-pad or left stick, A to accept and B to go back (on Nintendo
//             pads the A and B they print), LB / RB change the settings tab
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Gamepad;

// Seconds spent in the calls that drive the motors (a pad's vibration, the
// wheel's force), added up until the window loop takes them: those calls wait
// for the device on the thread that shows the picture (out/slow.log)
extern double g_vibration_seconds;

// What the pads send to the menus (any pad)
enum class PadKey { Up, Down, Left, Right, Accept, Back, TabPrev, TabNext, Page, Start };   // Page: Y (G, a game's GitHub page)

struct PadState {
    bool connected = false;
    std::string name;
    bool start = false, back = false;   // pressed since the last poll
    // The car: left stick x steers (-1..1), RT / LT are the accelerator and
    // the brake (0..1, A and B too), RB / LB shift up and down (pressed
    // since the last poll), Y is VIEW CHANGE and X the hand brake
    float steer = 0, gas = 0, brake = 0;
    bool shift_up = false, shift_down = false, view = false, handbrake = false;
};

class Gamepads {
public:
    static constexpr int MAX = 4;

    bool init();   // false: no gamepads this run (the game works without them)
    void shutdown();
    // Trigger rumble asks SDL to open Xbox pads through raw input (with
    // Windows.Gaming.Input, which drives the trigger motors) instead of
    // XInput, which cannot: init() reads it (--no-trigger-rumble: off)
    bool trigger_rumble = true;
    // Reads the pads. `rumble`: vibration strength, 0 off .. 3 high;
    // `focused`: the window is in front (otherwise the pads are idle);
    // `menu`: a menu has the controls, so the d-pad, the left stick and the
    // buttons become PadKeys
    void poll(int rumble, bool focused, bool menu);
    const PadState& pad(int i) const { return state_[i]; }
    bool changed() { bool c = changed_; changed_ = false; return c; }   // a pad came or went
    std::vector<PadKey> take_keys();
    // Vibration, 0..1 per motor, scaled by the settings' strength
    void rumble(int i, float low, float high, int ms);
    // The motors in the triggers (Xbox One / Series pads), 0..1 each, scaled
    // the same; nothing on pads without them
    void rumble_triggers(int i, float left, float right, int ms);

private:
    struct Slot {
        SDL_Gamepad* pad = nullptr;
        uint32_t id = 0;
        bool nintendo = false;   // A on the right: accept and back swap
        bool trigger_motors = false;   // SDL can drive motors in its triggers
        bool buttons[32] = {}, left_trigger = false, right_trigger = false;
        bool menu_held[32] = {};   // held when the menu closed: nothing in the game until let go (gamepad.cpp)
        // Menu direction held, for the key repeat
        int dir = -1;
        double next_repeat = 0;
    };
    void open(uint32_t id);
    void close(uint32_t id);
    int slot_of(uint32_t id) const;

    bool ok_ = false, changed_ = false;
    Slot slot_[MAX];
    PadState state_[MAX];
    std::vector<PadKey> keys_;
    int rumble_ = 2;
};
