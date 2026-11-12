// Records and achievements (stats.h), Sega Rally 2.
//
// Everything is read from the game's RAM at every vblank (docs/game_ram.md has
// how each address was found); no game function is hooked. The game structure
// is at 0x22CF14: +0x00 the mode (0 championship, 2 practice), +0x34 the round
// (the course of the race: course id + 6 for a practice race), +0x70 the clock
// (2 while it counts), +0x74 the time left in frames, +0x7C the goal flag (1
// from the frame the finish line is crossed; TIME UP leaves it at 0), +0x90
// the stages cleared, +0x94 the laps done, +0x98 the checkpoints passed on the
// lap, +0x104 the rank (as the CLEARED card shows it), +0x168 / +0x16C the
// championship total and the race's time, +0x684 the lap times, +0x6F0 1 once
// every stage of the game is cleared, +0x6F4 the place a finished game took in
// the records table (-1 none). The main sequence 0x22D618 is 1 from the credit
// to GAME OVER and the phase 0x22D624 1 through the selects and the races
// (2 to 5 the podium, replay, ranking and GAME OVER screens). The HUD's
// position is at 0x13AB74 (0 = 1st), the cars in the race at 0x225A18 and the
// SELECT CAR and transmission choices at 0x224324 / 0x224334.
//
// What the window shows is a copy taken under a mutex. Debug runs (tests,
// --auto, the DEBUG jumps, TIME INFINITE) count nothing; SR2_STATS_TEST=1
// counts in scripted runs too (testing the achievements: with a save folder
// of its own).
#include "stats.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "ppc_rt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif
#endif

