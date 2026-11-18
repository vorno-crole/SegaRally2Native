#include "r3d_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>

#include "r3d_scene.h"

// The game's dark patch under a car (its shadow): a model of see-through,
// black, self-lit, untextured polygons only
static bool is_patch(const R3DInstance& in, const R3DScene& out, const R3DViewport& v) {
    if (in.opaque[1] > in.opaque[0] || in.trans[1] <= in.trans[0]) return false;
    const R3DVertex& x0 = out.vertices[v.trans_first + in.trans[0]];
    return (x0.flags & (R3D_LUMINOUS | R3D_TEXTURED)) == R3D_LUMINOUS && (x0.color & 0x00FFFFFFu) == 0;
}

// Sega Rally 2 (not in the original): the roadside boards, fences and walls
// do not reach the ground in the game's models: some 10 cm of air under them.
// Seen from the car it does not show as a gap, but under each board on the
// sun's side a strip of sunlit ground did: the ground behind the board, seen
// under it, and the sun let through onto the ground in front (a bright strip
// along the foot of the boards, between them and their shadow on the road).
// The foot of what stands upright near the camera is taken down to the
// ground under it, where that is no more than 30 cm below (the picture on the
// board stretches by as much: not to be seen). The scenery only (the models
// that move: settle_actors). SR2_SCENERY_AS_IS: as modelled, to compare
static void settle_scenery(const std::vector<R3DInstance>& list, const float* axes, R3DScene& out, const R3DViewport& v) {
    static const bool off = getenv("SR2_SCENERY_AS_IS") != nullptr;
    if (off) return;
    const float *ax = axes, *up = axes + 3, *az = axes + 6;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float reach = 60.0f, cell = 2.0f, deepest = 0.3f;
    struct Ground {
        float u[3], h[3], w[3];
    };
    std::vector<Ground> grounds;
    std::vector<std::pair<uint64_t, uint32_t>> cells;   // cell, ground
    std::vector<uint32_t> upright;                      // first vertex of each
    auto cell_key = [&](int cu, int cw) { return (uint64_t(uint32_t(cu)) << 32) | uint32_t(cw); };
    for (const R3DInstance& in : list) {
        if (!r3d_same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i + 2 < in.opaque[1]; i += 3) {
            const R3DVertex* t = &out.vertices[v.opaque_first + i];
            if (t[0].flags & (R3D_ACTOR | R3D_LUMINOUS)) continue;
            float d2 = std::min({dot(t[0].pos, t[0].pos), dot(t[1].pos, t[1].pos), dot(t[2].pos, t[2].pos)});
            if (d2 > reach * reach) continue;
            float e1[3], e2[3];
            for (int c = 0; c < 3; c++) e1[c] = t[1].pos[c] - t[0].pos[c], e2[c] = t[2].pos[c] - t[0].pos[c];
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            float len = std::sqrt(dot(n, n));
            if (len <= 0) continue;
            float rise = std::fabs(dot(n, up)) / len;
            if (rise < 0.3f) {
                upright.push_back(v.opaque_first + i);
            } else if (rise > 0.85f && len > 6.0f) {
                // (the ground: flat and of some size, 3 square metres at least. A
                // banner's slanting corner below another, high on its poles, is not:
                // the banner above it was pulled down to it)
                Ground g;
                for (int k = 0; k < 3; k++) g.u[k] = dot(t[k].pos, ax), g.h[k] = dot(t[k].pos, up), g.w[k] = dot(t[k].pos, az);
                int u0 = int(std::floor(std::min({g.u[0], g.u[1], g.u[2]}) / cell)), u1 = int(std::floor(std::max({g.u[0], g.u[1], g.u[2]}) / cell));
                int w0 = int(std::floor(std::min({g.w[0], g.w[1], g.w[2]}) / cell)), w1 = int(std::floor(std::max({g.w[0], g.w[1], g.w[2]}) / cell));
                if ((u1 - u0 + 1) * (w1 - w0 + 1) > 400) continue;   // (a huge one: the far plain, the sea)
                for (int cu = u0; cu <= u1; cu++)
                    for (int cw = w0; cw <= w1; cw++) cells.push_back({cell_key(cu, cw), uint32_t(grounds.size())});
                grounds.push_back(g);
            }
        }
    }
    if (upright.empty() || grounds.empty()) return;
    std::sort(cells.begin(), cells.end());
    // The gap under a point: down to the highest ground below it within reach, or 0
    auto gap_under = [&](float u, float h, float w) {
        uint64_t key = cell_key(int(std::floor(u / cell)), int(std::floor(w / cell)));
        float best = 0;
        for (auto it = std::lower_bound(cells.begin(), cells.end(), std::make_pair(key, uint32_t(0)));
             it != cells.end() && it->first == key; ++it) {
            const Ground& g = grounds[it->second];
            float d = (g.w[1] - g.w[2]) * (g.u[0] - g.u[2]) + (g.u[2] - g.u[1]) * (g.w[0] - g.w[2]);
            if (std::fabs(d) < 1e-9f) continue;
            float a = ((g.w[1] - g.w[2]) * (u - g.u[2]) + (g.u[2] - g.u[1]) * (w - g.w[2])) / d;
            float b = ((g.w[2] - g.w[0]) * (u - g.u[2]) + (g.u[0] - g.u[2]) * (w - g.w[2])) / d;
            float c = 1.0f - a - b;
            const float edge = -0.02f;   // (a little past its edges: the boards stand along the road's)
            if (a < edge || b < edge || c < edge) continue;
            float below = h - (a * g.h[0] + b * g.h[1] + c * g.h[2]);
            if (below > 0.02f && below <= deepest && (best == 0 || below < best)) best = below;
        }
        return best;
    };
    for (uint32_t first : upright) {
        R3DVertex* t = &out.vertices[first];
        float h[3];
        for (int k = 0; k < 3; k++) h[k] = dot(t[k].pos, up);
        float foot = std::min({h[0], h[1], h[2]});
        for (int k = 0; k < 3; k++) {
            if (h[k] > foot + 0.05f) continue;
            float gap = gap_under(dot(t[k].pos, ax), h[k], dot(t[k].pos, az));
            if (gap <= 0) continue;
            for (int c = 0; c < 3; c++) {
                t[k].pos[c] -= up[c] * (gap + 0.03f);
                t[k].prev[c] -= up[c] * (gap + 0.03f);
            }
        }
    }
}

