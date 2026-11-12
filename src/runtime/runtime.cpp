// The emulator: Model 3 memory, CROM and banks, hardware registers, vblank
// and decrementer, the game thread, game states and a diagnostic report. The
// window, the renderer and the menus are in app.cpp; runtime.h is the seam.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <atomic>
#include <mutex>

#include "eeprom.h"
#include "inputs.h"
#include "wheel.h"
#include "fns.h"
#include "runtime.h"
#include "drive_board.h"
#include "jtag.h"
#include "scsi53c810.h"
#include "../video/tilegen.h"
#include "../audio/audio_out.h"
#include "../audio/sound_board.h"
#include "../video/r3d_state.h"
#include "stats.h"
#include "options.h"

extern Options g_opt;   // app.cpp: command line and menu settings (DIFFICULTY)

Ctx g_ctx;
uint8_t* g_ram;
uint8_t g_trace_bits[512];
std::map<uint32_t, uint64_t> g_trace_hits;   // function -> hit count
uint32_t g_recent_fn[64];
uint32_t g_recent_idx = 0;

void rt_print_recent() {
    fprintf(stderr, "  last functions:");
    for (int i = 24; i >= 1; i--) fprintf(stderr, " %X", g_recent_fn[(g_recent_idx - i) & 63]);
    fprintf(stderr, "\n");
}

// --diff FN@FRAME: on the first call of FN at or after FRAME, saves the CPU
// state and RAM before and after running it (out/diff_before.bin,
// out/diff_after.bin) plus every device read (out/diff_mmio.bin), then exits.
// tools/diff_unicorn.py replays the call on an independent PowerPC emulator.
uint32_t g_diff_fn = 0xFFFFFFFF;
uint64_t g_diff_frame = 0;
bool g_diff_active = false;
std::vector<uint32_t> g_diff_mmio;   // (address, size, value) triples
void diff_run(Ctx& c, uint32_t fn);

void rt_trace(Ctx& c, uint32_t fn) {
    if (fn == g_diff_fn && !g_diff_active) {
        diff_run(c, fn);
        return;
    }
    auto it = g_trace_hits.find(fn);
    if (it == g_trace_hits.end()) return;   // filter collision
    if (++it->second > 3) return;            // full detail only for the first 3
    printf("[trace] fn_%08X lr=%08X\n", fn, c.lr);
    for (int i = 0; i < 32; i++) printf("  r%-2d=%08X%s", i, c.r[i], i % 4 == 3 ? "\n" : "");
    for (int i = 3; i <= 10; i++) {
        uint32_t p = c.r[i];
        if (p < RAM_SIZE - 32 || p >= 0xFF000000) {
            printf("  [r%d]:", i);
            for (int k = 0; k < 8; k++) printf(" %08X", rd32(p + 4 * k));
            printf("\n");
        }
    }
}

// ---- Shared with the window side (app.cpp): see runtime.h ----
std::vector<uint8_t> g_crom, g_banked, g_vrom;
std::vector<uint8_t> g_drive_rom;
std::string g_data_dir = "data";
int g_frame_limit = INT32_MAX;   // --frames; with --headless, 600 by default
bool g_collect_stats = false;    // keep access statistics and the full texture stream for report()
Eeprom93C46 g_eeprom;
SoundBoard g_sound;
AudioOut g_audio;
AutoInput g_auto;
std::shared_ptr<const R3DFrameState> g_r3d_latest;                  // last closed 3D frame
std::condition_variable g_r3d_cv;                                   // notified when it changes
std::mutex g_r3d_mutex;
R3DTextureRam g_texture_ram;
std::mutex g_video_mutex;
std::atomic<float> g_wide_view{1.0f}, g_wide_applied{1.0f};   // widescreen culling (runtime.h)
std::vector<uint8_t> g_video_vram(TILEGEN_SIZE);
uint64_t g_video_frame = 0;
bool g_realtime = false;
LARGE_INTEGER g_qpf, g_start_time;
// CPU time of the game thread per emulated frame, without the waits (frame
// pacing, menus): summed in microseconds, read by the window thread once a second
std::atomic<uint64_t> g_game_busy_us{0};
std::atomic<uint64_t> g_game_frames{0};
// The WARNING notice and the copyright line shown at power-on are run through
// at full speed with nothing on screen: the game appears at the SEGA logo.
std::atomic<bool> g_boot_skip{false};
std::atomic<int> g_arcade_entry{0};
std::atomic<int> g_debug_jump{-1};
std::atomic<bool> g_debug_freeze_time{false};
std::atomic<bool> g_skip_attract{false}, g_game_over_menu{false}, g_in_attract{false};
// Menus: the game stands still (and so does its sound) while set. The boot
// screens still run through first, so Play starts at the SEGA logo.
std::atomic<bool> g_hold{false};
// Shutdown: the window thread sets g_quit; the game thread parks itself at
// its next frame event (its native stack cannot be unwound) and reports it
// in g_parked, so the audio can be stopped with nobody pushing samples.
std::atomic<bool> g_quit{false}, g_parked{false};

double qpc_seconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(freq.QuadPart);
}

