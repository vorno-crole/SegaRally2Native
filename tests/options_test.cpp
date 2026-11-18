// The command-line table (options.cpp): every kind of entry, the fallback for
// options of other modules and the test-run detection that leaves the menu out
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "options.h"

static int g_failed = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAILED line %d: %s\n", __LINE__, #cond);            \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Parses a list of arguments; returns whether the game would run
static bool parse(Options& o, std::vector<const char*> args, int* code = nullptr, ExtraOption extra = nullptr) {
    args.insert(args.begin(), "SegaRally2.exe");
    int exit_code = -1;
    bool run = parse_options(o, int(args.size()), const_cast<char**>(args.data()), extra, exit_code);
    if (code) *code = exit_code;
    return run;
}

int main() {
    {   // defaults
        Options o;
        CHECK(parse(o, {}));
        CHECK(o.msaa == 4 && !o.dlaa && o.aniso == 8 && near(o.bloom, 1) && near(o.ao, 1) && near(o.haze, 1));
        CHECK(near(o.saturation, 1.0f) && o.frame_limit == INT32_MAX && !o.headless);
        CHECK(o.hdr && o.sun && o.shadows && o.menu && o.data_dir == "data" && o.rom_zip.empty());
    }
    {   // the capture used to verify the renderer: values, optional strengths and plain flags
        Options o;
        CHECK(parse(o, {"--shot", "2600", "--bloom", "1.6", "--haze", "1", "--ao", "1"}));
        CHECK(o.shot_frame == 2600 && near(o.bloom, 1.6f) && near(o.haze, 1) && near(o.ao, 1));
        Options p;
        CHECK(parse(p, {"--no-ao", "--no-bloom", "--no-haze", "--ao", "--no-sun", "--letterbox", "--pixel2d"}));
        CHECK(near(p.ao, 1) && near(p.bloom, 0) && near(p.haze, 0) && !p.sun && p.letterbox && p.pixel_2d);
        Options q;   // a strength followed by another option keeps the default 1
        CHECK(parse(q, {"--no-haze", "--haze", "--no-hdr", "--bloom", "0.5"}));
        CHECK(near(q.haze, 1) && !q.hdr && near(q.bloom, 0.5f));
    }
    {   // custom values
        Options o;
        CHECK(parse(o, {"--aa", "dlaa", "--aspect", "21:9", "--aim",
                        "100,200", "--msaa", "8", "--scale", "1.5", "--volume", "3", "--wav", "out/a.wav", "--data",
                        "roms"}));
        CHECK(o.dlaa && o.msaa == 8 && near(o.max_aspect, 21.0f / 9.0f));
        // --aspect keeps square pixels whatever ASPECT (settings.ini) gave; the operator
        // settings and the experience stay as the EEPROM says unless the menu sets them
        Options a;
        a.pixel_aspect = 1.032f;
        CHECK(parse(a, {"--aspect", "4:3"}) && near(a.pixel_aspect, 1.0f) && near(a.max_aspect, 4.0f / 3.0f));
        CHECK(a.game_mode == -1 && a.default_view == -1 && a.speed_kmh == -1 && a.free_play == -1 && !a.arcade);
        CHECK(o.fixed_aim && o.aim_x == 100 && o.aim_y == 200 && near(o.supersample, 1.5f) && near(o.volume, 3));
        CHECK(o.wav_path == "out/a.wav" && o.data_dir == "roms");
        Options r;
        CHECK(parse(r, {"--rom", "D:\\roms\\srally2.zip"}) && r.rom_zip == "D:\\roms\\srally2.zip");
        Options gi;   // global illumination is on or off (the old "high" is on, "low" too)
        CHECK(gi.gi == 1 && parse(gi, {"--gi", "off"}) && gi.gi == 0);
        CHECK(parse(gi, {"--gi", "high"}) && gi.gi == 1);
        CHECK(parse(gi, {"--gi", "low"}) && gi.gi == 1 && parse(gi, {"--no-gi"}) && gi.gi == 0);
        Options sp;
        CHECK(sp.speakers == 0 && sp.cubic_samples && sp.rear_boost == 6);
        CHECK(parse(sp, {"--speakers", "headphones", "--linear-samples"}));
        CHECK(sp.speakers == 2 && !sp.cubic_samples);
        CHECK(parse(sp, {"--speakers", "surround"}) && sp.speakers == 1);
        CHECK(parse(sp, {"--rear-boost", "0"}) && sp.rear_boost == 0);
        Options p;
        CHECK(parse(p, {"--aa", "none", "--aspect", "1.5"}));
        CHECK(!p.dlaa && p.msaa == 1 && near(p.max_aspect, 1.5f));
        Options q;
        CHECK(parse(q, {"--aa", "msaa", "--auto", "coin@10", "--auto", "start@20", "--trace",
                        "10F650,10D564", "--diff", "10F650@300", "--replay", "out/snap/snap_1.bin"}));
        CHECK(!q.dlaa && q.msaa == 4 && q.auto_scripts.size() == 2);
        Options d;
        CHECK(parse(d, {"--aa", "dlss"}) && d.dlaa && d.dlss == 1 && d.dlss_model == 0);
        Options b;   // the other DLSS modes and the model
        CHECK(parse(b, {"--aa", "dlss_balanced", "--dlss-model", "m"}) && b.dlaa && b.dlss == 2 && b.dlss_model == 2);
        CHECK(parse(b, {"--aa", "dlss_performance", "--dlss-model", "l"}) && b.dlss == 3 && b.dlss_model == 3);
        CHECK(parse(b, {"--aa", "dlss_ultra", "--dlss-model", "auto"}) && b.dlss == 4 && b.dlss_model == 0);
        CHECK(parse(b, {"--aa", "dlaa", "--dlss-model", "k"}) && b.dlaa && b.dlss == 0 && b.dlss_model == 1);
        CHECK(q.auto_scripts[1] == "start@20");
        CHECK(q.traces.size() == 1 && q.diff == "10F650@300" && q.replay == "out/snap/snap_1.bin");
    }
    {   // run modes
        Options o;
        CHECK(parse(o, {"--headless"}));
        CHECK(o.headless && o.frame_limit == INT32_MAX);   // 600 is applied by main()
        Options p;
        CHECK(parse(p, {"--headless", "--frames", "50", "--no-menu", "--boot"}));
        CHECK(p.headless && p.frame_limit == 50 && !p.menu && p.show_boot);
    }
    {   // errors and help
        Options o;
        int code = -1;
        CHECK(!parse(o, {"--frames"}, &code) && code == 1);   // value missing
        CHECK(!parse(o, {"--bogus"}, &code) && code == 1);
        CHECK(!parse(o, {"--help"}, &code) && code == 0);
        CHECK(!parse(o, {"--shot", "1", "-h"}, &code) && code == 0);
    }
    {   // options of another module through the fallback
        std::vector<std::string> seen;
        ExtraOption extra = [&](Options& o, const std::string& name, const char* next) -> int {
            if (name == "--arena") {
                seen.push_back(next ? next : "(none)");
                return 2;
            }
            if (name == "--trace-arena") {
                o.menu = false;
                return 1;
            }
            return 0;
        };
        Options o;
        CHECK(parse(o, {"--arena", "2:3", "--trace-arena", "--msaa", "2"}, nullptr, extra));
        CHECK(seen.size() == 1 && seen[0] == "2:3" && !o.menu && o.msaa == 2);
        int code = -1;
        CHECK(!parse(o, {"--other"}, &code, extra) && code == 1);
    }
    {   // test runs leave the menu out
        const char* a1[] = {"x", "--bloom", "1.6", "--shot", "1400"};
        const char* a2[] = {"x", "--bloom", "1.6", "--no-hdr"};
        const char* a3[] = {"x", "--trace-arena"};
        CHECK(options_disable_menu(5, const_cast<char**>(a1)));
        CHECK(!options_disable_menu(4, const_cast<char**>(a2)));
        CHECK(!options_disable_menu(2, const_cast<char**>(a3)));
        CHECK(options_disable_menu(2, const_cast<char**>(a3), {"--trace-arena"}));
    }
    if (g_failed) {
        printf("options: %d checks failed\n", g_failed);
        return 1;
    }
    printf("options: all checks passed\n");
    return 0;
}
