#include "drive_board.h"

#include <algorithm>
#include <cstring>

void DriveBoard::init(std::vector<uint8_t> rom) {
    rom_ = std::move(rom);
    reset();
}

void DriveBoard::reset() {
    z80_init(&cpu_);
    cpu_.read_byte = mem_read;
    cpu_.write_byte = mem_write;
    cpu_.port_in = port_in;
    cpu_.port_out = port_out;
    cpu_.userdata = this;
    memset(ram_, 0, sizeof(ram_));
    for (CtcChannel& c : ctc_) c = CtcChannel{};
    nmi_enabled_ = false;
    nmi_timer_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.clear();
        last_command_ = 0;
    }
    sent_ = reply_ = 0;
    initialized_ = false;
    adc_port_ = 0;
    adc_bit_ = 0;
    out42_ = out45_ = 0x7F;
    out46_ = 0;
    gain_ = 0x7F;
    vib1_ = vib2_ = vibrate_ = 0;
    force_ = 0;
}

void DriveBoard::command(uint8_t v) {
    if (rom_.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (v == last_command_) return;   // the board sees changes, as on the real bus
    if (commands_.size() >= 16) commands_.pop_front();
    commands_.push_back(v);
    last_command_ = v;
}

void DriveBoard::run_frame() {
    if (rom_.empty()) return;
    {   // the next command, shown to the Z80 for the whole frame
        std::lock_guard<std::mutex> lock(mutex_);
        if (!commands_.empty()) {
            sent_ = commands_.front();
            commands_.pop_front();
        }
    }
    long frame = long(CLOCK / 57.524);
    while (frame > 0) {
        long budget = nmi_enabled_ ? std::min(frame, std::max(nmi_timer_, 1L)) : frame;
        unsigned long start = cpu_.cyc;
        while (long(cpu_.cyc - start) < budget) z80_step(&cpu_);
        long ran = long(cpu_.cyc - start);
        frame -= ran;
        if (nmi_enabled_) {
            nmi_timer_ -= ran;
            if (nmi_timer_ <= 0) {
                z80_gen_nmi(&cpu_);
                restart_nmi_timer();
            }
        }
    }
}

// ---- Z80 bus ----
uint8_t DriveBoard::mem_read(void* self, uint16_t a) {
    auto* b = static_cast<DriveBoard*>(self);
    if (a < 0x9000) return a < b->rom_.size() ? b->rom_[a] : 0xFF;   // ROM 0x0000-0x8FFF
    if (a >= 0xE000) return b->ram_[a - 0xE000];                     // RAM 0xE000-0xFFFF
    return 0xFF;
}

void DriveBoard::mem_write(void* self, uint16_t a, uint8_t v) {
    if (a >= 0xE000) static_cast<DriveBoard*>(self)->ram_[a - 0xE000] = v;
}

uint8_t DriveBoard::port_in(z80* z, uint8_t port) { return static_cast<DriveBoard*>(z->userdata)->in(port); }
void DriveBoard::port_out(z80* z, uint8_t port, uint8_t v) { static_cast<DriveBoard*>(z->userdata)->out(port, v); }

uint8_t DriveBoard::in(uint8_t port) {
    switch (port) {
    case 0x20: return 0xCF;   // DIP switches 1
    case 0x21: return 0xFF;   // DIP switches 2
    case 0x24:                // ADC channel 1: stick Y
    case 0x25:                // ADC channel 2: stick X
    case 0x26:
    case 0x27: {
        if (port != adc_port_ || adc_bit_-- <= 0) return 0xFF;
        uint8_t v = 0x80;     // centre until the board has initialised (its centring test)
        if (initialized_ && port == 0x24) v = stick_y_;
        if (initialized_ && port == 0x25) v = stick_x_;
        return (v >> adc_bit_) & 1;
    }
    case 0x28: return sent_;  // command from the main board
    case 0x2C: return 0x00;   // encoder status: no error
    default: return 0xFF;
    }
}

void DriveBoard::out(uint8_t port, uint8_t v) {
    switch (port) {
    case 0x10: case 0x11: case 0x12: case 0x13: ctc_write(port & 3, v); break;
    case 0x24: case 0x25: case 0x26: case 0x27:   // ADC conversion start: 8 bits follow, MSB first
        adc_port_ = port;
        adc_bit_ = 8;
        break;
    case 0x29:                // reply to the main board
        reply_ = v;
        if (v == 0xCB) initialized_ = true;
        break;
    case 0x2A: out42_ = v; servo_write(); break;   // servo register value
    case 0x2D: out45_ = v; break;   // clutch / lamps on deluxe cabinets
    case 0x2E: out46_ = v; break;   // servo register
    default: break;                 // 7-segment displays, watchdog, parallel ports
    }
}

// The wheel board's servo registers (port 0x2E), each written through port
// 0x2A. Read as Supermodel's WheelBoard.cpp does; what the servo board does
// with them beyond the force and the vibration is not known
void DriveBoard::servo_write() {
    uint8_t v = out42_;
    switch (out46_) {
    case 0xFB:   // torque gain, 0x80 (none) to 0xFF (full); 0x05 at start-up is not one
        if (v & 0x80) gain_ = v & 0x7F;
        break;
    case 0xFC:   // bit 2: centring (our own spring stands in for it); else vibration nibbles
        if (!(v & 0x04)) {
            uint8_t& b = (v & 0x02) ? vib2_ : vib1_;
            uint8_t n = v >> 4;
            b = (v & 0x01) ? uint8_t((b & 0x0F) | (n << 4)) : uint8_t((b & 0xF0) | n);
            if ((v & 0x03) == 3) {   // set once the last nibble is in
                int s = vib1_ && vib2_ ? ((vib1_ >> 1) - 7) * 0x50 + ((vib2_ >> 1) - 5) * 0x10 + 0x0F : 0;
                vibrate_ = uint8_t(std::clamp(s, 0, 0xFF));
            }
        }
        break;
    case 0xFE:   // constant force: 0x80 none (0x7F and 0x81 too), 0x40-0x7E right, 0x82-0xC0 left
        if (v > 0x81) force_ = v <= 0xC0 ? 2 * (0x81 - v) : -0x80;
        else if (v < 0x7F) force_ = v >= 0x40 ? 2 * (0x7F - v) : 0x7F;
        else force_ = 0;
        break;
    case 0xFF:   // motor idle, whatever the value
        force_ = 0;
        vibrate_ = 0;
        break;
    default: break;
    }
}

// ---- Z80 CTC (only what the board uses: channel 1 as the NMI timer) ----
void DriveBoard::ctc_write(int ch, uint8_t v) {
    CtcChannel& c = ctc_[ch];
    if (v & 0x01) {   // control word
        c.control = v;
        c.counter_mode = v & 0x40;
        c.prescaler256 = v & 0x20;
        c.rising = v & 0x10;
        c.manual = v & 0x08;
        c.wait_constant = v & 0x04;
        if (v & 0x02) { c.counter = 0; c.running = false; }   // software reset
    } else if (c.wait_constant) {   // time constant (0 = 256)
        c.constant = v ? v : 256;
        c.counter = c.constant;
        c.wait_constant = false;
        c.running = !c.manual;
    }
    if (ch == 1) {
        nmi_enabled_ = c.running;
        if (nmi_enabled_) restart_nmi_timer();
    }
}

unsigned DriveBoard::ctc_frequency(int ch, unsigned clock) const {
    const CtcChannel& c = ctc_[ch];
    if (!c.counter_mode) return c.constant ? clock / (c.constant * (c.prescaler256 ? 256 : 16)) : 0;
    if (c.rising && ch > 0 && c.counter) return ctc_frequency(ch - 1, clock) / c.counter;   // counts the previous channel
    return 0;
}

void DriveBoard::restart_nmi_timer() {
    unsigned f = ctc_frequency(1, CLOCK);
    nmi_timer_ = f ? long(CLOCK / f) : long(CLOCK / 57.524);
}