namespace {

constexpr uint64_t CPU_HZ = 166666667;      // PowerPC 603ev at 166 MHz (Step 2.1). TODO: DEC and TB still tick every 4 cycles, as tuned for Lost World
constexpr double FRAME_HZ = 57.524;         // Model 3 refresh rate
constexpr uint64_t FRAME_CYCLES = uint64_t(CPU_HZ / FRAME_HZ);
constexpr uint64_t SAMPLE_CYCLES = 200000;  // sampling profiler
constexpr uint32_t MSR_EE = 0x8000, MSR_IP = 0x40;

uint64_t g_frame = 0, g_next_sample = SAMPLE_CYCLES;
// Frame timeline (424 lines: 40 of VBlank, then 384 active; spec section 8)
constexpr uint64_t LINE_CYCLES = FRAME_CYCLES / 424;
uint64_t g_frame_start = 0, g_next_phase = 0;
int g_phase = 0;              // 0 = VBlank start, 1 = end of VBlank, 2 = ping-pong flip, 3 = last line
uint8_t g_midi_ctrl = 0;      // sound board MIDI control port
int g_midi_pulses = 0;        // MIDI interrupts left in this VBlank
uint64_t g_midi_next = 0;
uint32_t flip_line();

// Interrupt controller (0xF0100014 mask, 0xF0100018 status)
uint8_t g_irq_state = 0, g_irq_enable = 0;   // level-triggered, acknowledged by writing 0xF0100018
bool g_in_irq = false;
uint64_t g_irq_count[8] = {};

// Decrementer
uint32_t g_dec_value = 0xFFFFFFFF;
uint64_t g_dec_base = 0;
bool g_dec_armed = false;
uint64_t g_dec_irqs = 0;

[[noreturn]] void exit_from_game_thread(int code);
uint8_t g_adc_channel = 0;
Ncr53c810 g_scsi;
DriveBoard g_drive;   // force feedback board of the joystick (its own Z80)
Jtag g_jtag;   // Real3D ASIC IDs, checked at boot on Step 2.1

// Time base (TB): the game resets it to 0 and measures delays with it
uint64_t g_tb_base = 0;

// Real3D: a status bit (0x84000000) toggles every frame;
// the game uses it to know the chip has finished drawing.
uint32_t g_real3d_status = 0xFFFFFFFD;   // CPU view: bit 1 = ping-pong

// PCI configuration space. Step 2.1 has an MPC106 bridge: its address register
// is at 0xFEC00000 and its data register at 0xFEE00000; io_addr() folds them
// onto the MPC105 addresses Lost World used (0xF0800CF8 address, 0xF0C00CFC data)
std::map<uint32_t, uint32_t> g_pci_cfg;

uint32_t le32_at(const uint8_t* p) { return uint32_t(p[0]) | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

uint32_t pci_read(uint32_t cfg) {
    uint32_t dev = (cfg >> 11) & 0x1F, reg = cfg & 0xFC;
    if (reg == 0) {
        if (dev == 0x00) return 0x00021057;   // the bridge itself: Motorola MPC106
        if (dev == 0x0D) return 0x178611DB;   // Real3D Pro-1000, Step 2.x (Step 1.x: 0x16C311DB)
        return 0xFFFFFFFF;                    // no device (no 53C810 on Step 2 boards)
    }
    auto it = g_pci_cfg.find(cfg & 0xFFFFFFFC);
    return it == g_pci_cfg.end() ? 0 : it->second;
}

// Device memory: 64 KB pages allocated on demand
std::unordered_map<uint32_t, std::vector<uint8_t>> g_dev;
std::vector<uint8_t> g_texture_fifo;   // the part not yet uploaded (the whole stream with g_collect_stats)
std::unordered_set<uint32_t> g_r3d_dirty;                          // pages touched since the last frame
size_t g_fifo_consumed = 0;                                         // bytes of the FIFO already uploaded
uint64_t g_r3d_frames = 0;
void r3d_flush();
void texture_port_word(uint32_t w);

struct Stat { uint64_t reads = 0, writes = 0; uint32_t last = 0; };
std::unordered_map<uint32_t, Stat> g_stats;  // per address (control) or per page (everything else)
std::map<uint32_t, uint64_t> g_missing;      // called functions that don't exist
std::map<uint32_t, uint64_t> g_bad;          // untranslated instructions
std::unordered_map<uint32_t, uint64_t> g_samples;

// Control registers by exact address; memories (Real3D, backup RAM) by page
bool is_page(uint32_t a) {
    if (a >= 0xC1000000 && a < 0xC1000100) return false;   // 53C810 registers
    return !(a >= 0xF0000000 && a < 0xF1000000 && !(a >= 0xF00C0000 && a < 0xF0100000));
}
uint32_t stat_key(uint32_t a) { return is_page(a) ? (a & ~0xFFFu) : a; }

uint8_t* dev_ptr(uint32_t a) {
    static uint32_t last_key = 0xFFFFFFFF;
    static uint8_t* last_page = nullptr;
    if ((a >> 16) != last_key) {
        auto& page = g_dev[a >> 16];
        if (page.empty()) page.assign(0x10000, 0);
        last_key = a >> 16;
        last_page = page.data();
    }
    return last_page + (a & 0xFFFF);
}

const uint8_t* rom_ptr(uint32_t a) {
    // Step 2.1: the fixed 8 MB CROM fills 0xFF800000-0xFFFFFFFF; below it, the
    // 0xFF000000 window shows one 8 MB bank of the banked CROM.
    // The bank is the inverted low nibble of system register 0x08 (written
    // at 0xFE100008, folded onto 0xF0100008 by io_addr)
    if (a >= 0xFF800000) return &g_crom[(a - 0xFF800000) % g_crom.size()];
    if (a >= 0xFF000000 && !g_banked.empty()) {
        uint32_t bank = (~*dev_ptr(0xF0100008)) & 0xF;
        size_t off = size_t(bank) * 0x800000 + (a - 0xFF000000);
        return &g_banked[off % g_banked.size()];
    }
    return nullptr;
}

// Step 2.1 boards decode the system devices (inputs, MIDI, backup RAM, system
// registers, RTC, security) at 0xFExxxxxx as a mirror of 0xF0xxxxxx; the MPC106
// configuration registers sit inside that mirror. Everything below is written
// against the 0xF0 addresses, so device accesses go through here first.
uint32_t io_addr(uint32_t a) {
    if ((a >> 24) != 0xFE) return a;
    if (a >= 0xFEC00000 && a < 0xFEE00000) return 0xF0800CF8 + (a & 3);   // MPC106 CONFIG_ADDR
    if (a >= 0xFEE00000 && a < 0xFEF00000) return 0xF0C00CFC + (a & 3);   // MPC106 CONFIG_DATA
    return 0xF0000000 | (a & 0x00FFFFFF);
}

// Real3D DMA device (Step 2.x, 0xC2000000). The game writes its 32-bit
// registers byte-reversed (little-endian device), so le32_at() reads them:
// 0x00 source, 0x04 destination, 0x08 length in words (writing it runs the
// copy), 0x0C status (bit 0 IRQ pending), 0x0D IRQ acknowledge, 0x0E config
// (bit 0x80 byte-reverses each word, bit 0x01 raises the IRQ when done),
// 0x10 command, 0x14 command result.
bool g_dma_irq = false;   // non-maskable in the board's IRQ controller

static void r3d_dma_copy() {
    uint32_t src = le32_at(dev_ptr(0xC2000000)), dst = le32_at(dev_ptr(0xC2000004));
    uint32_t words = le32_at(dev_ptr(0xC2000008));
    uint8_t config = *dev_ptr(0xC200000E);
    rt_dma_copy(dst, src, words * 4, config & 0x80);
    if (config & 0x01) {
        *dev_ptr(0xC200000C) |= 0x01;
        g_dma_irq = true;
        g_ctx.next_event = g_ctx.cycles;
    }
}

static void r3d_dma_command(uint32_t cmd) {
    uint32_t result = 0;
    if (cmd & 0x20000000) result = 0x16C311DB;   // ID command: the games expect the Step 1.x ID here
    // Real3D register read. g_real3d_status is the CPU's view through lwz at
    // 0x84000000, i.e. byte-swapped; the register itself is the swap of it
    // (the game reads the result with lwbrx and waits for ping-pong bit 0x02000000)
    else if (cmd & 0x80000000) result = (cmd & 0x3F) == 0 ? bswap32(g_real3d_status) : 0;
    uint8_t* p = dev_ptr(0xC2000014);
    for (int i = 0; i < 4; i++) p[i] = uint8_t(result >> (8 * i));
}

// RTC-72421 at 0xF0140000: one digit per register, register n in the high
// byte of word n (0 = seconds, 1 = tens of seconds ... 12 = day of the week);
// the next byte reads 0x03 (battery voltage test). The clock runs on game
// time (host time at start + cycles), so the boot code's CPU speed
// measurement (TB ticks over one RTC second, fn_000F15E4) stays consistent
// at any emulation speed.
static uint8_t rtc_read(uint32_t a) {
    if ((a & 3) == 1) return 0x03;
    if (a & 3) return 0;
    static const time_t start = time(nullptr);
    time_t now = start + time_t(g_ctx.cycles / CPU_HZ);
    struct tm t;
    localtime_s(&t, &now);
    switch ((a >> 2) & 0xF) {
    case 0: return uint8_t(t.tm_sec % 10);
    case 1: return uint8_t(t.tm_sec / 10);
    case 2: return uint8_t(t.tm_min % 10);
    case 3: return uint8_t(t.tm_min / 10);
    case 4: return uint8_t(t.tm_hour % 10);
    case 5: return uint8_t(t.tm_hour / 10);
    case 6: return uint8_t(t.tm_mday % 10);
    case 7: return uint8_t(t.tm_mday / 10);
    case 8: return uint8_t((t.tm_mon + 1) % 10);
    case 9: return uint8_t((t.tm_mon + 1) / 10);
    case 10: return uint8_t(t.tm_year % 10);
    case 11: return uint8_t((t.tm_year % 100) / 10);
    case 12: return uint8_t(t.tm_wday);
    default: return 0;
    }
}

// Security board (315-5881 crypto chip) at 0xF01A0000. The game reads 112
// bytes through the data register (0x1C, two bytes per word in its high half,
// low byte first) and compares them with its copyright text at RAM 0x1B1E80
// ("===== STAR WARS =====..."); a mismatch prints "not correct" and a screen
// of numbers (fn_000A3600). The chip decrypts data stored on the board; here
// it hands back the text the game expects. The first read after the address
// registers (0x10/0x14) are written returns 0xFFFF.
static bool g_security_first = true;
static uint32_t g_security_pos = 0;
static uint8_t security_read(uint32_t a) {
    static uint8_t word[4] = {0xFF, 0xFF, 0, 0};
    if (a == 0xF01A001C) {
        if (g_security_first) {
            g_security_first = false;
            word[0] = word[1] = 0xFF;
        } else {
            uint32_t p = 0x1B1E80 + 2 * (g_security_pos++ % 0x38);
            word[1] = rd8(p);       // byte stored first (bits 16-23)
            word[0] = rd8(p + 1);   // then bits 24-31
        }
    }
    if (a >= 0xF01A001C && a < 0xF01A0020) return word[a - 0xF01A001C];
    if (a < 0xF01A0004) return 0;   // status: ready
    return 0xFF;
}

uint8_t dev_read8(uint32_t a) {
    a = io_addr(a);
    if ((a >> 8) == 0xF01A00) return security_read(a);
    if (a == 0xF0100018) return g_irq_state;
    if (a == 0xF0100014) return g_irq_enable;
    if (a == 0xF010001C) return 0xFF;
    if (a == 0xF0100010) return g_jtag.read();
    if (a == 0xF0080000) return 0x00;   // MIDI data port
    if (a == 0xF0080004) return 0x83;   // MIDI control port
    if ((a >> 16) == 0xF014) return rtc_read(a);
    if (a == 0xF0040004) {
        bool bank1 = *dev_ptr(0xF0040000) & 0x01;   // bank selected by writing 0xF0040000
        uint8_t v = g_inputs.system_port(bank1);
        return bank1 ? uint8_t((v & 0xDF) | (g_eeprom.read_do() << 5)) : v;
    }
    // Player buttons: active low, 0xFF = nothing pressed
    if (a == 0xF0040008) return g_inputs.player_port();
    if (a == 0xF004000C) return g_drive.reply();   // force feedback board reply (Lost World: player 2 buttons)
    if (a == 0xF0040018) return 0x7F;
    if (a == 0xF0040034) return 0x00;   // serial FIFO flags (no light gun board)
    if (a == 0xF004003C) {              // ADC: wheel and pedals, channel auto-increments
        uint8_t v = g_inputs.adc(g_adc_channel & 7);
        g_adc_channel++;
        return v;
    }
    if (a > 0xF0040000 && a < 0xF0040040 && (a & 3) == 0) return 0xFF;   // unused inputs read as 1s (0x00 reads back the bank)
    if (a >= 0x84000000 && a < 0x84000004) return uint8_t(g_real3d_status >> (24 - 8 * (a & 3)));
    if (a >= 0xF0C00CFC && a < 0xF0C00D00) {
        uint32_t v = pci_read(le32_at(dev_ptr(0xF0800CF8)));
        return uint8_t(v >> (8 * (a - 0xF0C00CFC)));
    }
    if (a >= 0xC1000000 && a < 0xC1000100) {
        uint8_t v;
        if (g_scsi.read8(a & 0xFF, v)) return v;
    }
    // Network (link) board at 0xC0000000 (Sega Rally 2 can link cabinets): none
    // here. Read as open bus, the game skips it; read back as RAM it uploads its
    // program to the board and waits forever at 0x00079424 for it to start
    if ((a >> 20) == 0xC00) return 0xFF;
    return *dev_ptr(a);
}

void dev_write8(uint32_t a, uint8_t v) {
    a = io_addr(a);
    if (a >= 0xF01A0010 && a < 0xF01A0018) { g_security_first = true; g_security_pos = 0; }
    if ((a >> 8) == 0xC20000) {   // Real3D DMA registers
        if (a == 0xC200000D) {   // IRQ acknowledge
            if (v & 1) { *dev_ptr(0xC200000C) &= ~1; g_dma_irq = false; }
            return;
        }
        *dev_ptr(a) = v;
        if (a == 0xC200000B) r3d_dma_copy();
        if (a == 0xC2000013) r3d_dma_command(le32_at(dev_ptr(0xC2000010)));
        if (a == 0xC2000017) for (int i = 0; i < 4; i++) dev_ptr(0xC2000014)[i] = 0xFF;
        return;
    }
    if (a == 0xF0100014) { g_irq_enable = v; g_ctx.next_event = g_ctx.cycles; }
    if (a == 0xF0100018) g_irq_state &= ~v;   // acknowledge
    if (a == 0xF010000C) g_jtag.write(v);
    if (a == 0xF0040010) g_drive.command(v);   // command to the force feedback board
    if (a == 0xF1180010) g_irq_state &= ~v;   // tilegen IRQ acknowledge register (LE low byte)
    if (a == 0xF0080000) {   // MIDI byte to the sound board
        g_irq_state &= ~0x40;
        g_sound.midi_write(v);
    }
    if (a == 0xF0080004) {
        g_midi_ctrl = v;
        if (!(v & 0x20)) g_irq_state &= ~0x40;
    }
    if (a == 0xF0040000) g_eeprom.write_pins(v);
    if (a == 0xF004003C) g_adc_channel = v & 7;
    *dev_ptr(a) = v;
    if (a == 0xF0C00CFF)
        g_pci_cfg[le32_at(dev_ptr(0xF0800CF8)) & 0xFFFFFFFC] = le32_at(dev_ptr(0xF0C00CFC));
    if (a >= 0xC1000000 && a < 0xC1000100) g_scsi.wrote8(a & 0xFF, v, dev_ptr(0xC1000000));
    // 0x94000000 is a texture FIFO: the whole stream is stored in order
    if ((a >> 24) == 0x94) g_texture_fifo.push_back(v);
    // 0x90000000: texture port, three words (source, header, trigger) per upload
    if ((a >> 24) == 0x90 && (a & 3) == 3) texture_port_word(le32_at(dev_ptr(a & ~3u)));
    uint32_t region = a >> 24;
    if (region == 0x8C || region == 0x8E || region == 0x98) g_r3d_dirty.insert(a >> 16);
    // Writing to the command port (0x88000000) closes the 3D frame
    if (a == 0x88000003) r3d_flush();
}

void texture_port_word(uint32_t w) {
    static uint32_t words[2];
    static int count = 0;
    if (count < 2) {
        words[count++] = w;
        return;
    }
    count = 0;   // third write triggers the upload; its data is ignored
    uint32_t src = words[0] & 0xFFFFFF, header = words[1];
    if (src >= 0x100000) {
        if (g_vrom.empty()) return;
        size_t words_total = g_vrom.size() / 4;
        size_t start = src % words_total;
        const uint16_t* p = reinterpret_cast<const uint16_t*>(g_vrom.data() + start * 4);
        g_texture_ram.upload_direct(header, p, (words_total - start) * 2);
    } else {
        // Polygon RAM source: copy the (at most 1 MB) rest of it
        std::vector<uint16_t> buf((0x100000 - src) * 2);
        for (size_t i = 0; i < buf.size() / 2; i++) {
            uint32_t v = le32_at(dev_ptr(0x98000000 + uint32_t(src + i) * 4));
            buf[2 * i] = uint16_t(v);
            buf[2 * i + 1] = uint16_t(v >> 16);
        }
        g_texture_ram.upload_direct(header, buf.data(), buf.size());
    }
}

// When the game writes the command port the Real3D memory is published as an
// immutable frame (unchanged pages are shared) and the texture FIFO collected
// since the previous flush is applied to texture RAM.

void r3d_flush() {
    auto state = std::make_shared<R3DFrameState>();
    state->flushed = state->time = qpc_seconds();
    state->vblank = g_frame;
    // At real speed the frame is dated by its vblank: the game finishes its
    // frames a few ms after the vblank, by a varying amount, and that jitter
    // would make the interpolated motion uneven
    if (g_realtime && !g_boot_skip) {
        double vblank = double(g_start_time.QuadPart) / double(g_qpf.QuadPart) + double(g_frame) / FRAME_HZ;
        if (state->flushed >= vblank && state->flushed < vblank + 0.05) state->time = vblank;
    }
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        if (g_r3d_latest) state->pages = g_r3d_latest->pages;
    }
    double t_pages = qpc_seconds();
    size_t dirty_pages = g_r3d_dirty.size();
    for (uint32_t key : g_r3d_dirty)
        state->pages[key] = std::make_shared<const R3DPage>(*reinterpret_cast<const R3DPage*>(g_dev[key].data()));
    g_r3d_dirty.clear();
    state->frame = ++g_r3d_frames;

    double t_tex = qpc_seconds();
    size_t words = (g_texture_fifo.size() - g_fifo_consumed) / 4;
    if (words) {
        std::vector<uint32_t> fifo(words);
        memcpy(fifo.data(), g_texture_fifo.data() + g_fifo_consumed, words * 4);
        g_fifo_consumed += words * 4;
        g_texture_ram.upload_fifo(fifo);
        // The whole stream is only needed by dump_video_memory (report());
        // otherwise the uploaded part is dropped so the vector does not grow all session
        if (!g_collect_stats) {
            g_texture_fifo.erase(g_texture_fifo.begin(), g_texture_fifo.begin() + ptrdiff_t(g_fifo_consumed));
            g_fifo_consumed = 0;
        }
    }
    double t_end = qpc_seconds();
    frame_log("F %llu %.3f %.3f %zu %zu\n", (unsigned long long)g_frame, (t_tex - t_pages) * 1000,
              (t_end - t_tex) * 1000, dirty_pages, words);
    {
        std::lock_guard<std::mutex> lock(g_r3d_mutex);
        g_r3d_latest = std::move(state);
    }
    g_r3d_cv.notify_all();
}

// Dumps the Real3D and 2D layer memories for the future renderer
void dump_video_memory() {
    CreateDirectoryA("out", nullptr);   // CreateDirectory does not create the parent
    CreateDirectoryA("out/real3d", nullptr);
    struct Region { uint32_t base, size; const char* name; };
    const Region regions[] = {
        {0x8C000000, 0x01000000, "culling_lo"}, {0x8E000000, 0x01000000, "culling_hi"},
        {0x98000000, 0x01000000, "polygon"},    {0xF1000000, 0x00200000, "tilegen"},
    };
    for (auto& r : regions) {
        // The 3D memories are taken from the last closed frame (consistent state)
        std::unordered_map<uint32_t, const uint8_t*> pages;
        if ((r.base >> 24) != 0xF1 && g_r3d_latest)
            for (auto& [key, page] : g_r3d_latest->pages) pages[key] = page->data();
        else
            for (auto& [key, page] : g_dev) pages[key] = page.data();
        uint32_t top = 0;
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + r.size) top = std::max(top, (key << 16) + 0x10000 - r.base);
        if (!top) continue;
        std::vector<uint8_t> buf(top, 0);
        for (auto& [key, page] : pages)
            if ((key << 16) >= r.base && (key << 16) < r.base + top) memcpy(&buf[(key << 16) - r.base], page, 0x10000);
        std::string path = std::string("out/real3d/") + r.name + ".bin";
        if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
        else fprintf(stderr, "[warning] could not write %s\n", path.c_str());
        printf("  %-12s %08X  %u KB\n", r.name, r.base, top / 1024);
    }
    if (FILE* f = fopen("out/real3d/texture_fifo.bin", "wb")) {
        fwrite(g_texture_fifo.data(), 1, g_texture_fifo.size(), f);
        fclose(f);
    }
    printf("  texture_fifo %08X  %zu KB\n", 0x94000000u, g_texture_fifo.size() / 1024);
}

