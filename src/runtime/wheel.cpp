#include "wheel.h"

#include "gamepad.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

namespace {

constexpr const char* INI_FILE = "save/wheel.ini";
// Buttons read from the wheel: Moza and Fanatec bases go past 64 (a Moza HGP
// shifter on the base gives its gears as buttons 112-119, issue #62)
constexpr int MAX_BUTTONS = 128;
constexpr int NO_REST = 100000;            // a pedal's resting value not given in wheel.ini
constexpr int REST_END = 30000;            // a pedal's rest learnt late must be near one end of its axis

// Every control the wheel side can give, each an axis or a button on some
// device: the wheel unless wheel.ini names another for it (issue #73: pedals,
// shifters, hand brakes and button boxes that are USB devices of their own)
enum Ctl {
    STEER, GAS, BRAKE, HANDBRAKE, HANDBRAKE_AXIS, SHIFT_UP, SHIFT_DOWN, GEAR1, GEAR2, GEAR3, GEAR4,
    VIEW, COIN, START, MENU_ACCEPT, MENU_BACK, CTL_COUNT
};
// The older keys that give a whole group its device (pedals_device...)
enum Role { NO_ROLE = -1, PEDALS, SHIFTER, HANDBRAKE_ROLE };
struct CtlInfo {
    const char* key;   // in wheel.ini
    int def;           // its number by default (-1 = not used)
    int role;
};
const CtlInfo CTL[CTL_COUNT] = {
    {"steer_axis", 0, NO_ROLE},    {"gas_axis", 1, PEDALS},   {"brake_axis", 2, PEDALS},
    {"handbrake", 1, HANDBRAKE_ROLE}, {"handbrake_axis", -1, HANDBRAKE_ROLE},
    {"shift_up", 4, NO_ROLE},      {"shift_down", 5, NO_ROLE},
    {"gear1", -1, SHIFTER},        {"gear2", -1, SHIFTER},    {"gear3", -1, SHIFTER},    {"gear4", -1, SHIFTER},
    {"view", 3, NO_ROLE},          {"coin", 8, NO_ROLE},      {"start", 9, NO_ROLE},
    {"menu_accept", 0, NO_ROLE},   {"menu_back", 2, NO_ROLE}};

struct Config {
    std::string device;   // the wheel: part of its name (any case) or VID:PID, "#2" for the second of a name; "" = choose by itself
    // Pedals, an H shifter or a hand brake that are USB devices of their own: the
    // device of the controls in their group that name none
    std::string pedals_device, shifter_device, handbrake_device;
    int ctl[CTL_COUNT];
    std::string dev[CTL_COUNT];   // "key = 16 @ name": that control's own device
    float steer_range = 1.0f;     // fraction of the axis' travel (each side) that gives the full lock
    bool steer_invert = false;
    float steer_dead = 0.0f;
    bool combined_pedals = false;   // one axis: accelerator one way, brake the other
    bool combined_gas_negative = true;
    int gas_rest = NO_REST, brake_rest = NO_REST;   // set by hand in wheel.ini; else learnt
    int handbrake_rest = NO_REST;
    float ffb_strength = 0.7f;    // the game's motor force
    float ffb_spring = 0.25f;     // centring spring (0 = none)
    bool ffb_invert = false;
    bool setup_done = false;      // WHEEL SETUP finished once
    Config() {
        for (int c = 0; c < CTL_COUNT; c++) ctl[c] = CTL[c].def;
    }
};

const std::string& role_device(const Config& c, int r) {
    static const std::string none;
    return r == PEDALS ? c.pedals_device : r == SHIFTER ? c.shifter_device : r == HANDBRAKE_ROLE ? c.handbrake_device : none;
}

// The device a control is read from ("" = the wheel)
const std::string& ctl_device(const Config& c, int k) {
    static const std::string wheel;
    if (k == STEER) return wheel;   // the steering is what makes a device the wheel
    return !c.dev[k].empty() ? c.dev[k] : role_device(c, CTL[k].role);
}

void write_ini(const Config& c) {
    SDL_CreateDirectory("save");
    FILE* f = fopen(INI_FILE, "w");
    if (!f) return;
    auto line = [&](int k) {
        if (c.dev[k].empty()) fprintf(f, "%s = %d\n", CTL[k].key, c.ctl[k]);
        else fprintf(f, "%s = %d @ %s\n", CTL[k].key, c.ctl[k], c.dev[k].c_str());
    };
    auto rest = [&](const char* key, int v, const char* example) {
        if (v == NO_REST) fprintf(f, "; %s = %s\n", key, example);
        else fprintf(f, "%s = %d\n", key, v);
    };
    fprintf(f,
            "; Sega Rally 2 Native: steering wheel settings. OPTIONS > WHEEL sets them:\n"
            "; choose a control and move it. They can be edited here too.\n"
            "; Numbers of axes and buttons are SDL's, counted from 0 (-1 = not used). Run the game\n"
            "; with SR2_WHEEL_LOG=1 (set SR2_WHEEL_LOG=1 in a command prompt, then SegaRally2.exe)\n"
            "; to see which axis or button moves. A control on another USB device than the wheel\n"
            "; (pedals, a shifter, a hand brake, a button box) names it after an @: part of its\n"
            "; name as the [joystick] lines of the log show it, or its VID:PID, and #2 for the\n"
            "; second of two with the same name, for example: view = 16 @ RaceDeck\n"
            "\n"
            "; The wheel (the device with the steering). Empty: the game picks a device SDL\n"
            "; knows as a wheel, then one with force feedback, then the first.\n"
            "device = %s\n"
            "\n"
            "; The device of the pedals, the H shifter and the hand brake when their lines\n"
            "; below name none (empty: the wheel)\n"
            "pedals_device = %s\n"
            "shifter_device = %s\n"
            "handbrake_device = %s\n"
            "\n"
            "; Steering: the axis, and the part of its travel (each side) that gives the full lock.\n"
            "; The arcade wheel turns about 270 degrees: with your wheel set to 900 degrees in its\n"
            "; driver use about 0.3; set it to 270 in the driver and leave 1.0.\n",
            c.device.c_str(), c.pedals_device.c_str(), c.shifter_device.c_str(), c.handbrake_device.c_str());
    line(STEER);
    fprintf(f, "steer_range = %.3f\nsteer_invert = %d\nsteer_dead = %.2f\n\n", c.steer_range, c.steer_invert, c.steer_dead);
    fprintf(f,
            "; Pedals: one axis each, or combined_pedals = 1 for one axis (accelerator one way,\n"
            "; brake the other: combined_gas_negative = 1 when the accelerator moves it below\n"
            "; the middle). Their resting position is learnt when they are set in CONTROLS or\n"
            "; when the game starts (keep your feet off them then). If a pedal still reads as\n"
            "; pressed when released, give its resting value (from SR2_WHEEL_LOG).\n");
    line(GAS);
    line(BRAKE);
    fprintf(f, "combined_pedals = %d\ncombined_gas_negative = %d\n", c.combined_pedals, c.combined_gas_negative);
    rest("gas_rest", c.gas_rest, "32767");
    rest("brake_rest", c.brake_rest, "32767");
    fprintf(f, "\n; Hand brake: a button, or an analog one's axis (on when pulled past half its travel)\n");
    line(HANDBRAKE);
    line(HANDBRAKE_AXIS);
    rest("handbrake_rest", c.handbrake_rest, "-32768");
    fprintf(f, "\n; Buttons\n");
    for (int k : {SHIFT_UP, SHIFT_DOWN, VIEW, COIN, START}) line(k);
    fprintf(f,
            "; Menus and the pause screen: the hat (d-pad) moves, menu_accept and menu_back\n"
            "; choose and go back, start resumes, the shift paddles change the settings tab\n");
    line(MENU_ACCEPT);
    line(MENU_BACK);
    fprintf(f, "; H shifter (gears 1-4): moving it to a gear or to neutral sets the gear; the\n"
               "; paddles still change it while it stays put\n");
    for (int k = GEAR1; k <= GEAR4; k++) line(k);
    fprintf(f,
            "\n; Force feedback (0..1): the game's own force (bumps, slides, the road) and a\n"
            "; centring spring. ffb_invert = 1 if the wheel pulls the wrong way.\n"
            "ffb_strength = %.2f\nffb_spring = %.2f\nffb_invert = %d\n"
            "\n; OPTIONS > WHEEL > WHEEL SETUP was finished once\nsetup_done = %d\n",
            c.ffb_strength, c.ffb_spring, c.ffb_invert, c.setup_done);
    fclose(f);
}

std::string trimmed(const char* s) {
    std::string out = s;
    while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
    return out;
}

Config read_ini(bool create = true) {
    Config c;
    FILE* f = fopen(INI_FILE, "r");
    if (!f) {
        if (!create) return c;
        write_ini(c);
        printf("[wheel] settings written to %s\n", INI_FILE);
        return c;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], text[256];
        auto text_key = [&](const char* name, std::string& out) {
            std::string format = std::string(" ") + name + " = %255[^\r\n]";
            if (sscanf(line, format.c_str(), text) != 1) return false;
            out = trimmed(text);
            return true;
        };
        if (text_key("device", c.device) || text_key("pedals_device", c.pedals_device) ||
            text_key("shifter_device", c.shifter_device) || text_key("handbrake_device", c.handbrake_device))
            continue;
        float v;
        if (line[0] == ';' || sscanf(line, " %63[a-z_0-9] = %f", key, &v) != 2) continue;
        std::string k = key;
        int i = int(std::lround(v));
        const char* at = strchr(line, '@');
        bool is_ctl = false;
        for (int n = 0; n < CTL_COUNT; n++) {
            if (k != CTL[n].key) continue;
            c.ctl[n] = i;
            c.dev[n] = at ? trimmed(at + 1) : std::string();
            is_ctl = true;
        }
        if (is_ctl) continue;
        if (k == "steer_range") c.steer_range = std::clamp(v, 0.05f, 1.0f);
        else if (k == "steer_invert") c.steer_invert = i != 0;
        else if (k == "steer_dead") c.steer_dead = std::clamp(v, 0.0f, 0.5f);
        else if (k == "combined_pedals") c.combined_pedals = i != 0;
        else if (k == "combined_gas_negative") c.combined_gas_negative = i != 0;
        else if (k == "gas_rest") c.gas_rest = std::clamp(i, -32768, 32767);
        else if (k == "brake_rest") c.brake_rest = std::clamp(i, -32768, 32767);
        else if (k == "handbrake_rest") c.handbrake_rest = std::clamp(i, -32768, 32767);
        else if (k == "ffb_strength") c.ffb_strength = std::clamp(v, 0.0f, 1.0f);
        else if (k == "ffb_spring") c.ffb_spring = std::clamp(v, 0.0f, 1.0f);
        else if (k == "ffb_invert") c.ffb_invert = i != 0;
        else if (k == "setup_done") c.setup_done = i != 0;
    }
    fclose(f);
    return c;
}

