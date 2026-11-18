#include "inputs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

Inputs g_inputs;
std::atomic<float> g_ffb_shake{0.0f};
std::atomic<float> g_trigger_rumble[2] = {0.0f, 0.0f};

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A button held for 0.1 s (a pad's BACK = coin, START = start), so the game
// sees the press (it reads the buttons at 57.5 Hz)
static double g_coin_at = -1, g_start_at = -1;
void press_coin() { g_coin_at = now_seconds(); }
void press_start() { g_start_at = now_seconds(); }

void press_update() {
    double now = now_seconds();
    if (g_coin_at >= 0 && now - g_coin_at < 0.1) g_inputs.coin[0] = true;
    if (g_start_at >= 0 && now - g_start_at < 0.1) g_inputs.start[0] = true;
}

void AutoInput::parse(const std::string& s) {
    size_t p = 0;
    while (p < s.size()) {
        size_t comma = s.find(',', p);
        std::string item = s.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t at = item.find('@');
        if (at != std::string::npos) events.push_back({item.substr(0, at), strtoull(item.c_str() + at + 1, nullptr, 10)});
        if (comma == std::string::npos) break;
        p = comma + 1;
    }
}

void AutoInput::apply(uint64_t frame) {
    Inputs& in = g_inputs;
    for (auto& e : events) {
        // A trailing "+" holds the button from then on ("gas+@4200", "test+@900")
        bool hold = !e.name.empty() && e.name.back() == '+';
        bool on = frame >= e.frame && (hold || frame < e.frame + 6);
        if (frame < e.frame || (!hold && frame > e.frame + 6)) continue;
        std::string name = hold ? e.name.substr(0, e.name.size() - 1) : e.name;
        if (name == "coin") in.coin[0] = on;
        else if (name == "start") in.start[0] = on;
        else if (name == "coin2") in.coin[1] = on;
        else if (name == "start2") in.start[1] = on;
        else if (name == "gas") in.accel = on ? 0xFF : 0;
        else if (name == "brake") in.brake = on ? 0xFF : 0;
        else if (name == "view") in.view = on;
        else if (name == "hbrake") in.handbrake = on;
        else if ((name == "up" || name == "down") && frame == e.frame)   // the shifter one gear up / down (1..4)
            in.gear = std::clamp(in.gear + (name == "up" ? 1 : -1), 1, 4);
        else if (name.size() == 4 && name.rfind("bit", 0) == 0) {   // bitA..bitH: bit 0..7 of the player's port held low
            int b = (name[3] - 'A') & 7;
            if (on) in.port_clear |= 1 << b;
            else in.port_clear &= ~(1 << b);
        } else if (name == "test") in.test = on;
        else if (name == "service") in.service = on;
        else if (name.rfind("aim", 0) == 0 && frame == e.frame) {   // aimX/Y: the wheel's raw position from then on
            int x = 128, y = 128;
            if (sscanf(name.c_str() + 3, "%d/%d", &x, &y) == 2) in.steer = x;
        }
    }
}