// SR2_TILEGEN_DUMP=first,last,step: the tilegen memory of those frames to
// out/real3d/tilegen_<frame>.bin (to follow the 2D layers during the boot)
constexpr uint32_t TILEGEN_DUMP_SIZE = 0x190000;
static void tilegen_dump_frame(uint64_t frame) {
    static int first = -1, last = 0, step = 1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("SR2_TILEGEN_DUMP")) sscanf(e, "%d,%d,%d", &first, &last, &step);
        if (step < 1) step = 1;
    }
    if (first < 0 || frame < uint64_t(first) || frame > uint64_t(last) || (frame - first) % step) return;
    CreateDirectoryA("out", nullptr);
    CreateDirectoryA("out/real3d", nullptr);
    std::vector<uint8_t> buf(TILEGEN_DUMP_SIZE, 0);
    for (auto& [key, page] : g_dev)
        if ((key << 16) >= 0xF1000000u && (key << 16) < 0xF1000000u + TILEGEN_DUMP_SIZE)
            memcpy(&buf[(key << 16) - 0xF1000000u], page.data(), 0x10000);
    char path[64];
    snprintf(path, sizeof path, "out/real3d/tilegen_%llu.bin", (unsigned long long)frame);
    if (FILE* f = fopen(path, "wb")) { fwrite(buf.data(), 1, buf.size(), f); fclose(f); }
}