struct Wheel {
    SDL_Joystick* joy = nullptr;
    uint32_t id = 0;
    std::string label;   // in the log: "wheel", or the device's name
    int axes = 0, buttons = 0;
    int16_t rest[16] = {};
    bool rest_known[16] = {};   // false: rest is still SDL's first value, which can be a stray 0
    int menu_dir = -1;          // the hat in the menus, repeated while held
    double next_repeat = 0;
    bool was_down[MAX_BUTTONS] = {}, down[MAX_BUTTONS] = {};
    int16_t last_axis[16] = {};
    SDL_Haptic* haptic = nullptr;
    int constant = -1, spring = -1;
    int16_t level = 0;
    bool rumble = false;   // no haptic effects: shakes as rumble
    int score = 0;         // how sure we are it is the wheel (pick_score)
};

bool g_ok = false;
Config g_cfg;
Wheel g_wheel;   // one wheel: the best that arrives
// The other devices that controls name in wheel.ini (pedals, shifters, hand
// brakes, button boxes), each opened once whatever controls share it
constexpr int MAX_DEVICES = 8;
Wheel g_devices[MAX_DEVICES];
Wheel g_none;                 // a device named in wheel.ini but not plugged in: reads nothing
Wheel* g_src[CTL_COUNT] = {};  // where each control is read (resolve)
int g_last_gear = -2;         // the H shifter's position at the last poll
WheelLive g_live;             // wheel_live()
WheelState g_state;
std::atomic<float> g_force{0}, g_shake{0};   // from the game thread (runtime.cpp ffb_frame)
bool g_focused = false;
// WHEEL SETUP's force test: a push as the game would give it (+ to the right), instead of the game's
bool g_testing = false;
float g_test_force = 0;
// The force sent while driving: the last drive's and this one's (GAIN's hint)
struct FfbStats {
    double frames = 0, clipped = 0;
    float peak = 0;
} g_ffb_last, g_ffb_now;
const bool g_log = getenv("SR2_WHEEL_LOG") != nullptr;

// A joystick that is a wheel, or anything with axes that SDL cannot use as a
// gamepad (pedals and wheels that SDL does not know as such)
bool is_wheel(uint32_t id) {
    SDL_JoystickType t = SDL_GetJoystickTypeForID(id);
    if (t == SDL_JOYSTICK_TYPE_WHEEL) return true;
    return !SDL_IsGamepad(id) && t != SDL_JOYSTICK_TYPE_THROTTLE;
}

// A device named in wheel.ini: VID:PID (hex), or part of its name in any case
bool matches_plain(uint32_t id, std::string want) {
    if (want.empty()) return false;
    unsigned vid = 0, pid = 0;
    char extra = 0;
    if (sscanf(want.c_str(), "%4x:%4x%c", &vid, &pid, &extra) == 2)
        return SDL_GetJoystickVendorForID(id) == vid && SDL_GetJoystickProductForID(id) == pid;
    const char* n = SDL_GetJoystickNameForID(id);
    std::string name = n ? n : "";
    for (char& c : name) c = char(std::tolower((unsigned char)c));
    for (char& c : want) c = char(std::tolower((unsigned char)c));
    return name.find(want) != std::string::npos;
}

// "name #2": the second of the devices that match "name" (in the order SDL
// found them), for drivers that give one base as two joysticks with the same name
bool matches(uint32_t id, const std::string& want) {
    size_t hash = want.rfind('#');
    int n = hash != std::string::npos && hash + 1 < want.size() ? atoi(want.c_str() + hash + 1) : 0;
    if (n < 1) return matches_plain(id, want);
    std::string base = trimmed(want.substr(0, hash).c_str());
    if (!matches_plain(id, base)) return false;
    int count = 0, nth = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++)
        if (ids[i] <= id && matches_plain(ids[i], base)) nth++;
    SDL_free(ids);
    return nth == n;
}

// How wheel.ini names a device: its name, with #N when another has the same
std::string device_name(uint32_t id) {
    const char* n = SDL_GetJoystickNameForID(id);
    std::string name = n && *n ? n : "";
    if (name.empty()) {
        char vp[16];
        snprintf(vp, sizeof vp, "%04X:%04X", SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id));
        return vp;
    }
    int count = 0, same = 0, nth = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        const char* o = SDL_GetJoystickNameForID(ids[i]);
        if (!o || name != o) continue;
        same++;
        if (ids[i] <= id) nth++;
    }
    SDL_free(ids);
    return same > 1 ? name + " #" + std::to_string(nth) : name;
}

float axis(const Wheel& w, int a) {
    return a >= 0 && a < w.axes ? float(SDL_GetJoystickAxis(w.joy, a)) : 0.0f;
}

// 0..1 from the resting value towards whichever end the pedal goes
float pedal(const Wheel& w, int a) {
    if (a < 0 || a >= w.axes) return 0.0f;
    float v = axis(w, a), r = float(w.rest[std::min(a, 15)]);
    float t;
    if (r < -16000.0f) t = (v - r) / (32767.0f - r);
    else if (r > 16000.0f) t = (r - v) / (r + 32768.0f);
    else t = std::fabs(v - r) / 32767.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.03f ? 0.0f : (t - 0.03f) / 0.97f;
}

// The centring spring as ffb_spring has it (none at 0): made again when
// CENTRING SPRING changes, so it is felt at once
void make_spring(Wheel& w) {
    if (!w.haptic) return;
    if (w.spring >= 0) SDL_DestroyHapticEffect(w.haptic, w.spring);
    w.spring = -1;
    if (!(SDL_GetHapticFeatures(w.haptic) & SDL_HAPTIC_SPRING) || g_cfg.ffb_spring <= 0) return;
    SDL_HapticEffect e;
    SDL_zero(e);
    e.type = SDL_HAPTIC_SPRING;
    e.condition.direction.type = SDL_HAPTIC_CARTESIAN;
    e.condition.direction.dir[0] = 1;
    e.condition.length = SDL_HAPTIC_INFINITY;
    uint16_t sat = uint16_t(0xFFFF * g_cfg.ffb_spring);
    int16_t coeff = int16_t(0x7FFF * g_cfg.ffb_spring);
    e.condition.right_sat[0] = e.condition.left_sat[0] = sat;
    e.condition.right_coeff[0] = e.condition.left_coeff[0] = coeff;
    w.spring = SDL_CreateHapticEffect(w.haptic, &e);
    if (w.spring >= 0) SDL_RunHapticEffect(w.haptic, w.spring, 1);
}

