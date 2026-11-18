#include "settings_bridge.h"

#include <algorithm>

#include "../ui/frontend.h"

const float BLOOM_STRENGTH[4] = {0.0f, 0.6f, 1.0f, 1.6f};

void options_from_settings(Options& o, const FrontendSettings& ini) {
    o.volume = float(ini.volume);
    o.music = float(ini.music);
    o.speakers = ini.speakers;
    o.cubic_samples = ini.cubic_samples;
    o.rear_boost = ini.rear_boost;
    o.dlaa = ini.aa == 1 || ini.aa >= 3;
    o.dlss = ini.aa >= 3 ? ini.aa - 2 : 0;   // 3 QUALITY .. 6 ULTRA PERFORMANCE
    o.dlss_model = ini.dlss_model;
    if (ini.aa == 2) o.msaa = 1;
    o.haze = ini.haze ? 1.0f : 0.0f;
    o.bloom = BLOOM_STRENGTH[ini.bloom];
    o.ao = ini.ao ? 1.0f : 0.0f;
    o.sun = ini.sun;
    o.shadows = ini.shadows;
    o.ray_tracing = ini.ray_tracing;
    o.soft = ini.soft;
    o.hdr = ini.hdr;
    o.reflections = ini.reflections;
    o.gi = ini.gi;
    o.dust = ini.dust;
    o.sharpen = ini.sharpen;
    o.classic_hud = !ini.hd_hud;
    o.classic_liveries = !ini.hd_liveries;
    o.arcade = ini.experience == 1;
    if (o.arcade) {   // ARCADE ORIGINAL: the operator's own settings (the EEPROM's), and its pause's FREE PLAY / COIN
        o.difficulty = o.game_mode = o.default_view = o.speed_kmh = -1;
        o.free_play = ini.free_play;
    } else {   // PC EDITION: PLAY > ARCADE CLASSIC's race set-up
        o.difficulty = ini.difficulty;
        o.game_mode = ini.game_mode;
        o.default_view = ini.default_view;
        o.speed_kmh = ini.speed_kmh;
        o.free_play = -1;
    }
    // ASPECT (--aspect wins): the widest picture, its pixels' shape, 4:3 keeps the HUD in its place
    o.max_aspect = FrontendSettings::aspect_ratio(ini.aspect);
    o.pixel_aspect = FrontendSettings::pixel_aspect(ini.aspect);
    o.hud_centre = ini.aspect == 4;
    o.interp = ini.smooth;
    o.saturation = ini.saturation / 100.0f;
    o.gamma = ini.gamma / 100.0f;
    o.supersample = ini.render_scale / 100.0f;   // RENDER SCALE (--scale wins)
}

void settings_from_options(FrontendSettings& s, const Options& o) {
    s.volume = std::clamp(int(o.volume + 0.5f), 0, 10);
    s.music = std::clamp(int(o.music + 0.5f), 0, 10);
    s.speakers = std::clamp(o.speakers, 0, 2);
    s.cubic_samples = o.cubic_samples;
    s.rear_boost = std::clamp(o.rear_boost / 3 * 3, 0, 12);
    s.aa = o.dlss ? 2 + std::clamp(o.dlss, 1, 4) : o.dlaa ? 1 : o.msaa <= 1 ? 2 : 0;
    s.dlss_model = std::clamp(o.dlss_model, 0, 3);
    s.haze = o.haze > 0;
    s.bloom = o.bloom <= 0 ? 0 : o.bloom < 0.8f ? 1 : o.bloom < 1.3f ? 2 : 3;
    s.ao = o.ao > 0;
    s.sun = o.sun;
    s.shadows = o.shadows;
    s.ray_tracing = o.ray_tracing;
    s.soft = o.soft;
    s.hdr = o.hdr;
    s.reflections = o.reflections;
    s.gi = o.gi;
    s.dust = o.dust;
    s.sharpen = o.sharpen;
    s.hd_hud = !o.classic_hud;
    s.hd_liveries = !o.classic_liveries;
    if (o.difficulty >= 0) s.difficulty = o.difficulty;
    if (o.game_mode >= 0) s.game_mode = o.game_mode;
    if (o.default_view >= 0) s.default_view = o.default_view;
    if (o.speed_kmh >= 0) s.speed_kmh = o.speed_kmh;
    s.smooth = o.interp;
    s.saturation = std::clamp(int(o.saturation * 100 + 0.5f), 0, 200);
    s.gamma = std::clamp(int(o.gamma * 100 + 0.5f), 50, 150);
    s.render_scale = std::clamp(int(o.supersample * 100 + 0.5f), 50, 200);
}