void note(uint32_t a, bool write, uint32_t v) {
    if (!g_collect_stats) return;   // the report only comes with --frames / --headless
    auto& s = g_stats[stat_key(a)];
    (write ? s.writes : s.reads)++;
    s.last = v;
}

uint32_t find_function(uint32_t pc) {
    int lo = 0, hi = g_fn_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr <= pc) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best >= 0 ? g_fn_table[best].addr : 0;
}

// Ping-pong flip line: tilegen register 0x08 (device word = LE bytes)
uint32_t flip_line() {
    uint32_t v = le32_at(dev_ptr(0xF1180008));
    return v ? v : 238;
}

void schedule(Ctx& c) {
    if (g_diff_active) { c.next_event = ~0ull; return; }
    uint64_t n = std::min(g_next_phase, g_next_sample);
    if (g_dec_armed) n = std::min(n, g_dec_base + uint64_t(g_dec_value) * 4);
    if (g_midi_pulses > 0) n = std::min(n, std::max(g_midi_next, c.cycles + 1));
    if (((g_irq_state & g_irq_enable) || g_dma_irq) && (c.msr & MSR_EE)) n = c.cycles;
    c.next_event = n;
}

// A handler that returns (rfi) somewhere else than where it interrupted:
// Sega Rally 2's external interrupt (0x500) restarts the program at 0x4C
// when 0x137120 is set (the TEST button). The translated code cannot jump
// there from inside the interrupt: the C++ stack is unwound to the game
// thread's top (game_thread), which goes on from that address
struct WarmRestart {
    uint32_t pc;
};

void exception(Ctx& c, uint32_t vector) {
    g_in_irq = true;
    uint32_t back = c.pc_hint;
    c.srr0 = back;
    c.srr1 = c.msr;
    // What the handler reads and writes with mfspr / mtspr (SPR 26, 27)
    c.spr[26] = back;
    c.spr[27] = c.msr;
    c.msr &= ~(MSR_EE | 0x4000u /*PR*/ | 0x30u /*IR/DR*/);
    rt_call(c, ((c.srr1 & MSR_IP) ? 0xFFF00000u : 0u) + vector);
    c.msr = c.srr1;   // rfi
    g_in_irq = false;
    if (c.spr[26] != back && vector == 0x500) throw WarmRestart{c.spr[26]};
}

void report() {
    Ctx& c = g_ctx;
    printf("\n================ REPORT ================\n");
    printf("Frames: %llu  cycles: %llu  MSR: %08X\n", (unsigned long long)g_frame,
           (unsigned long long)c.cycles, c.msr);
    printf("Interrupts: mask=%02X state=%02X  delivered per bit:", g_irq_enable, g_irq_state);
    for (int i = 0; i < 8; i++) printf(" %llu", (unsigned long long)g_irq_count[i]);
    printf("  decrementer: %llu\n", (unsigned long long)g_dec_irqs);

    printf("\nWhere the time goes (samples per function):\n");
    std::map<uint32_t, uint64_t> per_fn;
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& [pc, n] : g_samples) { per_fn[find_function(pc)] += n; hot.push_back({n, pc}); }
    std::vector<std::pair<uint64_t, uint32_t>> fns;
    for (auto& [f, n] : per_fn) fns.push_back({n, f});
    std::sort(fns.rbegin(), fns.rend());
    std::sort(hot.rbegin(), hot.rend());
    for (size_t i = 0; i < fns.size() && i < 12; i++)
        printf("  fn_%08X  %llu\n", fns[i].second, (unsigned long long)fns[i].first);
    printf("Hottest single spots:\n");
    for (size_t i = 0; i < hot.size() && i < 8; i++)
        printf("  %08X  %llu\n", hot[i].second, (unsigned long long)hot[i].first);

    printf("\nSound: %u notes restarted on stopped voices (they used to be lost)\n", unsigned(Scsp::restarted()));
    printf("\nHardware accesses (register or memory range, reads, writes, last value):\n");
    std::map<uint32_t, Stat> stats(g_stats.begin(), g_stats.end());
    for (auto it = stats.begin(); it != stats.end();) {
        // Consecutive 4 KB pages are merged into a single range
        uint32_t first = it->first, last = first;
        Stat s = it->second;
        auto next = std::next(it);
        bool page = is_page(first);
        while (page && next != stats.end() && next->first == last + 0x1000) {
            last = next->first;
            s.reads += next->second.reads;
            s.writes += next->second.writes;
            s.last = next->second.last;
            ++next;
        }
        if (page)
            printf("  %08X-%08X  R=%-9llu W=%-9llu last=%08X\n", first, last + 0xFFF,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        else
            printf("  %08X           R=%-9llu W=%-9llu last=%08X\n", first,
                   (unsigned long long)s.reads, (unsigned long long)s.writes, s.last);
        it = next;
    }

    g_scsi.report();
    g_sound.close_wav();
    if (!g_trace_hits.empty()) {
        printf("\nTraced functions (times called):\n");
        for (auto& [fn, n] : g_trace_hits) printf("  fn_%08X  %llu\n", fn, (unsigned long long)n);
    }
    if (FILE* f = fopen("out/ram.bin", "wb")) {   // for analysis with the tools
        fwrite(g_ram, 1, RAM_SIZE, f);
        fclose(f);
    }
    printf("\nVideo memories dumped to out/real3d/:\n");
    dump_video_memory();

    if (!g_missing.empty()) {
        printf("\nCalls to functions not recompiled:\n");
        for (auto& [a, n] : g_missing) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    if (!g_bad.empty()) {
        printf("\nInvalid instructions executed:\n");
        for (auto& [a, n] : g_bad) printf("  %08X x%llu\n", a, (unsigned long long)n);
    }
    fflush(stdout);
}

}  // namespace

