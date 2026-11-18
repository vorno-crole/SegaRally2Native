// A debug snapshot of the game read on its own, without the game: the file
// F9 writes (out/snap/snap_<frame>.bin, save_snapshot in
// src/runtime/app_files.cpp) holds the Real3D memory of the frame on screen,
// the whole texture RAM and the 2D layer memory.
//
//   u32  magic "LWS1"
//   u64  frame number
//   u32  number of pages
//   per page: u32 key (PowerPC address >> 16), 65536 bytes
//   2048 x 2048 u16 texels (texture RAM)
//   0x190000 bytes (tile generator memory)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "r3d_state.h"

struct Capture {
    std::shared_ptr<R3DFrameState> state;   // culling and polygon RAM of the frame
    std::vector<uint16_t> texels;           // texture RAM, 2048 x 2048
    std::vector<uint8_t> vram;              // 2D layer memory (TILEGEN_SIZE bytes)
    std::string path;
};

// false with the reason in `error` (the file cannot be opened, is not a
// snapshot or stops short)
bool load_capture(const std::string& path, Capture& out, std::string& error);
// The same format written back (the tests)
bool save_capture(const std::string& path, const Capture& c);
