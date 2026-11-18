# Game RAM (Sega Rally 2)

PowerPC RAM addresses (big-endian, 32-bit words unless said otherwise) for the
game's race state, found for statistics and achievements (src/runtime/stats.cpp
still holds Star Wars Trilogy's table). "Confirmed" means seen changing in real
runs, not just read in the disassembly. Addresses of code are the original
PowerPC ones (fn_XXXXXXXX in build/gen).

How it was found:

- The game keeps its own debug-display routines and their format strings
  (0xF26F8 `rank=%d/%d`, 0xF29F8 `rankingRank = %d`, 0xF2A2C `goalFlag = %s`,
  0xF2DA8 `lap %2d/%2d`, `sect`, `sectTime`...). The code that prints them
  (0x4C1F8-0x4D760, e.g. fn_0004C560 GAME, fn_0004D19C Section Time) names
  the fields of the structures below.
- F9 snapshots (out/snap/*.ram + .bmp, Sergi's own races): RAM compared with
  what the HUD shows in the picture (position, times, km/h, gear).
- Headless scripted runs (`--headless --no-menu --auto coin@900,start@960,gas
  every 300 from 1500,gas+@4200`, about 180 frames/s, no window) with
  SR2_RAM_DUMPS every 30 frames, diffed in Python. The self-driven car only
  reaches a wall, so the finish was reached through the game's own progress
  code: `SR2_POKE=22CFBC=N` (see "Progress" below) makes the game pass its
  section, checkpoint, lap and goal points itself, with all their normal
  effects (FINISH!!, CLEARED card, next stage, ending, records). Practice:
  `SR2_POKE=22432C=1@960` picks PRACTICE at SELECT GAME.
- One windowed run in the background (LW_BACKGROUND=1, LW_SHOTS=60) of the
  same forced championship, to see what each phase is on screen.

Time units used by the game:

- **frames**: 1/60 s (the clock, lap frame counter).
- **ticks**: 1/6000 s (1/100 of a frame): stage and championship times, lap
  times, records. 137160 ticks = 22.86 s, shown as 00'22"86.
- **HUD BCD**: 0xMSSmmm (minutes, seconds, thousandths as hex digits):
  0x151466 is 1'51"466.

## Main sequence and phase

| Address | Type | What it is |
|---|---|---|
| 0x22D618 | int | Main sequence: 0 attract, 1 game (credit to GAME OVER), -1 power on. Table 0xEDAF0 (known, runtime.cpp) |
| 0x22D624 | int | Phase inside the game (sequence 1), table 0xE911C (12 bytes: init, frame, exit). Confirmed: **1** selects and the races (all stages of a championship), **2** the crowd/podium scene after the last stage of a championship, **3** the replay (championship: replay with the staff credits, TOTAL and per-stage times at the bottom; practice: replay of the race), **4** RANKING table, **5** GAME OVER. Next phase at 0x22D628 (written by the game, e.g. 3 after a practice, 2 at a game over) |
| 0x22435C | int | Select step (known): 3 SELECT GAME, 6 course, 9 car, 12 transmission, 15 loading, 17 racing |

## Game structure 0x22CF14

Base 0x22CF14 (`lis 0x23; addi -0x30EC`). Offsets are from the base.

| Address | Offset | Type | What it is |
|---|---|---|---|
| 0x22CF14 | +0x00 | int | Mode: **0 championship, 2 practice** (-1 before the mode is chosen). Confirmed |
| 0x22CF18 | +0x04 | int | Difficulty (0 NORMAL, 1 HARD, 2 VERYHARD, 3 VERYEASY, 4 EASY) |
| 0x22CF1C | +0x08 | 3 bytes | Player's name (3 letters, '.' padded), copied at race set-up from 0x4C271C (fn_00046AAC). "..." when no name was given |
| 0x22CF28 | +0x14 | int | The car in another numbering (NOT the model index: SELECT CAR 0, 1, 2, 3 gave 7, 4, 5, 2 here). Use 0x224324, the SELECT CAR choice, which stays set through the races: the model index (0 Peugeot 306, 1 Toyota Corolla, 2 Ford Escort, 3 Subaru Impreza, 4 Mitsubishi Lancer, 5 Lancia Stratos, 6 Toyota Celica and 7 Lancia Delta hidden) |
| 0x22CF2C | +0x18 | int | Transmission (0 AT, 1 MT); 0x224334, the select's choice, is the same and is what stats.cpp reads |
| 0x22CF48 | +0x34 | int | **Round** = course of the race being run, table 0xF244C: course id + 6 × kind. Kinds: championship (C_) 0-5, practice (P_) 6-11, attract (A_) 12-17, 18 = the championship ending scene. Course ids: 0 DESERT_OLD, 1 FOREST, **2 MOUNTAIN, 3 DESERT, 4 MONACO (Riviera), 5 SWEDISH (Snowy)**. Confirmed championship order: 3 Desert, 2 Mountain, 5 Snowy, 4 Riviera; practice Desert 9, Mountain 8, Riviera 10 |
| 0x22CF4C | +0x38 | int | Stages in this game: 4 championship, 1 practice |
| 0x22CF50 | +0x3C | int | Laps of this race: 1 in championship stages except Riviera (2); practice 3 (Desert, Mountain), 5 (Riviera) |
| 0x22CF54 | +0x40 | int | Sections per lap (8) |
| 0x22CF58 | +0x44 | int | Checkpoints per lap (Desert 2) |
| 0x22CF5C | +0x48 | int | Course length in track points (256 for the four courses) |
| 0x22CF80 | +0x6C | int | Car status: 0 STOP, 1 GO (racing), 2 BRAKE (time just ran out), 3 OFF (race over) |
| 0x22CF84 | +0x70 | int | Clock state: 1 before the start, 2 counting, 3 stopped (goal or time up) |
| 0x22CF88 | +0x74 | int | Time left, frames (known) |
| 0x22CF8C | +0x78 | int | Time the next stage starts with (carry-over) |
| 0x22CF90 | +0x7C | int | **goalFlag**: 1 from the frame the player crosses the finish line (set by fn_0003E3D4), back to 0 when the next stage starts. Confirmed |
| 0x22CF94 | +0x80 | int | Progress point of the next checkpoint (-1: none left) |
| 0x22CF98 | +0x84 | int | Progress point of the next section |
| 0x22CF9C | +0x88 | int | Progress point of the next lap line |
| 0x22CFA4 | +0x90 | int | **Stages cleared** in this game (0, 1, 2, 3, 4). Confirmed |
| 0x22CFA8 | +0x94 | int | **Laps completed** in this race (0 at the start; reaches +0x3C at the finish). Confirmed. Note: it also moves during the replay (phase 3), so only count it in phase 1 |
| 0x22CFAC | +0x98 | int | Checkpoints passed on this lap |
| 0x22CFB0 | +0x9C | int | Section on this lap (0-7) |
| 0x22CFB8 | +0xA4 | int | Player car's track point last frame |
| 0x22CFBC | +0xA8 | int | **Progress**: track points covered, unwrapped (lap n starts at n × 256). See "Progress" |
| 0x22D018 | +0x104 | int | Race rank, 0 = 1st, as the CLEARED card shows it (RANK 15th = 14). Not refreshed every frame during the race (it can lag the HUD by several places); right at the finish it is the classification. Confirmed against the CLEARED cards |
| 0x22D07C | +0x168 | int | Championship total time, ticks (sum of the stages; the CONGRATULATIONS / ranking time) |
| 0x22D080 | +0x16C | int | Current stage time, ticks (0 at each stage start) |
| 0x22D598 | +0x684 | int × laps | **Lap times**, ticks: lap 1 at 0x22D598, lap 2 at +4, ... written when each lap is completed. Confirmed in practice (218040, 117180, 119940) |
| 0x22D5C0 | +0x6AC | BCD | Last split time (section / lap), HUD BCD |
| 0x22D5F4 | +0x6E0 | int | Countdown of the result / GAME OVER screens |
| 0x22D604 | +0x6F0 | int | 1 once every stage of the game is cleared (championship or practice finished) |
| 0x22D608 | +0x6F4 | int | **rankingRank**: the place in the records table a finished game went into (0 = 1st). -1 = not ranked (set at the start of fn_0002BD60, e.g. after a time up). Bit 0x1000 = the per-car table instead. NB its value before any game is 0 too; read it only after phase leaves 1 |
| 0x22D630 | +0x71C | int | **Race result**: 0 before the start, 1 racing, **2 finished (goal)**, **3 time up**. Confirmed |

## HUD values

| Address | Type | What it is |
|---|---|---|
| 0x13AB74 | int | **Position** shown on the HUD, 0 = 1st (15th/16 = 14). Confirmed on 5 snapshot pictures (8th, 9th, 15th...). Live, every frame |
| 0x225A18 | int | Cars in the race (16 championship, 2 practice) |
| 0x13AB7C | int | Seconds left shown (known) |
| 0x13AB80 | int | Gear shown (1-4). Confirmed |
| 0x13AB8C | BCD | **TOTAL TIME** (championship: all stages so far). Confirmed on pictures (0x151466 = 1'51"466 vs 1'51"43 shown a frame earlier) |
| 0x13AB90 | BCD | **LAP TIME** (current lap; -1 between stages) |
| 0x13AB94 | int | Current lap, 0-based (HUD lap marker) |
| 0x1B0EB0 | int | Current lap time in frames (same value as LAP TIME) |
| 0x22F3A8 | float | **Speed, m/s** (player car). × 3.6 = the HUD km/h (checked on 6 pictures: 47.9→47, 60.7→60, 150.5→151, 185.7→186, 212.8→212). 0x1B0C24 is a copy |

## Progress, checkpoints, lap and goal

fn_0003F1AC (every frame) reads the player car's track point (car object at
[0x2242B0] + 0x48), adds the signed difference to the last point (+0xA4) into
the progress counter +0xA8 (0x22CFBC). fn_0003EA54 then compares the progress
with the next event points:

- progress >= +0x84 → fn_0003DFDC: next section (+0x9C), split time.
- progress >= +0x88 → fn_0003DEA4: **lap completed** (+0x94 + 1, next lap line + 256).
- progress >= +0x80 → fn_0003E308: **checkpoint** (+0x98 + 1, next checkpoint;
  the clock gets its extension).
- laps completed (+0x94) >= laps (+0x3C) → fn_0003E3D4: **GOAL** (goalFlag = 1,
  stage time added to the totals at +0x168/+0x16C).

Checkpoint extension, confirmed: championship Desert checkpoint 1 added 1800
frames (+30 s) to the time left, counted in one second a frame (the time left
goes up by 59 a frame until the extension is in); practice Desert
+10 s per checkpoint, and crossing the lap line adds time as well. The simplest
detection is the time left (0x22CF88) going **up** while the clock counts
(0x22CF84 == 2); the checkpoint counter 0x22CFAC + 1 is the same event
(lap-line extensions don't move it). DEBUG TIME INFINITE holds the time left
at its maximum, so it hides the jumps; the game is marked cheated then anyway.

## Finish, stage cleared, time up, championship result

Confirmed sequence for each championship stage (forced runs, every 30 frames):

1. Goal frame: 0x22CF90 0→1, 0x22D630 1→2, clock state 3, car status 3,
   laps completed = laps. Screen: FINISH!!
2. About 7 s later: CLEARED <COURSE> STAGE! RANK nn / CLEAR TIME / TOTAL TIME.
   Stages cleared (0x22CFA4) + 1 as the card ends, when the next stage's round
   is already in 0x22CF48 (so the course cleared is the round seen at the
   goal); the stage-clear code compares it with the stage count (0x22CF4C).
3. Next stage: round (0x22CF48) changes (3 → 2 → 5 → 4), goal 0x22CF90 and
   0x22D630 go back to 0, clock state 1, then 2 when it starts.
4. After the last stage (Riviera, 2 laps): CONGRATULATIONS!! card with RANK
   and the four stage times; 0x22D604 = 1; then phase 2 (crowd), 3 (replay with
   credits), 4 (RANKING CHAMPIONSHIP), 5 (GAME OVER), then sequence 0.

So, for achievements:

- **Stage cleared**: 0x22CF90 0→1 while 0x22D618 == 1 and phase 0x22D624 == 1.
  Which stage: 0x22CF48 (round) or 0x22CFA4 (stages cleared before it).
  Classification: 0x22D018 + 1 (or HUD 0x13AB74 + 1, which can differ by one
  right at the line).
- **Time up / game over**: 0x22D630 == 3 (0x22CF90 stays 0). Then rankingRank
  is set to -1 and the phase goes to 5 (GAME OVER), sequence 0 after it.
- **Championship finished**: 0x22D604 0→1 with mode 0 (or stages cleared
  0x22CFA4 reaching 4). Final position = the position at the last goal
  (0x22D018 + 1; the CONGRATULATIONS card showed RANK 15th = 14 in the forced
  run). Final time: 0x22D07C (ticks).
- **Practice finished**: 0x22D604 0→1 with mode 2 (goal 0x22CF90 = 1 on the
  same race); course from the round (seen: 9 Desert, 8 Mountain, 10 Riviera;
  11 Snowy by the table); lap times at 0x22D598. Then phase 3 (replay), 4
  (ranking).
- **Best lap** of a race: min of the lap times 0x22D598 + 4 × i for i < laps
  completed. The game also keeps best-lap record lists in backup RAM
  (0x4B2D20 area; the practice Desert run put its three laps into the list at
  0x4B2D7C, sorted, ahead of the default 1860000-tick entries). Not mapped per
  course.

## Records and the name

fn_0002BD60 puts a finished game into the records (the game's backup RAM
block 0x4B2208..0x4B3D1C, per its own `bkram` debug lines):

- 0x4B2210: CHAMPIONSHIP table, 16 entries of 16 bytes: name (3 letters + 4
  '.' at +0), flags byte at +7, time in ticks at +8, car/transmission byte at
  +0xC. Default entries SAS 10'00"00, POO 10'10"00, MOR... (what the RANKING
  screen and the HUD's STAGE TOP 3 show). Confirmed: the forced 22"86
  championship went in at 1st as "..." (time 0x0217C8 = 137160).
- 0x4B2310 + car × 0x80: 8-entry table per car (rankingRank | 0x1000).
- 0x4B2810, 0x4B2890, 0x4B2910, 0x4B2990: per-stage tables, 8 entries each
  (they changed when the stages were cleared).
- Callers: fn_0004AE60 (championship, at the final result), the stage-clear
  code at 0x2B6A4-0x2B748 (practice, after its race; it also sets 0x22D604 and
  picks phase 3), fn_0002B81C (game over).

**Name entry: not found.** No name-entry screen appeared in the forced runs
even with a 1st place in the championship table; the record was written
with the default name "..." taken from 0x4C271C at race set-up, and nothing
in the code writes the name there at a fixed offset. It may be off in the
game's settings, need an input this script never gave, or belong to the link
(network) entry states (NS_ENTRY_* strings). To detect "the player set a
record" without it: rankingRank 0x22D608 >= 0 read once the phase leaves 1
after a finished game (it is -1 after a time up), or a change in the
0x4B2210 table.

## Operator settings (EEPROM copy) and difficulty

The game keeps the whole 93C46 EEPROM (64 words) in RAM at 0x4A9D00. The
operator's GAME ASSIGNMENTS are one word at **0x4A9D18**:

| Bits (mask) | Field |
|---|---|
| 0x80000000 | ADVERTISE |
| (w >> 26) & 3 | CABINET |
| (w >> 22) & 0xF | LINK TYPE |
| **0x00380000** ((w >> 19) & 7) | **DIFFICULTY**: 0 NORMAL, 1 HARD, 2 VERY HARD, 3 VERY EASY, 4 EASY |
| 0x00040000 | DEFAULT VIEW |
| 0x00002000 | SPEED MPH / KMH |
| 0x00000100 | DISPLAY URL |
| 0x00000020 | GAME MODE |
| 0x00000010 | COCKPIT REACTION |

At each new game (code at 0x2CC2C) the difficulty is copied into 0x22CF18
(game structure +0x04). Race set-up, fn_0004041C, picks with it a row of
the time table (pointer per mode at 0xE8348: championship 0xE58C8, practice
0xE62C8): the start time and then each checkpoint's extension, in frames,
-1 ending the list. Championship rows are 0x80 bytes, one per difficulty in
the game's numbering, with 16 bytes per course id. Only the time limits
change, not the rivals. Seconds (read from the ROM; start, checkpoints):

| Difficulty | Desert | Mountain | Snowy | Riviera |
|---|---|---|---|---|
| NORMAL | 50, 30 | 30, 27 | 30, 26 | 38, 34 |
| HARD | 50, **28** | 30, 27 | 30, 26 | 38, 34 |
| VERY HARD | 50, **26** | 30, 27 | 30, 26 | 38, 34 |
| VERY EASY | 50, 30 | **32, 29** | **32, 28** | 38, 34 |
| EASY | 50, 30 | **32**, 27 | **32**, 26 | 38, 34 |

Confirmed in headless runs (`--difficulty X`, checkpoint forced with
`SR2_POKE=22CFBC=110@5000-5001`): 0x22CF18 follows the option (3, 4, 0, 1,
2), the Desert championship starts with 3000 frames (50 s) at every
difficulty, and its first checkpoint adds 1800 frames (30 s) on NORMAL,
EASY and VERY EASY, 1680 (28 s) on HARD and 1560 (26 s) on VERY HARD.

Practice rows (0xE62C8) are 0x30 bytes per course id, 0x180 per difficulty.
Every practice race starts with 60 s; only the **first** extension depends on
the difficulty, and by much more than in the championship. Seconds (start,
then each extension in order):

| Difficulty | Desert | Mountain | Snowy | Riviera |
|---|---|---|---|---|
| NORMAL | 60, 20, 20, 30, 20, 30 | 60, 32, 25, 30, 25, 30 | 60, 30, 23, 26, 23, 26 | 60, 30, 30, 30, 30 |
| HARD | 60, **18**, ... | 60, **30**, ... | 60, **28**, ... | 60, **28**, ... |
| VERY HARD | 60, **13**, ... | 60, **25**, ... | 60, **23**, ... | 60, **23**, ... |
| VERY EASY | 60, **35**, ... | 60, **47**, ... | 60, **45**, ... | 60, **45**, ... |
| EASY | 60, **25**, ... | 60, **37**, ... | 60, **35**, ... | 60, **35**, ... |

Confirmed in headless practice runs (`SR2_POKE=22432C=1@960-1100`, checkpoint
forced with `22CFBC=110@5100-5101`): Desert starts with 3600 frames at every
difficulty and its first checkpoint adds 2100 / 1500 / 1200 / 1080 / 780
frames (35 / 25 / 20 / 18 / 13 s) on VERY EASY / EASY / NORMAL / HARD /
VERY HARD.

DIFFICULTY (PLAY > ARCADE CLASSIC's race set-up, `--difficulty`) rewrites these three
bits of the RAM copy every frame when they differ (runtime.cpp); the EEPROM
itself (save/eeprom.bin) keeps the operator's setting.

The test menu's GAME ASSIGNMENTS rows (table at 0xF94xx: name, values,
count, field) list each field's values in bit order, 0 first: SPEED
0 MPH / 1 KM/H, DEFAULT VIEW 0 DRIVER / 1 BEHIND, GAME MODE 0 NORMAL /
1 SPECIAL. Read back from RAM, the default word 0xC402B61F is KM/H, DRIVER,
NORMAL, difficulty NORMAL. When each one is read:

- SPEED: by the HUD as it draws, so the bit works at any time.
- GAME MODE: at the race set-up (0x46AAC calls 0x2C854 / 0x2C978, which put
  0/1 or 2/3 in the game structure's +0x00 and +0x10: 0 championship, 1
  championship SPECIAL, 2 practice, 3 practice SPECIAL). The owner's manual:
  "Setting of Laps... SPECIAL (more laps, a long-time game mode for events)".
  The laps (+0x3C) come from the table at 0xE8388, 8 words per mode, one per
  course (0x404BC): 1 lap (Riviera 2), SPECIAL 3 (Riviera 5); practice 3
  (Riviera 5), SPECIAL 10. SPECIAL also started a Desert championship with
  80 s instead of 50.
- DEFAULT VIEW: copied at the credit (0x2C7D4, from the main loop at 0x2DD04)
  into the game structure's +0x20 (0x22CF34): 0xC9 DRIVER, 0xCA BEHIND. PLAY
  finds the game past that point, so the race set-up also writes 0x22CF34
  while the selects are on (select step 3 to 14).

### Coin assignments, FREE PLAY

The COIN ASSIGNMENTS word is at **0x4A9D10** (EEPROM copy): bit
**0x00200000 = FREE PLAY**, (w >> 22) & 0xFF the COIN/CREDIT SETTING number.
The game tests the bit before it counts credits (0x1958, 0x1FE4, 0x27C4C,
0x29E2C, 0x2CAF8, 0x663B4; credits at 0x4B21A0). The test menu edits a copy
of it at 0x222CC4 (writing there changes nothing in the game). With the bit
set the attract shows PRESS START BUTTON and FREE PLAY, and START alone
begins a game.

Changed while the attract is on screen, the game prints CREDIT 0 over FREE
PLAY without clearing the line (CREDITL0.Y). ARCADE ORIGINAL therefore
restarts the program once the attract is reached, as a cabinet does when the
operator leaves the TEST menu: at **0x0** (the program's start: power-on,
SEGA logo, attract, about 400 frames). 0x4C is not that: it enters the TEST
menu first (0x137124 = 1, then 0x7107C), which ran unseen for some 1900
frames. Writing -1 (power on) to the main sequence's next entry leaves the
game stuck.

## Other fields seen (not needed so far)

| Address | What it is |
|---|---|
| 0x225960 | Car display / LOD block (known: +0xCC count, +0xD0 list, +0xF0 car pointers). +0xB4 (0x225A14) is the player's position too (0-based, same as the HUD), +0xB8 the car count, +0xC0 a normalised speed (not km/h), +0xC4 gear, +0xC8 player car geometry pointer |
| 0x22D61C | Next main sequence (known) |
| 0x22D604 | See above (+0x6F0) |
| 0x22CFD8 / 0x22CFDC | Counters that step with the co-driver's calls (not checked) |