bool load_rom(const std::string& name, std::vector<uint8_t>& out, bool required) {
    std::string path = g_data_dir + "/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (required) fprintf(stderr, "Cannot find %s (run tools/romtool.py first)\n", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    out.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

DWORD WINAPI game_thread(LPVOID) {
    Ctx& c = g_ctx;
    c.msr = MSR_IP;
    c.next_event = 0;
    // PVR: PowerPC 603ev ("Goldeneye", 2.1). The boot code wants a version
    // above 0x00070000 (error 0x10 on the LEDs otherwise)
    c.spr[287] = 0x00071202;
    g_drive.init(g_drive_rom);
    if (g_drive_rom.empty()) printf("No force feedback board program (epr-21119): the game will stop at FAILED FEEDBACK LEVER\n");
    // Test switches left set in the console (set SR2_...=1) change the game
    // in ways that look like bugs later on: listed, so a log shows them
    if (char** env = _environ)
        for (; *env; env++)
            if (strncmp(*env, "SR2_", 4) == 0 || strncmp(*env, "LW_", 3) == 0) printf("[env] %s\n", *env);
    printf("Running the boot code from 0xFFF00100...\n");
    // The boot code goes on into the program itself (a tail branch to 0x100),
    // so the whole game normally runs inside this first call
    uint32_t entry = 0;
    for (;;) {
        try {
            if (!entry) {
                fn_FFF00100(c);
                printf("Boot done: jumping to %08X (MSR=%08X)\n", c.lr, c.msr);
                entry = c.lr;
            }
            rt_call(c, entry);
            break;
        } catch (const WarmRestart& r) {   // exception(): an interrupt that restarts the program
            printf("[boot] the program restarts at %08X (frame %llu)\n", r.pc, (unsigned long long)g_frame);
            fflush(stdout);
            g_in_irq = false;
            entry = r.pc;
        }
    }
    printf("The main program returned (unexpected)\n");
    report();
    return 0;
}

// ---- Slow memory: CROM, banks and devices ----
uint8_t rt_rd8_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return *p;
    uint8_t v = dev_read8(a);
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 1u, v});
    return v;
}
uint16_t rt_rd16_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint16_t(p[0] << 8 | p[1]);
    uint16_t v = uint16_t(dev_read8(a) << 8 | dev_read8(a + 1));
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 2u, v});
    return v;
}
uint32_t rt_rd32_slow(uint32_t a) {
    if (auto p = rom_ptr(a)) return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
    uint32_t v = uint32_t(dev_read8(a)) << 24 | dev_read8(a + 1) << 16 | dev_read8(a + 2) << 8 | dev_read8(a + 3);
    note(a, false, v);
    if (g_diff_active) g_diff_mmio.insert(g_diff_mmio.end(), {a, 4u, v});
    return v;
}
// Stores outside RAM and outside any device usually mean a bad pointer:
// report the first ones with the recent call history
static void check_wild_write(uint32_t a, uint32_t v) {
    static int reported = 0;
    if (a >= RAM_SIZE && a < 0x80000000u && reported < 8) {
        reported++;
        fprintf(stderr, "[warning] write outside RAM %08X = %08X (pc ~%08X, frame %llu)\n", a, v, g_ctx.pc_hint,
                (unsigned long long)g_frame);
        rt_print_recent();
    }
}

// SR2_WATCH=lo,hi: the first 300 CPU writes in [lo, hi) with the code that made them
// (pc of the function, its caller and three more frames of the stack's back chain)
static void watch_write(uint32_t a, uint32_t v, int bytes) {
    static uint32_t lo = 1, hi = 0;
    static int left = 300;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("SR2_WATCH")) sscanf(e, "%x,%x", &lo, &hi);
    }
    if (a < lo || a >= hi || left <= 0) return;
    left--;
    printf("[watch] frame %llu %08X = %0*X  pc~%08X lr=%08X", (unsigned long long)g_frame, a, bytes * 2, v,
           g_ctx.pc_hint, g_ctx.lr);
    uint32_t sp = g_ctx.r[1];
    for (int i = 0; i < 4 && sp && sp < RAM_SIZE - 8; i++) {
        sp = rd32(sp);
        if (!sp || sp >= RAM_SIZE - 8) break;
        printf(" <%08X", rd32(sp + 4));
    }
    printf("\n");
}

void rt_wr8_slow(uint32_t a, uint8_t v) { watch_write(a, v, 1); check_wild_write(a, v); note(a, true, v); dev_write8(a, v); }
void rt_wr16_slow(uint32_t a, uint16_t v) {
    watch_write(a, v, 2);
    check_wild_write(a, v);
    note(a, true, v);
    dev_write8(a, uint8_t(v >> 8));
    dev_write8(a + 1, uint8_t(v));
}
void rt_wr32_slow(uint32_t a, uint32_t v) {
    watch_write(a, v, 4);
    check_wild_write(a, v);
    note(a, true, v);
    for (int i = 0; i < 4; i++) dev_write8(a + i, uint8_t(v >> (24 - 8 * i)));
}

// Access statistics for a run of bytes, as the byte-by-byte path would count them
static void note_range(uint32_t a, uint32_t n, bool write, uint8_t last) {
    if (!g_collect_stats) return;
    while (n) {
        uint32_t run = std::min(n, 0x1000 - (a & 0xFFF));
        auto& s = g_stats[stat_key(a)];
        (write ? s.writes : s.reads) += run;
        s.last = last;
        a += run;
        n -= run;
    }
}

// Block copy for the 53C810 Memory Move and the Real3D DMA. Plain memories
// (RAM, ROM, the Real3D pages and the texture FIFO) are copied in runs that
// stay inside one 64 KB page, with the same side effects as the byte path
// (dirty pages, FIFO stream, statistics); registers and ports go through
// rd/wr so their side effects stay exact. `swap` reverses the bytes of each
// 32-bit word (Real3D DMA config bit 0x80), word by word in address order
// as the per-word copy did, so overlapping RAM copies give the same result.
void rt_dma_copy(uint32_t dst, uint32_t src, uint32_t n, bool swap) {
    if (swap) n &= ~3u;
    while (n) {
        uint32_t run = std::min(n, 0x10000 - (src & 0xFFFF));
        run = std::min(run, 0x10000 - (dst & 0xFFFF));
        if (swap) run &= ~3u;   // a word that straddles a page goes the slow way
        const uint8_t* sp = nullptr;
        if (src < RAM_SIZE) sp = g_ram + src;
        else if (src >= 0xFF000000) sp = rom_ptr(src);   // contiguous within a 64 KB page
        uint8_t* dp = nullptr;
        uint32_t region = dst >> 24;
        bool r3d = region == 0x8C || region == 0x8E || region == 0x98, fifo = region == 0x94;
        if (dst < RAM_SIZE) dp = g_ram + dst;
        else if (r3d || fifo) dp = dev_ptr(dst);
        if (!sp || !dp || !run) {   // a device: as the chip would, one byte (or word) at a time
            if (swap) {
                wr32(dst, bswap32(rd32(src)));
                dst += 4, src += 4, n -= 4;
            } else {
                wr8(dst, rd8(src));
                dst++, src++, n--;
            }
            continue;
        }
        if (swap) {
            for (uint32_t i = 0; i < run; i += 4) {
                uint8_t b0 = sp[i], b1 = sp[i + 1], b2 = sp[i + 2], b3 = sp[i + 3];
                dp[i] = b3, dp[i + 1] = b2, dp[i + 2] = b1, dp[i + 3] = b0;
            }
        } else {
            memmove(dp, sp, run);   // RAM to RAM may overlap
        }
        if (dst >= RAM_SIZE) {
            note_range(dst, run, true, dp[run - 1]);
            if (r3d) g_r3d_dirty.insert(dst >> 16);
            if (fifo) g_texture_fifo.insert(g_texture_fifo.end(), dp, dp + run);
        }
        dst += run, src += run, n -= run;
    }
}

// ---- Differential testing (--diff) ----
static void diff_save(const char* path, const Ctx& c, uint32_t fn) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[8] = {0x4C574446 /*LWDF*/, fn, c.cr, c.xer, c.lr, c.ctr, c.msr, c.fpscr};
    fwrite(hdr, 4, 8, f);
    fwrite(c.r, 4, 32, f);
    for (int i = 0; i < 32; i++) fwrite(&c.f[i].u, 8, 1, f);
    uint8_t bank = *dev_ptr(0xF0100008);
    fwrite(&bank, 1, 1, f);
    fwrite(g_ram, 1, RAM_SIZE, f);
    fclose(f);
}

void diff_run(Ctx& c, uint32_t fn) {
    if (g_frame < g_diff_frame) return;
    g_diff_active = true;
    c.next_event = ~0ull;
    diff_save("out/diff_before.bin", c, fn);
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {   // run the function once, with events held off
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr == fn) { g_fn_table[mid].fn(c); break; }
        if (g_fn_table[mid].addr < fn) lo = mid + 1; else hi = mid - 1;
    }
    diff_save("out/diff_after.bin", c, fn);
    if (FILE* f = fopen("out/diff_mmio.bin", "wb")) {
        fwrite(g_diff_mmio.data(), 4, g_diff_mmio.size(), f);
        fclose(f);
    }
    printf("diff: fn_%08X at frame %llu saved (%zu device reads)\n", fn,
           (unsigned long long)g_frame, g_diff_mmio.size() / 3);
    exit_from_game_thread(0);
}