// The highest surface of the scenery under each of some points (not what is
// above them: a roof, a branch): the points in cells of 2 m across
struct GroundQuery {
    float u, h, w;           // across, height, across
    float ground = -1e30f;   // (stays so where there is none)
};
static void ground_under(const std::vector<R3DInstance>& list, const float* axes, const R3DScene& out, const R3DViewport& v,
                         std::vector<GroundQuery>& queries) {
    const float *ax = axes, *up = axes + 3, *az = axes + 6;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float cell = 2.0f;
    auto cell_of = [&](float u, float w) {
        return (uint64_t(uint32_t(int32_t(std::floor(u / cell)))) << 32) | uint32_t(int32_t(std::floor(w / cell)));
    };
    std::vector<std::pair<uint64_t, int>> cells;
    for (size_t m = 0; m < queries.size(); m++) cells.push_back({cell_of(queries[m].u, queries[m].w), int(m)});
    std::sort(cells.begin(), cells.end());
    for (const R3DInstance& in : list) {
        if (!r3d_same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i + 2 < in.opaque[1]; i += 3) {
            const R3DVertex* t = &out.vertices[v.opaque_first + i];
            if (t[0].flags & R3D_ACTOR) continue;   // (a car turned the scenery's way, settle_cars)
            float u[3], h[3], w[3];
            for (int k = 0; k < 3; k++) u[k] = dot(t[k].pos, ax), h[k] = dot(t[k].pos, up), w[k] = dot(t[k].pos, az);
            float u0 = std::min({u[0], u[1], u[2]}), u1 = std::max({u[0], u[1], u[2]});
            float w0 = std::min({w[0], w[1], w[2]}), w1 = std::max({w[0], w[1], w[2]});
            if (u1 - u0 > 60.0f || w1 - w0 > 60.0f) continue;   // (huge ones: none is ground near a model)
            for (float cu = std::floor(u0 / cell) * cell; cu <= u1; cu += cell)
                for (float cw = std::floor(w0 / cell) * cell; cw <= w1; cw += cell) {
                    uint64_t key = cell_of(cu + 0.5f * cell, cw + 0.5f * cell);
                    auto it = std::lower_bound(cells.begin(), cells.end(), std::make_pair(key, -1));
                    for (; it != cells.end() && it->first == key; ++it) {
                        GroundQuery& m = queries[it->second];
                        // Inside the triangle across (barycentric weights)
                        float d = (w[1] - w[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (w[0] - w[2]);
                        if (std::fabs(d) < 1e-6f) continue;
                        float a = ((w[1] - w[2]) * (m.u - u[2]) + (u[2] - u[1]) * (m.w - w[2])) / d;
                        float b = ((w[2] - w[0]) * (m.u - u[2]) + (u[0] - u[2]) * (m.w - w[2])) / d;
                        if (a < 0 || b < 0 || a + b > 1) continue;
                        float g = a * h[0] + b * h[1] + (1 - a - b) * h[2];
                        if (g <= m.h + 0.3f) m.ground = std::max(m.ground, g);   // (not a roof above it)
                    }
                }
        }
    }
}

// Sega Rally 2: the cars of the attract's demos (and of the replays) are
// all models of the world, not in viewports of their own as the player's and
// the rivals near it are in a race, and the game keeps them above the road it
// draws just the same: on its own ground, 25 cm over the road in the desert,
// 15 in the mountain. They are set down as R3DSceneBuilder::settle_player_car
// does those: by the height of the dark patch the game lays under each car
// (at its tyres when it stands, left on the ground when it jumps) over the
// road drawn below it, the middle one of the patch's corners, followed
// smoothly from frame to frame and kept while it cannot be measured. All of
// the car goes down by it, whatever its own height: every model drawn over
// the patch (bodywork, wheels, the glass and the painted shine, see-through
// models of their own, the polygons facing away that still cast shadows), and
// the patch stops just clear of the road. Measured at the car's lowest point
// instead, a jump read as floating and was pressed flat, then let go at once;
// and the long lenses film from 40 m and more, where only some pieces were
// looked at: the bodywork came down without its wheels (matt: not taken for
// a car's), which showed through the wings, or without its glass, left over
// the roof. `taken`: the models dealt with here. SR2_WORLD_CARS_AS_IS=1:
// left to settle_actors, as before
static void settle_cars(const std::vector<R3DInstance>& list, const float* axes, R3DScene& out, const R3DViewport& v,
                        std::vector<char>& taken) {
    static const bool off = getenv("SR2_WORLD_CARS_AS_IS") != nullptr;
    static const bool log = getenv("SR2_ACTORLOG") != nullptr;
    struct Lift {
        float height = -1;   // how far down (-1: not known yet; a little under 0: up)
        uint32_t skips = 0;
        bool kerb = false;                 // a step under the patch is being waited out
        float raw = 0;                     // the last measurement
        uint64_t seen = 0, measured = 0;   // frames
        float at[3] = {0, 0, 0};           // where its patch was
        // The car's models, for the frames its patch is not drawn: their keys,
        // and their model and place last frame (a key the game changes)
        struct Piece {
            uint32_t key, model;
            float at[3];
        };
        std::vector<Piece> pieces;
    };
    static std::unordered_map<uint64_t, Lift> lifts;
    if (off) return;
    const float *ax = axes, *up = axes + 3, *az = axes + 6;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float reach = 250.0f;   // metres from the camera
    // A car's patch: a model of black, self-lit, untextured see-through polygons, the size of a car
    struct Car {
        size_t patch;
        float lo[3], hi[3];
        size_t first, end;   // its corners among the queries
        float down;
        uint64_t lift;
    };
    std::vector<Car> cars;
    std::vector<GroundQuery> queries;
    for (size_t n = 0; n < list.size(); n++) {
        const R3DInstance& in = list[n];
        if (r3d_same_axes(in.axes, axes) || !is_patch(in, out, v) || dot(in.origin, in.origin) > reach * reach) continue;
        Car c{n, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, queries.size(), 0, 0, (uint64_t(in.key) << 32) | in.model};
        for (uint32_t i = in.trans[0]; i < in.trans[1]; i++) {
            const R3DVertex& x = out.vertices[v.trans_first + i];
            float p[3] = {dot(x.pos, ax), dot(x.pos, up), dot(x.pos, az)};
            for (int k = 0; k < 3; k++) c.lo[k] = std::min(c.lo[k], p[k]), c.hi[k] = std::max(c.hi[k], p[k]);
            if ((i - in.trans[0]) % 3 == 0) queries.push_back({p[0], p[1], p[2]});
        }
        float longest = std::max(c.hi[0] - c.lo[0], c.hi[2] - c.lo[2]);
        if (longest < 2.5f || longest > 7.0f || c.hi[1] - c.lo[1] > 3.0f) {
            queries.resize(c.first);   // (some other dark thing)
            continue;
        }
        c.end = queries.size();
        cars.push_back(c);
    }
    // A car's lift is kept for a second without its patch: the desert demo's
    // jump lasts 35 frames, and with half a second the car came up 14 cm for its
    // last frames in the air, then dropped 16 cm onto the road in one frame
    // as it landed. SR2_LIFT_HOLD=N: N frames (30 before, to compare)
    static const uint64_t hold = getenv("SR2_LIFT_HOLD") ? uint64_t(atoi(getenv("SR2_LIFT_HOLD"))) : 60;
    for (auto it = lifts.begin(); it != lifts.end();)
        it = out.frame - it->second.seen > hold ? lifts.erase(it) : std::next(it);
    if (cars.empty() && lifts.empty()) return;
    // A car driving straight along one of the world's axes is turned exactly
    // the scenery's way: its bodywork, glass and shine (the models placed at
    // the car's own origin; its wheels turn) were taken for scenery, left up
    // while the wheels and the patch went down, the body riding 20 cm over
    // its wheels, then dropping onto them as the car turned a little (the
    // desert demo, the Lancer coming along the fence). The models turned the
    // scenery's way with their origin over a patch, together the shine of a
    // car's paint (see `shine` below), are that car's, and its models of the
    // world (R3D_ACTOR: not ground, not scenery to set down, lit and shadowed
    // as a car)
    std::vector<int> aligned(list.size(), -1);
    for (size_t i = 0; i < cars.size(); i++) {
        const Car& c = cars[i];
        std::vector<std::pair<std::array<int32_t, 3>, size_t>> over;   // (origin in cm, instance)
        for (size_t n = 0; n < list.size(); n++) {
            const R3DInstance& in = list[n];
            if (!r3d_same_axes(in.axes, axes)) continue;
            float u = dot(in.origin, ax), h = dot(in.origin, up), w = dot(in.origin, az);
            if (u < c.lo[0] - 0.5f || u > c.hi[0] + 0.5f || w < c.lo[2] - 0.5f || w > c.hi[2] + 0.5f) continue;
            if (h < c.lo[1] - 1.0f || h > c.hi[1] + 4.0f) continue;
            over.push_back({{int32_t(std::lround(u * 100)), int32_t(std::lround(h * 100)), int32_t(std::lround(w * 100))}, n});
        }
        std::sort(over.begin(), over.end());
        for (size_t a = 0; a < over.size();) {
            size_t b = a;
            uint32_t shiny = 0;
            // ... and no bigger than a car over its patch: the snowy course's
            // icy road (shiny, the scenery's way) has models whose origin lies
            // under the player's patch on the grid; taken for the car's, the
            // road ahead of it went dark until the car drove off them
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            auto grow = [&](const R3DVertex& x) {
                float p[3] = {dot(x.pos, ax), dot(x.pos, up), dot(x.pos, az)};
                for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
            };
            for (; b < over.size() && over[b].first == over[a].first; b++) {
                const R3DInstance& in = list[over[b].second];
                for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) {
                    if (out.vertices[v.opaque_first + k].flags & R3D_SPECULAR) shiny++;
                    grow(out.vertices[v.opaque_first + k]);
                }
                for (uint32_t k = in.trans[0]; k < in.trans[1]; k++) grow(out.vertices[v.trans_first + k]);
            }
            bool car_sized = lo[0] >= c.lo[0] - 1.0f && hi[0] <= c.hi[0] + 1.0f && lo[2] >= c.lo[2] - 1.0f && hi[2] <= c.hi[2] + 1.0f &&
                             hi[1] - lo[1] <= 2.5f;
            if (shiny >= 300 && car_sized)
                for (size_t k = a; k < b; k++) {
                    const R3DInstance& in = list[over[k].second];
                    aligned[over[k].second] = int(i);
                    for (uint32_t q = in.opaque[0]; q < in.opaque[1]; q++) out.vertices[v.opaque_first + q].flags |= R3D_ACTOR;
                    for (uint32_t q = in.trans[0]; q < in.trans[1]; q++) out.vertices[v.trans_first + q].flags |= R3D_ACTOR;
                }
            a = b;
        }
    }
    if (!cars.empty()) ground_under(list, axes, out, v, queries);
    for (Car& c : cars) {
        const R3DInstance& in = list[c.patch];
        std::vector<float> gaps;
        for (size_t q = c.first; q < c.end; q++)
            if (queries[q].ground > -1e29f) gaps.push_back(queries[q].h - queries[q].ground);
        float raw = -1;
        if (gaps.size() >= 8) {
            std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
            raw = gaps[gaps.size() / 2];
        }
        Lift& lift = lifts[c.lift];
        float at[3], moved = 0;
        for (int k = 0; k < 3; k++) at[k] = 0.5f * (c.lo[k] + c.hi[k]), moved = std::max(moved, std::fabs(at[k] - lift.at[k]));
        // (another camera, another moment of the demo: nothing to follow on from)
        uint64_t frames = out.frame - lift.seen;
        if (frames > hold || moved > 3.0f * float(frames)) lift.height = -1, lift.skips = 0;
        lift.seen = out.frame;
        lift.pieces.clear();
        memcpy(lift.at, at, sizeof at);
        // (under the road too, where the game's ground dips below the one it draws: the car comes up)
        bool measured = raw > -0.15f && raw < 0.45f;
        // A kerb, a bump: the ground under the patch steps from one frame to the
        // next, and is not followed unless it stays. Where it changes little by
        // little (the road drawn coarser away from the camera) or was not seen
        // for a while (the patch left out as the car went past the camera), it
        // is followed at once. Never in one jump either way: a car at 40 m
        // hovered 15 to 28 cm while that was taken for a kerb, then dropped onto
        // the road in a single frame (the desert demo, the second car past the camera)
        bool step = measured && out.frame - lift.measured <= 3 && std::fabs(raw - lift.raw) > 0.05f;
        if (measured) {
            if (lift.height < -0.5f) lift.height = raw, lift.skips = 0, lift.kerb = false;
            else if (std::fabs(raw - lift.height) <= 0.1f) lift.height += 0.5f * (raw - lift.height), lift.skips = 0, lift.kerb = false;
            else {
                if (step) lift.kerb = true, lift.skips = 0;
                if (!lift.kerb || ++lift.skips >= 30)
                    lift.height += std::clamp(0.5f * (raw - lift.height), -0.03f, 0.03f);   // (3 cm a frame)
            }
            lift.raw = raw;
            lift.measured = out.frame;
        } else if (out.frame - lift.measured > 120) lift.height = -1;
        c.down = lift.height < -0.5f ? 0.0f : lift.height;
        if (log)
            printf("[car] frame %llu world car %06X at %.1f %.1f %.1f corners %zu raw %.3f down %.3f\n",
                   (unsigned long long)out.frame, in.model, at[0], at[1], at[2], gaps.size(), raw, c.down);
    }
    // The models of the cars not drawn over their patch this frame
    struct Held {
        Lift::Piece* piece;
        float height;
        bool claimed;
    };
    std::vector<Held> held;
    std::unordered_map<uint32_t, size_t> held_keys;
    for (auto& [key, lift] : lifts)
        if (lift.seen != out.frame && lift.height > -0.5f)
            for (Lift::Piece& piece : lift.pieces) held_keys[piece.key] = held.size(), held.push_back({&piece, lift.height, false});
    // Each model is one of them by its key, or else by its model and place
    // (within 3 m of where that one was last frame): the game hands a car's
    // models other keys now and then (the mountain demo's car far down the
    // road: its bodywork lost its lift every other frame, 16 cm up and down)
    std::vector<int> held_as(list.size(), -1);
    if (!held.empty()) {
        for (size_t n = 0; n < list.size(); n++) {
            auto it = held_keys.find(list[n].key);
            if (it != held_keys.end() && !held[it->second].claimed) held_as[n] = int(it->second), held[it->second].claimed = true;
        }
        for (size_t n = 0; n < list.size(); n++) {
            const R3DInstance& in = list[n];
            if (held_as[n] >= 0) continue;
            float best = 3.0f * 3.0f;
            for (size_t h = 0; h < held.size(); h++) {
                const Held& c = held[h];
                if (c.claimed || c.piece->model != in.model) continue;
                float d[3] = {in.origin[0] - c.piece->at[0], in.origin[1] - c.piece->at[1], in.origin[2] - c.piece->at[2]};
                if (dot(d, d) < best) best = dot(d, d), held_as[n] = int(h);
            }
            if (held_as[n] >= 0) held[held_as[n]].claimed = true;
        }
    }
    // Each model drawn over a patch (its origin: the car's, a wheel's) is that
    // car's. A patch with no car over it is left alone: in a race the cars near
    // the camera are in viewports of their own, over patches in the world like
    // these (R3DSceneBuilder::settle_player_car measures and moves those)
    std::vector<int> owner(list.size(), -1);
    // A car's shine: the corners of its paint and glass (specular, solid). A
    // man standing over a patch is no car: the start line's flagman walks into
    // the corner of the player's patch during the countdown in the desert,
    // some 2600 solid corners but only 12 shiny ones (the cars of the demos
    // have 1900 and more). Taken for its car, he set the patch down under the
    // road (it vanished for half a second) and sank into it himself. The
    // mountain's flagman waves a flag of 600 shiny corners over the player's
    // patch on the grid: whenever it dipped it made 612 with him, taken for
    // a car (300 then), the patch went down 5 cm and the player's car, set
    // down by its patch, hopped with it. The cars of the demos have 3800 and
    // more (the whole attract), so 1500
    std::vector<uint32_t> shine(cars.size(), 0);
    for (size_t n = 0; n < list.size(); n++) {
        const R3DInstance& in = list[n];
        if (r3d_same_axes(in.axes, axes) && aligned[n] < 0) continue;
        float u = dot(in.origin, ax), h = dot(in.origin, up), w = dot(in.origin, az);
        float nearest = 1e30f;
        owner[n] = aligned[n];
        for (size_t i = 0; i < cars.size() && aligned[n] < 0; i++) {
            const Car& c = cars[i];
            if (n == c.patch) {
                owner[n] = int(i);
                break;
            }
            if (u < c.lo[0] - 0.5f || u > c.hi[0] + 0.5f || w < c.lo[2] - 0.5f || w > c.hi[2] + 0.5f) continue;
            if (h < c.lo[1] - 1.0f || h > c.hi[1] + 4.0f) continue;
            float du = u - 0.5f * (c.lo[0] + c.hi[0]), dw = w - 0.5f * (c.lo[2] + c.hi[2]);
            if (du * du + dw * dw < nearest) nearest = du * du + dw * dw, owner[n] = int(i);
        }
        if (owner[n] >= 0)
            for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++)
                if (out.vertices[v.opaque_first + k].flags & R3D_SPECULAR) shine[owner[n]]++;
    }
    // ... and only over a car of the world's own: its bodywork (a model of 200
    // polygons and more) right over the middle of the patch. The player's car
    // (and a rival near it) is drawn in a viewport of its own over a patch in
    // the world, which R3DSceneBuilder::settle_player_car measures; at the
    // start a rival in the world beside it reached over the edge of that patch
    // with shine enough to be taken for its car: the patch went down here, the
    // player's car went down with it until it lost its shadow, then sprang
    // back up once the rival had gone
    for (size_t i = 0; i < cars.size(); i++) {
        const Car& c = cars[i];
        float cu = 0.5f * (c.lo[0] + c.hi[0]), cw = 0.5f * (c.lo[2] + c.hi[2]);
        bool body = false;
        for (size_t n = 0; n < list.size() && !body; n++) {
            const R3DInstance& in = list[n];
            if (owner[n] != int(i) || in.polygons < 200 || in.opaque[1] <= in.opaque[0]) continue;
            float du = dot(in.origin, ax) - cu, dw = dot(in.origin, az) - cw;
            body = du * du + dw * dw < 1.0f;
        }
        if (!body) shine[i] = 0;
    }
    std::vector<float> downs(list.size(), 0.0f);
    for (size_t n = 0; n < list.size(); n++) {
        const R3DInstance& in = list[n];
        if (r3d_same_axes(in.axes, axes) && aligned[n] < 0 && held_as[n] < 0) continue;
        float down = 0;
        if (owner[n] >= 0) {
            const Car& c = cars[owner[n]];
            if (shine[owner[n]] < 1500) continue;
            down = c.down - (n == c.patch ? 0.03f : 0.0f);   // (the patch on the road itself would flicker through it)
            lifts[c.lift].pieces.push_back({in.key, in.model, {in.origin[0], in.origin[1], in.origin[2]}});
        } else {
            // ... or was a car's while its patch was drawn, a moment ago (the game leaves the patch out now and then)
            if (held_as[n] < 0) continue;
            Lift::Piece& piece = *held[held_as[n]].piece;
            piece.key = in.key;   // (followed from here)
            memcpy(piece.at, in.origin, sizeof piece.at);
            down = held[held_as[n]].height;
        }
        taken[n] = 1;
        downs[n] = down;
        if (aligned[n] < 0 && r3d_same_axes(in.axes, axes)) {   // (held: a car's turned the scenery's way)
            for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) out.vertices[v.opaque_first + k].flags |= R3D_ACTOR;
            for (uint32_t k = in.trans[0]; k < in.trans[1]; k++) out.vertices[v.trans_first + k].flags |= R3D_ACTOR;
        }
    }
    // What rides in a car goes down with it, by exactly as much: the drivers'
    // heads, the wheels, the glass. While the patch is not drawn (the car in
    // the air: the desert demo's jump) the car's models were found by the keys
    // they had a moment ago, and the heads are drawn through other nodes from
    // one frame to the next (a new key almost every frame): left at the game's
    // height while the bodywork went down, they rose 14 cm out of their necks
    // until the car landed (and the wheels with them). A model turned its own
    // way (not scenery, not a sprite, not the patch) whose origin is inside a
    // car's bodywork, its box in the bodywork's own axes, goes down as that
    // bodywork does: the closest fitting model round it. A bodywork this does
    // not set down (a car with no patch, near the camera: settle_actors grounds
    // it) takes its riders along there instead: the snowy demo's car with one
    // head held 19 cm over the other. SR2_RIDERS_AS_IS=1: as before (to compare)
    static const bool riders_as_is = getenv("SR2_RIDERS_AS_IS") != nullptr;
    struct Body {
        const R3DInstance* in;
        float lo[3], hi[3], down;
        bool taken;
    };
    std::vector<Body> bodies;
    for (size_t n = 0; n < list.size() && !riders_as_is; n++) {
        const R3DInstance& in = list[n];
        if (in.polygons < 200 || in.opaque[1] <= in.opaque[0] || (!taken[n] && r3d_same_axes(in.axes, axes))) continue;
        Body b{&in, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, downs[n], taken[n] != 0};
        for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) {
            const float* p = out.vertices[v.opaque_first + k].pos;
            float d[3] = {p[0] - in.origin[0], p[1] - in.origin[1], p[2] - in.origin[2]};
            for (int c = 0; c < 3; c++) {
                float l = dot(d, in.axes + c * 3);
                b.lo[c] = std::min(b.lo[c], l), b.hi[c] = std::max(b.hi[c], l);
            }
        }
        bodies.push_back(b);
    }
    for (size_t n = 0; n < list.size() && !bodies.empty(); n++) {
        const R3DInstance& in = list[n];
        if (in.polygons < 8 || is_patch(in, out, v)) continue;
        bool scenery_way = r3d_same_axes(in.axes, axes);
        const Body* ride = nullptr;
        auto size = [](const Body& b) { return (b.hi[0] - b.lo[0]) * (b.hi[1] - b.lo[1]) * (b.hi[2] - b.lo[2]); };
        for (const Body& b : bodies) {
            if (b.in == &in || b.in->polygons <= in.polygons || (ride && size(*ride) <= size(b))) continue;
            // (turned the scenery's way too: a head at rest in a car driving
            // straight along one of the world's axes, the first frame of a shot
            // of the desert demo; then all of it inside the bodywork, as no
            // piece of the road or the scenery around the car is)
            if (scenery_way && !r3d_same_axes(in.axes, b.in->axes)) continue;
            auto inside = [&](const float* p) {
                float d[3] = {p[0] - b.in->origin[0], p[1] - b.in->origin[1], p[2] - b.in->origin[2]};
                for (int c = 0; c < 3; c++) {
                    float l = dot(d, b.in->axes + c * 3);
                    if (l < b.lo[c] - 0.1f || l > b.hi[c] + 0.1f) return false;
                }
                return true;
            };
            bool in_car = inside(in.origin);
            for (uint32_t k = in.opaque[0]; k < in.opaque[1] && in_car && scenery_way; k++) in_car = inside(out.vertices[v.opaque_first + k].pos);
            for (uint32_t k = in.trans[0]; k < in.trans[1] && in_car && scenery_way; k++) in_car = inside(out.vertices[v.trans_first + k].pos);
            if (in_car) ride = &b;
        }
        if (!ride) continue;
        if (!ride->taken) {
            if (in.polygons < 200) taken[n] = 0, downs[n] = 0;   // (with its bodywork, settle_actors)
            continue;
        }
        taken[n] = 1;
        downs[n] = ride->down;
        if (scenery_way) {   // (a car's: not ground, lit and shadowed as a car)
            for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) out.vertices[v.opaque_first + k].flags |= R3D_ACTOR;
            for (uint32_t k = in.trans[0]; k < in.trans[1]; k++) out.vertices[v.trans_first + k].flags |= R3D_ACTOR;
        }
    }
    for (size_t n = 0; n < list.size(); n++) {
        if (!taken[n]) continue;
        const R3DInstance& in = list[n];
        float down = downs[n];
        if (std::fabs(down) <= 0.002f) continue;
        auto lower = [&](R3DVertex& x) {
            for (int k = 0; k < 3; k++) x.pos[k] -= down * up[k], x.prev[k] -= down * up[k];
        };
        for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) lower(out.vertices[v.opaque_first + k]);
        for (uint32_t k = in.trans[0]; k < in.trans[1]; k++) lower(out.vertices[v.trans_first + k]);
        for (uint32_t k = in.back[0]; k < in.back[1] && v.back_first + k < out.backs.size(); k++) lower(out.backs[v.back_first + k]);
    }
}