void start_haptics(Wheel& w) {
    w.haptic = SDL_OpenHapticFromJoystick(w.joy);
    if (!w.haptic) {
        w.rumble = true;
        printf("[wheel] no force feedback effects (%s): road shakes as rumble if it has motors\n", SDL_GetError());
        return;
    }
    uint32_t f = SDL_GetHapticFeatures(w.haptic);
    if (f & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(w.haptic, 0);   // the spring below instead
    if (f & SDL_HAPTIC_GAIN) SDL_SetHapticGain(w.haptic, 100);
    if (f & SDL_HAPTIC_CONSTANT) {
        SDL_HapticEffect e;
        SDL_zero(e);
        e.type = SDL_HAPTIC_CONSTANT;
        e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
        e.constant.direction.dir[0] = 1;
        e.constant.length = SDL_HAPTIC_INFINITY;
        e.constant.level = 0;
        w.constant = SDL_CreateHapticEffect(w.haptic, &e);
        if (w.constant >= 0) SDL_RunHapticEffect(w.haptic, w.constant, 1);
    }
    make_spring(w);
    if (w.constant < 0 && w.spring < 0) w.rumble = true;
    printf("[wheel] force feedback: %s%s%s\n", w.constant >= 0 ? "constant force " : "", w.spring >= 0 ? "spring " : "",
           w.rumble ? "rumble" : "");
}

void stop_haptics(Wheel& w) {
    if (!w.haptic) return;
    SDL_StopHapticEffects(w.haptic);
    SDL_CloseHaptic(w.haptic);
    w.haptic = nullptr;
    w.constant = w.spring = -1;
}

// Some wheels drop the effects after a while, a hard knock or a full turn
// (direct drive bases: a Simagic in issue #33, a Thrustmaster T598 in #93),
// and nothing starts them again: an update only changes the force of an
// effect that is no longer playing, so the wheel stays loose until the game
// restarts. As other recomps do, they are run again every second while
// driving, even if the wheel says they still play, and at once when an
// update fails
void restart_haptics(Wheel& w) {
    static bool logged = false;
    bool ok = true;
    if (w.constant >= 0) ok &= SDL_RunHapticEffect(w.haptic, w.constant, 1);
    if (w.spring >= 0) ok &= SDL_RunHapticEffect(w.haptic, w.spring, 1);
    if (!ok && !logged) {
        printf("[wheel] force feedback: could not start the effects again (%s)\n", SDL_GetError());
        logged = true;
    }
}

void apply_force(Wheel& w) {
    float f = g_testing ? -g_test_force   // (the game's sign: runtime.cpp gives the wheel -force)
              : g_focused ? g_force.load() * g_cfg.ffb_strength : 0.0f;
    if (g_focused && !g_testing && std::fabs(g_force.load()) > 0.01f) {   // the game pushing: how hard
        g_ffb_now.frames++;
        g_ffb_now.clipped += std::fabs(f) >= 0.98f;
        g_ffb_now.peak = std::max(g_ffb_now.peak, std::fabs(f));
    }
    if (g_cfg.ffb_invert) f = -f;
    bool restart = false;
    if (w.haptic && w.constant >= 0) {
        int16_t level = int16_t(std::clamp(f, -1.0f, 1.0f) * 32767.0f);
        if (std::abs(level - w.level) > 300 || (level == 0 && w.level != 0)) {
            SDL_HapticEffect e;
            SDL_zero(e);
            e.type = SDL_HAPTIC_CONSTANT;
            e.constant.direction.type = SDL_HAPTIC_CARTESIAN;
            e.constant.direction.dir[0] = 1;
            e.constant.length = SDL_HAPTIC_INFINITY;
            e.constant.level = level;
            Uint64 from = SDL_GetTicksNS();
            if (SDL_UpdateHapticEffect(w.haptic, w.constant, &e)) {
                w.level = level;
            } else {   // tried again on the next poll, after the restart
                static bool logged = false;
                if (!logged) printf("[wheel] force feedback: an update failed (%s), starting the effects again\n", SDL_GetError());
                logged = true;
                restart = true;
            }
            g_vibration_seconds += double(SDL_GetTicksNS() - from) * 1e-9;
        }
    }
    if (w.haptic && (w.constant >= 0 || w.spring >= 0)) {
        static Uint64 next_restart = 0;
        Uint64 now = SDL_GetTicksNS();
        if ((g_focused || g_testing) && now >= next_restart) restart = true;
        if (restart) {
            restart_haptics(w);
            g_vibration_seconds += double(SDL_GetTicksNS() - now) * 1e-9;
            next_restart = now + SDL_NS_PER_SECOND;
        }
    }
    float shake = g_shake.exchange(0.0f);
    if (w.rumble && g_focused && shake > 0.08f) {
        uint16_t m = uint16_t(std::min(1.0f, shake * g_cfg.ffb_strength) * 0xFFFF);
        Uint64 from = SDL_GetTicksNS();
        SDL_RumbleJoystick(w.joy, m, m / 2, 60);
        g_vibration_seconds += double(SDL_GetTicksNS() - from) * 1e-9;
    }
}

// Tests without a wheel: SR2_FAKE_WHEEL=steer,gas,brake,hand[,at] plugs in an
// SDL virtual wheel (axis 0 steering, 1 accelerator, 2 brake, button 1 hand
// brake, as wheel.ini's defaults), pedals at rest, that goes to those values
// (steering -1..1 of its axis, the rest 0..1) `at` seconds after the start (1).
// SR2_FAKE_SCRIPT="2.5 a0 0.25; 4 a1 1; 4.5 a1 -1; 6 b4 1; 6.2 b4 0" then moves it
// on: at those seconds after it is plugged in, axis N to a raw -1..1, button N
// down (1) or up (0), to go through WHEEL SETUP
struct Fake {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    float v[4] = {};
    double at = 1.0, from = 0;
    bool moved = false;
    struct Event {
        double t;
        bool button;
        int n;
        float v;
    };
    std::vector<Event> script;
    size_t next = 0;
} g_fake;

void fake_init() {
    const char* e = std::getenv("SR2_FAKE_WHEEL");
    if (!e || !*e) return;
    Fake& f = g_fake;
    std::sscanf(e, "%f,%f,%f,%f,%lf", &f.v[0], &f.v[1], &f.v[2], &f.v[3], &f.at);
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_WHEEL;
    d.naxes = 3;
    d.nbuttons = 12;
    d.nhats = 1;
    d.name = "SR2 Test Wheel";
    f.id = SDL_AttachVirtualJoystick(&d);
    f.joy = f.id ? SDL_OpenJoystick(f.id) : nullptr;
    if (!f.joy) {
        printf("[wheel] SR2_FAKE_WHEEL: no virtual wheel (%s)\n", SDL_GetError());
        return;
    }
    if (const char* sc = std::getenv("SR2_FAKE_SCRIPT")) {
        for (const char* p = sc; *p;) {
            double t;
            char kind;
            int n, used = 0;
            float v;
            if (std::sscanf(p, " %lf %c%d %f%n", &t, &kind, &n, &v, &used) != 4 || used <= 0) break;
            f.script.push_back({t, kind == 'b', n, v});
            p += used;
            while (*p == ' ' || *p == ';') p++;
        }
        printf("[wheel] SR2_FAKE_SCRIPT: %d moves\n", int(f.script.size()));
    }
    SDL_SetJoystickVirtualAxis(f.joy, 1, -32768);   // the pedals let go
    SDL_SetJoystickVirtualAxis(f.joy, 2, -32768);
    f.from = double(SDL_GetTicksNS()) * 1e-9;
    printf("[wheel] SR2_FAKE_WHEEL: virtual wheel plugged in, moving in %.1f s\n", f.at);
}

void fake_poll() {
    Fake& f = g_fake;
    if (!f.joy) return;
    double t = double(SDL_GetTicksNS()) * 1e-9 - f.from;
    for (; f.next < f.script.size() && t >= f.script[f.next].t; f.next++) {
        const Fake::Event& e = f.script[f.next];
        if (e.button) SDL_SetJoystickVirtualButton(f.joy, e.n, e.v >= 0.5f);
        else SDL_SetJoystickVirtualAxis(f.joy, e.n, int16_t(std::clamp(e.v, -1.0f, 1.0f) * 32767.0f));
        printf("[wheel] SR2_FAKE_SCRIPT %.1f s: %s %d = %.2f\n", t, e.button ? "button" : "axis", e.n, e.v);
    }
    if (f.moved || t < f.at) return;
    f.moved = true;
    auto pedal_raw = [](float t) { return int16_t(std::clamp(-32768.0f + std::clamp(t, 0.0f, 1.0f) * 65535.0f, -32768.0f, 32767.0f)); };
    SDL_SetJoystickVirtualAxis(f.joy, 0, int16_t(std::clamp(f.v[0], -1.0f, 1.0f) * 32767.0f));
    SDL_SetJoystickVirtualAxis(f.joy, 1, pedal_raw(f.v[1]));
    SDL_SetJoystickVirtualAxis(f.joy, 2, pedal_raw(f.v[2]));
    SDL_SetJoystickVirtualButton(f.joy, 1, f.v[3] >= 0.5f);
    printf("[wheel] SR2_FAKE_WHEEL: steering %.2f, gas %.2f, brake %.2f, hand brake %s\n", f.v[0], f.v[1], f.v[2],
           f.v[3] >= 0.5f ? "on" : "off");
}

}  // namespace

void wheels_init() {
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) {
        printf("[wheel] SDL could not initialize joysticks: %s\n", SDL_GetError());
        return;
    }
    g_ok = true;   // the wheels already plugged in arrive as SDL_EVENT_JOYSTICK_ADDED
    g_cfg = read_ini(false);   // for OPTIONS > WHEEL before any wheel arrives (written once one does, or a row changes)
    for (Wheel*& s : g_src) s = &g_none;
    fake_init();
}

bool wheel_is_ours(uint32_t id) {
    if (g_wheel.joy && g_wheel.id == id) return true;
    for (const Wheel& d : g_devices)
        if (d.joy && d.id == id) return true;
    return false;
}

// Every joystick SDL finds, wheel or not, so that a log tells why a wheel
// was not taken (issue #33)
static void log_joystick(uint32_t id) {
    static const char* const TYPES[] = {"unknown", "gamepad", "wheel", "arcade stick", "flight stick",
                                        "dance pad", "guitar", "drum kit", "arcade pad", "throttle"};
    SDL_JoystickType t = SDL_GetJoystickTypeForID(id);
    const char* name = SDL_GetJoystickNameForID(id);
    printf("[joystick] %s (%04X:%04X, %s%s)\n", name ? name : "?", SDL_GetJoystickVendorForID(id),
           SDL_GetJoystickProductForID(id), unsigned(t) < 10 ? TYPES[t] : "?",
           SDL_IsGamepad(id) ? ", mapped as a gamepad" : "");
}

