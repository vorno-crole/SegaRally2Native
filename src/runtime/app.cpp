// The window side: command line and settings, the Vulkan window, the
// presentation loop (2D composition, scene building, rendering, captures),
// and the menus. The emulator runs on its
// own thread in runtime.cpp; runtime.h is what this side uses of it.
// Also of this side: app_input.cpp (keyboard, mouse, gamepads, aiming),
// app_files.cpp (ROM loading, snapshots, captures) and settings_bridge.cpp
// (save/settings.ini <-> the command-line options), sharing app_internal.h
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
#include "wheel.h"
#include "../audio/sound_board.h"
#include "../ui/achievement_toast.h"
#include "../ui/frontend.h"
#include "../ui/menu_music.h"
#include "../video/pack2d.h"
#include "../video/r3d_dust.h"
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
#include "settings_bridge.h"

Options g_opt;   // command line and menu settings (options.h)

// When the image being recorded will be on screen: with vsync the images go
// out once per refresh, evenly, while this thread wakes up for them at uneven
// moments (it waits for the GPU and the display, then builds the scene). The
// moment is the next vblank of the monitor, which Windows reports (DWM); if
// it does not, a clock that advances one refresh per image and follows the
// earliest wake-ups (never earlier than a vblank). Either way it moves on by
// whole refreshes, and starts again after a stall (a window drag, a slow frame).
struct DisplayClock {
    double at = 0, last = 0, period = 1.0 / 60;
    double tick() {
        double now = qpc_seconds();
        DWM_TIMING_INFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.qpcRefreshPeriod > 0) {
            double f = double(g_qpf.QuadPart), vblank = double(info.qpcVBlank) / f;
            period = double(info.qpcRefreshPeriod) / f;
            double next = vblank + std::ceil((now - vblank) / period) * period;
            // One refresh per image unless one was really skipped
            at = at > 0 && next < at + 0.5 * period && next > at - 2 * period ? at + period : next;
            last = now;
            return at;
        }
        if (last > 0 && now - last < 0.1) period += 0.002 * (std::clamp(now - last, 1.0 / 500, 0.05) - period);
        last = now;
        if (at > 0 && std::abs(now - (at + period)) < 2 * period) {
            at += period;
            at += now < at ? now - at : 0.01 * (now - at);
        } else {
            at = now;
        }
        return at;
    }
};

// Debugging aid: LW_PACING=1 measures how evenly the interpolated motion
// runs. Every 5 s it prints, over the presents, the game time shown per
// present divided by the time between their moments on screen (1.00 =
// perfectly even; "dispersion" is its standard deviation) and the presents
// where the picture stood still or went back, and the same for the previous
// method (frames dated when the game finished them and placed at the moment
// the image is recorded, no delay for late frames)
static const bool g_pacing_log = getenv("LW_PACING") != nullptr;
static std::atomic<float> g_build_ms{0}, g_build_worst_ms{0};   // building the 3D scenes (out/slow.log)
static int g_tex_rows_second = 0;   // texture RAM rows uploaded this second (out/slow.log)

// Debugging aid: LW_FRAMELOG=path writes one line per present to that file
// (when the image was prepared, which game frames it shows, where the time
// went) and, with VK_KHR_present_wait, one per present when it reached the
// screen; the game thread adds one per game frame (runtime.cpp). Read with
// tools/frame_log.py
static FILE* g_frame_log = getenv("LW_FRAMELOG") ? fopen(getenv("LW_FRAMELOG"), "w") : nullptr;
static std::mutex g_frame_log_mutex;

void frame_log(const char* fmt, ...) {
    if (!g_frame_log) return;
    va_list args;
    va_start(args, fmt);
    std::lock_guard<std::mutex> lock(g_frame_log_mutex);
    vfprintf(g_frame_log, fmt, args);
    va_end(args);
}

static void pacing_sample(double now, double display, double shown, const R3DFrameState& built) {
    struct Method {
        double last_shown = 0, sum = 0, sum2 = 0;
        int n = 0, still = 0, back = 0;
        void add(double dt, double value) {
            if (last_shown != 0 && dt > 0) {
                double rate = (value - last_shown) / dt;
                if (std::abs(rate) < 4) {   // pauses and cuts are left out
                    sum += rate;
                    sum2 += rate * rate;
                    n++;
                    still += rate < 0.05;
                    back += rate < -0.05;
                }
            }
            last_shown = value;
        }
        void print(const char* name) {
            double mean = n ? sum / n : 0, dev = n ? std::sqrt(std::max(sum2 / n - mean * mean, 0.0)) : 0;
            printf("[pacing] %s: speed %.3f  dispersion %.3f  still %d  back %d  (%d presents)\n", name, mean, dev,
                   still, back, n);
            *this = Method{};
        }
    };
    static Method current, previous;
    static double last_display = 0, last_print = now, prev_flushed = 0, prev_time = 0;
    static double prev_period = 1 / 57.524, game_period = 1 / 57.524;
    static uint64_t prev_frame = 0;
    if (built.frame != prev_frame) {   // the previous method's frame period, from the finishing times
        if (prev_frame) {
            prev_period = std::clamp(built.flushed - prev_flushed, 1.0 / 240, 0.1);
            game_period = std::clamp(built.time - prev_time, 1.0 / 240, 0.1);
        }
        prev_frame = built.frame;
        prev_flushed = built.flushed;
        prev_time = built.time;
    }
    // Both shown as game time: the scenes themselves belong to their vblanks
    double t_old = std::clamp((now - built.flushed) / prev_period, 0.0, 1.0);
    double dt = display - last_display;
    current.add(dt, shown);
    previous.add(dt, built.time - game_period * (1 - t_old));
    last_display = display;
    if (now - last_print >= 5) {
        last_print = now;
        current.print("vblank  ");
        previous.print("previous");
        fflush(stdout);
    }
}

// save/settings.ini as it was read and the settings at launch (command-line
// options over it). A command-line option lasts only for that run: what it
// set is written back as the file had it, unless changed in the menu since
// (a test run with --volume 0 once silenced the game for good)
static FrontendSettings g_settings_file, g_settings_launch;

static std::vector<std::pair<std::string, std::string>> settings_lines(const FrontendSettings& s) {
    std::vector<std::pair<std::string, std::string>> lines;
    std::string text = s.text();
    size_t p = 0;
    while (p < text.size()) {
        size_t end = text.find('\n', p);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(p, end - p);
        size_t eq = line.find(" = ");
        lines.emplace_back(eq == std::string::npos ? line : line.substr(0, eq), line);
        p = end + 1;
    }
    return lines;
}

static void save_settings(const FrontendSettings& s) {
    auto now = settings_lines(s), file = settings_lines(g_settings_file), launch = settings_lines(g_settings_launch);
    auto find = [](const auto& lines, const std::string& key) -> const std::string* {
        for (const auto& l : lines) if (l.first == key) return &l.second;
        return nullptr;
    };
    std::string out;
    for (const auto& l : now) {
        const std::string *f = find(file, l.first), *at_launch = find(launch, l.first);
        bool from_command_line = f && at_launch && *f != *at_launch;
        out += (from_command_line && l.second == *at_launch ? *f : l.second) + "\n";
    }
    if (FILE* fp = fopen("save/settings.ini", "w")) {
        fputs(out.c_str(), fp);
        fclose(fp);
    }
}

// The first run (no anti-aliasing in save/settings.ini yet) picks it for the
// GPU and the screen instead of MSAA 4x for everyone: with all the ray tracing
// on, MSAA 4x does not fit in a 120 Hz refresh at 4K even on an RTX 5090 (the
// 3D pass alone took 8 to 10 ms there, twice what it takes with DLAA; GitHub
// issue 14), and that was the game's first impression at that size. On an
// NVIDIA card DLSS is asked for: DLAA, or DLSS QUALITY on a screen larger than
// 1440p (the game starts in a window, but full screen is the screen's size).
// A card or a build without DLSS falls back to MSAA as it always did.
static bool nvidia_card() {
    DISPLAY_DEVICEA d{};
    d.cb = sizeof(d);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &d, 0); i++)
        if (strstr(d.DeviceString, "NVIDIA")) return true;
    return false;
}

// The height in pixels of the screen the window is on (0: not known)
// (debugging aid: SR2_SCREEN_HEIGHT=2160 stands for a screen of that height)
static int screen_height(HWND hwnd) {
    if (const char* e = getenv("SR2_SCREEN_HEIGHT")) return atoi(e);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;   // (real pixels, whatever the scaling)
    return int(std::min(mode.dmPelsWidth, mode.dmPelsHeight));   // (the shorter side: a screen turned upright too)
}

static std::atomic<bool> g_terminal_prompt{false};   // started from a terminal, nothing redirected: its prompt to bring back

// The game is a windowed program (no console window of its own). Started from
// a terminal, its log goes to that terminal; redirected to a file or a pipe
// (the tests), it goes there as it always did
static void attach_parent_console() {
    auto valid = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    };
    bool out_ok = valid(STD_OUTPUT_HANDLE), err_ok = valid(STD_ERROR_HANDLE);
    if (out_ok && err_ok) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // double click: no console at all
    if (!out_ok) freopen("CONOUT$", "w", stdout);
    if (!err_ok) freopen("CONOUT$", "w", stderr);
    g_terminal_prompt = !out_ok && !err_ok;   // (something redirected: a script's run, left as it is)
    atexit(console_prompt);                   // the ways out of main() that return
}

// A terminal does not wait for a windowed program: its prompt came back as the
// game started and the log was written after it, so when the game ended the
// terminal looked busy until Enter was pressed. The game presses it: an Enter
// in the terminal's input, and the terminal draws its prompt again
void console_prompt() {
    if (!g_terminal_prompt.exchange(false)) return;
    fflush(stdout);
    HANDLE in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    if (in == INVALID_HANDLE_VALUE) return;
    INPUT_RECORD key[2]{};   // down and up
    for (int i = 0; i < 2; i++) {
        key[i].EventType = KEY_EVENT;
        KEY_EVENT_RECORD& k = key[i].Event.KeyEvent;
        k.bKeyDown = i == 0;
        k.wRepeatCount = 1;
        k.wVirtualKeyCode = VK_RETURN;
        k.wVirtualScanCode = WORD(MapVirtualKeyA(VK_RETURN, MAPVK_VK_TO_VSC));
        k.uChar.AsciiChar = '\r';
    }
    DWORD written = 0;
    WriteConsoleInputA(in, key, 2, &written);
    CloseHandle(in);
}