// Sega Rally 2: some of the models that stand on the ground hang a little
// above it in the game's data (the flagman at the start line, ~12 cm). The
// game's dark patch under them hid it; the traced sun shadows showed a lit
// strip between his shoes and their shadow, as if he floated. Each model
// near the camera (its pieces, turned their own way, taken together where
// they touch: a man's limbs, a car's body and wheels; the see-through ones
// too, models without a solid polygon: the glass of a car) whose lowest
// point is up to 30 cm above the highest surface of the scenery below it is
// set down on it (fading out to 60 cm: not a car in the air). In a race the
// rivals' cars in the world already stand on the road (within 1 cm) and
// stay; the cars over a patch of their own are settle_cars' business
static void settle_actors(const std::vector<R3DInstance>& list, const float* axes, R3DScene& out, const R3DViewport& v) {
    static const bool off = getenv("SR2_ACTORS_AS_IS") != nullptr;   // (as the game places them, to compare)
    if (off) return;
    std::vector<char> taken(list.size(), 0);
    settle_cars(list, axes, out, v, taken);
    const float *ax = axes, *up = axes + 3, *az = axes + 6;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float reach = 40.0f;   // metres from the camera
    // The models' pieces near the camera, as boxes in (across x, height, across z)
    struct Piece {
        const R3DInstance* in;
        float lo[3], hi[3];
        int group;
    };
    std::vector<Piece> pieces;
    for (size_t n = 0; n < list.size(); n++) {
        const R3DInstance& in = list[n];
        if (taken[n] || r3d_same_axes(in.axes, axes)) continue;
        bool solid = in.opaque[1] > in.opaque[0];
        if (!solid && in.trans[1] <= in.trans[0]) continue;
        if (dot(in.origin, in.origin) > reach * reach) continue;
        Piece p{&in, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, int(pieces.size())};
        auto take = [&](const R3DVertex& x) {
            float c[3] = {dot(x.pos, ax), dot(x.pos, up), dot(x.pos, az)};
            for (int k = 0; k < 3; k++) p.lo[k] = std::min(p.lo[k], c[k]), p.hi[k] = std::max(p.hi[k], c[k]);
        };
        for (uint32_t i = in.opaque[0]; i < in.opaque[1]; i++) take(out.vertices[v.opaque_first + i]);
        if (!solid)
            for (uint32_t i = in.trans[0]; i < in.trans[1]; i++) take(out.vertices[v.trans_first + i]);
        pieces.push_back(p);
    }
    if (pieces.empty()) return;
    // Pieces that touch (boxes within 10 cm) are one model
    auto root = [&](int i) {
        while (pieces[i].group != i) i = pieces[i].group = pieces[pieces[i].group].group;
        return i;
    };
    for (size_t i = 0; i < pieces.size(); i++)
        for (size_t j = i + 1; j < pieces.size(); j++) {
            bool touch = true;
            for (int k = 0; k < 3; k++)
                if (pieces[i].lo[k] > pieces[j].hi[k] + 0.1f || pieces[j].lo[k] > pieces[i].hi[k] + 0.1f) touch = false;
            if (touch) pieces[root(int(i))].group = root(int(j));
        }
    // Each model's lowest point (of its solid polygons), and the scenery under
    // it: the lowest under any of its corners within 2 cm of the lowest (on a
    // slope the corner downhill touches it, the one uphill is in the bank). A
    // flat foot has several (a board's bottom edge), and which of them came
    // out lowest changed from frame to frame with the rounding of the view:
    // over a sloping verge each one stood over other ground, and the chevron
    // boards at the bends of the mountain demo were set down and raised by up
    // to 34 cm frame after frame (rising out of the ground and sinking back)
    std::vector<int> model_of(pieces.size(), -1);
    std::vector<GroundQuery> models;
    std::vector<int> index(pieces.size(), -1);
    for (size_t i = 0; i < pieces.size(); i++) {
        int r = root(int(i));
        if (index[r] < 0) index[r] = int(models.size()), models.push_back({0, 1e30f, 0});
        model_of[i] = index[r];
        GroundQuery& m = models[index[r]];
        const R3DInstance& in = *pieces[i].in;
        for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) {
            const R3DVertex& x = out.vertices[v.opaque_first + k];
            float h = dot(x.pos, up);
            if (h < m.h) m.h = h, m.u = dot(x.pos, ax), m.w = dot(x.pos, az);
        }
    }
    std::vector<GroundQuery> feet;
    std::vector<int> foot_of;
    std::vector<int> feet_of_model(models.size(), 0);
    for (size_t i = 0; i < pieces.size(); i++) {
        const R3DInstance& in = *pieces[i].in;
        int mi = model_of[i];
        for (uint32_t k = in.opaque[0]; k < in.opaque[1] && feet_of_model[mi] < 32; k++) {
            const R3DVertex& x = out.vertices[v.opaque_first + k];
            float h = dot(x.pos, up);
            if (h > models[mi].h + 0.02f) continue;
            feet.push_back({dot(x.pos, ax), h, dot(x.pos, az)});
            foot_of.push_back(mi);
            feet_of_model[mi]++;
        }
    }
    ground_under(list, axes, out, v, feet);
    for (size_t f = 0; f < feet.size(); f++) {
        float& g = models[foot_of[f]].ground;
        if (feet[f].ground > -1e29f) g = g > -1e29f ? std::min(g, feet[f].ground) : feet[f].ground;
    }
    static const bool log = getenv("SR2_ACTORLOG") != nullptr;
    for (size_t i = 0; i < pieces.size(); i++) {
        const GroundQuery& m = models[model_of[i]];
        float gap = m.h - m.ground;
        float down = gap <= 0.3f ? gap : std::max(0.0f, 0.6f - gap);
        if (log && root(int(i)) == int(i))
            printf("[actor] model at %.1f %.1f %.1f gap %.3f down %.3f\n", m.u, m.h, m.w, gap, down);
        if (down <= 0.002f) continue;
        const R3DInstance& in = *pieces[i].in;
        auto lower = [&](R3DVertex& x) {
            for (int k = 0; k < 3; k++) x.pos[k] -= down * up[k], x.prev[k] -= down * up[k];
        };
        for (uint32_t k = in.opaque[0]; k < in.opaque[1]; k++) lower(out.vertices[v.opaque_first + k]);
        for (uint32_t k = in.trans[0]; k < in.trans[1]; k++) lower(out.vertices[v.trans_first + k]);
        for (uint32_t k = in.back[0]; k < in.back[1] && v.back_first + k < out.backs.size(); k++) lower(out.backs[v.back_first + k]);
    }
}