// Which of the joysticks is the wheel, when there are several (a shifter,
// pedals, a button box... issue #33): the one named in wheel.ini, then one
// SDL knows as a wheel, then one with force feedback, then the first.
// Between two that rank the same (a Fanatec base seen as two joysticks with
// one name, issue #73), the one with force feedback
static int pick_score(uint32_t id, SDL_Joystick* j, const char** why) {
    int haptic = SDL_IsJoystickHaptic(j) ? 1 : 0;
    if (matches(id, g_cfg.device)) return *why = haptic ? "named in wheel.ini, with force feedback" : "named in wheel.ini", 8 + haptic;
    if (SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL)
        return *why = haptic ? "a wheel for SDL, with force feedback" : "a wheel for SDL", 6 + haptic;
    if (haptic) return *why = "it has force feedback", 4;
    return *why = "the first joystick", 2;
}

// Where each control is read: the wheel, the device it names, or nothing
// when that device is not plugged in
static void resolve() {
    for (int k = 0; k < CTL_COUNT; k++) {
        const std::string& want = ctl_device(g_cfg, k);
        Wheel* src = &g_none;
        if (want.empty() || (g_wheel.joy && matches(g_wheel.id, want))) src = &g_wheel;
        else
            for (Wheel& d : g_devices)
                if (d.joy && matches(d.id, want)) src = &d;
        g_src[k] = src;
    }
}

// Axes, buttons and the axes' resting values of a device just opened
static void init_device(Wheel& w, SDL_Joystick* j, uint32_t id) {
    w = Wheel{};
    w.joy = j;
    w.id = id;
    w.axes = SDL_GetNumJoystickAxes(j);
    w.buttons = std::clamp(SDL_GetNumJoystickButtons(j), 0, MAX_BUTTONS);
    for (int a = 0; a < std::min(w.axes, 16); a++) {
        int16_t v = 0;
        if (!SDL_GetJoystickAxisInitialState(j, a, &v)) v = SDL_GetJoystickAxis(j, a);
        // Some wheels (a Logitech G29's pedals) give 0 here, and keep 0 until
        // their first report, seconds later: a pedal's 0 is checked in wheels_poll
        w.rest[a] = w.last_axis[a] = v;
        w.rest_known[a] = v != 0;
    }
}

// gas_rest / brake_rest / handbrake_rest from wheel.ini, on whichever device has them
static void set_rests() {
    auto set_rest = [](int k, int v) {
        Wheel& d = *g_src[k];
        int a = g_cfg.ctl[k];
        if (v == NO_REST || !d.joy || a < 0 || a >= std::min(d.axes, 16)) return;
        d.rest[a] = int16_t(v);
        d.rest_known[a] = true;
    };
    set_rest(GAS, g_cfg.gas_rest);
    set_rest(BRAKE, g_cfg.brake_rest);
    set_rest(HANDBRAKE_AXIS, g_cfg.handbrake_rest);
}

static void not_used(SDL_Joystick* j) {
    const char* name = SDL_GetJoystickName(j);
    printf("[wheel] %s not used (if you use it in the car, set its controls in OPTIONS > WHEEL)\n",
           name ? name : "?");
}

static void close_wheel() {
    stop_haptics(g_wheel);
    SDL_CloseJoystick(g_wheel.joy);
    g_wheel = Wheel{};
    g_state = WheelState{};
}

// The controls a device was opened for, for the log
static std::string uses_of(const Wheel& d) {
    static const char* const NAMES[CTL_COUNT] = {"steering", "accelerator", "brake", "hand brake", "hand brake",
                                                 "shift up", "shift down", "gear 1", "gear 2", "gear 3", "gear 4",
                                                 "view", "coin", "start", "menu accept", "menu back"};
    std::string out;
    for (int k = 0; k < CTL_COUNT; k++) {
        if (g_src[k] != &d || g_cfg.ctl[k] < 0 || out.find(NAMES[k]) != std::string::npos) continue;
        out += (out.empty() ? "" : ", ") + std::string(NAMES[k]);
    }
    return out.empty() ? "nothing yet" : out;
}

static void add_device(uint32_t id, bool quiet) {
    if (!g_ok) return;
    if (!quiet) log_joystick(id);
    if (wheel_is_ours(id)) return;
    g_cfg = read_ini();
    // A device that is the wheel (named as it, or a wheel for SDL) stays the
    // wheel: controls that name it read it from there
    bool wheel_like = matches(id, g_cfg.device) || SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL;
    bool wanted = false;
    for (int k = 0; k < CTL_COUNT && !wheel_like; k++) wanted |= matches(id, ctl_device(g_cfg, k));
    if (wanted) {
        Wheel* slot = nullptr;
        for (Wheel& d : g_devices)
            if (!d.joy && !slot) slot = &d;
        SDL_Joystick* j = slot ? SDL_OpenJoystick(id) : nullptr;
        if (!j) {
            printf("[wheel] %s not opened: %s\n", SDL_GetJoystickNameForID(id), slot ? SDL_GetError() : "too many devices");
            return;
        }
        init_device(*slot, j, id);
        const char* name = SDL_GetJoystickName(j);
        slot->label = name ? name : "device";
        resolve();
        set_rests();
        printf("[wheel] %s: %d axes, %d buttons, for the %s (named in %s)\n", slot->label.c_str(), slot->axes,
               slot->buttons, uses_of(*slot).c_str(), INI_FILE);
        return;
    }
    if (!is_wheel(id)) return;
    SDL_Joystick* j = SDL_OpenJoystick(id);
    if (!j) return;
    int axes = SDL_GetNumJoystickAxes(j);
    if (axes < 1) {   // buttons only: not a wheel
        not_used(j);
        SDL_CloseJoystick(j);
        return;
    }
    const char* why = "";
    int score = pick_score(id, j, &why);
    if (g_wheel.joy && score <= g_wheel.score) {   // the one we have is as good
        not_used(j);
        SDL_CloseJoystick(j);
        return;
    }
    if (g_wheel.joy) {
        printf("[wheel] %s replaces %s\n", SDL_GetJoystickName(j), g_state.name.c_str());
        close_wheel();
    }
    Wheel& w = g_wheel;
    init_device(w, j, id);
    w.label = "wheel";
    w.score = score;
    resolve();
    set_rests();
    const char* name = SDL_GetJoystickName(j);
    g_state = WheelState{};
    g_state.connected = true;
    g_state.name = name ? name : "Wheel";
    printf("[wheel] %s: %d axes, %d buttons, taken as the wheel: %s (settings in %s)\n", g_state.name.c_str(), axes,
           w.buttons, why, INI_FILE);
    start_haptics(w);
}

void wheels_added(uint32_t id) { add_device(id, false); }

void wheels_removed(uint32_t id) {
    for (Wheel& d : g_devices) {
        if (!d.joy || d.id != id) continue;
        printf("[wheel] %s disconnected\n", d.label.c_str());
        SDL_CloseJoystick(d.joy);
        d = Wheel{};
    }
    if (g_wheel.joy && g_wheel.id == id) {
        close_wheel();
        printf("[wheel] disconnected\n");
    }
    resolve();
}

// After the settings change (OPTIONS > WHEEL): every joystick taken again
// as they now say
static void reopen_all() {
    for (Wheel& d : g_devices) {
        if (d.joy) SDL_CloseJoystick(d.joy);
        d = Wheel{};
    }
    if (g_wheel.joy) close_wheel();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) add_device(ids[i], true);
    SDL_free(ids);
    resolve();
}

// ---- OPTIONS > WHEEL: a control set by moving it, and WHEEL SETUP ----

namespace {

// The settings after the controls (Row::ctl below 0)
enum { SET_GAIN = -1, SET_SPRING = -2, SET_INVERT = -3, SET_ROTATION = -4, SET_DEAD = -5, SET_SETUP = -6 };
struct Row {
    const char* group;
    const char* label;
    int ctl;   // a control, or a SET_ setting
};
constexpr const char* SETUP = "SETUP";
constexpr const char* CONTROLS = "CONTROLS";
constexpr const char* FFB = "FORCE FEEDBACK";
constexpr const char* ADVANCED = "ADVANCED";
const Row ROWS[] = {{SETUP, "WHEEL SETUP", SET_SETUP},
                    {CONTROLS, "STEERING", STEER},      {CONTROLS, "ACCELERATOR", GAS},   {CONTROLS, "BRAKE", BRAKE},
                    {CONTROLS, "HAND BRAKE", HANDBRAKE}, {CONTROLS, "SHIFT UP", SHIFT_UP}, {CONTROLS, "SHIFT DOWN", SHIFT_DOWN},
                    {CONTROLS, "GEAR 1", GEAR1},         {CONTROLS, "GEAR 2", GEAR2},      {CONTROLS, "GEAR 3", GEAR3},
                    {CONTROLS, "GEAR 4", GEAR4},         {CONTROLS, "VIEW CHANGE", VIEW},  {CONTROLS, "START", START},
                    {FFB, "GAIN", SET_GAIN},             {FFB, "CENTRING SPRING", SET_SPRING}, {FFB, "INVERT FORCES", SET_INVERT},
                    {ADVANCED, "WHEEL ROTATION", SET_ROTATION}, {ADVANCED, "STEERING DEAD ZONE", SET_DEAD}};
constexpr int ROW_COUNT = int(sizeof ROWS / sizeof ROWS[0]);
constexpr int MAX_DEAD = 20;   // STEERING DEAD ZONE: up to 20 %
// Listening: devices just opened settle first; one control's row gives up after a
// while (WHEEL SETUP waits: each step can be skipped); a control taken is shown a moment
constexpr double SETTLE = 0.4, LISTEN_FOR = 10.0, SHOW_DONE = 1.1;
constexpr double HOLD_FOR = 1.5;                  // WHEEL ROTATION: held still this long
constexpr double HANDS_OFF = 2.0, PUSH_FOR = 0.7;  // the force test: time to let go, then the push
constexpr float PUSH = 0.45f;                     // its strength (of the wheel's most)

bool is_axis_ctl(int k) { return k == STEER || k == GAS || k == BRAKE || k == HANDBRAKE_AXIS; }

const char* ctl_label(int k) {
    if (k == HANDBRAKE_AXIS) k = HANDBRAKE;
    for (const Row& r : ROWS)
        if (r.ctl == k) return r.label;
    return CTL[k].key;
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper((unsigned char)c));
    return s;
}

