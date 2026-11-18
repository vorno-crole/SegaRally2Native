// The cabinet's controls. Written by the window (keyboard), the pads and
// wheels or by --auto, and read by the emulated hardware at 0xF0040000.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct Inputs {
    // The system port's coin chutes and start buttons (the car is player 1's;
    // the second pair is wired all the same)
    std::atomic<bool> coin[2] = {false, false}, start[2] = {false, false};
    std::atomic<bool> service{false}, test{false};
    // The car: steering wheel and pedals as raw ADC values (wheel 0x80 =
    // centred, above it = right; pedals 0 = released), the 4-speed H shifter
    // (0 = neutral, 1..4), VIEW CHANGE and the hand brake
    std::atomic<int> steer{128}, accel{0}, brake{0};
    std::atomic<int> gear{1};
    std::atomic<bool> view{false}, handbrake{false};
    std::atomic<int> port_clear{0};   // --auto bitN: raw bits of the player port held low (probing)

    // F0040004, active low. Bank 0: bit0 coin 1, bit1 coin 2, bit2 test,
    // bit3 service, bit4 start 1, bit5 start 2. Bank 1: bit5 = EEPROM (set by runtime)
    uint8_t system_port(bool bank1) const {
        uint8_t v = 0xFF;
        if (!bank1) {
            if (coin[0]) v &= ~0x01;
            if (coin[1]) v &= ~0x02;
            if (test) v &= ~0x04;
            if (service) v &= ~0x08;
            if (start[0]) v &= ~0x10;
            if (start[1]) v &= ~0x20;
        }
        return v;
    }
    // F0040008 (the game's INPUT TEST, fn_00074CD8): bit0 VIEW CHANGE, bit1
    // hand brake; the shifter on bits 4-6, read as the index (bit5 4, bit4 2,
    // bit6 1) of "NN3142NN"
    uint8_t player_port() const {
        uint8_t v = uint8_t(0xFF & ~port_clear);
        if (view) v &= ~0x01;
        if (handbrake) v &= ~0x02;
        static constexpr uint8_t GEAR_BITS[5] = {0x00, 0x50, 0x60, 0x10, 0x20};
        int g = gear;
        if (g >= 0 && g <= 4) v &= uint8_t(~GEAR_BITS[g]);
        return v;
    }
    // ADC channels: 0 = steering, 1 = accelerator, 2 = brake
    uint8_t adc(int channel) const {
        return channel == 0 ? uint8_t(steer) : channel == 1 ? uint8_t(accel) : channel == 2 ? uint8_t(brake) : 0x80;
    }
    // Nothing of the car stays held (the window in the background, a menu open)
    void release_car() {
        accel = brake = 0;
        steer = 128;
        view = handbrake = false;
    }
};

extern Inputs g_inputs;

// The shakes of the wheel's force feedback since the pads last took them
// (0..1; runtime.cpp, from the drive board's motors)
extern std::atomic<float> g_ffb_shake;
// What the triggers of a pad that has motors in them (Xbox One / Series)
// feel since the pads last took it, 0..1: [0] the brake (left), [1] the
// accelerator (right). runtime.cpp, trigger_frame
extern std::atomic<float> g_trigger_rumble[2];

// A pad's BACK and START: the coin / the start button held for 0.1 s. Driven
// by the clock, so whoever polls the controls (the window at the monitor's
// rate) calls press_update() each time.
void press_coin();
void press_start();
void press_update();

// --auto "coin@1500,start@1600": scheduled presses (6 frames each; a trailing
// "+" holds from then on). coin, start, coin2, start2, test, service, gas,
// brake, view, hbrake, up / down (the shifter), aimX/Y (the wheel's ADC value
// X from then on; Y is not used), bitA..bitH (raw bits of the player port)
struct AutoInput {
    struct Event { std::string name; uint64_t frame; };
    std::vector<Event> events;
    void parse(const std::string& s);
    void apply(uint64_t frame);
};
