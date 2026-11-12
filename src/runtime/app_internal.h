// What app.cpp's pieces share: the settings, the controls' state and the
// helpers moved out of app.cpp (app_input.cpp: keyboard, gamepads and
// wheels; app_files.cpp: ROM loading, snapshots and captures)
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <cstdint>
#include <vector>

#include "gamepad.h"
#include "options.h"

struct R3DFrameState;
struct R3DScene;

extern Options g_opt;   // command line and menu settings (options.h)

// app_input.cpp
extern bool g_menu_open;    // a menu has the controls
extern Gamepads g_pads;
extern int g_pad_rumble;    // the pads' vibration: 0 off, 1 low, 2 medium, 3 high
void poll_keyboard(HWND hwnd);
bool poll_gamepads(HWND hwnd);   // true: a pad asked for the pause menu

// app_files.cpp
bool load_game_rom(std::vector<uint8_t>& sound_program, std::vector<uint8_t>& sound_samples,
                   std::vector<uint8_t>& dsb_program, std::vector<uint8_t>& dsb_mpeg);
void save_snapshot(const char* path, const R3DFrameState& state, const std::vector<uint8_t>& vram);
bool load_snapshot(const char* path);
void save_bmp(const char* path, const uint32_t* px, int w, int h);
void dump_scene(const R3DScene& scene, const uint32_t* back, const uint32_t* front, const char* path);