// While listening: every joystick open (the ones not in use only for this),
// and what each one read when it started
struct Snap {
    SDL_Joystick* joy;
    uint32_t id;
    int16_t axis[16];
    bool button[MAX_BUTTONS];
    int axes, buttons;
};

// WHEEL SETUP's steps, in order (the gears skipped together)
enum Step { ST_INTRO, ST_STEER, ST_ROTATION, ST_GAS, ST_BRAKE, ST_HANDBRAKE, ST_SHIFT_UP, ST_SHIFT_DOWN,
            ST_GEAR1, ST_GEAR2, ST_GEAR3, ST_GEAR4, ST_VIEW, ST_FORCE, ST_FORCE_ASK, ST_DONE, ST_COUNT };
const int STEP_CTL[ST_COUNT] = {-1, STEER, -1, GAS, BRAKE, HANDBRAKE, SHIFT_UP, SHIFT_DOWN,
                                GEAR1, GEAR2, GEAR3, GEAR4, VIEW, -1, -1, -1};
const int STAGE[ST_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 9, 10, 10, 0};   // "STEP n OF 10"
constexpr int STAGES = 10;

// The window over the WHEEL tab: one control listening, or WHEEL SETUP
struct Prompt {
    bool on = false;
    bool setup = false;
    int step = ST_INTRO;
    int ctl = -1;             // the control listened for now (-1: none)
    double since = 0;         // when it started listening, or this step began
    std::vector<Snap> snaps;
    float progress = 0;       // what moves most, towards being taken (or the hold)
    std::string moving;
    double done_at = -1;      // a control just taken: shown until done_at + SHOW_DONE
    std::string done, note;
    float hold = 0;           // ST_ROTATION: the turn held, since hold_since
    double hold_since = -1;
    double push_from = -1;    // ST_FORCE: when the push starts
    std::vector<std::string> summary;   // WHEEL SETUP: what it set
    int done_ctl = -1;        // one control's window: that control
} g_p;
int g_none_row = -1;          // a row chosen with nothing to listen to: says so for a moment
double g_none_until = 0;

double now_s() { return double(SDL_GetTicksNS()) * 1e-9; }

void take_snap(Snap& s) {
    for (int a = 0; a < s.axes; a++) s.axis[a] = SDL_GetJoystickAxis(s.joy, a);
    for (int b = 0; b < s.buttons; b++) s.button[b] = SDL_GetJoystickButton(s.joy, b);
}

void close_snaps() {
    for (Snap& s : g_p.snaps) SDL_CloseJoystick(s.joy);   // SDL counts the opens: ours only
    g_p.snaps.clear();
}

// Every joystick that is not a gamepad (a wheel SDL maps as one is kept)
int open_snaps() {
    close_snaps();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        if (SDL_IsGamepad(ids[i]) && SDL_GetJoystickTypeForID(ids[i]) != SDL_JOYSTICK_TYPE_WHEEL) continue;
        SDL_Joystick* j = SDL_OpenJoystick(ids[i]);
        if (!j) continue;
        Snap s{};
        s.joy = j;
        s.id = ids[i];
        s.axes = std::min(SDL_GetNumJoystickAxes(j), 16);
        s.buttons = std::clamp(SDL_GetNumJoystickButtons(j), 0, MAX_BUTTONS);
        take_snap(s);
        g_p.snaps.push_back(s);
    }
    SDL_free(ids);
    return int(g_p.snaps.size());
}

// The force test's push off, the centring spring as it was
void force_test_off() {
    if (!g_testing) return;
    g_testing = false;
    g_test_force = 0;
    make_spring(g_wheel);
}

void end_prompt() {
    close_snaps();
    force_test_off();
    g_p = Prompt{};
}

void start_listen(int k) {
    int n = open_snaps();
    g_p.ctl = k;
    g_p.since = now_s();
    g_p.progress = 0;
    g_p.moving.clear();
    printf("[wheel] %s: listening to %d devices\n", ctl_label(k), n);
}

void enter_step(int step);

void next_step() {
    if (!g_p.setup) {
        end_prompt();
        return;
    }
    enter_step(g_p.step + 1);
}

void enter_step(int step) {
    close_snaps();
    g_p.ctl = -1;
    g_p.done.clear();
    g_p.note.clear();
    g_p.done_at = -1;
    g_p.progress = 0;
    g_p.moving.clear();
    g_p.since = now_s();
    bool force_ok = g_wheel.joy && g_wheel.haptic && g_wheel.constant >= 0;
    if (step == ST_ROTATION && !(g_src[STEER] && g_src[STEER]->joy)) step++;   // no steering to measure
    if (step == ST_FORCE && !force_ok) {
        g_p.summary.push_back(g_wheel.joy ? "FORCE FEEDBACK: NONE" : "FORCE FEEDBACK: NO WHEEL");
        step = ST_DONE;
    }
    g_p.step = step;
    if (step < ST_COUNT && STEP_CTL[step] >= 0) start_listen(STEP_CTL[step]);
    if (step == ST_ROTATION) g_p.hold_since = -1;
    if (step == ST_FORCE) {   // the spring off, so the push turns the wheel
        g_testing = true;
        g_test_force = 0;
        if (g_wheel.spring >= 0) {
            SDL_DestroyHapticEffect(g_wheel.haptic, g_wheel.spring);
            g_wheel.spring = -1;
        }
        restart_haptics(g_wheel);
        g_p.push_from = now_s() + HANDS_OFF;
    }
    if (step == ST_DONE) {
        force_test_off();
        g_cfg.setup_done = true;
        write_ini(g_cfg);
        printf("[wheel] WHEEL SETUP done\n");
    }
}

// A control moved: in the settings (another control that had it gets this
// one's old binding: a swap), then wheel.ini, then every device taken again
void take(int k, uint32_t id, bool is_axis, int index, int16_t from, int16_t to) {
    std::string name = device_name(id);
    bool on_wheel = g_wheel.joy && g_wheel.id == id;
    Config& c = g_cfg;
    int target = k == HANDBRAKE && is_axis ? HANDBRAKE_AXIS : k;
    int old_n = c.ctl[target];
    std::string old_dev = c.dev[target];
    std::string note;
    if (k != STEER) {
        for (const Row& r : ROWS) {   // the same axis or button on the same device, in another control
            int j = r.ctl == HANDBRAKE && is_axis ? HANDBRAKE_AXIS : r.ctl;
            if (j < 0 || j == target || is_axis_ctl(j) != is_axis || c.ctl[j] != index) continue;
            const Wheel* src = g_src[j];
            if (!src || !src->joy || src->id != id) continue;
            c.ctl[j] = old_n;
            c.dev[j] = old_dev;
            note = std::string(ctl_label(j)) + " had it: now " +
                   (old_n < 0 ? std::string("NONE") : (is_axis ? "AXIS " : "BUTTON ") + std::to_string(old_n));
        }
    }
    if (k == STEER) {
        c.device = name;
        c.ctl[STEER] = index;
        c.steer_invert = to < from;   // asked to turn right
    } else {
        // The wheel's own controls name no device unless their group's does
        bool plain = on_wheel && role_device(c, CTL[k].role).empty();
        c.ctl[target] = index;
        c.dev[target] = plain ? std::string() : name;
        if (k == HANDBRAKE) c.ctl[is_axis ? HANDBRAKE : HANDBRAKE_AXIS] = -1;
        if (k == GAS || k == BRAKE) c.combined_pedals = false;
        if (k == GAS) c.gas_rest = from;
        if (k == BRAKE) c.brake_rest = from;
        if (target == HANDBRAKE_AXIS) c.handbrake_rest = from;
    }
    write_ini(c);
    printf("[wheel] %s: %s %d on %s\n", ctl_label(k), is_axis ? "axis" : "button", index, name.c_str());
    std::string dev = on_wheel || k == STEER ? std::string("WHEEL") : upper(name);
    if (dev.size() > 24) dev = dev.substr(0, 23) + ".";
    g_p.done = (is_axis ? "AXIS " : "BUTTON ") + std::to_string(index) + "  -  " + dev;
    g_p.note = note;
    g_p.done_at = now_s();
    if (k >= GEAR1 && k <= GEAR4) {   // the gears in one line of the summary
        std::string line = "H SHIFTER: GEAR " + std::to_string(k - GEAR1 + 1);
        if (!g_p.summary.empty() && g_p.summary.back().rfind("H SHIFTER: GEAR", 0) == 0)
            g_p.summary.back() += ", " + std::to_string(k - GEAR1 + 1);
        else
            g_p.summary.push_back(line);
    } else {
        g_p.summary.push_back(std::string(ctl_label(k)) + ": " + g_p.done);
    }
    close_snaps();
    g_p.ctl = -1;
    g_p.progress = 1;
    reopen_all();
}

