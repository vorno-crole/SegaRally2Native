// Records and achievements (Sega Rally 2): what the player has done over all
// games, saved in save/stats.ini, and local achievements
// (save/achievements.ini) announced with a toast and listed in the RECORDS
// screen of the main menu.
//
// The game thread reads the game's race state at every vblank (its game
// structure at 0x22CF14, the HUD's position, the phase of the game; see
// stats.cpp and docs/game_ram.md). The window thread reads a copy of
// everything to draw.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The four courses, in the championship's order (the game's course ids are
// 3, 2, 5 and 4: its round at 0x22CF48 is the id + 6 for a practice race)
constexpr int STAT_COURSES = 4;
inline constexpr const wchar_t* STAT_COURSE_NAMES[STAT_COURSES] = {L"DESERT", L"MOUNTAIN", L"SNOWY", L"RIVIERA"};
// Cars by the game's model index (SELECT CAR, 0x224324): 0 Peugeot 306, 1
// Toyota Corolla, 2 Ford Escort, 3 Subaru Impreza, 4 Mitsubishi Lancer, 5
// Lancia Stratos, then the two hidden ones (6 Toyota Celica, 7 Lancia Delta)
constexpr int STAT_CARS = 8, STAT_OPEN_CARS = 6;

// Times are the game's: 1/6000 s (1/100 of a frame), 0 = none yet
struct Stats {
    // Totals
    int64_t games = 0, play_frames = 0;
    int64_t races_started = 0, races_finished = 0;   // finished: the finish line crossed, not TIME UP
    int64_t stages_cleared = 0;                      // championship stages
    int64_t championships = 0;                       // finished, all four stages
    int best_champ_position = 0;                     // 1 = 1st; 0 none yet
    int64_t best_champ_time = 0;                     // total of a finished championship
    // Per course: [0] championship, [1] practice
    struct Course {
        int clears = 0;                              // championship stages of this course cleared
        int64_t best_lap[2] = {0, 0}, best_time[2] = {0, 0};
    } course[STAT_COURSES];
    int cars_finished = 0;                           // bit per model: a race finished with it
    int practice_courses = 0;                        // bit per course: a practice race finished on it
    int championships_won = 0;                       // finished 1st
    int from_the_back = 0;                           // stages won from the last place on the grid
    int manual_championships = 0;                    // finished with the manual gearbox
    int last_gasps = 0;                              // checkpoints reached with under 1 s left
    int records = 0;                                 // games that got into the records table
};

// Achievement table (stats.cpp): progress is read from Stats
struct AchievementInfo {
    const char* key;          // in save/achievements.ini
    const wchar_t* name;
    const wchar_t* text;      // what it asks
    int64_t goal;             // progress needed (1 for yes / no ones)
};
const std::vector<AchievementInfo>& achievement_table();
int64_t achievement_progress(const Stats& s, int index);   // capped at the goal

// Window thread
void stats_init();                  // a normal launch: tracking on, files loaded (tests leave it off)
void stats_game_cheated();          // DEBUG jumps, TIME INFINITE: the game on counts nothing more
struct StatsView {
    Stats stats;
    std::vector<int64_t> unlocked;  // per achievement: unix time, 0 locked
};
StatsView stats_view();
std::vector<int> stats_take_new_achievements();   // unlocked since the last call, for the toast
void stats_save();                  // at exit, waiting for the files (it also saves by itself as things change, the play time every 5 min)

// Game thread
void stats_frame();                 // every vblank