// ---- Indirect calls ----
void rt_call(Ctx& c, uint32_t addr) {
    if (addr == 0 && g_frame > 0) {   // a null function pointer restarts the game on real hardware
        static int reported = 0;
        if (reported++ < 3) {
            fprintf(stderr, "[warning] call to address 0 (frame %llu, lr=%08X, r3=%08X)\n",
                    (unsigned long long)g_frame, c.lr, c.r[3]);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", rd32(c.r[3] + 4 * i));
            fprintf(stderr, "\n");
            rt_print_recent();
        }
    }
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].addr == addr) { g_fn_table[mid].fn(c); return; }
        if (g_fn_table[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    if (g_missing[addr]++ == 0) {
        fprintf(stderr, "[warning] call to %08X, not recompiled (from ~%08X)\n", addr, c.pc_hint);
        // recomp.py will include it in the next build
        if (FILE* f = fopen("out/missing.txt", "a")) {
            fprintf(f, "%08X  # called from ~%08X\n", addr, c.pc_hint);
            fclose(f);
        }
    }
}

void rt_bad(Ctx&, uint32_t addr) {
    if (g_bad[addr]++ == 0) fprintf(stderr, "[warning] invalid instruction at %08X\n", addr);
}

void rt_sc(Ctx& c, uint32_t addr) {
    c.pc_hint = addr;
    exception(c, 0xC00);
}

// ---- Events: frames, vblank, decrementer, sampling ----
// ---- Video: copy of the 2D layer on every vblank for the window thread ----
namespace {

// (Widescreen culling: the game culls to its own 4:3 view. Its viewport
// records are not known yet: g_wide_applied stays 1)

void video_snapshot() {
    std::lock_guard<std::mutex> lock(g_video_mutex);
    for (uint32_t off = 0; off < TILEGEN_SIZE; off += 0x10000) {
        auto it = g_dev.find((TILEGEN_BASE + off) >> 16);
        if (it != g_dev.end()) memcpy(&g_video_vram[off], it->second.data(), std::min<uint32_t>(0x10000, TILEGEN_SIZE - off));
    }
    g_video_frame = g_frame;
}

double g_wait_seconds = 0;   // time spent waiting since the last frame event (game thread only)

// Keeps the game at the original speed (vblank at FRAME_HZ)
void pace_frame() {
    double wait_from = qpc_seconds();
    struct Done {   // whichever way pace_frame returns, the wait is counted
        double from;
        ~Done() { g_wait_seconds += qpc_seconds() - from; }
    } done{wait_from};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double target = double(g_frame) / FRAME_HZ;
    double elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    if (elapsed > target + 0.25) {   // far behind (e.g. window being dragged): don't try to catch up
        g_start_time.QuadPart = now.QuadPart - LONGLONG(target * double(g_qpf.QuadPart));
        return;
    }
    while (elapsed < target) {
        if (target - elapsed > 0.002) Sleep(1);
        QueryPerformanceCounter(&now);
        elapsed = double(now.QuadPart - g_start_time.QuadPart) / double(g_qpf.QuadPart);
    }
}

ULONGLONG g_boot_ms = GetTickCount64();

// Exit from the game thread itself: nothing else pushes audio, so it is
// stopped here before the process goes away
[[noreturn]] void exit_from_game_thread(int code) {
    stats_save();   // runs that end here (--frames, --headless) keep what they counted
    fflush(stdout);
    g_audio.stop();
    console_prompt();
    ExitProcess(code);
}

// The SEGA logo is the first boot screen that fills the screen (grey backdrop);
// the ones before it are a few lines of text on black
bool boot_logo_up() {
    static std::vector<uint32_t> back(BACK_W * SCREEN_H), front(SCREEN_W * SCREEN_H), raw(SCREEN_W * SCREEN_H);
    TileGen::compose(g_video_vram.data(), {.back = back.data(), .front = front.data(), .front_raw = raw.data()});
    int lit = 0;
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t b = back[y * BACK_W + BACK_MARGIN + x], f = raw[y * SCREEN_W + x];
            lit += ((f >> 24) && (f & 0xFFFFFF)) || ((b >> 24) && (b & 0xFFFFFF));
        }
    return lit * 4 > SCREEN_W * SCREEN_H;
}
}  // namespace

bool g_fast_idle = false;

// Sega Rally 2's main loop (0x0002DD94) runs a sequence: the current entry
// at 0x22D618 (0 attract, 1 the game: from the credit on, through the mode,
// car and transmission selects, the race and its game over; -1 at power on),
// the next one at 0x22D61C. When they differ, the loop makes the next one
// current and runs its init (table 0xEDAF0, 12 bytes an entry), then its
// frame function every frame. The credit / START screen counts its steps at
// 0x22436C: 1 a credit taken, 4 once START is taken and SELECT GAME comes up
constexpr uint32_t SR2_SEQUENCE = 0x22D618, SR2_SEQUENCE_NEXT = 0x22D61C, SR2_START_STEP = 0x22436C;

static bool in_attract() {
    return int32_t(rd32(SR2_SEQUENCE)) != 1;
}

// The game's selects keep their state at 0x224320 (set up by 0x00046160):
// the step (+0x3C: 3 SELECT GAME, 6 SELECT COURSE, 9 SELECT CAR, 12 the
// transmission, 15 on the way to the race, 17 racing), the mode (+0x0C: 0
// championship, 1 practice; the SELECT GAME cursor itself), the course (+0x08:
// 0 desert, 1 mountain, 2 riviera, 3 snowy; set when SELECT COURSE is confirmed,
// its carousel has a cursor of its own), the car (+0x04: the SELECT CAR cursor,
// an index into the model table at 0xF1F00) and the transmission (+0x14: 0
// automatic, 1 manual). The race is set up from them (0x00046AAC)
constexpr uint32_t SR2_SELECT_STEP = 0x22435C, SR2_SELECT_MODE = 0x22432C, SR2_SELECT_COURSE = 0x224328,
                   SR2_SELECT_CAR = 0x224324, SR2_SELECT_GEARS = 0x224334;

// DEBUG (menu): from SELECT GAME on to a practice race of the course, car and
// transmission asked for, unseen. Each select gets its choice written and the
// pedal (it confirms them all); the course again on every later select, as its
// own confirm takes the carousel's. True once the race is on
static bool debug_jump_frame(uint64_t from) {
    stats_game_cheated();   // a race jumped to: no records
    int choice = g_debug_jump;
    uint32_t course = uint32_t(choice & 0xF) & 3, car = uint32_t(choice >> 4 & 0xF) & 7, gears = uint32_t(choice >> 8 & 1);
    Inputs& p = g_inputs;
    int32_t step = int32_t(rd32(SR2_SELECT_STEP));
    auto finish = [&](const char* why) {
        printf("[debug] %s: step %d after %llu frames (course %u car %u gears %u)\n", why, step,
               (unsigned long long)(g_frame - from), course, car, gears);
        p.accel = 0;
        g_debug_jump = -1;
        return true;
    };
    if (g_frame - from > 12000) return finish("gave up");
    if (step >= 17) return finish("racing");
    static const bool champ = getenv("SR2_DEBUG_CHAMP") != nullptr;   // testing aid: the championship (16 cars) instead
    if (step <= 3) wr32(SR2_SELECT_MODE, champ ? 0 : 1);   // practice
    if (step > 6) wr32(SR2_SELECT_COURSE, course);
    if (step > 6) wr32(SR2_SELECT_CAR, car);
    if (step > 9) wr32(SR2_SELECT_GEARS, gears);
    p.accel = (g_frame - from) % 40 < 6 ? 0xFF : 0;   // the pedal confirms
    return false;
}

// The race's clock is a block at 0x22CF80: +0x04 its state (2 counting
// down, 3 when it ran out: TIME IS UP), +0x08 the time left in 1/60 s
// (3600 = the 60 s a practice starts with; it counts down one a frame),
// +0x0C the time the next start sets it to.
// DEBUG (menu): TIME INFINITE holds the time left at the most it has been
// while the clock runs (a checkpoint's extension still adds to it)
constexpr uint32_t SR2_CLOCK_STATE = 0x22CF84, SR2_CLOCK_LEFT = 0x22CF88;

static void debug_time_frame() {
    static uint32_t held = 0;
    if (!g_debug_freeze_time || in_attract() || rd32(SR2_CLOCK_STATE) != 2) {
        held = 0;
        return;
    }
    uint32_t left = rd32(SR2_CLOCK_LEFT);
    if (left > 0x7FFFFFFF) return;
    if (left >= held) {
        if (!held) stats_game_cheated();   // a race with no end: no records
        held = left;
    } else {
        wr32(SR2_CLOCK_LEFT, held);
    }
}

// PLAY from the menu (app.cpp): the game runs on unseen, a coin and START
// every second while it is in the attract, then START until it takes it;
// true once its SELECT GAME screen is up.
// QUIT TO MAIN MENU (g_arcade_entry 3): the game is sent back to its attract
// the way it does that itself (sequence -1, step 1: 0x00027C70), and on to
// SELECT GAME again, all behind the menu
// The attract's screens would draw over the 2D text layers (A, A') before
// the next game; run past unseen they don't, and what the last game left
// there stayed on SELECT GAME (practice's big lap clock at the top left,
// in tiles whose patterns the selects' background had taken). Cleared when
// the attract is reached on the way back; the selects draw their own
static void clear_text_layers() {
    memset(dev_ptr(TILEGEN_BASE + 0xF8000), 0, 2 * 0x2000);   // name tables of layers A and A'
}

