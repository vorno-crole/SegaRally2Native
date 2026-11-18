// Steering wheels and other joysticks that are not gamepads (SDL's joystick
// API, no SDL window): Logitech, Thrustmaster, Fanatec, arcade-style USB
// wheels... Written without a wheel at hand, so everything that differs from
// one wheel to another is in save/wheel.ini, written with the defaults the
// first time a wheel is plugged in:
//   - steering: one axis (0 by default), the part of its travel that gives
//     the full lock (the arcade wheel turns about 270 degrees, so a 900
//     degree wheel set to 900 in its driver wants about 0.3)
//   - pedals: one axis each, or one combined axis (accelerator one way, brake
//     the other). Their resting value is learnt when the wheel arrives (the
//     pedals let go), so it does not matter which way an axis runs; a 0 from
//     SDL waits for the pedal's first reading near an end of its axis, and
//     gas_rest / brake_rest in wheel.ini can give it by hand
//   - menus and the pause screen: the hat moves, two buttons accept and go
//     back, start resumes, the shift paddles change the settings tab
//   - buttons: shift up / down (the paddles), VIEW CHANGE, hand brake, coin,
//     start, and an H shifter's gears 1-4 when the wheel has one
//   - force feedback (SDL haptic): the game's own motor force as a constant
//     force, plus a centring spring, with their strengths
//   - pedals, an H shifter or a hand brake that are USB devices of their own
//     (issue #73): pedals_device / shifter_device / handbrake_device name
//     them (part of the name, or VID:PID), and their axes and buttons are
//     then read there; handbrake_axis takes an analog hand brake
// SR2_WHEEL_LOG=1 prints every axis and button that moves, to find them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class PadKey;   // gamepad.h

struct WheelState {
    bool connected = false;
    std::string name;
    float steer = 0;                // -1 (left) .. 1 (right)
    float gas = 0, brake = 0;       // 0..1
    bool view = false, handbrake = false;     // held
    bool shift_up = false, shift_down = false, coin = false, start = false;   // pressed since the last poll
    int gear = -1;                  // H shifter: -1 none configured, 0 neutral, 1..4
    bool moved = false;             // something moved in the last poll (the wheel takes over)
};

// Called by Gamepads (gamepad.cpp), which owns SDL and its events
void wheels_init();                         // after SDL_Init: SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC too
void wheels_added(uint32_t joystick_id);    // SDL_EVENT_JOYSTICK_ADDED
void wheels_removed(uint32_t joystick_id);  // SDL_EVENT_JOYSTICK_REMOVED
bool wheel_is_ours(uint32_t joystick_id);   // a wheel: Gamepads leaves it alone even if SDL maps it as a gamepad
void wheels_poll(bool focused, bool menu, std::vector<PadKey>& keys);   // menu: PadKeys from the hat and buttons
void wheels_shutdown();

const WheelState& wheel_state();

// OPTIONS > WHEEL: one row per control (STEERING, ACCELERATOR...,
// each showing its axis or button and device) and the force feedback and
// rotation settings. Choosing a control's row listens to every joystick until
// one moves, and that becomes the control, saved in wheel.ini; Left / Right
// change the two settings
struct WheelRow {
    std::string group;        // the heading it is under: CONTROLS, FORCE FEEDBACK, ADVANCED
    std::string label, value, hint;
    float fill = -1;          // a setting's bar, 0..1 (-1: none)
    int meter = 0;            // what the control reads now, in `live`: 0 nothing, 1 the steering -1..1,
    float live = 0;           // 2 a pedal or analog hand brake 0..1, 3 a button (1 held)
};
std::vector<WheelRow> wheel_rows();
void wheel_row_input(int row, int dir);   // a control's row: listen; a setting's: change it by dir
void wheel_row_clear(int row);             // a control's row: not set any more
// While a control listens, or WHEEL SETUP runs: a window over the tab
// (wheel_prompt), Enter on one of its choices, Esc stops it
bool wheel_listening();
void wheel_listen_cancel();
struct WheelPrompt {
    bool on = false;
    enum Icon { INTRO, TURN_RIGHT, QUARTER_TURN, PEDAL, HAND_BRAKE, BUTTON, GEAR, FORCE, DONE } icon = INTRO;
    std::string kicker, title, action, hint;
    std::string glyph;         // PEDAL: GAS / BRAKE; BUTTON: what is on it (+, -, VIEW...)
    int gear = 0;              // GEAR: 1..4
    int step = 0, steps = 0;   // WHEEL SETUP: step of steps (0: none shown)
    float progress = 0;        // what moves, towards being taken; the hold of a quarter turn
    float time_left = -1;      // a countdown 1..0 (-1: none)
    std::string moving;        // what moves most now ("FANATEC WHEEL  -  AXIS 0")
    std::string done, note;    // just taken (shown for a moment), and a control it took from
    float done_age = -1;       // seconds since (-1: nothing taken)
    std::vector<std::string> lines;     // the devices found, or what the setup set
    std::vector<std::string> choices;   // along the bottom
};
WheelPrompt wheel_prompt();
void wheel_prompt_choose(int choice);
// The wheel as it is now, in the menus too (WheelState gives the game nothing
// there): the WHEEL tab's drawing of the wheel and the pedals
struct WheelLive {
    bool on = false;          // a wheel is plugged in
    float steer = 0;          // -1..1, as the game gets it (rotation and dead zone)
    float gas = 0, brake = 0, handbrake = 0;   // 0..1
    int gear = -1;            // H shifter: -1 none, 0 neutral, 1..4
};
const WheelLive& wheel_live();
// Force feedback from the game's drive board: `force` -1..1 (the motor, 0 =
// none; the sign as the board drives it), `shake` 0..1 (sudden changes)
void wheel_set_force(float force, float shake);