// WHEEL ROTATION from a quarter turn: the axis reads r of its half travel at
// 90 degrees, so the wheel turns 180 / r degrees in all; steer_range is the
// part of it for the arcade's 135 degrees each side
void take_rotation(float r) {
    r = std::fabs(r);
    int deg = int(std::lround(180.0f / std::max(r, 0.01f) / 10.0f)) * 10;
    std::string note;
    if (deg < 270) {
        note = "Under 270 degrees: the game needs at least the arcade's 270. Set more in your wheel's software";
        deg = 270;
    }
    deg = std::min(deg, 3600);
    g_cfg.steer_range = std::clamp(270.0f / float(deg), 0.05f, 1.0f);
    write_ini(g_cfg);
    printf("[wheel] WHEEL ROTATION: %d degrees (a quarter turn read %.3f of the axis)\n", deg, r);
    g_p.done = std::to_string(deg) + "\xB0 WHEEL ROTATION";
    g_p.note = note;
    g_p.done_at = now_s();
    g_p.summary.push_back("WHEEL ROTATION: " + std::to_string(deg) + "\xB0");
    g_p.progress = 1;
}

float steer_raw() {   // the steering axis -1..1, turned right as +
    const Wheel* w = g_src[STEER];
    if (!w || !w->joy) return 0;
    float r = axis(*w, g_cfg.ctl[STEER]) / 32767.0f;
    return g_cfg.steer_invert ? -r : r;
}

// Each poll while the window is up
void prompt_poll() {
    if (!g_p.on) return;
    double now = now_s();
    if (g_p.done_at >= 0) {   // a control just taken, shown: then on
        if (now >= g_p.done_at + SHOW_DONE) next_step();
        return;
    }
    if (g_p.ctl >= 0) {
        double t = now - g_p.since;
        if (!g_p.setup && t > LISTEN_FOR) {
            printf("[wheel] nothing moved: %s left as it was\n", ctl_label(g_p.ctl));
            end_prompt();
            return;
        }
        if (t < SETTLE) {   // freshly opened devices can read 0 at first
            for (Snap& s : g_p.snaps) take_snap(s);
            return;
        }
        int k = g_p.ctl;
        bool axes_ok = k == STEER || k == GAS || k == BRAKE || k == HANDBRAKE;
        bool buttons_ok = k != STEER && k != GAS && k != BRAKE;
        int need = k == STEER ? 6000 : 16000;   // a quarter turn right on a 900 degree wheel, or half a pedal
        float best = 0;
        std::string moving;
        for (Snap& s : g_p.snaps) {
            for (int b = 0; buttons_ok && b < s.buttons; b++)
                if (SDL_GetJoystickButton(s.joy, b) && !s.button[b]) return take(k, s.id, false, b, 0, 0);
            for (int a = 0; axes_ok && a < s.axes; a++) {
                int16_t v = SDL_GetJoystickAxis(s.joy, a);
                float f = float(std::abs(int(v) - int(s.axis[a]))) / float(need);
                if (f >= 1.0f) return take(k, s.id, true, a, s.axis[a], v);
                if (f > best) {
                    best = f;
                    moving = upper(device_name(s.id)) + "  -  AXIS " + std::to_string(a);
                }
            }
        }
        g_p.progress = best;
        g_p.moving = best > 0.12f ? moving : std::string();
        return;
    }
    if (g_p.step == ST_ROTATION) {   // held still, a fair way from the centre
        float r = steer_raw();
        if (std::fabs(r) < 0.04f || std::fabs(r - g_p.hold) > 0.006f) {
            g_p.hold = r;
            g_p.hold_since = now;
            g_p.progress = 0;
        } else {
            g_p.progress = float((now - g_p.hold_since) / HOLD_FOR);
            if (g_p.progress >= 1.0f) take_rotation(r);
        }
        return;
    }
    if (g_p.step == ST_FORCE && g_p.push_from > 0) {
        if (now >= g_p.push_from + PUSH_FOR) {
            g_test_force = 0;
            g_p.push_from = -1;
            g_p.step = ST_FORCE_ASK;
        } else if (now >= g_p.push_from) {
            g_test_force = PUSH;   // to the right, as the game would push it
        }
    }
}

// The joysticks found (gamepads apart), for WHEEL SETUP's first step
std::vector<std::string> found_devices() {
    std::vector<std::string> out;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    for (int i = 0; ids && i < count; i++) {
        if (SDL_IsGamepad(ids[i]) && SDL_GetJoystickTypeForID(ids[i]) != SDL_JOYSTICK_TYPE_WHEEL) continue;
        std::string n = device_name(ids[i]);
        if (g_wheel.joy && g_wheel.id == ids[i])
            n += g_wheel.haptic && g_wheel.constant >= 0 ? "  (the wheel, force feedback)" : "  (the wheel)";
        out.push_back(n);
    }
    SDL_free(ids);
    return out;
}

bool held_now(int k) {
    const Wheel* d = g_src[k];
    int b = g_cfg.ctl[k];
    return d && d->joy && b >= 0 && b < d->buttons && d->down[b];
}

}  // namespace

bool wheel_listening() { return g_p.on; }

void wheel_listen_cancel() {
    if (!g_p.on) return;
    printf("[wheel] %s stopped\n", g_p.setup ? "WHEEL SETUP" : "listening");
    end_prompt();
}

WheelPrompt wheel_prompt() {
    WheelPrompt p;
    if (!g_p.on) return p;
    p.on = true;
    double now = now_s();
    int step = g_p.setup ? g_p.step : -1;
    p.kicker = g_p.setup ? "WHEEL SETUP" : "SET CONTROL";
    if (g_p.setup && STAGE[step] > 0) {
        p.step = STAGE[step];
        p.steps = STAGES;
    }
    p.progress = std::clamp(g_p.progress, 0.0f, 1.0f);
    p.moving = g_p.moving;
    p.done = g_p.done;
    p.note = g_p.note;
    p.done_age = g_p.done_at >= 0 ? float(now - g_p.done_at) : -1;
    p.time_left = !g_p.setup && g_p.ctl >= 0 ? float(std::clamp(1.0 - (now - g_p.since) / LISTEN_FOR, 0.0, 1.0)) : -1;
    int ctl = g_p.setup ? STEP_CTL[std::clamp(step, 0, ST_COUNT - 1)] : g_p.done_ctl;
    p.title = ctl >= 0 ? ctl_label(ctl) : "";
    switch (ctl) {
    case STEER:
        p.icon = WheelPrompt::TURN_RIGHT;
        p.action = "TURN THE WHEEL TO THE RIGHT";
        p.hint = "A quarter turn is enough. Every wheel, pedal set and shifter is listened to (gamepads apart)";
        break;
    case GAS:
    case BRAKE:
        p.icon = WheelPrompt::PEDAL;
        p.glyph = ctl == GAS ? "GAS" : "BRAKE";
        p.action = "PRESS THE PEDAL ALL THE WAY DOWN";
        p.hint = "From rest, the other pedals let go. Its resting position is saved too";
        break;
    case HANDBRAKE:
        p.icon = WheelPrompt::HAND_BRAKE;
        p.action = "PULL THE HAND BRAKE";
        p.hint = "An analog hand brake (on past half its travel) or a button";
        break;
    case SHIFT_UP:
    case SHIFT_DOWN:
        p.icon = WheelPrompt::BUTTON;
        p.glyph = ctl == SHIFT_UP ? "+" : "-";
        p.action = ctl == SHIFT_UP ? "PULL THE SHIFT UP PADDLE" : "PULL THE SHIFT DOWN PADDLE";
        p.hint = "Or the button you want to use. They also change the settings section in the menus";
        break;
    case GEAR1:
    case GEAR2:
    case GEAR3:
    case GEAR4:
        p.icon = WheelPrompt::GEAR;
        p.gear = ctl - GEAR1 + 1;
        p.action = "PUT THE H SHIFTER IN GEAR " + std::to_string(p.gear);
        p.hint = "Sega Rally 2 has four gears. The paddles still change gear while the stick stays put";
        break;
    case VIEW:
    case START:
        p.icon = WheelPrompt::BUTTON;
        p.glyph = ctl == VIEW ? "VIEW" : "START";
        p.action = ctl == VIEW ? "PRESS THE BUTTON FOR VIEW CHANGE" : "PRESS THE BUTTON FOR START";
        p.hint = ctl == VIEW ? "The camera: behind the car or from the bonnet" : "Starts a race; in the race it pauses";
        break;
    default:
        break;
    }
    if (g_p.setup) {
        switch (step) {
        case ST_INTRO:
            p.icon = WheelPrompt::INTRO;
            p.title = "WHEEL SETUP";
            p.action = "YOUR WHEEL, PEDALS AND SHIFTER, STEP BY STEP";
            p.hint = "Move each control when asked: every device below is listened to. Skip what you don't have";
            p.lines = found_devices();
            if (p.lines.empty()) p.lines.push_back("No wheel, pedals or shifter found. Plug them in, they show here");
            p.choices = {"START"};
            break;
        case ST_ROTATION:
            p.icon = WheelPrompt::QUARTER_TURN;
            p.title = "WHEEL ROTATION";
            p.action = "TURN A QUARTER TURN RIGHT, AND HOLD IT";
            p.hint = "The top of the wheel at 3 o'clock (90\xB0). From there the game works out the rotation "
                     "set in your wheel's software";
            p.choices = {"SKIP"};
            break;
        case ST_FORCE:
            p.icon = WheelPrompt::FORCE;
            p.title = "FORCE FEEDBACK";
            p.action = g_p.push_from > 0 && now >= g_p.push_from ? "WATCH THE WHEEL..." : "LET GO OF THE WHEEL";
            p.hint = "In a moment the game pushes it by itself. See which way it turns";
            if (g_p.push_from > 0 && now < g_p.push_from)
                p.time_left = float(std::clamp((g_p.push_from - now) / HANDS_OFF, 0.0, 1.0));
            p.choices = {"SKIP"};
            break;
        case ST_FORCE_ASK:
            p.icon = WheelPrompt::FORCE;
            p.title = "FORCE FEEDBACK";
            p.action = "WHICH WAY DID THE WHEEL TURN?";
            p.hint = "The game pushed it to the right. If it went left, the forces are turned round for you";
            p.choices = {"RIGHT", "LEFT", "IT DID NOT MOVE", "AGAIN"};
            break;
        case ST_DONE:
            p.icon = WheelPrompt::DONE;
            p.title = "ALL SET";
            p.action = "YOUR WHEEL IS READY";
            p.hint = "Change any of it in this tab: choose a control and move it";
            p.lines = g_p.summary;
            if (p.lines.empty()) p.lines.push_back("Nothing changed");
            p.choices = {"FINISH"};
            break;
        default:
            p.choices = {step == ST_GEAR1 ? "NO H SHIFTER" : "SKIP"};
            break;
        }
    }
    return p;
}