static bool arcade_entry_frame() {
    static uint64_t from = 0, select_at = 0, settle = 0;
    static bool start_pressed = false;   // the start step reads 4 from the last game until START goes in again
    Inputs& in = g_inputs;
    std::atomic<bool>&coin = in.coin[0], &start = in.start[0];
    in.accel = in.brake = 0;
    bool attract = in_attract();
    if (g_arcade_entry == 3) {
        from = g_frame, select_at = 0;
        if (g_debug_jump >= 0 && !attract && int32_t(rd32(SR2_SELECT_STEP)) == 3 && rd32(SR2_START_STEP) == 4) {
            // DEBUG with the game waiting at SELECT GAME (behind the menu): on from there
            g_arcade_entry = 2;
            select_at = g_frame;
            start_pressed = true;
        } else if (attract) {
            clear_text_layers();
            g_arcade_entry = 2;
        } else {
            wr32(SR2_SEQUENCE_NEXT, 0);   // the attract, entered as the game does after a game over
            g_arcade_entry = 4;
        }
    }
    if (g_arcade_entry == 4) {
        coin = start = false;
        // the attract itself (0), not the restart on the way to it (-1)
        if (int32_t(rd32(SR2_SEQUENCE)) != 0 && g_frame - from < 12000) return false;
        printf("[play] back in the attract after %llu frames\n", (unsigned long long)(g_frame - from));
        clear_text_layers();
        g_arcade_entry = 1;
        settle = 120;
    }
    if (g_arcade_entry == 1) {
        g_arcade_entry = 2;
        from = g_frame, select_at = 0;
    }
    if (g_frame == from && !select_at) start_pressed = false;
    uint64_t t = g_frame - from;
    if (t < settle) {
        coin = start = false;
        return false;
    }
    uint64_t k = (t - settle) % 60;
    bool taken = !attract && start_pressed && rd32(SR2_START_STEP) == 4;
    if (!select_at && !taken && t < 9000) {   // (once at SELECT GAME, the start step is the past)
        coin = attract && k < 6;      // a credit (the attract takes it at any point)
        start = !attract && k >= 30 && k < 36;
        start_pressed |= bool(start);
        return false;
    }
    coin = start = false;
    if (!select_at) {
        select_at = g_frame;
        printf("[play] SELECT GAME after %llu frames%s\n", (unsigned long long)t, taken ? "" : ": not reached, given up");
    }
    if (g_debug_jump >= 0) {   // DEBUG: on through the selects to the race, then its first frames drawn
        if (!debug_jump_frame(select_at)) return false;
        select_at = g_frame;
    }
    if (g_frame - select_at < 45) return false;   // its first frames drawn (it fades in)
    g_arcade_entry = 0;
    settle = 0;
    return true;
}

// The attract never shows with the menu on: a game that ends (the game over,
// or the name entry of a ranked time) goes back to the attract (sequence 0).
// On that frame, before its picture is taken, the run on to SELECT GAME
// starts behind the main menu, unseen and muted, as QUIT TO MAIN MENU does
static void attract_guard_frame() {
    static bool was = true;
    static int32_t seq_was = 0;
    int32_t seq = int32_t(rd32(SR2_SEQUENCE));
    // Back to power on (-1): the program restarted, as on leaving the TEST
    // menu: the SEGA logo and the attract would follow, so the same as a game over
    bool restarted = seq == -1 && seq_was != -1;
    if (seq != seq_was) printf("[play] sequence %d -> %d (frame %llu)\n", seq_was, seq, (unsigned long long)g_frame);
    seq_was = seq;
    bool attract = seq != 1;
    bool ended = (!was && attract) || restarted;
    was = attract;
    if (!ended || !g_skip_attract || g_arcade_entry || g_boot_skip) return;
    printf("[play] the game is over: back to the menu, on to SELECT GAME unseen (frame %llu)\n",
           (unsigned long long)g_frame);
    g_boot_skip = g_fast_idle = true;
    g_sound.set_dsp_enabled(false);
    g_sound.set_muted(true);
    g_arcade_entry = 3;
    g_game_over_menu = true;
}

// The wheel's force feedback as gamepad rumble: the drive board drives two
// motors (0x7F = no force). Their slow part is the spring that holds the
// wheel to the middle (it follows the wheel); the quick changes on top are
// the game's jolts and shakes (bumps, crashes), kept for the pads
// (app_input.cpp). SR2_FFB_LOG prints the motors as they change
static void ffb_frame() {
    static const bool log = getenv("SR2_FFB_LOG") != nullptr;
    static float slow[2] = {0, 0};
    static int last[2] = {-1, -1};
    int m[2] = {g_drive.motor_x(), g_drive.motor_y()};
    float shake = 0;
    for (int a = 0; a < 2; a++) {
        float f = float(m[a] - 0x7F);
        slow[a] += 0.2f * (f - slow[a]);
        shake = std::max(shake, std::fabs(f - slow[a]) / 128.0f);
    }
    if (log && (m[0] != last[0] || m[1] != last[1]))
        printf("[ffb] frame %llu  x %02X y %02X  shake %.2f  wheel %d\n", (unsigned long long)g_frame, m[0], m[1], shake,
               int(g_inputs.steer));
    last[0] = m[0], last[1] = m[1];
    float was = g_ffb_shake.load();
    if (shake > was) g_ffb_shake = shake;
    // A steering wheel's force feedback (wheel.cpp): the motor as a force,
    // 0x80 = none, smoothed over a few frames
    static float force = 0;
    force += 0.5f * ((float(m[0]) - 128.0f) / 128.0f - force);
    wheel_set_force(std::clamp(force, -1.0f, 1.0f), shake);
}

