// ROM loading, debug snapshots (F9, --replay) and captures (moved from app.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(__APPLE__)
#include <dwmapi.h>     // DwmGetCompositionTimingInfo
#include <mmsystem.h>   // timeBeginPeriod
#include <windowsx.h>   // GET_X_LPARAM
#endif

#include "gamepad.h"
#include "inputs.h"
#include "options.h"
#include "ppc_rt.h"
#include "rom_zip.h"
#include "runtime.h"
#include "stats.h"
#include "../audio/sound_board.h"
#include "../ui/achievement_toast.h"
#include "../ui/frontend.h"
#include "../ui/menu_music.h"
#include "../video/pack2d.h"
#include "../video/r3d_renderer.h"
#include "../video/r3d_scene.h"
#include "../video/r3d_screen.h"
#include "../video/r3d_state.h"
#include "../video/sky_pack.h"
#include "../video/text_hd.h"
#include "../video/texture_pack.h"
#include "../video/tilegen.h"
#include "../video/ui_overlay.h"
#include "../video/vk_presenter.h"


#include "app_internal.h"

// Debugging aid for --shot: every triangle with its screen bounding box
// (native pixels), plus the 2D layers' alpha along two columns
void dump_scene(const R3DScene& scene, const uint32_t* back, const uint32_t* front, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (size_t vi = 0; vi < scene.viewports.size(); vi++) {
        const R3DViewport& v = scene.viewports[vi];
        fprintf(f, "viewport %zu prio %d rect %.0f %.0f %.0f %.0f opaque %u trans %u\n", vi, v.priority, v.x, v.y,
                v.w, v.h, v.opaque_count / 3, v.trans_count / 3);
        fprintf(f, "  fog density %.5f start %.3f colour %.3f %.3f %.3f ambient %.3f scroll %.3f far %.1f world %d sun %.2f %.2f %.2f x %.2f\n",
                v.fog_density, v.fog_start, v.fog_color[0], v.fog_color[1], v.fog_color[2], v.fog_ambient,
                v.scroll_fog, v.scenery_far, int(v.world), v.sun[0], v.sun[1],
                v.sun[2], v.sun_intensity);
        fprintf(f, "  node %08X words 08-13", v.node);
        for (uint32_t w : v.frustum) {
            float x;
            memcpy(&x, &w, 4);
            fprintf(f, " %08X(%g)", w, x);
        }
        fprintf(f, "\n");
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, zmin = 1e9f;
                const R3DVertex* tri = &scene.vertices[first + t];
                for (int k = 0; k < 3; k++) {
                    const float* p = tri[k].pos;
                    float cx = v.proj[0] * p[0] + v.proj[4] * p[1] + v.proj[8] * p[2] + v.proj[12];
                    float cy = v.proj[1] * p[0] + v.proj[5] * p[1] + v.proj[9] * p[2] + v.proj[13];
                    float cw = v.proj[3] * p[0] + v.proj[7] * p[1] + v.proj[11] * p[2] + v.proj[15];
                    if (cw <= 1e-6f) cw = 1e-6f;
                    float sx = v.x + (cx / cw + 1) / 2 * v.w, sy = v.y + (cy / cw + 1) / 2 * v.h;
                    x0 = std::min(x0, sx), x1 = std::max(x1, sx), y0 = std::min(y0, sy), y1 = std::max(y1, sy);
                    zmin = std::min(zmin, -p[2]);
                }
                fprintf(f, "  tri v%zu %s bbox %.1f %.1f %.1f %.1f z %.3f color %08x flags %x tex %08x\n", vi,
                        pass ? "T" : "O", x0, y0, x1, y1, zmin, tri[0].color, tri[0].flags, tri[0].tex);
            }
        }
    }
    for (int y = 0; y < SCREEN_H; y += 4)
        fprintf(f, "2d row %d: back[0] %08x back[248] %08x front[0] %08x front[248] %08x\n", y,
                back[y * BACK_W + BACK_MARGIN], back[y * BACK_W + BACK_MARGIN + 248], front[y * SCREEN_W],
                front[y * SCREEN_W + 248]);
    fclose(f);
}


// Debug snapshot (F9): the Real3D memory of the frame on screen, texture RAM
// and 2D layer memory in one file, to look at a scene again with --replay
static const uint32_t SNAPSHOT_MAGIC = 0x3153574C;   // "LWS1"

void save_snapshot(const char* path, const R3DFrameState& state, const std::vector<uint8_t>& vram) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t count = uint32_t(state.pages.size());
    fwrite(&SNAPSHOT_MAGIC, 4, 1, f);
    fwrite(&state.frame, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (auto& [key, page] : state.pages) {
        fwrite(&key, 4, 1, f);
        fwrite(page->data(), 1, page->size(), f);
    }
    std::vector<uint16_t> texels;
    g_texture_ram.copy_all(texels);
    fwrite(texels.data(), 2, texels.size(), f);
    fwrite(vram.data(), 1, vram.size(), f);
    fclose(f);
}