void wheel_prompt_choose(int choice) {
    if (!g_p.on) return;
    if (g_p.done_at >= 0) {   // the control just taken: on at once, as the message's end would
        next_step();
        return;
    }
    if (!g_p.setup) return;
    switch (g_p.step) {
    case ST_INTRO: enter_step(ST_STEER); break;
    case ST_GEAR1:   // NO H SHIFTER: the four gears skipped
        g_p.summary.push_back("H SHIFTER: NONE");
        enter_step(ST_VIEW);
        break;
    case ST_FORCE:
        force_test_off();
        enter_step(ST_DONE);
        break;
    case ST_FORCE_ASK:
        if (choice == 3) {   // AGAIN
            g_p.step = ST_FORCE;
            g_testing = true;
            g_p.push_from = now_s() + HANDS_OFF * 0.6;
            break;
        }
        if (choice == 1) g_cfg.ffb_invert = !g_cfg.ffb_invert;
        write_ini(g_cfg);
        g_p.done = choice == 0   ? "FORCES THE RIGHT WAY ROUND"
                   : choice == 1 ? std::string("FORCES TURNED ROUND: INVERT FORCES ") + (g_cfg.ffb_invert ? "ON" : "OFF")
                                 : "NO FORCE FELT";
        g_p.note = choice == 2 ? "Check GAIN, and that no other program (the wheel's own software, SimHub...) holds the wheel"
                               : "";
        g_p.summary.push_back(choice == 0 ? "FORCE FEEDBACK: OK" : choice == 1 ? "FORCE FEEDBACK: INVERTED" : "FORCE FEEDBACK: NOT FELT");
        g_p.done_at = now_s();
        force_test_off();
        g_p.push_from = -1;   // (on to ST_DONE after the message)
        break;
    case ST_DONE: end_prompt(); break;
    default: next_step(); break;   // SKIP
    }
}

std::vector<WheelRow> wheel_rows() {
    std::vector<WheelRow> rows;
    const Config& c = g_cfg;
    for (int r = 0; r < ROW_COUNT; r++) {
        WheelRow row;
        row.group = ROWS[r].group;
        row.label = ROWS[r].label;
        int k = ROWS[r].ctl;
        if (r == g_none_row && now_s() < g_none_until) {
            row.value = "NO WHEEL FOUND";
            row.hint = "No wheel, pedals or other joystick found (gamepads are set apart). Plug it in and choose this again";
        } else if (k == SET_SETUP) {
            row.value = c.setup_done ? "START" : "RECOMMENDED";
            row.hint = "Step by step: the wheel and its rotation, the pedals, the paddles, an H shifter, and which "
                       "way the forces push. Any step can be skipped";
        } else if (k == SET_GAIN) {
            row.value = c.ffb_strength > 0 ? std::to_string(int(std::lround(c.ffb_strength * 100))) + "%" : "OFF";
            row.fill = c.ffb_strength;
            row.hint = "How hard the wheel pushes back: the drive board's motor, bumps, slides and the road";
            if (g_ffb_last.frames > 300) {   // the last drive: how strong, and how often at the wheel's most
                int peak = int(std::lround(g_ffb_last.peak * 100));
                int clip = int(std::lround(100.0 * g_ffb_last.clipped / g_ffb_last.frames));
                row.hint += ".  Last drive: strongest " + std::to_string(peak) + "%, at the limit " +
                            std::to_string(clip) + "% of the time" +
                            (clip >= 5 ? ": lower GAIN for more detail" : peak < 50 ? ": there is room to raise GAIN" : "");
            }
        } else if (k == SET_SPRING) {
            row.value = c.ffb_spring > 0 ? std::to_string(int(std::lround(c.ffb_spring * 100))) + "%" : "OFF";
            row.fill = c.ffb_spring;
            row.hint = "Pulls the wheel back to the centre when you let go. Wheels without a spring effect leave it to their driver";
        } else if (k == SET_INVERT) {
            row.value = c.ffb_invert ? "ON" : "OFF";
            row.hint = "Turn it on if the wheel pulls the wrong way: away from the centre in the bends. WHEEL SETUP tests it";
        } else if (k == SET_ROTATION) {
            int deg = int(std::lround(270.0f / c.steer_range / 10.0f)) * 10;
            row.value = std::to_string(deg) + "\xB0";
            row.fill = std::min(float(deg) / 1080.0f, 1.0f);
            row.hint = "The rotation your wheel is set to in its driver (G HUB, Fanatec, Pit House...): "
                       "the game turns it into the arcade wheel's 270 degrees. WHEEL SETUP measures it";
        } else if (k == SET_DEAD) {
            int pct = int(std::lround(c.steer_dead * 100));
            row.value = pct > 0 ? std::to_string(pct) + "%" : "OFF";
            row.fill = float(pct) / float(MAX_DEAD);
            row.hint = "Around the centre, a part of the turn that does not steer yet. For a worn wheel that does not rest still";
        } else {
            int use = k == HANDBRAKE && c.ctl[HANDBRAKE] < 0 && c.ctl[HANDBRAKE_AXIS] >= 0 ? HANDBRAKE_AXIS : k;
            int n = c.ctl[use];
            bool is_axis = is_axis_ctl(use);
            const Wheel* src = g_src[use];
            bool found = src && src->joy;
            std::string where = src == &g_wheel && found ? g_state.name
                                : found ? src->label
                                : ctl_device(c, use).empty() ? std::string("The wheel (not plugged in)")
                                                             : ctl_device(c, use) + " (not plugged in)";
            if (n < 0) {
                row.value = "NONE";
                row.hint = "Not set. Choose it and move the control to set it";
            } else {
                std::string what = (is_axis ? "AXIS " : "BUTTON ") + std::to_string(n);
                std::string dev = !found ? "NOT FOUND" : src == &g_wheel ? "WHEEL" : upper(where);
                if (dev.size() > 12) dev = dev.substr(0, 11) + ".";
                row.value = what + " - " + dev;
                row.hint = where + ", " + (is_axis ? "axis " : "button ") + std::to_string(n) +
                           ". Choose it and move the control to set it again; Delete clears it";
                // What it reads now: an axis' travel, a button held
                if (found && use == STEER) row.meter = 1, row.live = g_live.steer;
                else if (found && use == GAS) row.meter = 2, row.live = g_live.gas;
                else if (found && use == BRAKE) row.meter = 2, row.live = g_live.brake;
                else if (found && use == HANDBRAKE_AXIS) row.meter = 2, row.live = g_live.handbrake;
                else if (found) row.meter = 3, row.live = held_now(use) ? 1.0f : 0.0f;
            }
        }
        rows.push_back(row);
    }
    return rows;
}

void wheel_row_input(int r, int dir) {
    if (r < 0 || r >= ROW_COUNT || !g_ok) return;
    Config& c = g_cfg;
    int k = ROWS[r].ctl;
    auto step = [&](float& v, int steps, int lo, int hi) {   // v in 1/steps
        v = float(std::clamp(int(std::lround(v * float(steps))) + dir, lo, hi)) / float(steps);
    };
    if (k == SET_SETUP) {
        end_prompt();
        g_p.on = g_p.setup = true;
        printf("[wheel] WHEEL SETUP\n");
        enter_step(ST_INTRO);
        return;
    }
    if (k < 0) {
        if (k == SET_GAIN) step(c.ffb_strength, 20, 0, 20);   // 0..100 % in steps of 5
        if (k == SET_SPRING) step(c.ffb_spring, 20, 0, 20);
        if (k == SET_INVERT) c.ffb_invert = !c.ffb_invert;
        if (k == SET_DEAD) step(c.steer_dead, 100, 0, MAX_DEAD);   // 0..20 % in steps of 1
        if (k == SET_ROTATION) {   // 270..1080 degrees in steps of 90, the arcade's 270 as 1.0
            int deg = int(std::lround(270.0f / c.steer_range / 90.0f)) * 90;
            deg = std::clamp(deg + 90 * dir, 270, 1080);
            c.steer_range = 270.0f / float(deg);
        }
        if (k == SET_SPRING) make_spring(g_wheel);
        write_ini(c);
        return;
    }
    // A control: every joystick listened to until one of them moves
    end_prompt();
    g_p.on = true;
    g_p.done_ctl = k;
    start_listen(k);
    if (g_p.snaps.empty()) {   // nothing to wait for
        end_prompt();
        g_none_row = r;
        g_none_until = now_s() + 3.0;
    }
}

