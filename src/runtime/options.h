// Command-line options of SegaRally2.exe. One table in options.cpp drives the
// parser and --help; the menu (frontend.h) reads and writes the same struct
// while the game runs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

struct Options {
    // Run
    std::string rom_zip;             // --rom: srally2.zip, read directly (SR2_ROM_ZIP when not given)
    std::string data_dir = "data";   // --data: the ROM files tools/romtool.py extracts
    int frame_limit = INT32_MAX;     // --frames; with --headless, 600 by default
    bool headless = false;           // --headless: no window, full speed, report at the end
    bool menu = true;                // the main menu and save/settings.ini; off in test runs
    bool show_boot = false;          // --boot: the power-on screens before the SEGA logo
    int difficulty = -1;             // --difficulty: the game's number (0 normal, 1 hard, 2 very hard,
                                     // 3 very easy, 4 easy) held in the operator settings; -1: as the EEPROM says
    // The other operator settings PLAY's race set-up changes (GAME ASSIGNMENTS, docs/game_ram.md);
    // -1: as the EEPROM says
    int game_mode = -1;              // 0 NORMAL, 1 SPECIAL
    int default_view = -1;           // 0 DRIVER (in the car), 1 BEHIND
    int speed_kmh = -1;              // 0 MPH, 1 KM/H (the test menu's order)
    int free_play = -1;              // COIN ASSIGNMENTS: 1 FREE PLAY, 0 coins; -1 as the EEPROM says
    bool arcade = false;             // EXPERIENCE ARCADE ORIGINAL: the game as the cabinet ran it (app.cpp)
    // Picture
    float supersample = 1.0f;        // --scale: internal 3D resolution factor
    int msaa = 4;                    // --msaa: samples per pixel (1 = off)
    bool dlaa = false;               // --aa dlaa: NVIDIA DLAA instead of MSAA (also set by --aa dlss*)
    int dlss = 0;                    // --aa dlss*: 0 none (DLAA), 1 QUALITY, 2 BALANCED, 3 PERFORMANCE, 4 ULTRA PERFORMANCE
                                     // (the 3D drawn smaller and upscaled)
    int dlss_model = 0;              // --dlss-model: 0 auto, 1 K, 2 M, 3 L
    int aniso = 8;                   // --aniso: anisotropic taps (0 = no mipmaps, original look)
    float max_aspect = 16.0f / 9.0f; // --aspect: widest 3D view (4:3 = original)
    float pixel_aspect = 1.0f;       // the picture's pixels this much wider than tall (ASPECT 4:3: 1.032,
                                     // the cabinet's 496x384 on a 4:3 monitor)
    bool pixel_2d = false;           // --pixel2d: 2D layers without xBR upscaling
    bool hud_centre = false;         // --hud-centre: keep the HUD in the 4:3 area (otherwise its corners go out, up to a 16:9 view's edges)
    bool trigger_rumble = true;      // --no-trigger-rumble: Xbox pads through XInput, no motors in the triggers
    bool letterbox = false;          // --letterbox: keep the black cutscene bars
    bool classic_hud = false;        // --classic-hud: the ROM's own HUD and screens (pixel letters, no redrawn dials, signs...)
    bool classic_liveries = false;   // --classic-liveries: the cars with the ROM's own paint (no texture pack in textures/)
    bool interp = true;              // --no-interp: show game frames as they are
    bool dump_textures = false;      // --dump-textures: write new textures to out/textures
    float light_gain = 1.0f;         // --light-gain: light multiplier on the sun and the ambient
    float bloom = 1;                 // --bloom [N] / --no-bloom: glow around bright things
    float ao = 1;                    // --ao [N] / --no-ao: ambient occlusion (up to 1)
    float haze = 1;                  // --haze [N] / --no-haze: distance haze that hides the draw distance
    float saturation = 1.0f;         // --saturation: colour saturation (1 = original, the arcade's bright colours)
    float gamma = 1;                 // --gamma: 1 = original, higher = brighter
    bool hdr = true;                 // --no-hdr: the original's clipped lighting
    bool reflections = true;         // --no-reflections: no ray traced reflections on the cars
    int gi = 1;                      // --gi off|on: global illumination (the sun's light bounced off the world, ray traced)
    bool dust = true;                // --classic-dust: the game's own flat fans of dust, no particles
    bool sharpen = true;             // --no-sharpen: no sharpening of the 3D image
    bool soft = true;                // --no-soft: hard edges where translucent effects meet geometry
    bool shadows = true;             // --no-shadows: no sun shadows
    int ray_tracing = 2;             // --rt off|shadows|ao (--no-rt): traced sun shadows (1) and ambient occlusion (2)
    bool sun = true;                 // --no-sun: the game's own sunlight (SUN off)
    // Sound
    float volume = 5.0f;             // --volume: output gain
    float music = 5.0f;              // --music: the music board's gain
    int speakers = 0;                // --speakers stereo|surround|headphones: 0, 1, 2 (AudioStream::Speakers)
    int rear_boost = 6;              // --rear-boost: extra dB on the rear pair (0, 3, 6, 9, 12)
    bool cubic_samples = true;       // --linear-samples: the SCSP's own linear interpolation instead of cubic
    std::string wav_path;            // --wav: record the sound output
    // Debugging and tests
    uint64_t shot_frame = 0;         // --shot N: screenshot of frame N, then exit
    std::string replay;              // --replay: a debug snapshot (or a folder of them) instead of the game
    std::vector<std::string> auto_scripts;   // --auto coin@F,start@F (each occurrence)
    bool fixed_aim = false;          // --aim X,Y: the steering wheel's raw ADC value X at the start (Y is not used)
    int aim_x = 128, aim_y = 128;
    std::string diff;                // --diff FN@FRAME
    std::vector<std::string> traces; // --trace A,B,... (each occurrence)
};

// Handler for options the table does not know (a module with options of its
// own): returns how many arguments it consumed, 0 if the option is
// not its. `next` is the following argument or nullptr.
using ExtraOption = std::function<int(Options&, const std::string& name, const char* next)>;

// Fills `o` from the command line. Returns true to run the game; otherwise
// (--help, an unknown option or a bad value) the usage or the error has been
// printed and exit_code says how to end
bool parse_options(Options& o, int argc, char** argv, const ExtraOption& extra, int& exit_code);

// Whether an argument asks for a test run, which leaves the menu out so tests
// always start from the same defaults: the options marked so in the table,
// plus the names in `more`
bool options_disable_menu(int argc, char** argv, std::initializer_list<const char*> more = {});

void print_usage(FILE* out);