bool load_snapshot(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    auto state = std::make_shared<R3DFrameState>();
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == SNAPSHOT_MAGIC && fread(&state->frame, 8, 1, f) == 1 &&
              fread(&count, 4, 1, f) == 1;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t key;
        auto page = std::make_shared<R3DPage>();
        ok = fread(&key, 4, 1, f) == 1 && fread(page->data(), 1, page->size(), f) == page->size();
        state->pages[key] = page;
    }
    std::vector<uint16_t> texels(2048 * 2048);
    ok = ok && fread(texels.data(), 2, texels.size(), f) == texels.size();
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    ok = ok && fread(vram.data(), 1, vram.size(), f) == vram.size();
    fclose(f);
    if (!ok) return false;
    g_texture_ram.load_all(texels);
    {
        std::lock_guard<std::mutex> lock(g_video_mutex);
        g_video_vram = vram;
        g_video_frame = state->frame;
    }
    std::lock_guard<std::mutex> lock(g_r3d_mutex);
    g_r3d_latest = std::move(state);
    return true;
}

void save_bmp(const char* path, const uint32_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(w) * h * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px, 4, size_t(w) * h, f);
    fclose(f);
}

// The folder of SegaRally2.exe, with its trailing backslash
static std::string exe_folder() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path, n);
    return s.substr(0, s.find_last_of("\\/") + 1);
}
static bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::string full_path(const std::string& path) {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
    return n && n < MAX_PATH ? std::string(full, n) : path;
}


// Loads the ROM from the first of: --rom or SR2_ROM_ZIP (srally2.zip read
// directly), --data, data\ in the current folder or next to the exe (what
// build.bat extracts), srally2.zip next to the exe or in the current folder.
// If none is there, says where it looked and how to give the path
bool load_game_rom(std::vector<uint8_t>& sound_program, std::vector<uint8_t>& sound_samples,
                          std::vector<uint8_t>& dsb_program, std::vector<uint8_t>& dsb_mpeg) {
    std::string zip = g_opt.rom_zip;
    if (zip.empty() && getenv("SR2_ROM_ZIP")) zip = getenv("SR2_ROM_ZIP");
    std::vector<std::string> looked;   // full paths, for the error
    auto missing = [&](const std::string& path) {
        std::string f = full_path(path);
        if (std::find(looked.begin(), looked.end(), f) == looked.end()) looked.push_back(f);
    };
    if (zip.empty()) {
        std::vector<std::string> dirs = {g_opt.data_dir};
        if (g_opt.data_dir == "data") dirs.push_back(exe_folder() + "data");   // --data not given
        for (const std::string& d : dirs) {
            if (file_exists(d + "/crom.bin")) {
                g_data_dir = d;
                if (!load_rom("crom.bin", g_crom, true)) return false;
                load_rom("banked_crom.bin", g_banked, false);
                load_rom("vrom.bin", g_vrom, false);
                load_rom("drive_program.bin", g_drive_rom, false);
                load_rom("sound_program.bin", sound_program, false);
                load_rom("sound_samples.bin", sound_samples, false);
                load_rom("dsb_program.bin", dsb_program, false);
                load_rom("dsb_mpeg.bin", dsb_mpeg, false);
                if (g_drive_rom.empty()) {
                    // An older romtool.py extracted the rest of a set without
                    // epr-21119: say so here, not with the game's own
                    // FAILED FEEDBACK LEVER INITIALIZE
                    std::string msg =
                        "The ROM in " + full_path(d) + " has no drive_program.bin (epr-21119.ic8, the force\n"
                        "feedback board of the joystick). Without it the game stops at FAILED FEEDBACK LEVER\n"
                        "INITIALIZE.\n\n"
                        "Your srally2.zip is probably an older set that lacks that chip. Get a current,\n"
                        "complete srally2.zip (Rev. A), put it next to build.bat and run build.bat again.";
                    fprintf(stderr, "%s\n", msg.c_str());
                    if (!g_opt.headless) MessageBoxA(nullptr, msg.c_str(), "Sega Rally 2 Native", MB_OK | MB_ICONERROR);
                    return false;
                }
                printf("ROM: %s\n", d.c_str());
                return true;
            }
            missing(d + "\\crom.bin");
        }
        if (g_opt.data_dir == "data")
            for (const std::string& z : {exe_folder() + "srally2.zip", std::string("srally2.zip")}) {
                if (file_exists(z)) {
                    zip = z;
                    break;
                }
                missing(z);
            }
    }
    std::string error;
    if (!zip.empty()) {
        RomSet rom;
        if (load_rom_zip(zip, rom, error)) {
            g_crom = std::move(rom.crom);
            g_drive_rom = std::move(rom.drive_program);
            g_banked = std::move(rom.banked_crom);
            g_vrom = std::move(rom.vrom);
            sound_program = std::move(rom.sound_program);
            sound_samples = std::move(rom.sound_samples);
            dsb_program = std::move(rom.dsb_program);
            dsb_mpeg = std::move(rom.dsb_mpeg);
            printf("ROM: %s\n", zip.c_str());
            return true;
        }
        error = "Cannot load the ROM: " + error + ".\n";
    } else {
        error = "Cannot find the Star Wars Trilogy ROM (srally2.zip). Looked in:\n";
        for (const std::string& l : looked) error += "  " + l + "\n";
    }
    error += "\nPut srally2.zip next to SegaRally2.exe, or give its path with\n"
             "  SegaRally2.exe --rom D:\\roms\\srally2.zip\n"
             "or in the SR2_ROM_ZIP environment variable.";
    fprintf(stderr, "%s\n", error.c_str());
    if (!g_opt.headless) MessageBoxA(nullptr, error.c_str(), "Sega Rally 2 Native", MB_OK | MB_ICONERROR);
    return false;
}