void wheel_row_clear(int r) {
    if (r < 0 || r >= ROW_COUNT || !g_ok || g_p.on) return;
    int k = ROWS[r].ctl;
    if (k < 0) return;
    g_cfg.ctl[k] = -1;
    g_cfg.dev[k].clear();
    if (k == HANDBRAKE) {
        g_cfg.ctl[HANDBRAKE_AXIS] = -1;
        g_cfg.dev[HANDBRAKE_AXIS].clear();
    }
    write_ini(g_cfg);
    printf("[wheel] %s cleared\n", ctl_label(k));
    reopen_all();
}

void wheels_poll(bool focused, bool menu, std::vector<PadKey>& keys) {
    bool driving = focused && !menu;
    menu = focused && menu;
    g_focused = driving;
    fake_poll();
    prompt_poll();
    if (!driving && g_ffb_now.frames > 0) {   // a drive over (or paused): its force for GAIN's hint
        if (g_ffb_now.frames > 300) g_ffb_last = g_ffb_now;
        g_ffb_now = FfbStats{};
    }
    Wheel& w = g_wheel;
    WheelState& st = g_state;
    st.shift_up = st.shift_down = st.coin = st.start = st.moved = false;
    if (!w.joy) {
        g_live = WheelLive{};
        return;
    }
    // A pedal whose rest SDL gave as 0: while it still reads 0 it has not
    // reported yet. If its first reading jumps straight to one end of the axis,
    // that end is its rest; anything else is a pedal that really rests in the
    // middle and is moving. Not with combined pedals, whose ends are full gas
    // or brake
    auto learn_rest = [](int k) {
        Wheel& d = *g_src[k];
        int a = g_cfg.ctl[k];
        if (!d.joy || a < 0 || a >= std::min(d.axes, 16) || d.rest_known[a]) return;
        int16_t v = SDL_GetJoystickAxis(d.joy, a);
        if (v == 0) return;
        d.rest_known[a] = true;
        if (std::abs(int(v)) < REST_END) return;
        d.rest[a] = v;
        printf("[%s] axis %d rest %d\n", d.label.c_str(), a, v);
    };
    if (!g_cfg.combined_pedals) {
        learn_rest(GAS);
        learn_rest(BRAKE);
    }
    learn_rest(HANDBRAKE_AXIS);
    // The wheel and the other devices: their buttons, and the log
    Wheel* devices[1 + MAX_DEVICES] = {&w};
    int count = 1;
    for (Wheel& d : g_devices)
        if (d.joy) devices[count++] = &d;
    for (int i = 0; i < count; i++) {
        Wheel& d = *devices[i];
        if (g_log) {
            for (int a = 0; a < std::min(d.axes, 16); a++) {
                int16_t v = SDL_GetJoystickAxis(d.joy, a);
                if (std::abs(v - d.last_axis[a]) > 2000) {
                    printf("[%s] axis %d = %d (rest %d)\n", d.label.c_str(), a, v, d.rest[a]);
                    d.last_axis[a] = v;
                }
            }
        }
        for (int b = 0; b < d.buttons; b++) {
            d.down[b] = SDL_GetJoystickButton(d.joy, b);
            if (g_log && d.down[b] != d.was_down[b]) printf("[%s] button %d %s\n", d.label.c_str(), b, d.down[b] ? "down" : "up");
            st.moved |= d.down[b] != d.was_down[b];
        }
    }
    auto held = [](int k) {
        const Wheel& d = *g_src[k];
        int b = g_cfg.ctl[k];
        return b >= 0 && b < d.buttons && d.down[b];
    };
    auto pressed = [&](int k) {
        const Wheel& d = *g_src[k];
        int b = g_cfg.ctl[k];
        return held(k) && !d.was_down[b];
    };
    auto pedal_of = [](int k) { return pedal(*g_src[k], g_cfg.ctl[k]); };
    // The wheel and the pedals, read in the menus too for the WHEEL tab's drawing
    float s = axis(w, g_cfg.ctl[STEER]) / 32767.0f / g_cfg.steer_range;
    if (g_cfg.steer_invert) s = -s;
    float m = std::fabs(s);
    m = m < g_cfg.steer_dead ? 0.0f : (m - g_cfg.steer_dead) / (1.0f - g_cfg.steer_dead);
    float steer = std::clamp(std::copysign(m, s), -1.0f, 1.0f), gas, brake;
    if (g_cfg.combined_pedals) {
        const Wheel& p = *g_src[GAS];
        int a = g_cfg.ctl[GAS];
        float v = (axis(p, a) - float(p.rest[std::clamp(a, 0, 15)])) / 32767.0f;
        if (g_cfg.combined_gas_negative) v = -v;
        gas = p.joy ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        brake = p.joy ? std::clamp(-v, 0.0f, 1.0f) : 0.0f;
    } else {
        gas = pedal_of(GAS);
        brake = pedal_of(BRAKE);
    }
    float handbrake = std::max(held(HANDBRAKE) ? 1.0f : 0.0f, pedal_of(HANDBRAKE_AXIS));
    int live_gear = -1;
    for (int g = 0; g < 4; g++) {
        if (g_cfg.ctl[GEAR1 + g] < 0) continue;
        if (live_gear < 0) live_gear = 0;
        if (held(GEAR1 + g)) live_gear = g + 1;
    }
    g_live = {true, steer, gas, brake, handbrake, live_gear};
    if (driving) {
        st.moved |= std::fabs(steer - st.steer) > 0.01f;
        st.steer = steer;
        st.gas = gas;
        st.brake = brake;
        st.moved |= st.gas > 0 || st.brake > 0;
        st.view = held(VIEW);
        st.handbrake = handbrake > 0.5f;
        st.shift_up = pressed(SHIFT_UP);
        st.shift_down = pressed(SHIFT_DOWN);
        st.coin = pressed(COIN);
        st.start = pressed(START);
        // H shifter: given only when the stick moves (0 = to neutral), so the
        // paddles still change gear while it stays put (issue #73)
        bool h_shifter = false;
        int gear = -1;
        for (int g = 0; g < 4; g++) {
            if (g_cfg.ctl[GEAR1 + g] < 0) continue;
            h_shifter = true;
            if (held(GEAR1 + g)) gear = g + 1;
        }
        if (h_shifter && gear < 0) gear = 0;
        st.gear = gear != g_last_gear ? gear : -1;
        g_last_gear = gear;
        if (g_log) {   // what the game gets, to tell a pedal's travel from the game's own response
            static int last[3] = {-1, -1, -1};
            int now[3] = {int(st.gas * 20), int(st.brake * 20), st.handbrake ? 1 : 0};
            if (now[0] != last[0] || now[1] != last[1] || now[2] != last[2])
                printf("[wheel] to the game: gas %d%%, brake %d%%, hand brake %s\n", now[0] * 5, now[1] * 5,
                       now[2] ? "on" : "off");
            std::copy(now, now + 3, last);
        }
    } else {
        st.steer = st.gas = st.brake = 0;
        st.view = st.handbrake = false;
        st.gear = -1;
        g_last_gear = -2;   // back in the race: the stick's position counts again
    }
    if (menu && !wheel_listening()) {   // (while a control listens, its buttons are not the menu's)
        if (pressed(MENU_ACCEPT)) keys.push_back(PadKey::Accept);
        if (pressed(MENU_BACK)) keys.push_back(PadKey::Back);
        if (pressed(START)) keys.push_back(PadKey::Start);
        if (pressed(SHIFT_DOWN)) keys.push_back(PadKey::TabPrev);
        if (pressed(SHIFT_UP)) keys.push_back(PadKey::TabNext);
        int dir = -1;
        if (SDL_GetNumJoystickHats(w.joy) > 0) {
            uint8_t h = SDL_GetJoystickHat(w.joy, 0);
            dir = h & SDL_HAT_UP ? 0 : h & SDL_HAT_DOWN ? 1 : h & SDL_HAT_LEFT ? 2 : h & SDL_HAT_RIGHT ? 3 : -1;
        }
        double now = now_s();
        if (dir != w.menu_dir || (dir >= 0 && now >= w.next_repeat)) {
            if (dir >= 0) {
                static const PadKey DIRS[4] = {PadKey::Up, PadKey::Down, PadKey::Left, PadKey::Right};
                keys.push_back(DIRS[dir]);
                w.next_repeat = now + (dir != w.menu_dir ? 0.4 : 0.11);
            }
            w.menu_dir = dir;
        }
    } else {
        w.menu_dir = -1;
    }
    for (int i = 0; i < count; i++) std::copy(std::begin(devices[i]->down), std::end(devices[i]->down), std::begin(devices[i]->was_down));
    apply_force(w);
}

void wheels_shutdown() {
    end_prompt();
    for (Wheel& d : g_devices)
        if (d.joy) wheels_removed(d.id);
    if (g_wheel.joy) wheels_removed(g_wheel.id);
    if (g_fake.joy) {
        SDL_CloseJoystick(g_fake.joy);
        SDL_DetachVirtualJoystick(g_fake.id);
        g_fake = Fake{};
    }
    g_ok = false;
}

const WheelState& wheel_state() { return g_state; }

const WheelLive& wheel_live() { return g_live; }

void wheel_set_force(float force, float shake) {
    g_force = force;
    if (shake > g_shake.load()) g_shake = shake;
}