namespace {

// Game RAM
constexpr uint32_t SEQUENCE = 0x22D618;       // 0 attract, 1 a game, -1 power on
constexpr uint32_t PHASE = 0x22D624;          // in a game: 1 selects and races, 2-5 the screens after the last
constexpr uint32_t GAME = 0x22CF14;           // the game structure
constexpr uint32_t G_MODE = GAME + 0x00, G_ROUND = GAME + 0x34, G_CLOCK = GAME + 0x70, G_LEFT = GAME + 0x74;
constexpr uint32_t G_GOAL = GAME + 0x7C, G_STAGES = GAME + 0x90, G_LAPS = GAME + 0x94, G_CHECKPOINTS = GAME + 0x98;
constexpr uint32_t G_RANK = GAME + 0x104, G_TOTAL = GAME + 0x168, G_RACE_TIME = GAME + 0x16C;
constexpr uint32_t G_LAP_TIMES = GAME + 0x684, G_COMPLETE = GAME + 0x6F0, G_RANKING = GAME + 0x6F4;
constexpr uint32_t HUD_POSITION = 0x13AB74, CARS = 0x225A18;
constexpr uint32_t SELECT_CAR = 0x224324, SELECT_GEARS = 0x224334;
constexpr int MODE_CHAMPIONSHIP = 0, MODE_PRACTICE = 2;
constexpr int CLOCK_COUNTING = 2;
constexpr int MAX_LAPS = 8;
constexpr double FRAME_HZ = 57.524;

constexpr const char* STATS_FILE = "save/stats.ini";
constexpr const char* ACHIEVEMENTS_FILE = "save/achievements.ini";

int64_t frames_of(double seconds) { return int64_t(seconds * FRAME_HZ); }

// The round's course (stats.h order) and kind (0 championship, 1 practice); -1 another
int course_of(int round) {
    if (round < 0 || round >= 12) return -1;
    switch (round % 6) {
    case 3: return 0;   // DESERT
    case 2: return 1;   // MOUNTAIN
    case 5: return 2;   // SWEDISH (snowy)
    case 4: return 3;   // MONACO (Riviera)
    default: return -1;   // the two courses of the first Sega Rally, not raced
    }
}

const std::vector<AchievementInfo> TABLE = {
    {"first_finish", L"FIRST FINISH", L"Finish a race", 1},
    {"desert", L"DESERT CLEARED", L"Clear the Desert stage in the championship", 1},
    {"mountain", L"MOUNTAIN CLEARED", L"Clear the Mountain stage in the championship", 1},
    {"snowy", L"SNOWY CLEARED", L"Clear the Snowy stage in the championship", 1},
    {"riviera", L"RIVIERA CLEARED", L"Clear the Riviera stage in the championship", 1},
    {"champion", L"CHAMPION", L"Finish the championship in 1st place", 1},
    {"from_the_back", L"FROM THE BACK", L"Win a championship stage starting last", 1},
    {"stick_shift", L"STICK SHIFT", L"Finish the championship with the manual gearbox", 1},
    {"full_garage", L"FULL GARAGE", L"Finish a race with every car", STAT_OPEN_CARS},
    {"training_days", L"TRAINING DAYS", L"Finish a practice race on all four courses", STAT_COURSES},
    {"last_gasp", L"LAST GASP", L"Reach a checkpoint with less than 1 second left", 1},
    {"hall_of_fame", L"HALL OF FAME", L"Get into the records table", 1},
    {"road_warrior", L"ROAD WARRIOR", L"Finish 50 races", 50},
    {"veteran", L"VETERAN", L"Play for 5 hours", frames_of(5 * 3600)},
};

int64_t raw_progress(const Stats& s, const std::string& k) {
    if (k == "first_finish" || k == "road_warrior") return s.races_finished;
    for (int i = 0; i < STAT_COURSES; i++)
        if (k == TABLE[size_t(1 + i)].key) return s.course[i].clears;
    if (k == "champion") return s.championships_won;
    if (k == "from_the_back") return s.from_the_back;
    if (k == "stick_shift") return s.manual_championships;
    if (k == "full_garage") return std::popcount(unsigned(s.cars_finished) & ((1u << STAT_OPEN_CARS) - 1));
    if (k == "training_days") return std::popcount(unsigned(s.practice_courses) & ((1u << STAT_COURSES) - 1));
    if (k == "last_gasp") return s.last_gasps;
    if (k == "hall_of_fame") return s.records;
    if (k == "veteran") return s.play_frames;
    return 0;
}

// ---- Files: "key = value" lines ----

struct Ini {
    std::vector<std::pair<std::string, int64_t>> rows;
    void put(const std::string& k, int64_t v) { rows.push_back({k, v}); }
    bool save(const char* path) const {
        std::string tmp = std::string(path) + ".tmp";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f) return false;
        for (auto& [k, v] : rows) fprintf(f, "%s = %lld\n", k.c_str(), (long long)v);
        bool ok = fclose(f) == 0;
#ifdef _WIN32
        return ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
#else
        return ok && rename(tmp.c_str(), path) == 0;
#endif
    }
    static Ini load(const char* path) {
        Ini ini;
        if (FILE* f = fopen(path, "r")) {
            char line[256], key[128];
            long long v;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, " %127[^= ] = %lld", key, &v) == 2) ini.put(key, v);
            fclose(f);
        }
        return ini;
    }
};

// Every field of Stats under its name in save/stats.ini
template <class F>
void each_field(Stats& s, F&& f) {
    f("games", s.games), f("play_frames", s.play_frames);
    f("races_started", s.races_started), f("races_finished", s.races_finished);
    f("stages_cleared", s.stages_cleared), f("championships", s.championships);
    f("best_championship_position", s.best_champ_position), f("best_championship_time", s.best_champ_time);
    static const char* const KEYS[STAT_COURSES] = {"desert", "mountain", "snowy", "riviera"};
    static const char* const MODES[2] = {"championship", "practice"};
    for (int i = 0; i < STAT_COURSES; i++) {
        std::string p = std::string(KEYS[i]) + "_";
        Stats::Course& c = s.course[i];
        f(p + "clears", c.clears);
        for (int m = 0; m < 2; m++) f(p + MODES[m] + "_best_lap", c.best_lap[m]), f(p + MODES[m] + "_best_time", c.best_time[m]);
    }
    f("cars_finished", s.cars_finished), f("practice_courses", s.practice_courses);
    f("championships_won", s.championships_won), f("from_the_back", s.from_the_back);
    f("manual_championships", s.manual_championships), f("last_gasps", s.last_gasps), f("records", s.records);
}

