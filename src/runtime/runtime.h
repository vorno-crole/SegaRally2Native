// What the window side (app.cpp) uses of the emulator in runtime.cpp: the
// game thread and the state the two threads share. Everything else in
// runtime.cpp is private to the emulation.
#pragma once
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "eeprom.h"
#include "inputs.h"
#include "../audio/audio_out.h"
#include "../audio/sound_board.h"
#include "../video/r3d_state.h"

// ROMs, loaded by main() from g_data_dir
extern std::vector<uint8_t> g_crom, g_banked, g_vrom;
extern std::vector<uint8_t> g_drive_rom;   // force feedback board program (epr-21119), optional

extern std::string g_data_dir;
bool load_rom(const std::string& name, std::vector<uint8_t>& out, bool required);

// Run limits and tracing (--frames, --diff, --trace)
extern int g_frame_limit;          // with --headless, 600 by default
extern bool g_collect_stats;       // keep access statistics and the full texture stream for report()
extern std::map<uint32_t, uint64_t> g_trace_hits;   // function -> hit count
extern uint32_t g_diff_fn;
extern uint64_t g_diff_frame;

// Devices shared with the window thread
extern Eeprom93C46 g_eeprom;
extern SoundBoard g_sound;
extern AudioOut g_audio;
extern AutoInput g_auto;

// The game thread: runs the recompiled program on a 512 MB native stack
DWORD WINAPI game_thread(LPVOID);
double qpc_seconds();              // seconds from a fixed origin (QueryPerformanceCounter)
void frame_log(const char* fmt, ...);   // LW_FRAMELOG (app.cpp); does nothing without it
void console_prompt();             // before the process ends: the prompt of the terminal it was started from (app.cpp)
extern LARGE_INTEGER g_qpf, g_start_time;
extern bool g_realtime;            // pace the game to real time (off with LW_FAST)
extern std::atomic<bool> g_boot_skip;   // the power-on screens run through with nothing on screen
extern std::atomic<int> g_arcade_entry;   // PLAY: the game runs through to the mission select with nothing on screen
// DEBUG (menu): from SELECT GAME on to a practice race, unseen:
// course | car << 4 | transmission << 8 (-1: none; runtime.cpp, debug_jump_frame)
extern std::atomic<int> g_debug_jump;
// DEBUG (menu): TIME INFINITE, the race's time left held (runtime.cpp, debug_time_frame)
extern std::atomic<bool> g_debug_freeze_time;
// The menu stands in for the attract: when set (app.cpp, with the menu on),
// a game that ends goes on to the mission select unseen instead of into the
// attract, and g_game_over_menu asks app.cpp to open the main menu
extern std::atomic<bool> g_skip_attract, g_game_over_menu;
// The game is in its attract (logos, title, demo, rankings, INSERT COIN):
// app.cpp shows the author's credit over it
extern std::atomic<bool> g_in_attract;
// Widescreen: how much wider than 4:3 the 3D view is (set by the window,
// 1 = 4:3) and the factor the game's horizontal field of view was last
// widened by (the scene builder takes it back out of the projection)
extern std::atomic<float> g_wide_view, g_wide_applied;
extern std::atomic<bool> g_hold;        // menus: the game stands still while set
extern std::atomic<bool> g_quit, g_parked;   // shutdown handshake: ask, then wait for the park
extern std::atomic<uint64_t> g_game_busy_us, g_game_frames;   // CPU time of the game thread, read once a second

// What the game drew: the last closed 3D frame and the 2D layer memory of the
// frame on screen, each behind its mutex
extern std::mutex g_r3d_mutex;
extern std::shared_ptr<const R3DFrameState> g_r3d_latest;
extern std::condition_variable g_r3d_cv;   // notified (after the mutex is released) when g_r3d_latest changes
extern R3DTextureRam g_texture_ram;
extern std::mutex g_video_mutex;
extern std::vector<uint8_t> g_video_vram;
extern uint64_t g_video_frame;