int main(int argc, char** argv) {
    attach_parent_console();
    setvbuf(stdout, nullptr, _IONBF, 0);   // keep logs even if the process dies
    // The menus (and the settings they save) are left out of test runs, so
    // those always start from the same defaults
    g_opt.menu = !options_disable_menu(argc, argv);
    FrontendSettings ini;
    bool aa_auto = false;   // the first run: the anti-aliasing chosen for the GPU and the screen (nvidia_card above)
    if (g_opt.menu) {   // save/settings.ini, written by the options menu; command-line options win
        ini.load("save/settings.ini");
        aa_auto = !ini.aa_saved && nvidia_card();
        if (aa_auto) ini.aa = 1;   // DLAA; DLSS QUALITY or MSAA once the screen and the GPU are known
        options_from_settings(g_opt, ini);
        g_pad_rumble = ini.vibration;
    }
    int exit_code = 0;
    if (!parse_options(g_opt, argc, argv, nullptr, exit_code)) return exit_code;
    for (int i = 1; i < argc; i++)   // chosen on the command line instead
        if (strcmp(argv[i], "--aa") == 0 || strcmp(argv[i], "--msaa") == 0) aa_auto = false;
    // What the options set outside the struct: emulation globals and the controls
    g_data_dir = g_opt.data_dir;
    g_frame_limit = g_opt.frame_limit;
    for (const std::string& script : g_opt.auto_scripts) g_auto.parse(script);
    // Debugging aid: SR2_AUTO, as --auto but with the menu and the saved settings
    // (with SR2_DEBUG_JUMP: "gas+@1" drives a practice race of any course by itself)
    if (const char* script = getenv("SR2_AUTO")) g_auto.parse(script);
    if (g_opt.fixed_aim) g_inputs.steer = g_opt.aim_x;   // --aim: the wheel's raw ADC value at the start
    if (!g_opt.diff.empty()) {   // FN@FRAME
        const std::string& d = g_opt.diff;
        g_diff_fn = strtoul(d.c_str(), nullptr, 16);
        size_t at = d.find('@');
        if (at != std::string::npos) g_diff_frame = strtoull(d.c_str() + at + 1, nullptr, 10);
        g_trace_hits[g_diff_fn] = 0;
        g_trace_bits[(g_diff_fn >> 5) & 511] |= uint8_t(1 << ((g_diff_fn >> 2) & 7));
    }
    for (const std::string& list : g_opt.traces) {   // comma-separated: --trace 10F650,10D564
        size_t p = 0;
        while (p < list.size()) {
            uint32_t fn = strtoul(list.c_str() + p, nullptr, 16);
            g_trace_hits[fn] = 0;
            g_trace_bits[(fn >> 5) & 511] |= uint8_t(1 << ((fn >> 2) & 7));
            p = list.find(',', p);
            if (p == std::string::npos) break;
            p++;
        }
    }
    if (g_opt.headless && g_frame_limit == INT32_MAX) {
        g_frame_limit = 600;
    }
    g_collect_stats = g_frame_limit != INT32_MAX;   // report() prints them when the run ends
    printf("Sega Rally 2 Native - part 4 (%s)\n", g_opt.headless ? "no window" : "Vulkan window, 3D + 2D");
    {
        std::vector<uint8_t> program, samples, dsb_program, dsb_mpeg;
        if (!load_game_rom(program, samples, dsb_program, dsb_mpeg)) return 1;
        // The menus' music: a tune of the music board (stream kMenuTrack,
        // channel kMenuChannel), decoded once from the ROM into data/ (not
        // distributed). The file name carries the choice, so a new one is decoded
        constexpr int kMenuTrack = 0, kMenuChannel = 0;
        char menu_wav[64];
        snprintf(menu_wav, sizeof menu_wav, "data/menu_title_%02d%c.wav", kMenuTrack, kMenuChannel ? 'R' : 'L');
        g_menu_music_path = menu_wav;
        if (g_opt.menu && !g_opt.headless && GetFileAttributesA(menu_wav) == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryA("data", nullptr), Dsb2::extract_track(dsb_program, dsb_mpeg, kMenuTrack, kMenuChannel, menu_wav);
        if (!getenv("LW_NOSOUND")) {
            g_sound.init(std::move(program), std::move(samples), g_opt.headless ? nullptr : &g_audio);
            // The music board (DSB2): its own 68000 and the MPEG streams
            if (!getenv("SR2_NOMUSIC") && g_sound.init_dsb(std::move(dsb_program), std::move(dsb_mpeg)))
                printf("[sound] music board ready\n");
            else
                printf("[sound] no music board (dsb_program / dsb_mpeg missing)\n");
        }
        if (!g_opt.wav_path.empty()) g_sound.dump_wav(g_opt.wav_path.c_str());
        g_sound.set_volume(g_opt.volume);
        g_sound.set_music_gain(g_opt.music);
        Scsp::set_cubic(g_opt.cubic_samples);
        g_sound.set_rear_boost(g_opt.rear_boost);
        g_audio.set_speakers(AudioStream::Speakers(g_opt.speakers));
    }
    g_ram = static_cast<uint8_t*>(calloc(RAM_SIZE + 8, 1));
    CreateDirectoryA("save", nullptr);
    CreateDirectoryA("out", nullptr);   // dumps, screenshots, out/missing.txt
    g_eeprom.load("save/eeprom.bin", false);
    // Testing aid: SR2_STATS_TEST=1, records and achievements counted in scripted
    // runs too (--auto, --headless, SR2_POKE...); give them a save folder of their own
    static const bool stats_test = getenv("SR2_STATS_TEST") != nullptr;
    if (stats_test && g_opt.replay.empty()) stats_init();

    // The game code uses the original CPU's stack as memory and nests
    // many calls: run it on a thread with 512 MB of native stack.
    if (g_opt.headless) {
        HANDLE t = CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        WaitForSingleObject(t, INFINITE);
        return 0;
    }

    // Window: the game runs on its own thread at real speed; this thread draws
    // the latest image at the monitor's refresh rate (vsync)
    VkPresenter presenter;
    if (!presenter.init("Sega Rally 2 Native by snicolast", int(SCREEN_H * 2 * std::max(g_opt.max_aspect, 4.0f / 3.0f)), SCREEN_H * 2,
                        g_opt.dlaa)) {
        fprintf(stderr, "Could not start Vulkan; try --headless\n");
        MessageBoxA(nullptr, "Could not start Vulkan. Update the graphics driver: the game needs Vulkan 1.3.",
                    "Sega Rally 2 Native", MB_OK | MB_ICONERROR);
        return 1;
    }
    printf("GPU: %s\n", presenter.device_name().c_str());
    if (aa_auto) g_opt.dlss = screen_height(presenter.hwnd()) > 1440;
    g_pads.init();
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_start_time);
    g_realtime = !getenv("LW_FAST");   // debugging aid: a window at full speed
    // --replay on a folder (recorded with LW_FILM): its snapshots one after
    // another, each captured to out/film_out, so effects that depend on the
    // camera motion can be compared frame by frame
    std::vector<std::string> film;
    size_t film_pos = 0;
    DWORD replay_attr = g_opt.replay.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesA(g_opt.replay.c_str());
    if (replay_attr != INVALID_FILE_ATTRIBUTES && (replay_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((g_opt.replay + "/*.bin").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do film.push_back(g_opt.replay + "/" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(film.begin(), film.end());
        if (film.empty()) {
            fprintf(stderr, "No snapshots in %s\n", g_opt.replay.c_str());
            return 1;
        }
        g_opt.replay = film[0];
        CreateDirectoryA("out/film_out", nullptr);
    }
    if (!g_opt.replay.empty()) {   // --replay: a frozen debug snapshot, the game does not run
        if (!load_snapshot(g_opt.replay.c_str())) {
            fprintf(stderr, "Could not read the snapshot %s\n", g_opt.replay.c_str());
            return 1;
        }
        printf("Snapshot %s (frame %llu)\n", g_opt.replay.c_str(), (unsigned long long)g_video_frame);
    } else {
        // Records and achievements: real games only, never the scripted or cheated test runs
        if (g_opt.menu && g_auto.events.empty() && !stats_test) stats_init();
        g_audio.start();
        g_hold = g_opt.menu;   // the main menu comes first
        g_skip_attract = g_opt.menu && g_auto.events.empty();   // the menu instead of the attract (runtime.cpp)
        g_boot_skip = g_fast_idle = !g_opt.show_boot;
        g_sound.set_dsp_enabled(!g_boot_skip);
        g_sound.set_muted(g_boot_skip);
        CreateThread(nullptr, 512u << 20, game_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }

    R3DRenderer renderer;
    if (!renderer.init(presenter, g_opt.supersample, g_opt.msaa, g_opt.aniso, g_opt.dlaa, g_opt.dlss)) {
        fprintf(stderr, "Could not start the 3D renderer\n");
        MessageBoxA(nullptr,
                    "Could not start the 3D renderer: the graphics card or its driver lacks something the game needs. "
                    "Run SegaRally2.exe from a command prompt to see the details.",
                    "Sega Rally 2 Native", MB_OK | MB_ICONERROR);
        return 1;
    }
    renderer.set_max_aspect(g_opt.max_aspect);
    renderer.set_smooth_2d(!g_opt.pixel_2d);
    renderer.set_hud_edges(!g_opt.hud_centre);
    renderer.set_distance_haze(g_opt.haze);
    renderer.set_bloom(g_opt.bloom);
    renderer.set_picture(g_opt.saturation, g_opt.gamma);
    renderer.set_ambient_occlusion(g_opt.ao);
    renderer.set_sun(g_opt.sun);
    renderer.set_shadows(g_opt.shadows ? 1.0f : 0.0f);
    renderer.set_ray_tracing(g_opt.ray_tracing);
    renderer.set_soft_particles(g_opt.soft);
    renderer.set_hdr(g_opt.hdr);
    renderer.set_reflections(g_opt.reflections);
    renderer.set_global_illumination(g_opt.gi);
    r3d_set_sr2_dust(g_opt.dust);
    renderer.set_sharpen(g_opt.sharpen ? 0.5f : 0.0f);
    if (g_opt.haze > 0) printf("[video] distance haze on (strength %.2f)\n", g_opt.haze);
    if (aa_auto) {   // what the first run got: the menu shows it, and saves it with the first setting changed
        if (!renderer.dlaa_active()) g_opt.dlaa = g_opt.dlss = false;
        ini.aa = g_opt.dlss ? 3 : g_opt.dlaa ? 1 : 0;
        printf("[video] first run: anti-aliasing chosen for this GPU and screen\n");
    }
    if (g_opt.dlaa && !renderer.dlaa_active()) printf("[video] DLAA not available: using MSAA\n");
    if (renderer.dlaa_active())
        printf("[video] antialiasing NVIDIA %s, anisotropic filtering %dx%s\n",
               renderer.dlss_upscaling() ? "DLSS (quality: the 3D at two thirds of the size)" : "DLAA", g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    else
        printf("[video] antialiasing MSAA %dx, anisotropic filtering %dx%s\n", renderer.msaa_samples(), g_opt.aniso,
               g_opt.aniso > 0 ? " with mipmaps" : " (no mipmaps)");
    if (g_opt.light_gain != 1.0f) {
        renderer.set_light_gain(g_opt.light_gain);
        printf("[video] light x%.2f on the sun and the ambient light\n", g_opt.light_gain);
    }
    R3DSceneBuilder builder;
    builder.set_remove_letterbox(!g_opt.letterbox);
    TexturePack pack;
    // (save/clean_textures: the originals of the replaced textures, seen once and kept, so a car's livery
    // is recognised with the mud or snow the game blends into it; texture_pack.h)
    pack.init("textures", g_opt.dump_textures ? "out/textures" : "", "save/clean_textures");
    pack.set_max_slots(renderer.texture_slots());
    builder.set_texture_pack(&pack, &g_texture_ram);
    // 2D replacements: the foreground (GPU slots 1-27) and the layers below the
    // 3D image (28-55; the backdrops of SkyPack use 56-63)
    Pack2D pack2d, pack2d_back;
    pack2d.init("textures/2d", 1, 27);
    pack2d_back.init("textures/2d_back", 28, SkyPack::FIRST_SLOT - 1);
    SkyPack sky;   // sky and other backdrops below the 3D image
    sky.init("textures/sky", g_opt.dump_textures ? "out/sky" : "");
    // Interpolation of the back layers (the sky): where each row's picture was
    // in the last two 2D frames composed, and the vblanks of the 2D frames
    // whose scroll is in sky_coords (the shift applies while the 3D goes
    // between the 3D frames of those same vblanks)
    std::vector<int32_t> back_rows(SCREEN_H * 3), back_rows_prev(SCREEN_H * 3, -1);
    uint64_t back_rows_frame = ~0ull, shift_from = ~0ull, shift_to = ~0ull, built_from = ~0ull;
    TextHD text;   // menus, captions and score drawn from vector fonts
    if ((g_opt.menu || !g_opt.classic_hud) && text.init("textures/text")) renderer.set_text_atlas(text);
    std::atomic<bool> capture_2d{false};   // F12 / --shot: the next 2D composition is saved
    // (debugging aid: LW_2D_CAPTURE=1 with --replay saves the snapshot's 2D layers to out/2d)
    if (!g_opt.replay.empty() && getenv("LW_2D_CAPTURE")) capture_2d = true;
    bool f12_was_down = false, f9_was_down = false;
    builder.set_vrom(g_vrom.data(), g_vrom.size());
    // The 2D layers of one vblank as the renderer takes them (compose_2d below)
    struct Layers2D {
        uint64_t frame = ~0ull;   // the vblank
        std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);   // its layer memory
        std::vector<uint32_t> back = std::vector<uint32_t>(BACK_W * SCREEN_H),
                              front = std::vector<uint32_t>(SCREEN_W * SCREEN_H),
                              text_cells = std::vector<uint32_t>(TextHD::ROW_TEXELS * TextHD::ROWS),
                              sky_coords = std::vector<uint32_t>(TileGen::SKY_COORDS_W * TileGen::SKY_COORDS_H);
        std::vector<RgbaImage> overlays, sky_pictures;   // new pictures for the GPU slots
        bool hud_band = false;
        int hud_splits[3] = {0, 0, 0};   // Sega Rally 2's race HUD (R3DRenderer::set_sr2_hud_splits)
        bool hud_over_picture = false;   // ... its texts over a 2D picture that fills the screen (set_sr2_hud_over_picture)
        float overlay_brightness = 1;
        uint64_t shift_from = ~0ull, shift_to = ~0ull;   // the 2D frames of the back layers' shift
    };
    // A game frame made ready for the screen ahead of time: its 3D scene and
    // the 2D layers of its vblank (the preparation thread, below)
    struct Prepared {
        std::shared_ptr<const R3DFrameState> state;
        R3DScene scene;
        Layers2D layers;
        uint64_t serial = 0;
        double ready_at = 0;   // when it was ready
    };
    R3DScene own_scene;   // built on this thread (no interpolation)
    const R3DScene* scene = &own_scene;
    Layers2D own_layers;   // composed on this thread (no 3D, or no interpolation)
    const Layers2D* layers_shown = &own_layers;
    std::shared_ptr<Prepared> built_prep, layers_prep;   // the prepared frames in use (scene, 2D layers)
    std::shared_ptr<const R3DFrameState> built;
    double frame_period = 1.0 / 57.524;   // between the frame built and the one before
    // Interpolation timing: the last frame the game published, the time
    // between its last two frames and how late after its vblank a frame gets
    // here (the game's CPU time plus this loop's wait for it; follows the
    // latest frames, easing down slowly)
    std::shared_ptr<const R3DFrameState> seen;
    double next_period = 1.0 / 57.524, lateness = 0.004;
    double last_interval = 1.0 / 57.524;   // between the two frames seen before the latest
    DisplayClock display_clock;

    struct Snapshot2D {
        uint64_t frame = ~0ull;
        std::vector<uint8_t> vram = std::vector<uint8_t>(TILEGEN_SIZE);
    };
    std::array<Snapshot2D, 4> snaps;   // the 2D layers of the last vblanks (snaps_mutex)
    size_t newest_snap = 0;
    std::mutex snaps_mutex;
    // Copies the game's newest 2D layers into the ring if they are new; the
    // caller holds snaps_mutex
    auto poll_snapshots = [&] {
        std::lock_guard<std::mutex> lock(g_video_mutex);
        if (g_video_frame != snaps[newest_snap].frame && !g_boot_skip) {
            newest_snap = (newest_snap + 1) % snaps.size();
            snaps[newest_snap].frame = g_video_frame;
            memcpy(snaps[newest_snap].vram.data(), g_video_vram.data(), TILEGEN_SIZE);
        }
    };
    // Copies the newest 2D layers at or before that vblank into `out` (its
    // frame stays ~0 if there are none); takes snaps_mutex
    auto take_snapshot = [&](uint64_t up_to, Layers2D& out) {
        std::lock_guard<std::mutex> lock(snaps_mutex);
        poll_snapshots();
        const Snapshot2D* pick = nullptr;
        for (const Snapshot2D& s : snaps)
            if (s.frame != ~0ull && s.frame <= up_to && (!pick || s.frame > pick->frame)) pick = &s;
        out.frame = pick ? pick->frame : ~0ull;
        if (pick) memcpy(out.vram.data(), pick->vram.data(), TILEGEN_SIZE);
    };
    // Scratch layers of the 2D composition (compose_2d)
    std::vector<uint32_t> front_raw(SCREEN_W * SCREEN_H), front_raw_top(SCREEN_W * SCREEN_H),
        back_raw(SCREEN_W * SCREEN_H), back_raw_top(SCREEN_W * SCREEN_H), back_under(SCREEN_W * SCREEN_H),
        front_under(SCREEN_W * SCREEN_H);
    std::vector<uint16_t> tex_rows;
    uint64_t shown = ~0ull, presents = 0;
    double scene_total = 0;   // seconds of 2D composition and scene building since the last title
    ULONGLONG last_title = GetTickCount64();

    // PC front end: main menu at launch, pause menu on Esc
    Frontend frontend;
    // The menus are being driven with a gamepad: no mouse pointer until the mouse moves
    bool pad_in_menu = false;
    UiOverlay ui;
    std::vector<uint32_t> ui_pixels;
    float haze_strength = g_opt.haze > 0 ? g_opt.haze : 1;
    float ao_strength = g_opt.ao > 0 ? g_opt.ao : 1;
    if (g_opt.menu) {
        if (!frontend.init("third_party/fonts")) printf("[menu] fonts missing: using the Windows ones\n");
        if (!ui.init(presenter)) g_opt.menu = false;
    }
    if (g_opt.menu) {
        FrontendSettings& s = frontend.settings;
        s = ini;   // all of save/settings.ini (the controls, the paths, the update check)...
        settings_from_options(s, g_opt);   // ...with the command-line options over it
        g_settings_file = ini;
        g_settings_launch = s;
        frontend.aa_at_launch = s.aa;
        frontend.rt_available = renderer.ray_tracing_available();
        if (s.fullscreen && !presenter.fullscreen()) presenter.toggle_fullscreen();
        // the logo first (not when a debugging aid drives the menu)
        if (getenv("LW_MENU_KEYS") || getenv("LW_MENU_SHOT")) frontend.open(Frontend::Screen::Main);
        else {
            frontend.open_splash();
            frontend.check_for_news();   // news.txt on GitHub while the logo plays
        }
        g_menu_open = true;
        presenter.input_hook = [&](UINT msg, WPARAM wp, LPARAM lp) -> bool {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (!frontend.active()) {   // in the game: Esc pauses
                if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
                    frontend.open(Frontend::Screen::Pause);
                    g_menu_open = g_hold = true;
                    return true;
                }
                return false;
            }
            switch (msg) {
            case WM_KEYDOWN:
                pad_in_menu = false;   // the keyboard again: its keys in the key help
                return frontend.key(int(wp));
            case WM_MOUSEMOVE: {
                // Windows also sends it when nothing moved (a window or the
                // pointer changing): those neither bring the pointer back nor
                // take the highlight from the pad's choice
                static int last_x = INT_MIN, last_y = INT_MIN;
                bool moved = x != last_x || y != last_y;
                last_x = x, last_y = y;
                if (!moved) return true;
                pad_in_menu = false;
                frontend.mouse_move(x, y);
                return true;
            }
            case WM_LBUTTONDOWN: frontend.click(x, y); return true;
            case WM_RBUTTONDOWN: frontend.key(VK_ESCAPE); return true;   // right button: back
            case WM_MOUSEWHEEL: {
                POINT p{x, y};   // screen coordinates for this one
                ScreenToClient(presenter.hwnd(), &p);
                frontend.wheel(p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
                return true;
            }
            }
            return false;
        };
    }
    // The game runs on unseen and muted, at full speed, to SELECT GAME (runtime.cpp,
    // arcade_entry_frame; 1: from wherever it is, 3: the game on is ended first)
    auto run_unseen = [](int entry) {
        g_arcade_entry = entry;
        g_boot_skip = g_fast_idle = true;
        g_sound.set_dsp_enabled(false);
        g_sound.set_muted(true);
    };
    auto close_menu = [&] {
        frontend.open(Frontend::Screen::None);
        ui.hide();
        g_menu_open = g_hold = false;
    };

    // With the menu the game runs on to SELECT GAME behind it once booted (1 =
    // still to start that run), so PLAY shows it at once
    int arcade_join = g_opt.menu ? 1 : 0;
    bool arcade_entered = false;
    bool play_waits = false;   // PLAY pressed while the game still gets to SELECT GAME

    // Achievement notices, over the game and under the menus
    AchievementToast toast;
    UiOverlay toast_layer;
    std::vector<uint32_t> toast_pixels;
    bool toast_shown = toast.init("third_party/fonts") && toast_layer.init(presenter);
    toast.set_volume(g_opt.volume);

    // Composes the 2D layers of a vblank (out.frame, its layer memory in
    // out.vram) for the renderer. One composition at a time (prep_mutex): the
    // 2D packs and the back layers' scroll keep state between them
    auto compose_2d = [&](Layers2D& out) {
        // HD HUD off (classic_hud): the ROM's own letters and pictures, no
        // vector text and no 2D replacements (the race HUD's dials, gear box,
        // co-driver signs and countdown)
        const bool hd_hud = !g_opt.classic_hud;
        const TextHD* hd_text = hd_hud ? &text : nullptr;
        if (!hd_text) std::fill(out.text_cells.begin(), out.text_cells.end(), 0u);
        bool capture = capture_2d.exchange(false);
        if (capture) sky.capture();   // F12 also saves the backdrops, for textures/sky
        bool sky_on = sky.active();
        // Raw layers for recognising the 2D replacements (and for F12)
        bool raw_front = !pack2d.empty() || capture, raw_back = !pack2d_back.empty() || capture;
        TileGen::compose(out.vram.data(),
                         {.back = out.back.data(),
                          .front = out.front.data(),
                          .front_raw = raw_front ? front_raw.data() : nullptr,
                          .front_raw_top = raw_front ? front_raw_top.data() : nullptr,
                          .front_under = raw_front ? front_under.data() : nullptr,
                          .back_raw = raw_back ? back_raw.data() : nullptr,
                          .back_raw_top = raw_back ? back_raw_top.data() : nullptr,
                          .back_under = raw_back ? back_under.data() : nullptr,
                          .text_cells = out.text_cells.data(),
                          .sky_coords = sky_on ? out.sky_coords.data() : nullptr,
                          .back_rows = back_rows.data()},
                         hd_text, sky_on ? &sky : nullptr);
        // How far each row of the back layers scrolled since the previous
        // 2D frame composed, for drawing them in between (renderer.set_back_shift)
        {
            uint32_t* shift_row = &out.sky_coords[(SCREEN_H + 1) * TileGen::SKY_COORDS_W];
            for (int y = 0; y < SCREEN_H; y++) {
                const int32_t* now_row = &back_rows[3 * y];
                const int32_t* was = &back_rows_prev[3 * y];
                int dx = ((now_row[1] - was[1] + 256) & 511) - 256, dy = ((now_row[2] - was[2] + 256) & 511) - 256;
                bool known = now_row[0] >= 0 && now_row[0] == was[0] && std::abs(dx) < 100 && std::abs(dy) < 100;
                shift_row[y] = known ? 0xFF000000u | 0xFFu | uint32_t(dx + 128) << 16 | uint32_t(dy + 128) << 8 : 0;
            }
            out.shift_from = back_rows_frame;
            out.shift_to = back_rows_frame = out.frame;
            back_rows_prev.swap(back_rows);
        }
        out.sky_pictures = sky.take_new_images();
        if (capture) {   // F12 or --shot: capture for tools/make_2d_replacement.py
            CreateDirectoryA("out/2d", nullptr);
            std::string path = "out/2d/capture_" + std::to_string(out.frame);
            Pack2D::save_capture(front_raw.data(), path);
            Pack2D::save_capture(front_raw_top.data(), path + "_layer_a");   // only the texts' layers
            Pack2D::save_capture(back_raw.data(), path + "_back");          // the layers below the 3D image
            Pack2D::save_capture(back_raw_top.data(), path + "_back_layer_a");
            printf("2D layer saved to %s.png\n", path.c_str());
        }
        {
            // Sega Rally 2's race HUD (to the window's corners, compose.frag):
            // where each band splits, an empty column between the corner's
            // group and the middle. Between the times and the time left on
            // top, between the time left (and the co-driver's arrow) and the
            // position; below, between the speed and the car's name. None
            // found (a message across it): that band stays as it is
            // (a pixel shows: in the foreground, or in a cell of high-resolution
            // text, which is left out of the foreground)
            auto shows = [&](int x, int y) {
                return (out.front[y * SCREEN_W + x] >> 24) != 0 ||
                       (out.text_cells[(y / 8) * TextHD::ROW_TEXELS + (x / 8) * TextHD::CELL_TEXELS] >> 24) != 0;
            };
            auto empty = [&](int x, int y0, int y1) {
                for (int y = y0; y < y1; y++)
                    if (shows(x, y)) return false;
                return true;
            };
            auto first_empty = [&](int x0, int x1, int y0, int y1) {
                for (int x = x0; x <= x1; x++)
                    if (empty(x, y0, y1)) return x;
                return 0;
            };
            auto last_empty = [&](int x0, int x1, int y0, int y1) {
                for (int x = x1; x >= x0; x--)
                    if (empty(x, y0, y1)) return x;
                return 0;
            };
            const int left = R3DRenderer::SR2_HUD_LEFT_ROWS, top = R3DRenderer::SR2_HUD_TOP_ROWS,
                      bottom = R3DRenderer::SR2_HUD_BOTTOM_ROW;
            // Only with the race HUD's TOTAL TIME and POSITION labels in place
            // (rows 19-28): not the results after it (RETIRED, the stages' list)
            auto lit_at = [&](int x0, int x1, int y0, int y1) {
                int n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) n += shows(x, y);
                return n;
            };
            bool race_hud = lit_at(16, 120, 18, 30) > 150 && lit_at(370, 480, 18, 30) > 150;
            // Its labels as high-resolution text (the text cells), which the
            // stage's START!! card does not have: with no game world in the
            // scene, the moment before the race appears
            // (R3DRenderer::sr2_between_screens)
            auto text_at = [&](int x0, int x1, int y0, int y1) {
                int n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++)
                        n += (out.text_cells[(y / 8) * TextHD::ROW_TEXELS + (x / 8) * TextHD::CELL_TEXELS] >> 24) != 0;
                return n;
            };
            out.hud_over_picture = text_at(16, 120, 18, 30) > 150 && text_at(370, 480, 18, 30) > 150;
            out.hud_splits[0] = race_hud ? first_empty(138, 214, 0, left) : 0;   // (practice's laps reach x 135)
            out.hud_splits[1] = race_hud ? last_empty(282, 362, 0, top) : 0;
            out.hud_splits[2] = race_hud ? first_empty(232, 384, bottom, SCREEN_H) : 0;
            out.hud_band = out.hud_splits[0] || out.hud_splits[1] || out.hud_splits[2];
        }
        int offset[3];
        TileGen::front_offset(out.vram.data(), offset);
        if (hd_hud && !pack2d.empty())
            pack2d.apply(out.front.data(), SCREEN_W, front_raw.data(), front_raw_top.data(), out.text_cells.data(),
                         false, front_under.data(), out.back.data() + BACK_MARGIN, BACK_W);
        if (hd_hud && !pack2d_back.empty())
            pack2d_back.apply(out.back.data() + BACK_MARGIN, BACK_W, back_raw.data(), back_raw_top.data(),
                              out.text_cells.data(), true, back_under.data());
        out.overlay_brightness = float(offset[0] + offset[1] + offset[2]) / (3 * 255.0f);
        out.overlays = pack2d.take_new_images();
        for (auto& img : pack2d_back.take_new_images()) out.overlays.push_back(std::move(img));
    };
    // Hands composed 2D layers to the renderer (their new pictures go separately)
    auto apply_2d = [&](const Layers2D& l) {
        renderer.set_sky_coords(l.sky_coords.data());
        renderer.set_hud_band(l.hud_band);
        renderer.set_sr2_hud_splits(l.hud_splits);
        renderer.set_sr2_hud_over_picture(l.hud_over_picture);
        renderer.set_overlay_brightness(l.overlay_brightness);
        renderer.set_text_cells(l.text_cells.data());
        renderer.set_layers(l.back.data(), l.front.data());
        shift_from = l.shift_from;
        shift_to = l.shift_to;
        shown = l.frame;
        layers_shown = &l;
    };

    // Preparation thread: while interpolating, each new game frame is made
    // ready as soon as the game finishes it, its 3D scene and its 2D layers
    // side by side, so this thread only draws and keeps up with every
    // refresh (the two took ~14 ms here, more than a refresh at 120 Hz, and
    // the images went out unevenly). The interpolation delay follows when
    // the frames are ready (lateness), so it includes this work
    std::mutex prep_mutex;    // one composition and one scene build at a time
    std::mutex ready_mutex;   // ready_queue and ready_pictures
    // The frames made ready and not yet taken, in order. Each one is taken in
    // its turn: with only the newest kept, a frame was lost whenever the
    // picture ran more than a frame period behind the game (after any hitch,
    // such as the start of a stage, for the seconds the delay took to ease
    // down), and the motion went at half speed for two images and then jumped
    std::deque<std::shared_ptr<Prepared>> ready_queue;
    uint64_t taken_serial = 0;   // the last one taken
    // New pictures for the GPU slots, in order: handed over when their frame
    // (or a later one) is taken, also those of frames never taken
    struct NewPictures {
        uint64_t serial = 0;
        std::vector<RgbaImage> replacements, overlays, sky;
    };
    std::deque<NewPictures> ready_pictures;
    auto give_pictures = [&](uint64_t serial) {
        std::vector<NewPictures> due;
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            while (!ready_pictures.empty() && ready_pictures.front().serial <= serial) {
                due.push_back(std::move(ready_pictures.front()));
                ready_pictures.pop_front();
            }
        }
        for (NewPictures& n : due) {
            if (!n.replacements.empty()) renderer.add_replacements(n.replacements);
            if (!n.overlays.empty()) renderer.add_overlays(n.overlays);
            if (!n.sky.empty()) renderer.add_sky_pictures(n.sky);
        }
    };
    std::atomic<bool> prep_on{false}, prep_stop{false};
    std::atomic<uint64_t> prep_us{0}, prep_frames{0};   // its work, read once a second
    std::thread prep_thread([&] {
        std::vector<std::shared_ptr<Prepared>> pool;   // reused once nobody else holds them
        std::shared_ptr<const R3DFrameState> done;
        uint64_t serial = 0;
        while (!prep_stop) {
            std::shared_ptr<const R3DFrameState> state;
            {
                std::unique_lock<std::mutex> lock(g_r3d_mutex);
                g_r3d_cv.wait_for(lock, std::chrono::milliseconds(20),
                                  [&] { return prep_stop || (prep_on && g_r3d_latest != done); });
                state = g_r3d_latest;
            }
            if (!prep_on) done.reset();
            if (prep_stop || !prep_on || !state || state == done) continue;
            done = state;
            std::shared_ptr<Prepared> p;
            for (auto& q : pool)
                if (q.use_count() == 1) {
                    p = q;
                    break;
                }
            if (!p) pool.push_back(p = std::make_shared<Prepared>());
            p->state = state;
            p->serial = ++serial;
            NewPictures pictures;
            pictures.serial = p->serial;
            {
                std::lock_guard<std::mutex> lock(prep_mutex);
                take_snapshot(state->vblank, p->layers);   // the 2D layers of the frame's vblank
                std::future<void> layers;
                if (p->layers.frame != ~0ull)
                    layers = std::async(std::launch::async, [&] { compose_2d(p->layers); });
                double from = qpc_seconds();
                builder.set_fov_widened(g_wide_applied);
                builder.set_hide_blob_shadows(g_opt.shadows && g_opt.ray_tracing >= 1 && renderer.ray_tracing_available());
                builder.set_view_wide(g_opt.max_aspect > 4.0f / 3.0f + 0.01f ? std::max(1.0f, g_wide_view.load()) : 1.0f);
                builder.build(*state, p->scene);
                double built_at = qpc_seconds();
                pictures.replacements = pack.take_new_images();
                if (layers.valid()) layers.get();
                double took = qpc_seconds() - from;
                prep_us += uint64_t(took * 1e6);
                prep_frames++;
                g_build_ms = float(took * 1000);
                if (took * 1000 > g_build_worst_ms) g_build_worst_ms = float(took * 1000);
                const float* st = builder.stage_ms();
                frame_log("R %llu %.6f %.3f %.3f  nodes %.2f track %.2f screen %.2f pack %.2f pair %.2f smooth %.2f sync %.2f  polys %zu\n",
                          (unsigned long long)state->frame, qpc_seconds(), (built_at - from) * 1000, took * 1000, st[0],
                          st[1], st[2], st[3], st[4], st[5], st[6], p->scene.polygons);
                pictures.overlays.swap(p->layers.overlays);
                pictures.sky.swap(p->layers.sky_pictures);
                p->layers.overlays.clear();
                p->layers.sky_pictures.clear();
            }
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready_pictures.push_back(std::move(pictures));
            p->ready_at = qpc_seconds();
            if (prep_on) {
                ready_queue.push_back(p);
                if (ready_queue.size() > 8) ready_queue.pop_front();   // (nobody is taking them)
            }
        }
    });
    // LW_FRAMELOG: a thread notes when each present reaches the screen
    std::atomic<uint64_t> presented_id{0};
    std::thread shown_waiter;
    if (g_frame_log && presenter.can_wait_present())
        shown_waiter = std::thread([&] {
            uint64_t next = 1;
            while (!prep_stop) {
                if (next > presented_id) {
                    Sleep(1);
                    continue;
                }
                if (presenter.wait_present(next, 100'000'000))
                    frame_log("S %llu %.6f\n", (unsigned long long)next, qpc_seconds());
                next++;
            }
        });
    // Both threads stop when the loop ends (and with this scope, however it ends)
    struct StopThreads {
        std::atomic<bool>& stop;
        std::thread &a, &b;
        void operator()() {
            stop = true;
            g_r3d_cv.notify_all();
            if (a.joinable()) a.join();
            if (b.joinable()) b.join();
        }
        ~StopThreads() { (*this)(); }
    } stop_threads{prep_stop, prep_thread, shown_waiter};

    // At most this many images wait for the screen (VK_KHR_present_wait): the
    // picture goes out that many refreshes after it is drawn (~17 ms at
    // 120 Hz) instead of after a full swapchain (~32 ms). When refreshes are
    // missed twice within 2 s (the GPU busier than usual), one more for 10 s.
    // Debugging aid: LW_QUEUE=N (0 = the whole swapchain)
    const int queue_depth = getenv("LW_QUEUE") ? atoi(getenv("LW_QUEUE")) : 2;
    int queue_extra = 0;
    double went_out = 0, last_miss = -100;
    // out/slow.log: what each image's turn of this loop went on, from one
    // present call to the next, in seconds (the longest of each second is
    // written there: which part of the loop a stutter came from)
    struct LoopParts {
        double total = 0;
        double wait = 0;        // for an earlier image to reach the screen (limit_queue)
        double window = 0;      // the window's messages
        double menu = 0;        // the menus and the achievement notice
        double input = 0;       // keyboard, pads and wheel...
        double vibration = 0;   // ...of which, the calls that drive their motors
        double scene = 0;       // 2D composition, the 3D scene, new pictures to the GPU
        double textures = 0;    // texture RAM rows to the GPU
        double gpu_wait = 0, acquire = 0, record = 0, present = 0;   // VkPresenter::LastFrame
    };
    LoopParts parts, worst_parts;
    double parts_from = 0;   // when the previous present call returned
    int parts_skip = 0;      // turns left out: a refresh skipped on purpose (the queue made shorter)
    for (;;) {
        double wait_from = qpc_seconds();
        {   // before the inputs are read
            int depth = queue_depth ? queue_depth + queue_extra : 0;
            double out = presenter.limit_queue(depth, 50'000'000);
            if (out > 0 && went_out > 0 && out - went_out > 1.5 * display_clock.period) {
                if (out - last_miss < 2 && !queue_extra) {
                    queue_extra = 1;
                    printf("[video] refreshes missed: %d images queued for a while\n", depth + 1);
                }
                last_miss = out;
            }
            if (queue_extra && out - last_miss > 10) {
                queue_extra = 0;
                out = 0;   // the next wait skips a refresh: not a miss
                parts_skip = 2;
            }
            went_out = out;
        }
        double window_from = qpc_seconds();
        if (!presenter.pump()) break;
        double loop_from = qpc_seconds();
        parts = LoopParts{};
        parts.wait = window_from - wait_from;
        parts.window = loop_from - window_from;
        // PLAY: straight to SELECT GAME. Once the game is past its boot it
        // runs on with nothing on screen, through the SEGA logo (which takes
        // no coin), while the coin and START go in for the player
        // (runtime.cpp, arcade_entry_frame)
        if (arcade_join && !g_boot_skip) {
            arcade_join = 0;
            arcade_entered = true;
            run_unseen(1);
        }
        // A game over: the main menu, while the game goes on to SELECT GAME
        // behind it instead of into the attract (runtime.cpp, attract_guard_frame)
        if (g_game_over_menu.exchange(false) && g_opt.menu) {
            arcade_join = 0;
            play_waits = false;
            frontend.starting = false;
            frontend.open(Frontend::Screen::Main);
            g_menu_open = g_hold = true;
        }
        g_debug_freeze_time = frontend.debug_time_frozen;   // DEBUG: TIME INFINITE
        {   // SR2_AUTOPLAY: PLAY pressed by itself once the game is ready behind the menu (testing)
            static bool autoplay = getenv("SR2_AUTOPLAY") != nullptr;
            static double quit_at = -1;   // SR2_AUTOQUIT=S: QUIT TO MAIN MENU S seconds after that
            if (autoplay && g_menu_open && !arcade_join && !g_arcade_entry && !g_boot_skip) {
                autoplay = false;
                close_menu();
                if (const char* q = getenv("SR2_AUTOQUIT"); q && *q) quit_at = qpc_seconds() + atof(q);
            }
            // SR2_DEBUG_JUMP=COURSE,CAR,GEARS: DEBUG's race chosen by itself once the game is ready behind the menu (testing)
            static const char* jump = getenv("SR2_DEBUG_JUMP");
            if (jump && *jump && g_menu_open && !arcade_join && !g_arcade_entry && !g_boot_skip) {
                int course = 0, car = 0, gears = 0;
                sscanf(jump, "%d,%d,%d", &course, &car, &gears);
                frontend.debug_choice = (course & 3) | (car & 7) << 4 | (gears & 1) << 8;
                frontend.request(Frontend::Action::StartDebug);
                jump = nullptr;
                // (SR2_AUTOQUIT=S here too: QUIT TO MAIN MENU S seconds into the practice, then PLAY again)
                if (const char* q = getenv("SR2_AUTOQUIT"); q && *q) quit_at = qpc_seconds() + atof(q);
            }
            if (quit_at > 0 && qpc_seconds() >= quit_at) {
                quit_at = -1;
                frontend.open(Frontend::Screen::Pause);   // as Esc does, then QUIT TO MAIN MENU
                g_menu_open = g_hold = true;
                frontend.request(Frontend::Action::QuitToMenu);
                autoplay = true;   // and PLAY again once the game is back at SELECT GAME
                _putenv("SR2_AUTOQUIT=");
            }
        }
        if (play_waits && !arcade_join && !g_arcade_entry && !g_boot_skip) {
            play_waits = false;
            frontend.starting = false;
            close_menu();
        }
        {   // the 3D view's width (widescreen culling and sight, runtime.cpp)
            RECT wrc;
            GetClientRect(presenter.hwnd(), &wrc);
            if (wrc.right > 0 && wrc.bottom > 0)
                g_wide_view = compute_layout(float(wrc.right), float(wrc.bottom), g_opt.max_aspect).view_w / SCREEN_W;
        }
        if (toast_shown) {
            for (int i : stats_take_new_achievements())
                toast.push(achievement_table()[size_t(i)].name, achievement_table()[size_t(i)].text);
            // The author's credit over the attract (seen with the menu off, --no-menu)
            toast.set_credit(L"PC port by snicolast", L"github.com/snicolast/SegaRally2Native",
                             g_in_attract && !g_menu_open && !g_boot_skip && !g_arcade_entry);
            RECT rc;
            GetClientRect(presenter.hwnd(), &rc);
            if (toast.draw(rc.right, rc.bottom, qpc_seconds(), toast_pixels))
                toast_layer.set_image(toast_pixels.data(), rc.right, rc.bottom);
        }
        if (g_opt.menu) {
            for (Frontend::Action a : frontend.take_actions()) {
                FrontendSettings& s = frontend.settings;
                switch (a) {
                case Frontend::Action::StartArcade:
                    if (!arcade_entered) arcade_join = 1;
                    // The menu stays until SELECT GAME is up behind it (the game
                    // runs there unseen, at full speed)
                    if (arcade_join || g_arcade_entry || g_boot_skip) {
                        play_waits = true;
                        frontend.starting = true;
                        frontend.invalidate();
                        break;
                    }
                    [[fallthrough]];
                case Frontend::Action::Resume:
                    close_menu();
                    break;
                case Frontend::Action::StartDebug:   // DEBUG: a practice race of a course with a car (runtime.cpp, debug_jump_frame)
                    g_debug_jump = frontend.debug_choice;
                    if (!arcade_entered) arcade_join = 1;
                    else if (!g_arcade_entry) run_unseen(3);   // at SELECT GAME: on from there; a game on: over, and on from SELECT GAME
                    play_waits = true;
                    frontend.starting = true;
                    frontend.invalidate();
                    break;
                case Frontend::Action::QuitToMenu:   // the game over and back to SELECT GAME, unseen (runtime.cpp)
                    arcade_join = 0;
                    run_unseen(3);
                    frontend.open(Frontend::Screen::Main);
                    break;
                case Frontend::Action::Quit: presenter.close(); break;
                case Frontend::Action::ToggleFullscreen: presenter.toggle_fullscreen(); break;
                case Frontend::Action::SettingsChanged:
                    g_sound.set_volume(float(s.volume));
                    g_sound.set_music_gain(float(s.music));
                    toast.set_volume(float(s.volume));
                    Scsp::set_cubic(g_opt.cubic_samples = s.cubic_samples);
                    g_sound.set_rear_boost(g_opt.rear_boost = s.rear_boost);
                    g_audio.set_speakers(AudioStream::Speakers(g_opt.speakers = s.speakers));
                    renderer.set_distance_haze(s.haze ? haze_strength : 0);
                    renderer.set_bloom(g_opt.bloom = BLOOM_STRENGTH[s.bloom]);
                    renderer.set_ambient_occlusion(s.ao ? ao_strength : 0);
                    renderer.set_sun(g_opt.sun = s.sun);
                    renderer.set_shadows((g_opt.shadows = s.shadows) ? 1.0f : 0.0f);
                    renderer.set_ray_tracing(g_opt.ray_tracing = s.ray_tracing);
                    renderer.set_soft_particles(g_opt.soft = s.soft);
                    renderer.set_hdr(g_opt.hdr = s.hdr);
                    renderer.set_reflections(g_opt.reflections = s.reflections);
                    renderer.set_global_illumination(g_opt.gi = s.gi);
                    r3d_set_sr2_dust(g_opt.dust = s.dust);
                    renderer.set_sharpen((g_opt.sharpen = s.sharpen) ? 0.5f : 0.0f);
                    if (g_opt.classic_hud != !s.hd_hud) {
                        g_opt.classic_hud = !s.hd_hud;
                        shown = ~0ull;   // compose the 2D layer again even while the game stands still
                    }
                    g_opt.difficulty = s.difficulty;   // the game thread puts it in the operator settings
                    if (g_opt.interp != s.smooth) {
                        g_opt.interp = s.smooth;
                        renderer.set_interpolation(1.0f);   // the latest game frame as it is
                        renderer.set_back_shift(0.0f);
                    }
                    renderer.set_picture(s.saturation / 100.0f, s.gamma / 100.0f);
                    g_pad_rumble = s.vibration;
                    save_settings(s);
                    break;
                default: break;
                }
            }
            if (frontend.settings.fullscreen != presenter.fullscreen()) {   // also F11 / Alt+Enter
                frontend.settings.fullscreen = presenter.fullscreen();
                save_settings(frontend.settings);
                frontend.invalidate();
            }
            // Debugging aid: LW_MENU_KEYS="40,13,27" presses those keys (one
            // every 10 presents; Esc in the game pauses; "cX/Y" clicks at window
            // pixel X,Y), then saves out/menu.bmp and quits
            static const char* menu_keys = getenv("LW_MENU_KEYS");
            static int menu_presents = 0;
            if (menu_keys && ++menu_presents % 10 == 0) {
                if (*menu_keys == 'c') {
                    int cx = 0, cy = 0, len = 0;
                    sscanf(menu_keys, "c%d/%d%n", &cx, &cy, &len);
                    menu_keys += len;
                    if (*menu_keys == ',') menu_keys++;
                    frontend.click(cx, cy);
                } else if (*menu_keys) {
                    char* end = nullptr;
                    int vk = int(strtol(menu_keys, &end, 10));
                    menu_keys = *end == ',' ? end + 1 : end;
                    if (frontend.active()) frontend.key(vk);
                    else if (vk == VK_ESCAPE) {
                        frontend.open(Frontend::Screen::Pause);
                        g_menu_open = g_hold = true;
                    }
                } else if (menu_presents % 30 == 0) {
                    presenter.capture_next("out/menu.bmp");
                    menu_keys = nullptr;
                    presenter.close();
                }
            }
            static Frontend::Screen menu_was = Frontend::Screen::None;
            bool counts = frontend.screen() == Frontend::Screen::Records || frontend.screen() == Frontend::Screen::Main;
            if (counts && menu_was != frontend.screen()) {   // the records, and the main menu's achievements and best score
                frontend.records = stats_view();   // the game stands still while the menu is open
                frontend.invalidate();
            }
            menu_was = frontend.screen();
            if (!frontend.active()) pad_in_menu = false;   // a menu opened later starts with the mouse
            {   // the menus' music: over the backdrop, not over a paused game nor during the splash
                static MenuMusic& music = *new MenuMusic;   // never destroyed: the audio thread reads it to the end
                static bool music_tried = false;
                static double music_time = qpc_seconds();
                if (!music_tried) {
                    music_tried = true;
                    if (music.open())
                        g_audio.set_extra([](void* m, float* out, size_t frames, int channels, int rate) {
                            static_cast<MenuMusic*>(m)->mix(out, frames, channels, rate);
                        }, &music);
                }
                double t = qpc_seconds();
                music.update(frontend.active() && !frontend.over_game() && !frontend.splash() && !frontend.starting &&
                                 frontend.settings.menu_music,
                             frontend.settings.music, t - music_time);
                music_time = t;
            }
            presenter.set_cursor_visible(frontend.active() && !pad_in_menu && !frontend.splash());
            static const bool menu_pad = getenv("SR2_MENU_PAD") != nullptr;   // debugging aid: the gamepad's key help
            if (frontend.pad_prompts != (pad_in_menu || menu_pad)) {   // the key help shows the gamepad's buttons or the keys
                frontend.pad_prompts = pad_in_menu || menu_pad;
                frontend.invalidate();
            }
            if (frontend.active()) {
                RECT rc;
                GetClientRect(presenter.hwnd(), &rc);
                double t0 = qpc_seconds();
                if (frontend.draw(rc.right, rc.bottom, ui_pixels)) {
                    ui.set_image(ui_pixels.data(), rc.right, rc.bottom);
                    double ms = (qpc_seconds() - t0) * 1000;
                    if (ms > 20) printf("[menu] slow redraw: %.0f ms\n", ms);
                    static const char* menu_shot = getenv("LW_MENU_SHOT");   // debugging aid: the menu picture
                    if (menu_shot) save_bmp(menu_shot, ui_pixels.data(), rc.right, rc.bottom);
                }
            }
        }
        double input_from = qpc_seconds();
        parts.menu = input_from - loop_from;
        g_vibration_seconds = 0;
        poll_keyboard(presenter.hwnd());
        bool pad_pause = poll_gamepads(presenter.hwnd());
        static std::string shown_wheel;   // the wheel the CONTROLS tab names ("" = none)
        const WheelState& wheel = wheel_state();
        std::string wheel_name = wheel.connected ? wheel.name : std::string();
        if (g_pads.changed() || wheel_name != shown_wheel) {   // the CONTROLS tab names the pads and the wheel
            shown_wheel = wheel_name;
            std::wstring names;
            for (int i = 0; i < Gamepads::MAX; i++) {
                const PadState& pad = g_pads.pad(i);
                if (!pad.connected) continue;
                names += (names.empty() ? L"" : L", ") + std::wstring(pad.name.begin(), pad.name.end());
            }
            if (!wheel_name.empty())
                names += (names.empty() ? L"" : L", ") + std::wstring(wheel_name.begin(), wheel_name.end());
            frontend.gamepads = names;
            frontend.invalidate();
        }
        for (PadKey k : g_pads.take_keys()) {   // the menus, as keys (START resumes from the pause menu)
            static const int VK[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, 'Q', 'E', 'G'};
            bool resume = frontend.screen() == Frontend::Screen::Pause;
            if (frontend.active()) {
                frontend.key(k == PadKey::Start ? (resume ? VK_ESCAPE : VK_RETURN) : VK[int(k)]);
                pad_in_menu = true;
            }
        }
        if (pad_pause && g_opt.menu && !frontend.active()) {
            frontend.open(Frontend::Screen::Pause);
            pad_in_menu = true;
            g_menu_open = g_hold = true;
        }
        press_update();
        parts.input = qpc_seconds() - input_from;
        parts.vibration = g_vibration_seconds;
        bool f12 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F12) & 0x8000);
        if (f12 && !f12_was_down) capture_2d = true;
        f12_was_down = f12;
        bool f9 = GetForegroundWindow() == presenter.hwnd() && (GetAsyncKeyState(VK_F9) & 0x8000);
        // (debugging aid: LW_SNAP_AT=N[,EVERY,COUNT] takes that snapshot by itself once the game
        // reaches frame N, then COUNT more one every EVERY frames)
        static uint64_t snap_at = 0, snap_every = 0, snap_left = 0;
        static bool snap_parsed = false;
        if (!snap_parsed) {
            snap_parsed = true;
            if (const char* e = getenv("LW_SNAP_AT")) sscanf(e, "%llu,%llu,%llu", &snap_at, &snap_every, &snap_left);
        }
        bool auto_snap = snap_at && built && built->frame >= snap_at;
        if (auto_snap) snap_at = snap_left && snap_every ? (snap_left--, built->frame + snap_every) : 0;
        if (((f9 && !f9_was_down) || auto_snap) && built) {   // debug snapshot of the scene on screen
            CreateDirectoryA("out/snap", nullptr);
            std::string base = "out/snap/snap_" + std::to_string(built->frame);
            save_snapshot((base + ".bin").c_str(), *built, layers_shown->vram);
            dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), (base + ".txt").c_str());
            presenter.capture_next(base + ".bmp");
            if (FILE* f = fopen((base + ".ram").c_str(), "wb")) {   // the game's RAM too (taken while it runs)
                fwrite(g_ram, 1, RAM_SIZE, f);
                fclose(f);
            }
            printf("Snapshot saved to %s.bin (wheel %d)\n", base.c_str(), int(g_inputs.steer));
        }
        f9_was_down = f9;
        if (g_opt.shot_frame && shown != ~0ull && shown + 1 >= g_opt.shot_frame) capture_2d = true;
        // The 3D frame to draw. While interpolating, the picture shows the game
        // as it was at `shown_time`: a frame period plus the frames' lateness
        // before this image's moment on screen, so the next frame is always
        // here before it is needed and the motion stays even; a new frame is
        // taken once the picture reaches the one built. Otherwise at once.
        // While interpolating the frames come from the preparation thread
        // (their lateness includes its work), otherwise straight from the game
        bool interpolating = g_opt.interp && g_opt.replay.empty() && !g_opt.shot_frame;
        if (interpolating != prep_on) {   // a frame it made earlier is not one to go back to
            prep_on = interpolating;
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready_queue.clear();
        }
        double display = display_clock.tick();
        std::shared_ptr<const R3DFrameState> latest;
        std::shared_ptr<Prepared> ready;                  // the newest one made ready
        std::vector<std::shared_ptr<Prepared>> waiting;   // those not taken yet, oldest first
        if (interpolating) {
            std::lock_guard<std::mutex> lock(ready_mutex);
            // (the newest stays, taken or not: the timing follows it)
            while (ready_queue.size() > 1 && ready_queue.front()->serial <= taken_serial) ready_queue.pop_front();
            if (!ready_queue.empty()) ready = ready_queue.back();
            for (auto& q : ready_queue)
                if (q->serial > taken_serial) waiting.push_back(q);
        } else {
            std::lock_guard<std::mutex> lock(g_r3d_mutex);
            latest = g_r3d_latest;
        }
        if (ready && built && ready->state->frame < built->frame) ready.reset();   // made before the switch
        if (ready) latest = ready->state;
        if (latest && latest != seen) {
            // The time between the game's frames: the shorter of its last two
            // intervals. One frame further from the last than the rest (the game
            // skips a vblank as a screen changes, or a frame is left out because
            // the one before took long to make ready, as when a stage starts and
            // its textures come in) is not the game's pace: taken as such it put
            // the picture a frame period back for one image and forward again
            // with the next frame, the motion going back and then jumping
            if (seen) {
                double interval = std::clamp(latest->time - seen->time, 1.0 / 240, 0.1);
                next_period = std::min(interval, last_interval);
                last_interval = interval;
            }
            // How long after its vblank the frame was ready to take, plus how
            // far ahead of now this image's moment is: from the preparation
            // thread, when it finished; from the game, when this loop saw it
            double late = ready ? ready->ready_at - latest->time + (display - display_clock.last) : display - latest->time;
            // It rises at once with 1.5 ms to spare and eases down 1.7 ms per
            // second, so that later frames by a little do not each step the
            // motion back
            // (a single frame much later than the others, as when a stage
            // starts, left the picture that far behind for the next half
            // minute: while the delay is well above what the frames of the last
            // seconds needed, it eases down ten times as fast, the picture 2%
            // ahead of the game's pace for a second or two)
            late = std::clamp(late, 0.0, 0.05);
            static double late_peak[2] = {0, 0};   // the latest of this stretch of 120 frames, and of the one before
            static int late_count = 0;
            if (++late_count >= 120) late_count = 0, late_peak[1] = late_peak[0], late_peak[0] = 0;
            double needed = std::max(late_peak[0], late_peak[1]);   // (without this frame: it may be the hitch itself)
            late_peak[0] = std::max(late_peak[0], late);
            double ease = lateness > needed + 0.004 ? 0.0003 : 0.00003;
            lateness = late > lateness ? late + 0.0015 : std::max(lateness - ease, late);
            seen = latest;
        }
        double shown_time = display - lateness - next_period - 0.0005;
        // The frame to take: the next one in turn once the picture reaches the
        // one built (after a long stop, the one the picture's time is in)
        std::shared_ptr<Prepared> next_ready;
        if (interpolating && ready && (!built || shown_time >= built->time)) {
            if (!built) {
                next_ready = ready;
            } else {
                for (auto& w : waiting) {
                    if (w->state->frame < built->frame) continue;   // made before the switch
                    next_ready = w;
                    if (w->state->time >= shown_time) break;
                }
            }
        }
        std::shared_ptr<const R3DFrameState> next_state = interpolating ? (next_ready ? next_ready->state : nullptr) : latest;
        bool take = next_state && next_state != built;
        // The 2D layers of each vblank are kept in a few copies: while the 3D
        // frames are interpolated, those of the 3D frame drawn are shown (the
        // picture runs behind the game; the preparation thread composes them
        // with it), unless the game stopped drawing 3D; otherwise the newest
        uint64_t newest_2d;
        {
            std::lock_guard<std::mutex> lock(snaps_mutex);
            poll_snapshots();
            newest_2d = snaps[newest_snap].frame;
        }
        bool follow = interpolating && latest && newest_2d != ~0ull && newest_2d <= latest->vblank + 4;
        double scene_from = qpc_seconds(), scene_seconds = 0;   // 2D composition and 3D scene build here (CPU)
        const uint64_t shown_before = shown;
        // (shown = ~0: composed again here, as after a change of settings in the pause menu)
        uint64_t compose_up_to = !follow ? (newest_2d != shown ? newest_2d : ~0ull)
                                         : shown == ~0ull && built ? built->vblank : ~0ull;
        if (compose_up_to != ~0ull) take_snapshot(compose_up_to, own_layers);
        if (compose_up_to != ~0ull && own_layers.frame != ~0ull) {
            std::lock_guard<std::mutex> lock(prep_mutex);
            compose_2d(own_layers);
            if (!own_layers.sky_pictures.empty()) renderer.add_sky_pictures(own_layers.sky_pictures);
            if (!own_layers.overlays.empty()) renderer.add_overlays(own_layers.overlays);
            own_layers.sky_pictures.clear();
            own_layers.overlays.clear();
            apply_2d(own_layers);
            layers_prep.reset();
            scene_seconds += qpc_seconds() - scene_from;
        }
        if (take) {
            if (built) frame_period = std::clamp(next_state->time - built->time, 1.0 / 240, 0.1);
            built_from = built ? built->vblank : ~0ull;
            scene_from = qpc_seconds();
            if (next_ready) {   // made ready by the preparation thread, with the 2D layers of its vblank
                give_pictures(next_ready->serial);
                taken_serial = next_ready->serial;
                built_prep = next_ready;
                scene = &next_ready->scene;
                // (a frame number going back a long way is the game restarted from a saved state)
                const Layers2D& l = next_ready->layers;
                if (follow && l.frame != ~0ull && (shown == ~0ull || l.frame > shown || shown - l.frame > 8)) {
                    apply_2d(l);
                    layers_prep = next_ready;
                }
            } else {
                std::lock_guard<std::mutex> lock(prep_mutex);
                builder.set_fov_widened(g_wide_applied);
                builder.set_hide_blob_shadows(g_opt.shadows && g_opt.ray_tracing >= 1 && renderer.ray_tracing_available());
                builder.set_view_wide(g_opt.max_aspect > 4.0f / 3.0f + 0.01f ? std::max(1.0f, g_wide_view.load()) : 1.0f);
                builder.build(*next_state, own_scene);
                auto images = pack.take_new_images();
                if (!images.empty()) renderer.add_replacements(images);
                built_prep.reset();
                scene = &own_scene;
            }
            scene_seconds += qpc_seconds() - scene_from;
            built = next_state;
            renderer.set_scene(scene);
            // Debugging aid: LW_SPOTLOG=path writes the world viewport's spotlight every game frame
            static FILE* spot_log = getenv("LW_SPOTLOG") ? fopen(getenv("LW_SPOTLOG"), "w") : nullptr;
            if (spot_log) {
                int worlds = 0;
                for (const R3DViewport& v : scene->viewports)
                    if (v.world) {
                        worlds++;
                        fprintf(spot_log,
                                "%llu vp%d col %.0f%.0f%.0f c %.4f %.4f r %.4f %.4f start %.3f ext %.2f sun %.3f amb %.3f "
                                "fog %.5f/%.3f scroll %.3f far %.1f "
                                "light %.3f %.3f %.3f world %.3f %.3f %.3f\n",
                                (unsigned long long)built->frame, worlds, v.spot_color[0], v.spot_color[1], v.spot_color[2],
                                v.spot_center[0], v.spot_center[1], v.spot_radius[0], v.spot_radius[1], v.spot_start,
                                v.spot_extent, v.sun_intensity, v.ambient, v.fog_density, v.fog_ambient, v.scroll_fog,
                                v.scenery_far, v.sun[0], v.sun[1], v.sun[2],
                                v.world_axes[0] * v.sun[0] + v.world_axes[1] * v.sun[1] + v.world_axes[2] * v.sun[2],
                                v.world_axes[3] * v.sun[0] + v.world_axes[4] * v.sun[1] + v.world_axes[5] * v.sun[2],
                                v.world_axes[6] * v.sun[0] + v.world_axes[7] * v.sun[1] + v.world_axes[8] * v.sun[2]);
                    }
                if (!worlds) fprintf(spot_log, "%llu no world\n", (unsigned long long)built->frame);
                fflush(spot_log);
            }
            // Debugging aid: LW_FILM=first,count records that many game frames
            // as snapshots in out/film, to replay as a sequence
            static const char* film_rec = getenv("LW_FILM");
            static int film_saved = 0;
            if (film_rec && g_opt.replay.empty()) {
                unsigned long long first = 0;
                int count = 0, every = 1;   // optional third value: keep one frame in that many
                sscanf(film_rec, "%llu,%d,%d", &first, &count, &every);
                static unsigned long long film_last = 0;
                if (built->frame >= first && film_saved < count && (!film_saved || built->frame >= film_last + every)) {
                    film_last = built->frame;
                    CreateDirectoryA("out/film", nullptr);
                    char path[64];
                    snprintf(path, sizeof(path), "out/film/f_%06llu.bin", (unsigned long long)built->frame);
                    save_snapshot(path, *built, layers_shown->vram);
                    film_saved++;
                }
            }
        }
        int y0, y1, tex_dirty_rows = 0;
        if (g_texture_ram.take_dirty(tex_rows, y0, y1)) {
            double from = qpc_seconds();
            renderer.upload_texture_rows(tex_rows, y0, y1);
            parts.textures = qpc_seconds() - from;
            tex_dirty_rows = y1 - y0;
            g_tex_rows_second += tex_dirty_rows;
        }

        bool shot = g_opt.shot_frame && shown != ~0ull && shown >= g_opt.shot_frame;   // --shot N: screenshot and exit
        // A replayed scene is captured after a second on screen, once temporal
        // anti-aliasing (DLAA) has settled
        static uint64_t replay_presents = 0;
        // (debugging aid: LW_SETTLE=N images instead, to compare one image with the next: flicker)
        static const uint64_t settle = getenv("LW_SETTLE") ? strtoull(getenv("LW_SETTLE"), nullptr, 10) : 60;
        if (shot && !g_opt.replay.empty() && replay_presents++ < settle) shot = false;
        // Debugging aid: LW_SHOTS=N[,FROM[,TO]] saves one game frame in N as out/shots/<frame>.bmp
        static int shots_every = 0;
        static unsigned long long shots_from = 0, shots_to = ~0ull;
        static bool shots_parsed = false;
        if (!shots_parsed) {
            shots_parsed = true;
            if (const char* e = getenv("LW_SHOTS")) sscanf(e, "%d,%llu,%llu", &shots_every, &shots_from, &shots_to);
        }
        static uint64_t last_shot = 0;
        if (shots_every > 0 && shown != ~0ull && shown >= shots_from && shown <= shots_to &&
            shown >= last_shot + shots_every) {
            last_shot = shown;
            CreateDirectoryA("out/shots", nullptr);
            char path[64];
            snprintf(path, sizeof(path), "out/shots/%06llu.bmp", (unsigned long long)shown);
            presenter.capture_next(path);
        }
        if (shot) {
            presenter.capture_next("out/capture.bmp");
            g_texture_ram.dump("out/texture_ram.bin");
            dump_scene(*scene, layers_shown->back.data(), layers_shown->front.data(), "out/scene.txt");
            if (built && g_opt.replay.empty()) {
                CreateDirectoryA("out/snap", nullptr);
                save_snapshot("out/snap/shot.bin", *built, layers_shown->vram);
            }
        }
        // out/slow.log: each second where the game made fewer frames than it
        // should, or the window waited long for one, with what the frames took
        // to build (finding slowdowns players see without a debugging build)
        if (built && g_opt.replay.empty()) {
            static double second_from = 0;
            static uint64_t frame_from = 0;
            static int second_presents = 0;
            double now = qpc_seconds();
            second_presents++;
            if (second_from == 0) second_from = now, frame_from = built->frame, worst_parts = LoopParts{};
            double worst_gap = worst_parts.total;   // the longest time from one present call to the next
            if (now - second_from >= 1.0) {
                double game_fps = double(built->frame - frame_from) / (now - second_from);
                // (none: booting, or paused; a stutter: an image waited for
                // more than two refreshes of a 60 Hz screen, or under 55 a second)
                // (or, on a faster screen, a tenth of its refreshes missed: at
                // 120 Hz an image has 8.3 ms, and a GPU that takes 9 shows 90
                // a second without any gap that long)
                // (or a single refresh missed, whatever the screen: the calls
                // come up to a quarter of a refresh early or late by themselves,
                // so a gap of one and three quarters is an image shown twice.
                // At 120 Hz the 34 ms let stutters of up to three refreshes by)
                double refreshes = display_clock.period > 0 ? (now - second_from) / display_clock.period : 0;
                double gap_limit = display_clock.period > 0 ? std::min(0.034, 1.75 * display_clock.period) : 0.034;
                if (game_fps > 0 && (game_fps < 50 || worst_gap > gap_limit || second_presents < 55 || second_presents < 0.9 * refreshes)) {
                    static FILE* slow = fopen("out/slow.log", "a");
                    if (slow) {
                        fprintf(slow, "frame %llu: game %.1f fps, %d presents, worst gap %.0f ms, build %.1f ms (worst %.1f), %zu polygons\n",
                                (unsigned long long)built->frame, game_fps, second_presents, worst_gap * 1000,
                                g_build_ms.load(), g_build_worst_ms.exchange(0), scene ? scene->polygons : size_t(0));
                        std::string gpu = renderer.worst_gpu_frame();
                        if (!gpu.empty()) fprintf(slow, "    %s; texture rows uploaded %d\n", gpu.c_str(), g_tex_rows_second);
                        // Where that longest gap went: `wait` is the screen or the GPU
                        // late with an earlier image, the rest this thread's own work
                        const LoopParts& w = worst_parts;
                        double other = w.total - (w.wait + w.window + w.menu + w.input + w.scene + w.textures + w.gpu_wait +
                                                  w.acquire + w.record + w.present);
                        fprintf(slow, "    worst gap: wait %.1f window %.1f menu %.1f input %.1f (vibration %.1f) scene %.1f textures %.1f "
                                      "gpu wait %.1f acquire %.1f record %.1f present %.1f other %.1f\n",
                                w.wait * 1000, w.window * 1000, w.menu * 1000, w.input * 1000, w.vibration * 1000, w.scene * 1000,
                                w.textures * 1000, w.gpu_wait * 1000, w.acquire * 1000, w.record * 1000, w.present * 1000,
                                other * 1000);
                        fflush(slow);
                    }
                }
                g_build_worst_ms = 0;
                g_tex_rows_second = 0;
                renderer.worst_gpu_frame();
                worst_parts = LoopParts{};
                second_from = now, frame_from = built->frame, second_presents = 0;
            }
        }
        // The game makes ~57 frames per second; in between, the 3D scene is
        // drawn part way from the previous frame to the one built, at shown_time
        // on the game's own evenly spaced timeline
        static const char* fixed_interp = getenv("LW_INTERP");   // debugging aid: fixed position, e.g. 0.5
        if ((interpolating || fixed_interp) && built) {
            double now = qpc_seconds();
            double t = std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0);
            if (fixed_interp) t = std::clamp(atof(fixed_interp), 0.0, 1.0);
            renderer.set_interpolation(float(t));
            // The back layers move with the 3D when their 2D frames are those
            // of the two 3D frames it goes between
            bool paired = shift_to == built->vblank && shift_from == built_from;
            renderer.set_back_shift(paired ? float(1 - t) : 0.0f);
            if (g_pacing_log) pacing_sample(now, display, built->time - frame_period * (1 - t), *built);
        }
        static int film_presents = 0;
        if (!film.empty() && ++film_presents == 3) {
            char path[64];
            snprintf(path, sizeof(path), "out/film_out/%04zu.bmp", film_pos);
            presenter.capture_next(path);
        }
        presenter.frame([&](VkCommandBuffer cmd, VkImageView view, VkExtent2D extent) {
            renderer.record(cmd, view, extent);
            ui.record(cmd, view, extent);
            if (toast_shown) toast_layer.record(cmd, view, extent);   // over the menus too: their backdrop is opaque
        });
        {   // this image's turn, for out/slow.log
            const VkPresenter::LastFrame& f = presenter.last_frame();
            double now = qpc_seconds();
            parts.scene = scene_seconds;
            parts.gpu_wait = f.fence, parts.acquire = f.acquire, parts.record = f.record, parts.present = f.present;
            if (parts_skip > 0) parts_skip--;
            else if (parts_from > 0) {
                parts.total = now - parts_from;
                if (parts.total > worst_parts.total) worst_parts = parts;
            }
            parts_from = now;
        }
        if (g_frame_log) {
            const VkPresenter::LastFrame& f = presenter.last_frame();
            double t = interpolating && built ? std::clamp((shown_time - built->time) / frame_period + 1.0, 0.0, 1.0) : 1;
            frame_log("P %llu %.6f %.6f %.6f %.4f %llu %llu %.6f %d %d %.3f %d %.3f %.3f %.3f %.3f %.3f %.6f\n",
                      (unsigned long long)f.present_id, loop_from, display, shown_time, t,
                      (unsigned long long)(built ? built->frame : 0), (unsigned long long)(latest ? latest->frame : 0),
                      latest ? latest->time : 0.0, int(take), int(shown != shown_before), scene_seconds * 1000,
                      tex_dirty_rows, f.fence * 1000, f.acquire * 1000, f.record * 1000, f.present * 1000,
                      (qpc_seconds() - loop_from) * 1000, lateness);
            // (L: the loop's other parts for that image, as in out/slow.log, in ms)
            frame_log("L %llu %.3f %.3f %.3f %.3f %.3f %.3f\n", (unsigned long long)f.present_id, parts.wait * 1000,
                      parts.window * 1000, parts.menu * 1000, parts.input * 1000, parts.vibration * 1000, parts.textures * 1000);
            if (f.present_id) presented_id = f.present_id;
        }
        if (!film.empty() && film_presents >= 4) {   // next snapshot of the sequence
            film_presents = 0;
            if (++film_pos >= film.size() || !load_snapshot(film[film_pos].c_str())) break;
        }
        if (shot) {
            printf("Capture saved to out/capture.bmp (frame %llu, %zu polygons, %zu of %zu vertices moving)\n",
                   (unsigned long long)shown, scene->polygons, scene->moving, scene->vertices.size());
            break;
        }
        presents++;
        scene_total += scene_seconds;
        ULONGLONG now = GetTickCount64();
        if (now - last_title >= 1000) {
            // Where the time goes, in ms per frame: the game thread (CPU, without
            // its waits), the preparation thread (per game frame), this thread's
            // scene work and command recording, and the time blocked on the GPU
            // and the display (vsync)
            VkPresenter::Timing t = presenter.take_timing();
            uint64_t game_us = g_game_busy_us.exchange(0), game_n = g_game_frames.exchange(0);
            double per = t.frames ? 1000.0 / t.frames : 0;   // seconds summed over the presents -> ms per present
            double game_ms = game_n ? double(game_us) / 1000.0 / double(game_n) : 0;
            double scene_ms = presents ? scene_total * 1000.0 / double(presents) : 0;
            uint64_t prep_total = prep_us.exchange(0), prep_n = prep_frames.exchange(0);
            double prep_ms = prep_n ? double(prep_total) / 1000.0 / double(prep_n) : 0;
            char title[256];
            snprintf(title, sizeof(title),
                     "Sega Rally 2 Native by snicolast - %llu Hz display - frame %llu - %zu polygons | ms/frame: game %.1f, "
                     "prep %.1f, scene %.1f, render %.1f, GPU %.1f",
                     (unsigned long long)(presents * 1000 / (now - last_title)), (unsigned long long)(shown == ~0ull ? 0 : shown),
                     scene->polygons, game_ms, prep_ms, scene_ms, t.record * per, (t.gpu_wait + t.present) * per);
            SetWindowTextA(presenter.hwnd(), title);
            static ULONGLONG last_log = now;
            if (now - last_log >= 5000) {   // the same, in the log, every 5 s
                last_log = now;
                printf("[timing] frame %llu  %zu polygons  ms/frame: game %.2f  prep %.2f  scene %.2f  render %.2f  "
                       "GPU wait %.2f  present %.2f  (%llu Hz display)\n",
                       (unsigned long long)(shown == ~0ull ? 0 : shown), scene->polygons, game_ms, prep_ms, scene_ms, t.record * per,
                       t.gpu_wait * per, t.present * per, (unsigned long long)(presents * 1000 / (now - last_title)));
                fflush(stdout);
            }
            presents = 0;
            scene_total = 0;
            last_title = now;
        }
    }
    // Orderly exit: park the game thread, stop the audio, then the video
    stop_threads();
    g_quit = true;
    if (g_opt.replay.empty())
        for (int i = 0; i < 100 && !g_parked; i++) Sleep(10);   // a frame takes ~17 ms; 1 s is plenty
    g_audio.stop();
    stats_save();
    fflush(stdout);
    ui.shutdown();
    if (toast_shown) toast_layer.shutdown();
    toast.shutdown();
    frontend.shutdown();
    renderer.shutdown();
    presenter.shutdown();
    g_pads.shutdown();
    console_prompt();
    ExitProcess(0);
}