void rt_event(Ctx& c) {
    if (c.cycles >= g_next_sample) {
        g_samples[c.pc_hint]++;
        g_next_sample = c.cycles + SAMPLE_CYCLES;
    }
    while (c.cycles >= g_next_phase) {
        switch (g_phase) {
        case 0: {   // start of VBlank: new frame
            g_drive.set_stick(uint8_t(g_inputs.steer), 0x80);
            g_drive.run_frame();
            ffb_frame();
            g_frame++;
            g_frame_start = g_next_phase;
            tilegen_dump_frame(g_frame);
            {   // CPU time of the frame just finished, without the waits
                static double last_frame_time = 0;
                double now_s = qpc_seconds();
                if (last_frame_time > 0) {
                    g_game_busy_us += uint64_t(std::max(now_s - last_frame_time - g_wait_seconds, 0.0) * 1e6);
                    g_game_frames++;
                }
                last_frame_time = now_s;
                g_wait_seconds = 0;
            }
            if (g_frame % 60 == 0) {
                static ULONGLONG last_ms = GetTickCount64();
                ULONGLONG now = GetTickCount64();
                double fps = now > last_ms ? 60000.0 / double(now - last_ms) : 0.0;
                last_ms = now;
                printf("  frame %llu  (pc ~%08X)  %.0f fps\n", (unsigned long long)g_frame, c.pc_hint, fps);
                fflush(stdout);
            }
            g_auto.apply(g_frame);
            {   // The rivals' detail. The game sorts the cars in view by their size on
                // screen (fn_00015FA4: 0x225960 + 0xF0 the cars, + 0x110 the sizes) and
                // draws them (fn_00013B90) with the first of four models whose size
                // threshold (0xEB538: 0.09, 0.018, 0.0054, 0.0027) the car passes while
                // fewer cars than its count (0xEB514: 2, 3, 4, 5) are drawn already: the
                // full model (wheels that turn, its dust, the dark patch under it) only
                // for two cars within some 30 m, a plain box of a car beyond, swapped in
                // one frame. The thresholds are divided by 3 (the full car to some 90 m,
                // where the swap is a few pixels), and every car drawn may be a full one.
                // SR2_CAR_LOD=F: another factor (1: as the game). Only over the game's
                // own values (written once the boot code has copied them to RAM)
                static const float far_cars = [] {
                    const char* e = getenv("SR2_CAR_LOD");
                    float f = e ? float(atof(e)) : 3.0f;
                    return f >= 1.0f ? f : 1.0f;
                }();
                static const uint32_t sizes[4] = {0x3DB851EB, 0x3C9374BD, 0x3BB0F27C, 0x3B30F27C}, counts[4] = {2, 3, 4, 5};
                if (far_cars > 1.0f && rd32(0xEB538) == sizes[0] && rd32(0xEB53C) == sizes[1] && rd32(0xEB514) == counts[0] &&
                    rd32(0xEB520) == counts[3]) {
                    for (int i = 0; i < 4; i++) {
                        float f;
                        memcpy(&f, &sizes[i], 4);
                        f /= far_cars;
                        uint32_t w;
                        memcpy(&w, &f, 4);
                        wr32(0xEB538 + 4 * uint32_t(i), w);
                        wr32(0xEB514 + 4 * uint32_t(i), counts[3]);
                    }
                }
            }
            {   // DIFFICULTY (menu, --difficulty): bits 19-21 of the operator settings word, the
                // EEPROM's copy in RAM (docs/game_ram.md). The game reads it at each new game
                // (into 0x22CF18) and picks the time table's row with it. Only the RAM copy:
                // the EEPROM keeps the operator's own setting
                const int d = g_opt.difficulty;
                if (d >= 0 && d <= 4) {
                    constexpr uint32_t kSettings = 0x4A9D18, kMask = 0x00380000;
                    uint32_t w = rd32(kSettings);
                    uint32_t want = (w & ~kMask) | (uint32_t(d) << 19);
                    if (w != want) wr32(kSettings, want);
                }
            }
            {   // SR2_POKE=ADDR=VALUE[@FROM[-TO]],...: a RAM word written every frame (from that frame on, or
                // up to TO; finding the game's state, testing the achievements)
                struct Poke { uint32_t addr, value; uint64_t from, to; };
                static std::vector<Poke> pokes = [] {
                    std::vector<Poke> v;
                    const char* e = getenv("SR2_POKE");
                    while (e && *e) {
                        unsigned a = 0, val = 0;
                        int n = 0;
                        if (sscanf(e, "%x=%u%n", &a, &val, &n) < 2) break;
                        e += n;
                        uint64_t from = 0, to = ~0ull;
                        if (*e == '@') {
                            char* end = nullptr;
                            from = strtoull(e + 1, &end, 10);
                            e = end;
                            if (*e == '-') {
                                to = strtoull(e + 1, &end, 10);
                                e = end;
                            }
                        }
                        v.push_back({a, val, from, to});
                        if (*e == ',') e++;
                    }
                    return v;
                }();
                for (const Poke& p : pokes)
                    if (g_frame >= p.from && g_frame <= p.to) wr32(p.addr, p.value);
            }
            {   // SR2_RAM_DUMPS=F1,F2,...: the RAM to out/ram_F.bin on those frames (finding the game's state)
                static std::vector<uint64_t> dumps = [] {
                    std::vector<uint64_t> v;
                    if (const char* e = getenv("SR2_RAM_DUMPS"))
                        for (const char* q = e; *q;) {
                            v.push_back(strtoull(q, const_cast<char**>(&q), 10));
                            while (*q == ',' || *q == ' ') q++;
                        }
                    return v;
                }();
                if (std::find(dumps.begin(), dumps.end(), g_frame) != dumps.end()) {
                    char name[64];
                    snprintf(name, sizeof name, "out/ram_%llu.bin", (unsigned long long)g_frame);
                    if (FILE* f = fopen(name, "wb")) {
                        fwrite(g_ram, 1, RAM_SIZE, f);
                        fclose(f);
                    }
                }
            }
            debug_time_frame();
            attract_guard_frame();   // before the frame's picture is taken
            g_in_attract = in_attract();
            stats_frame();
            video_snapshot();
            bool boot_done = g_arcade_entry ? arcade_entry_frame() : (g_frame % 4 == 0 && boot_logo_up()) || g_frame > 3000;
            if (g_boot_skip && boot_done) {
                g_boot_skip = g_fast_idle = false;
                g_sound.set_dsp_enabled(true);
                g_sound.set_muted(false);   // fades in from silence
                printf("[boot] start screens skipped in %.1f s (frame %llu)\n",
                       double(GetTickCount64() - g_boot_ms) / 1000.0, (unsigned long long)g_frame);
                LARGE_INTEGER now;   // real-time pacing starts from here
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_hold && !g_boot_skip) {
                double hold_from = qpc_seconds();
                while (g_hold && !g_quit && !g_boot_skip) Sleep(4);   // a skip run (PLAY, QUIT) goes on behind the menu
                g_wait_seconds += qpc_seconds() - hold_from;
                LARGE_INTEGER now;   // carry on at real speed from here, without catching up
                QueryPerformanceCounter(&now);
                g_start_time.QuadPart = now.QuadPart - LONGLONG(double(g_frame) / FRAME_HZ * double(g_qpf.QuadPart));
            }
            if (g_realtime && !g_boot_skip) pace_frame();
            frame_log("G %llu %.6f\n", (unsigned long long)g_frame, qpc_seconds());
            if (g_frame >= uint64_t(g_frame_limit)) {
                report();
                exit_from_game_thread(0);
            }
            if (g_quit) {   // the window is closing: stand still until the process ends
                g_parked = true;
                for (;;) Sleep(INFINITE);
            }
            {
                double t_sound = qpc_seconds();
                g_sound.run_frame(FRAME_HZ);
                double scsp_ms, dsb_ms;
                uint32_t midi;
                g_sound.frame_times(scsp_ms, dsb_ms, midi);
                frame_log("A %llu %.3f %.3f %.3f %u\n", (unsigned long long)g_frame, (qpc_seconds() - t_sound) * 1000, scsp_ms, dsb_ms, midi);
            }
            g_midi_pulses = (g_midi_ctrl & 0x20) ? 128 : 0;
            g_phase = 1;
            g_next_phase = g_frame_start + 40 * LINE_CYCLES;
            break;
        }
        case 1:   // end of VBlank
            g_irq_state |= 0x0D;
            g_midi_pulses = 0;
            g_phase = 2;
            g_next_phase = g_frame_start + (40 + std::min<uint32_t>(383, flip_line())) * LINE_CYCLES;
            break;
        case 2:   // Real3D ping-pong flip (line set in tilegen register 0x08)
            g_real3d_status ^= 0x00000002;   // the game reads it with lwz+stwbrx: bit 0x02000000 after the swap
            g_phase = 3;
            g_next_phase = g_frame_start + (40 + 383) * LINE_CYCLES;
            break;
        case 3:   // start of the last active line: VBlank interrupt
            g_irq_state |= 0x02;
            g_phase = 0;
            g_next_phase = g_frame_start + FRAME_CYCLES;
            break;
        }
    }
    // MIDI interrupts while the sound board wants commands (during VBlank)
    if (g_midi_pulses > 0 && (g_midi_ctrl & 0x20) && (g_irq_enable & 0x40) && !(g_irq_state & 0x40) &&
        c.cycles >= g_midi_next) {
        g_irq_state |= 0x40;
        g_midi_pulses--;
        g_midi_next = c.cycles + 1000;
    }
    if (!g_in_irq && (c.msr & MSR_EE)) {
        if (g_dec_armed && c.cycles >= g_dec_base + uint64_t(g_dec_value) * 4) {
            g_dec_armed = false;
            g_dec_irqs++;
            exception(c, 0x900);
        }
        uint8_t pend = g_irq_state & g_irq_enable;
        if (pend || g_dma_irq) {
            for (int i = 0; i < 8; i++) if (pend & (1 << i)) g_irq_count[i]++;
            exception(c, 0x500);
        }
    }
    schedule(c);
}

uint32_t rt_mfspr(Ctx& c, uint32_t spr) {
    switch (spr) {
    case 22: return g_dec_value - uint32_t((c.cycles - g_dec_base) / 4);
    case 268: return uint32_t((c.cycles - g_tb_base) / 4);
    case 269: return uint32_t(((c.cycles - g_tb_base) / 4) >> 32);
    default: return c.spr[spr];
    }
}

void rt_mtspr(Ctx& c, uint32_t spr, uint32_t v) {
    if (spr == 22) {
        g_dec_value = v;
        g_dec_base = c.cycles;
        g_dec_armed = !(v & 0x80000000u);
        schedule(c);
        return;
    }
    if (spr == 284) {   // TBL: only used to reset it to 0
        g_tb_base = c.cycles - uint64_t(v) * 4;
        return;
    }
    c.spr[spr] = v;
}

void rt_mtmsr(Ctx& c, uint32_t v) {
    c.msr = v;
    schedule(c);
}

void rt_lswi(Ctx& c, uint32_t ea, int rd, int nb) {
    int r = rd - 1, i = 0;
    while (nb > 0) {
        if (i == 0) { r = (r + 1) & 31; c.r[r] = 0; }
        c.r[r] |= uint32_t(rd8(ea++)) << (24 - i);
        i = (i + 8) & 31;
        nb--;
    }
}

void rt_stswi(Ctx& c, uint32_t ea, int rs, int nb) {
    int r = rs - 1, i = 0;
    while (nb > 0) {
        if (i == 0) r = (r + 1) & 31;
        wr8(ea++, uint8_t(c.r[r] >> (24 - i)));
        i = (i + 8) & 31;
        nb--;
    }
}