void keep_best(int64_t& best, int64_t t) {
    if (t > 0 && (best == 0 || t < best)) best = t;
}

double seconds_of(int64_t ticks) { return double(ticks) / 6000.0; }

// ---- Tracking ----

// What is read of the game every frame
struct Game {
    int sequence = 0, phase = 0, mode = -1, round = -1, clock = 0, left = 0, goal = 0;
    int stages = 0, laps = 0, checkpoints = 0, complete = 0, position = 0;

    static Game read() {
        Game g;
        g.sequence = int32_t(rd32(SEQUENCE)), g.phase = int32_t(rd32(PHASE));
        g.mode = int32_t(rd32(G_MODE)), g.round = int32_t(rd32(G_ROUND));
        g.clock = int32_t(rd32(G_CLOCK)), g.left = int32_t(rd32(G_LEFT)), g.goal = int32_t(rd32(G_GOAL));
        g.stages = int32_t(rd32(G_STAGES)), g.laps = int32_t(rd32(G_LAPS)), g.checkpoints = int32_t(rd32(G_CHECKPOINTS));
        g.complete = int32_t(rd32(G_COMPLETE)), g.position = int32_t(rd32(HUD_POSITION));
        return g;
    }
    bool racing() const {   // through a race of a game (not the attract's, not the replay)
        return sequence == 1 && phase == 1 && (mode == MODE_CHAMPIONSHIP || mode == MODE_PRACTICE);
    }
};

// ---- save/achievements.json: the table with the progress and unlock times ----
// Written for the launcher (NicoLauncher), which shows them on the game's page
// without knowing this game's table; the format is in its docs/manifest.md.
// Nothing reads it back: save/achievements.ini stays the game's own record.

void json_string(std::string& out, const wchar_t* w) {
    out += '"';
    for (; *w; w++) {
        uint32_t c = uint32_t(*w);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", unsigned(c));
            out += b;
        } else if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xE0 | ((c >> 12) & 0x0F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    out += '"';
}

void write_achievements_json(const Stats& s, const std::vector<int64_t>& unlocked) {
    std::string out = "{\n  \"schema\": 1,\n  \"achievements\": [\n";
    for (size_t i = 0; i < TABLE.size(); i++) {
        const AchievementInfo& a = TABLE[i];
        int64_t p = std::clamp<int64_t>(raw_progress(s, a.key), 0, a.goal);
        int64_t when = i < unlocked.size() ? unlocked[i] : 0;
        if (when) p = a.goal;
        out += "    {\"id\": \"" + std::string(a.key) + "\", \"name\": ";
        json_string(out, a.name);
        out += ", \"description\": ";
        json_string(out, a.text);
        out += ", \"goal\": " + std::to_string(a.goal) + ", \"progress\": " + std::to_string(p);
        if (std::string(a.key) == "veteran") {   // play time, counted in frames
            int minutes = int(double(p) / FRAME_HZ / 60);
            char b[48];
            snprintf(b, sizeof b, "%dh %02dm / %dh", minutes / 60, minutes % 60, int(double(a.goal) / FRAME_HZ / 3600 + 0.5));
            out += std::string(", \"progress_text\": \"") + b + "\"";
        }
        out += ", \"unlocked\": " + std::to_string(when) + "}";
        out += i + 1 < TABLE.size() ? ",\n" : "\n";
    }
    out += "  ]\n}\n";
    const char* path = "save/achievements.json";
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp.c_str(), path) == 0;
#endif
    if (!ok) printf("[achievements] could not save %s\n", path);
}

struct Tracker {
    bool on = false;
    std::mutex mutex;   // everything below: the window reads copies
    Stats stats;
    std::vector<int64_t> unlocked;
    std::vector<int> fresh;   // unlocked, not yet announced

    // Game thread
    uint64_t frame = 0;
    bool rebase = true;          // read the game again without counting the jump (start, game state loaded)
    bool in_game = false, cheated = false, game_counted = false;
    Game was;
    int start_position = -1, start_cars = 0;   // of the race on: its place on the grid
    bool was_extending = false;
    int finished_round = -1;   // of the race whose finish line was crossed last
    uint64_t saved_at = 0;
    bool dirty = false;