// Finds the viewport showing the game world and the camera position in a
// fixed world frame, for the atmosphere. The orientation shared by most
// of the drawn polygons is the world's; the camera position follows from
// how the scenery models moved relative to it since the previous frame.
void R3DWorldTracker::track(const std::vector<std::vector<R3DInstance>>& viewport_instances, R3DScene& out) {
    int best = -1;
    uint32_t best_polygons = 0;
    float axes[9];
    // The group that goes on with last frame's scenery models, if any
    int kept_vp = -1;
    uint32_t kept_models = 0, kept_polygons = 0;
    float kept_axes[9];
    for (size_t vi = 0; vi < viewport_instances.size() && vi < out.viewports.size(); vi++) {
        const auto& list = viewport_instances[vi];
        // Orientation groups weighted by polygons; the scenery is the largest
        struct Group {
            uint32_t polygons;
            size_t first;          // its first instance
            uint32_t was_scenery;  // instances of models that were scenery last frame
        };
        std::vector<Group> groups;
        for (size_t i = 0; i < list.size(); i++) {
            uint32_t was = tracking_ && scenery_models_.count(list[i].model) ? 1 : 0;
            bool found = false;
            for (auto& g : groups)
                if (r3d_same_axes(list[g.first].axes, list[i].axes)) {
                    g.polygons += list[i].polygons;
                    g.was_scenery += was;
                    found = true;
                    break;
                }
            if (!found) groups.push_back({list[i].polygons, i, was});
        }
        for (auto& g : groups) {
            if (g.polygons > best_polygons) {
                best_polygons = g.polygons;
                best = int(vi);
                memcpy(axes, list[g.first].axes, sizeof(axes));
            }
            if (g.was_scenery > kept_models) {
                kept_models = g.was_scenery;
                kept_polygons = g.polygons;
                kept_vp = int(vi);
                memcpy(kept_axes, list[g.first].axes, sizeof(axes));
            }
        }
    }
    // A big model close to the camera made of parts that share one
    // orientation (the Deinosuchus of stage 2 is two dozen) can outweigh
    // the scenery behind it, and the largest group would then go back and
    // forth between the two from frame to frame, flashing the haze, the
    // fade-in and the rim light. The scenery stays the world as long as most
    // of its models are still drawn with one orientation; a cut to another
    // place has other models, and the largest group wins again.
    if (kept_vp >= 0 && kept_models >= 8 && kept_models * 2 >= scenery_models_.size() &&
        kept_polygons * 4 >= best_polygons) {
        best = kept_vp;
        best_polygons = kept_polygons;
        memcpy(axes, kept_axes, sizeof(axes));
    }
    // Scenery models of this frame, by instance and by model (the n-th time
    // a model is drawn): the game sometimes rebuilds its display list in
    // another place, which changes every instance key at once
    Scenery scenery;
    scenery_models_.clear();
    if (best >= 0) {
        std::unordered_map<uint32_t, uint32_t> seen;
        for (const R3DInstance& in : viewport_instances[best])
            if (r3d_same_axes(in.axes, axes)) {
                scenery_models_.insert(in.model);
                Placement p;
                memcpy(p.origin, in.origin, sizeof(p.origin));
                memcpy(p.axes, in.axes, sizeof(p.axes));
                scenery.by_key[in.key] = p;
                scenery.by_model[r3d_mix_key(in.model, seen[in.model]++)] = p;
            }
    }
    // A world is many models placed around the camera; the title screens and
    // menus show one or two big models (logos) instead. A world built of a
    // few big pieces (Hoth's base: a handful of tunnel sections, each turned
    // its own way) is still one with thousands of polygons in three of them
    size_t min_models = best_polygons >= 1500 ? 3 : 8;
    if (best < 0 || best_polygons < 300 || scenery.by_key.size() < min_models) {
        tracking_ = false;
        scenery_prev_ = {};
        return;
    }
    // World position of a point given in view space, with the given axes and camera
    auto to_world = [](const float* ax, const float* cam, const float* p, float* w) {
        for (int k = 0; k < 3; k++) w[k] = cam[k] + ax[k * 3] * p[0] + ax[k * 3 + 1] * p[1] + ax[k * 3 + 2] * p[2];
    };
    // Every model seen in both frames gives an estimate of the camera: where
    // it was placed last frame minus where it is now relative to the camera,
    // each with its own axes (they differ slightly from model to model, which
    // is a whole unit of error on a model a hundred units away).
    // Most of them are fixed scenery and agree; the median ignores the ones
    // that move (animals, effects) or follow the camera (sky, the player's
    // vehicle), which would otherwise drag the fog along with the view.
    int agree = 0, common = 0;
    auto estimate = [&](const std::unordered_map<uint32_t, Placement>& now_map,
                        const std::unordered_map<uint32_t, Placement>& prev_map, float* cam) {
        std::vector<float> est[3];
        for (auto& [k, o] : now_map) {
            auto prev = prev_map.find(k);
            if (prev == prev_map.end()) continue;
            float was[3], now[3];
            to_world(prev->second.axes, camera_, prev->second.origin, was);
            to_world(o.axes, camera_, o.origin, now);
            for (int c = 0; c < 3; c++) est[c].push_back(camera_[c] + was[c] - now[c]);
        }
        int n = int(est[0].size()), ok = 0;
        if (n == 0) return std::make_pair(0, 0);
        for (int c = 0; c < 3; c++) {
            std::vector<float> e = est[c];
            std::nth_element(e.begin(), e.begin() + n / 2, e.end());
            cam[c] = e[n / 2];
        }
        for (int i = 0; i < n; i++)
            if (std::fabs(est[0][i] - cam[0]) + std::fabs(est[1][i] - cam[1]) + std::fabs(est[2][i] - cam[2]) < 0.5f)
                ok++;
        return std::make_pair(ok, n);
    };
    bool fresh = !tracking_;
    if (tracking_) {
        float cam[3], cam2[3];
        std::tie(agree, common) = estimate(scenery.by_key, scenery_prev_.by_key, cam);
        auto [agree2, common2] = estimate(scenery.by_model, scenery_prev_.by_model, cam2);
        if (agree2 > agree) {
            agree = agree2;
            common = common2;
            memcpy(cam, cam2, sizeof(cam));
        }
        if (agree >= 4 && agree * 3 >= common) {
            // The whole world jumping at once while the models still agree is
            // the game looping a stretch of scenery (the T-Rex chase runs
            // through the same forest every 2.4 s, 375 units back): the
            // atmosphere carries on from where the camera was heading, or
            // its fog banks would jump with the loop
            float jump = 0, speed = 0;
            for (int c = 0; c < 3; c++) {
                jump += std::fabs(cam[c] - camera_[c] - velocity_[c]);
                speed += std::fabs(velocity_[c]);
            }
            if (jump > 30.0f && jump > 8.0f * speed)
                for (int c = 0; c < 3; c++) loop_offset_[c] += camera_[c] + velocity_[c] - cam[c];
            else
                for (int c = 0; c < 3; c++) velocity_[c] = cam[c] - camera_[c];
            memcpy(camera_, cam, sizeof(cam));
            lost_ = 0;
        } else if (++lost_ <= 3) {
            // A frame that cannot be matched: the camera keeps its motion
            for (int c = 0; c < 3; c++) camera_[c] += velocity_[c];
        } else {
            fresh = true;   // a different scene: the camera stays where it was
            memset(velocity_, 0, sizeof(velocity_));
        }
    }
    tracking_ = true;
    scenery_prev_ = std::move(scenery);

    // Ground: median height of the upward-facing surfaces below the camera
    R3DViewport& v = out.viewports[best];
    const float* up = axes + 3;
    std::vector<float> heights;
    for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count; i += 3) {
        const R3DVertex& p = out.vertices[i];
        float nl = std::sqrt(p.nrm[0] * p.nrm[0] + p.nrm[1] * p.nrm[1] + p.nrm[2] * p.nrm[2]);
        if (nl <= 0 || (p.nrm[0] * up[0] + p.nrm[1] * up[1] + p.nrm[2] * up[2]) < 0.85f * nl) continue;
        float h = p.pos[0] * up[0] + p.pos[1] * up[1] + p.pos[2] * up[2];
        if (h < 0) heights.push_back(h);
    }
    if (heights.size() >= 16) {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        float g = camera_[1] + heights[heights.size() / 2];
        ground_ = fresh ? g : ground_ + 0.05f * (g - ground_);
    } else if (fresh) {
        ground_ = camera_[1] - 20.0f;
    }

    // Draw distance: nearly all the scenery is nearer than this. Only the
    // scenery counts: models that move on their own can stand far beyond it,
    // which would push the haze out to where nothing is
    std::vector<float> dists;
    for (const R3DInstance& in : viewport_instances[best]) {
        if (!r3d_same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i + 2 < in.opaque[1]; i += 3) {
            const float* p = out.vertices[v.opaque_first + i].pos;
            dists.push_back(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
        }
    }
    if (!dists.empty()) {
        size_t k = dists.size() * 95 / 100;
        std::nth_element(dists.begin(), dists.begin() + k, dists.end());
        float f = std::max(dists[k], 50.0f);
        far_ = fresh || far_ <= 0 ? f : far_ + 0.05f * (f - far_);
    }

    v.world = true;
    // Axes in view space: column k of the world-to-view rotation is world axis k
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 3; i++) v.world_axes[k * 3 + i] = axes[k * 3 + i];
    for (int c = 0; c < 3; c++) v.camera[c] = camera_[c] + loop_offset_[c];
    v.ground = ground_;
    v.scenery_far = far_;
    for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count; i++) out.vertices[i].flags |= R3D_WORLD;
    for (uint32_t i = v.trans_first; i < v.trans_first + v.trans_count; i++) out.vertices[i].flags |= R3D_WORLD;
    // Models not aligned with the scenery move on their own (dinosaurs, people,
    // vehicles): the rim light outlines them, never the scenery
    for (const R3DInstance& in : viewport_instances[best]) {
        if (r3d_same_axes(in.axes, axes)) continue;
        for (uint32_t i = in.opaque[0]; i < in.opaque[1]; i++) out.vertices[v.opaque_first + i].flags |= R3D_ACTOR;
        for (uint32_t i = in.trans[0]; i < in.trans[1]; i++) out.vertices[v.trans_first + i].flags |= R3D_ACTOR;
    }
    settle_actors(viewport_instances[best], axes, out, v);
    settle_scenery(viewport_instances[best], axes, out, v);
    // SR2_TINT (debugging aid): the models' polygons coloured by kind, to tell
    // bodywork from glass: magenta specular untextured, green luminous
    // untextured, cyan textured with a specular coefficient, yellow the rest textured
    static const bool tint = getenv("SR2_TINT") != nullptr;
    if (tint)
        for (uint32_t i = v.opaque_first; i < v.trans_first + v.trans_count; i++) {
            R3DVertex& x = out.vertices[i];
            if (!(x.flags & R3D_ACTOR)) continue;
            bool textured = x.flags & 1u;
            x.color = !textured ? ((x.flags & R3D_SPECULAR) ? 0xFFFF00FFu : (x.flags & R3D_LUMINOUS) ? 0xFF00FF00u : 0xFF0000FFu)
                                : x.spec > 0 ? 0xFFFFFF00u : 0xFF00FFFFu;
            x.flags &= ~1u;
        }
    static const bool debug = getenv("LW_FOGDBG") != nullptr;   // debugging aid
    if (debug)
        printf("[fog] frame %llu camera %.1f %.1f %.1f ground %.1f (height %.1f) far %.0f agree %d/%d models %zu "
               "polygons %u grounds %zu\n",
               (unsigned long long)out.frame, camera_[0], camera_[1], camera_[2], ground_, camera_[1] - ground_, far_,
               agree, common, scenery_prev_.by_key.size(), best_polygons, heights.size());
}
