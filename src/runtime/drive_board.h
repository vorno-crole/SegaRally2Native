// Force feedback board of the Star Wars Trilogy joystick (Sega 838-13992
// family): a Z80 at 8 MHz running epr-21119, a Z80 CTC whose channel 1 drives
// the NMI, an ADC that reads the stick's two pots one bit at a time, and the
// motor outputs. The main board writes a command byte to input register 0x10
// and reads the board's reply at input register 0x0C; at boot the game waits
// for the board to answer its initialisation (reply 0xCB) and stops with
// FAILED FEEDBACK LEVER INITIALIZE otherwise.
// Port map and timing follow the behaviour of Supermodel's
// Src/Model3/DriveBoard/JoystickBoard.cpp; the Z80 core is
// third_party/z80 (MIT).
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include "z80.h"
}

class DriveBoard {
public:
    // rom: epr-21119 (64 KB); empty = no board (reads 0xFF, like a detached one)
    void init(std::vector<uint8_t> rom);
    void reset();
    void run_frame();                  // one video frame of Z80 time
    void command(uint8_t v);           // main board writes input register 0x10
    uint8_t reply() const { return rom_.empty() ? 0xFF : reply_; }   // input register 0x0C
    bool initialized() const { return initialized_; }
    // Stick position seen by the board's ADC (0x80 = centre); the board only
    // reads it after it has initialised, before that it sees the centre
    void set_stick(uint8_t x, uint8_t y) { stick_x_ = x; stick_y_ = y; }
    // Sega Rally 2's wheel board (epr-20512) does not write a motor level:
    // port 0x2E picks a servo register and port 0x2A gives its value (decoded
    // as Supermodel's WheelBoard.cpp does). What the game asks for:
    float force() const { return force_ * gain_ / (127.0f * 128.0f); }    // -1..1, + turns the wheel right
    float vibration() const { return vibrate_ * gain_ / (127.0f * 255.0f); }   // 0..1
    uint8_t reg() const { return out46_; }      // the last register and value (SR2_FFB_LOG)
    uint8_t value() const { return out42_; }

private:
    static uint8_t mem_read(void* self, uint16_t a);
    static void mem_write(void* self, uint16_t a, uint8_t v);
    static uint8_t port_in(z80* z, uint8_t port);
    static void port_out(z80* z, uint8_t port, uint8_t v);
    uint8_t in(uint8_t port);
    void out(uint8_t port, uint8_t v);

    struct CtcChannel {
        uint8_t control = 0;
        bool counter_mode = false, prescaler256 = false, rising = false, manual = false, wait_constant = false;
        bool running = false;
        unsigned constant = 0, counter = 0;
    };
    void ctc_write(int ch, uint8_t v);
    unsigned ctc_frequency(int ch, unsigned clock) const;
    void restart_nmi_timer();

    static constexpr unsigned CLOCK = 8000000;
    z80 cpu_{};
    std::vector<uint8_t> rom_;
    uint8_t ram_[0x2000] = {};
    CtcChannel ctc_[4];
    bool nmi_enabled_ = false;
    long nmi_timer_ = 0;
    std::mutex mutex_;
    std::deque<uint8_t> commands_;
    uint8_t last_command_ = 0, sent_ = 0, reply_ = 0;
    bool initialized_ = false;
    uint8_t adc_port_ = 0;
    int adc_bit_ = 0;
    uint8_t stick_x_ = 0x80, stick_y_ = 0x80;
    uint8_t out42_ = 0x7F, out45_ = 0x7F, out46_ = 0;
    void servo_write();   // port 0x2A written: out42_ into the register out46_ picks
    uint8_t gain_ = 0x7F, vib1_ = 0, vib2_ = 0, vibrate_ = 0;
    int force_ = 0;       // -128..127
};