    bool counting() const { return in_game && !cheated; }

    void load() {
        Ini ini = Ini::load(STATS_FILE);
        each_field(stats, [&](const std::string& k, auto& field) {
            for (auto& [key, v] : ini.rows)
                if (key == k) field = decltype(+field)(v);
        });
        unlocked.assign(TABLE.size(), 0);
        Ini a = Ini::load(ACHIEVEMENTS_FILE);
        for (size_t i = 0; i < TABLE.size(); i++)
            for (auto& [key, v] : a.rows)
                if (key == TABLE[i].key) unlocked[i] = v;
    }

    void save() {
        Ini ini;
        each_field(stats, [&](const std::string& k, auto& field) { ini.put(k, int64_t(field)); });
        Ini a;
        for (size_t i = 0; i < TABLE.size(); i++)
            if (unlocked[i]) a.put(TABLE[i].key, unlocked[i]);
        if (!ini.save(STATS_FILE) || !a.save(ACHIEVEMENTS_FILE)) printf("[achievements] could not save save/stats.ini\n");
        write_achievements_json(stats, unlocked);
        saved_at = frame;
        dirty = false;
    }

    // With the mutex held
    void check_achievements() {
        for (size_t i = 0; i < TABLE.size(); i++)
            if (!unlocked[i] && raw_progress(stats, TABLE[i].key) >= TABLE[i].goal) {
                unlocked[i] = int64_t(time(nullptr));
                fresh.push_back(int(i));
                dirty = true;
                printf("[achievements] unlocked: %ls\n", TABLE[i].name);
            }
    }

    // The finish line crossed (the goal flag up): the race's times, its car and course
    void race_finished(const Game& g) {
        int course = course_of(g.round), kind = g.mode == MODE_CHAMPIONSHIP ? 0 : 1;
        int car = int32_t(rd32(SELECT_CAR));
        finished_round = g.round;
        int64_t time = int32_t(rd32(G_RACE_TIME)), best_lap = 0;
        for (int i = 0; i < std::min(g.laps, MAX_LAPS); i++) keep_best(best_lap, int32_t(rd32(G_LAP_TIMES + 4 * uint32_t(i))));
        printf("[achievements] race finished: round %d, %s, car %d, time %.2f s, best lap %.2f s, %d laps%s\n", g.round,
               kind ? "practice" : "championship", car, seconds_of(time), seconds_of(best_lap), g.laps,
               counting() ? "" : " (not counted)");
        if (!counting()) return;
        stats.races_finished++;
        if (car >= 0 && car < STAT_CARS) stats.cars_finished |= 1 << car;
        if (course >= 0) {
            Stats::Course& c = stats.course[course];
            keep_best(c.best_time[kind], time);
            keep_best(c.best_lap[kind], best_lap);
            if (kind == 1) stats.practice_courses |= 1 << course;
        }
        dirty = true;
    }

    // A championship stage cleared: the stages count goes up as the CLEARED card
    // ends, with the next stage's round already set (the course is the one of
    // the race last finished)
    void stage_cleared() {
        int course = course_of(finished_round), rank = int32_t(rd32(G_RANK));
        bool from_back = start_cars > 1 && start_position == start_cars - 1 && rank == 0;
        printf("[achievements] stage cleared: round %d, rank %d of %d (from %d)%s\n", finished_round, rank + 1,
               start_cars, start_position + 1, counting() ? "" : " (not counted)");
        if (!counting()) return;
        stats.stages_cleared++;
        if (course >= 0) stats.course[course].clears++;
        if (from_back) stats.from_the_back++;
        dirty = true;
    }

    // Every stage of the championship cleared
    void championship_finished() {
        int rank = int32_t(rd32(G_RANK)) + 1, gears = int32_t(rd32(SELECT_GEARS));
        int64_t total = int32_t(rd32(G_TOTAL));
        printf("[achievements] championship finished: %d, total %.2f s, %s%s\n", rank, seconds_of(total),
               gears ? "manual" : "automatic", counting() ? "" : " (not counted)");
        if (!counting()) return;
        stats.championships++;
        if (rank >= 1 && (stats.best_champ_position == 0 || rank < stats.best_champ_position)) stats.best_champ_position = rank;
        keep_best(stats.best_champ_time, total);
        if (rank == 1) stats.championships_won++;
        if (gears == 1) stats.manual_championships++;
        dirty = true;
    }

    void frame_tick() {
        frame++;
        Game g = Game::read();
        std::lock_guard<std::mutex> lock(mutex);
        if (rebase) {   // nothing counted across the jump
            rebase = false;
            was = g;
            return;
        }
        if (was.sequence != 1 && g.sequence == 1) {   // a credit taken: a new game
            in_game = true;
            cheated = game_counted = false;
        }
        if (g.racing() && counting()) {
            stats.play_frames++;
            dirty = true;
        }
        if (g.racing() && g.clock == CLOCK_COUNTING && was.clock != CLOCK_COUNTING) {   // a race starts
            start_position = g.position;
            start_cars = int32_t(rd32(CARS));
            printf("[achievements] race start: round %d, mode %d, %d of %d on the grid%s\n", g.round, g.mode,
                   start_position + 1, start_cars, counting() ? "" : " (not counted)");
            if (counting()) {
                if (!game_counted) stats.games++;
                game_counted = true;
                stats.races_started++;
                dirty = true;
            }
        }
        // A checkpoint (or the lap line) extends the time left, a second a frame
        // until the extension is in: LAST GASP with under 1 s left as it starts
        bool extending = g.racing() && g.clock == CLOCK_COUNTING && was.clock == CLOCK_COUNTING && g.left > was.left;
        if (extending && !was_extending) {
            printf("[achievements] time extended from %d frames left\n", was.left);
            if (was.left >= 0 && was.left < 60 && counting()) {
                stats.last_gasps++;
                dirty = true;
            }
        }
        was_extending = extending;
        if (g.racing() && g.goal == 1 && was.goal != 1) race_finished(g);
        if (g.racing() && g.mode == MODE_CHAMPIONSHIP && g.stages == was.stages + 1) {
            stage_cleared();
            if (g.complete == 1 && g.stages >= 4) championship_finished();
        }
        // The game's results go into the records table as it leaves the races
        // (a finished championship or practice; after TIME UP the place is -1)
        if (was.phase == 1 && g.phase != 1 && g.sequence == 1 && g.complete == 1) {
            int place = int32_t(rd32(G_RANKING));
            printf("[achievements] records table: place %d%s\n", place >= 0 ? (place & 0xFF) + 1 : -1,
                   counting() ? "" : " (not counted)");
            if (place >= 0 && counting()) {
                stats.records++;
                dirty = true;
            }
        }
        if (in_game && was.sequence == 1 && g.sequence != 1) {   // the game is over
            in_game = cheated = false;
            check_achievements();
            if (dirty) save();
        }
        was = g;
        check_achievements();
        if (dirty && frame - saved_at > uint64_t(frames_of(30))) save();
    }
};

Tracker g;

}  // namespace

const std::vector<AchievementInfo>& achievement_table() { return TABLE; }

int64_t achievement_progress(const Stats& s, int index) {
    const AchievementInfo& a = TABLE[size_t(index)];
    return std::clamp<int64_t>(raw_progress(s, a.key), 0, a.goal);
}

void stats_init() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.load();
    write_achievements_json(g.stats, g.unlocked);   // for the launcher, from the first launch of a new build
    g.on = true;
    g.rebase = true;
}

void stats_save() {
    if (!g.on) return;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.save();
}

StatsView stats_view() {
    std::lock_guard<std::mutex> lock(g.mutex);
    StatsView v{g.stats, g.unlocked};
    if (v.unlocked.size() != TABLE.size()) v.unlocked.assign(TABLE.size(), 0);
    return v;
}

std::vector<int> stats_take_new_achievements() {
    std::lock_guard<std::mutex> lock(g.mutex);
    std::vector<int> r;
    r.swap(g.fresh);
    return r;
}

void stats_frame() {
    if (g.on) g.frame_tick();
}

void stats_game_cheated() {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.cheated && g.on) printf("[achievements] this game counts nothing more (DEBUG)\n");
    g.cheated = true;
}
