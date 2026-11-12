#include "r3d_scene.h"

#include <chrono>

#include "r3d_dust.h"
#include "r3d_screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#ifdef _WIN32
#include <intrin.h>   // __popcnt: MSVC knows it without this, clang for Windows does not
#elif defined(__APPLE__)
#define __popcnt(x) __builtin_popcount(x)
#endif

namespace {

using std::uint32_t;

inline float as_float(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// Column-major 4x4 helpers
struct M4 {
    static void identity(float* m) {
        memset(m, 0, 64);
        m[0] = m[5] = m[10] = m[15] = 1;
    }
    static void mul(const float* a, const float* b, float* out) {
        float r[16];
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++)
                r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                                a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
        memcpy(out, r, 64);
    }
    static void point(const float* m, const float* p, float* out) {
        for (int i = 0; i < 3; i++) out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
    }
    static void vec(const float* m, const float* v, float* out) {
        for (int i = 0; i < 3; i++) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2];
    }
};

// Real3D 16-bit radius float: 6-bit signed exponent, 10-bit mantissa (spec 3.7)
float float16(uint32_t u) {
    int e = int(u >> 10) & 63;
    if (e >= 32) e -= 64;
    return std::ldexp(1.0f + float(u & 0x3FF) / 1024.0f, e);
}

inline float fixed_normal(uint32_t w) {   // 2.22 fixed point in bits 8-31
    return float(int32_t(w) >> 8) / 4194304.0f;
}

inline float vertex_normal(uint32_t w) {   // signed byte in bits 0-7
    return (2.0f * float(int8_t(w & 0xFF)) + 1.0f) / 255.0f;
}

}  // namespace

R3DSceneBuilder::Mat R3DSceneBuilder::matrix(uint32_t index) const {
    // 12 floats: translation, then rows 0..2 (spec 4.1)
    float f[12];
    for (int i = 0; i < 12; i++) f[i] = rd_.cullf(matrix_base_ + index * 12 + i);
    Mat m{};
    m.m[0] = f[3]; m.m[4] = f[4]; m.m[8] = f[5];  m.m[12] = f[0];
    m.m[1] = f[6]; m.m[5] = f[7]; m.m[9] = f[8];  m.m[13] = f[1];
    m.m[2] = f[9]; m.m[6] = f[10]; m.m[10] = f[11]; m.m[14] = f[2];
    m.m[15] = 1;
    return m;
}

uint32_t R3DSceneBuilder::model_word(uint32_t addr) const {
    addr &= 0xFFFFFF;
    if (addr < 0x100000) return rd_.poly(addr);
    if (!vrom_words_) return 0;
    // VROM mirrored over 64 MB (a mask for the usual power-of-two size: a
    // division per vertex word was measurable)
    size_t at = vrom_mask_ ? (addr & vrom_mask_) : addr % vrom_words_;
    uint32_t v;
    memcpy(&v, vrom_ + at * 4, 4);
    return v;
}

void R3DSceneBuilder::build(const R3DFrameState& state, R3DScene& out) {
    rd_.reset(state);
    out_ = &out;
    keys_.clear();
    instance_count_.clear();
    node_key_ = 0;
    out.vertices.clear();
    out.backs.clear();
    out.viewports.clear();
    out.frame = state.frame;
    out.polygons = 0;
    visited_viewports_.clear();
    viewport_instances_.clear();
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<float, std::milli>(b - a).count(); };
    auto t0 = clock::now();
    viewport(0x800000, 0);
    auto t1 = clock::now();
    world_.track(viewport_instances_, out);
    auto t2 = clock::now();
    // Draw order: priority 0..3, list order within a priority
    std::stable_sort(out.viewports.begin(), out.viewports.end(),
                     [](const R3DViewport& a, const R3DViewport& b) { return a.priority < b.priority; });
    if (remove_letterbox_) r3d_strip_letterbox(out);
    // SR2_SCENEDUMP=file (debugging aid): the first scene's viewports and vertices as the
    // game places them, to measure outside (heights over the road...)
    if (static int dumps = 0; getenv("SR2_SCENEDUMP") && dumps++ == 0)
        if (FILE* f = fopen(getenv("SR2_SCENEDUMP"), "wb")) {
            uint32_t n = uint32_t(out.viewports.size()), nv = uint32_t(out.vertices.size());
            fwrite(&n, 4, 1, f);
            fwrite(&nv, 4, 1, f);
            for (const R3DViewport& v : out.viewports) {
                uint32_t h[5] = {v.opaque_first, v.opaque_count, v.trans_first, v.trans_count, v.world ? 1u : 0u};
                fwrite(h, 4, 5, f);
                fwrite(v.world_axes, 4, 9, f);
            }
            for (const R3DVertex& x : out.vertices) {
                fwrite(x.pos, 4, 3, f);
                fwrite(&x.flags, 4, 1, f);
                fwrite(&x.color, 4, 1, f);
                fwrite(&x.tex, 4, 1, f);
            }
            fclose(f);
        }
    // The game's shadows under the cars: black, self-lit, see-through
    // untextured polygons in the world (several shapes laid over each other,
    // blended only once at each pixel: R3DRenderer blob_). Hidden when the
    // cars cast traced sun shadows of their own (set_hide_blob_shadows);
    // without ray tracing they are the cars' shadow, as in the arcade.
    // SR2_BLOB_SHADOW=F: F of their opacity where they show (to compare)
    static const float blob_keep = getenv("SR2_BLOB_SHADOW") ? float(atof(getenv("SR2_BLOB_SHADOW"))) : 0.7f;
    if (hide_blob_shadows_ || blob_keep != 1.0f)
        for (R3DVertex& x : out.vertices)
            if ((x.flags & (R3D_WORLD | R3D_LUMINOUS | R3D_TEXTURED)) == (R3D_WORLD | R3D_LUMINOUS) &&
                (x.color & 0x00FFFFFFu) == 0 && (x.color >> 24) < 255)
                x.color = uint32_t(std::clamp(float(x.color >> 24) * (hide_blob_shadows_ ? 0.0f : blob_keep), 0.0f, 255.0f) + 0.5f) << 24;
    // Sega Rally 2 draws the player's car in a full-screen viewport of its
    // own beside the world's (not the world: no fog, no haze), lit by the sun;
    // the HUD's 3D gauges are in small ones and the others hold a polygon or two
    // Not the big 3, 2, 1, GO of the start: 3D models too, in a full-screen
    // viewport lit by a sun, but drawn over everything with the HUD (a later
    // priority than the world's). Taken for the car, they cast a shadow on
    // the road and wore its paint's reflections
    const R3DViewport* world_vp = nullptr;
    for (const R3DViewport& v : out.viewports)
        if (v.world) world_vp = &v;
    for (const R3DViewport& v : out.viewports) {
        if (v.world || v.x > 0.5f || v.w < 495.5f || v.opaque_count < 200 || v.sun_intensity <= 0) continue;
        // Without a world viewport (menus: the course select's 3D frames round
        // the pictures) there is no car to light
        if (!world_vp || v.priority != world_vp->priority) continue;
        for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count; i++) out.vertices[i].flags |= R3D_CAR;
        for (uint32_t i = v.trans_first; i < v.trans_first + v.trans_count; i++) out.vertices[i].flags |= R3D_CAR;
    }
    settle_player_car(out);
    static const float shine = getenv("SR2_SHINE") ? float(atof(getenv("SR2_SHINE"))) : 0.0f;   // the cars' painted shine layer, F of its strength: left out (it veiled the paint, a grey box over the roof); SR2_SHINE=F brings it back
    if (shine != 1.0f)
        for (R3DVertex& x : out.vertices)
            if ((x.flags & (R3D_CAR | R3D_ACTOR)) && (x.flags & (R3D_LUMINOUS | R3D_TEX_ALPHA)) == (R3D_LUMINOUS | R3D_TEX_ALPHA))
                x.color = (x.color & 0x00FFFFFFu) | (uint32_t(std::clamp(float(x.color >> 24) * shine, 0.0f, 255.0f)) << 24);
    // Full-screen fills (fades, flashes) outside the world reach the window's sides
    r3d_widen_screen_fills(out, std::max(fov_widened_, view_wide_));
    auto t3 = clock::now();
    if (pack_ && texture_ram_ && pack_->active()) apply_texture_pack(out);
    auto t4 = clock::now();
    link_previous(out);
    static const bool no_smooth = getenv("SR2_NO_SMOOTH") != nullptr, no_sync = getenv("SR2_NO_SYNC") != nullptr;
    auto t4b = clock::now();
    if (!no_smooth) smooth_car_normals(out);
    auto t4c = clock::now();
    if (!no_sync) sync_car_layers(out);
    r3d_sr2_dust(out, viewport_instances_, texture_ram_);   // adds vertices: after the pairing
    auto t5 = clock::now();
    stage_ms_[0] = ms(t0, t1), stage_ms_[1] = ms(t1, t2), stage_ms_[2] = ms(t2, t3), stage_ms_[3] = ms(t3, t4), stage_ms_[4] = ms(t4, t4b), stage_ms_[5] = ms(t4b, t4c), stage_ms_[6] = ms(t4c, t5);
}

// Sega Rally 2: the game keeps the player's car above the road it draws: its
// tyres and the dark patch it lays under the car as its shadow are both at
// the same height, 25 cm over the road in the desert, 13 in the mountain, 4
// to 8 elsewhere (it changes along a stage). So are the rivals near it, each
// in a viewport and over a patch of its own (those far away, in the world's
// viewport, stand on the road itself). On the arcade the car sat on
// its patch and nothing showed; with a real shadow cast on the road the car
// hovers over it. The car's whole viewport (the car, its dust) is set down
// by that height, measured where the game's own patch is: the patch's height
// over the road drawn below it, followed smoothly from frame to frame and
// kept for a while when it cannot be measured or jumps (the car in the air;
// a car without a patch, far away, is left). The patch is set down with it,
// just clear of the road. SR2_CAR_AS_IS=1 leaves the car as the game places it
void R3DSceneBuilder::settle_player_car(R3DScene& out) {
    static const bool as_is = getenv("SR2_CAR_AS_IS") != nullptr;
    static const bool log = getenv("SR2_CARLOG") != nullptr;
    const R3DViewport* world = nullptr;
    for (const R3DViewport& v : out.viewports)
        if (v.world) {
            world = &v;
            break;
        }
    if (!world || as_is) {
        car_lifts_.clear();
        return;
    }
    const float *ax = world->world_axes, *up = ax + 3, *az = ax + 6;
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    std::vector<R3DVertex>& vs = out.vertices;
    auto patch = [](const R3DVertex& x) {
        return (x.flags & (R3D_WORLD | R3D_LUMINOUS | R3D_TEXTURED)) == (R3D_WORLD | R3D_LUMINOUS) &&
               (x.color & 0x00FFFFFFu) == 0 && (x.color >> 24) < 255;
    };
    std::unordered_set<uint32_t> set_down;   // the patches' vertices already lowered (two cars side by side)
    auto settle = [&](const R3DViewport* car, float& car_lift_, uint32_t& car_lift_skips_, uint64_t& patch_seen) {
    // The car's bodywork (not its glass, its dust): where it is, its lowest point
    double su = 0, sw = 0;
    size_t count = 0;
    float low[3] = {0, 1e30f, 0};
    for (uint32_t i = car->opaque_first; i < car->opaque_first + car->opaque_count; i++) {
        const R3DVertex& x = vs[i];
        if (!(x.flags & R3D_SPECULAR) || (x.flags & (R3D_TEX_ALPHA | R3D_LUMINOUS | R3D_FIXED_SHADE))) continue;
        float u = dot(x.pos, ax), h = dot(x.pos, up), w = dot(x.pos, az);
        su += u, sw += w, count++;
        if (h < low[1]) low[0] = u, low[1] = h, low[2] = w;
    }
    if (!count) return;
    float cu = float(su / double(count)), cw = float(sw / double(count));
    // The road around it: the world's solid triangles within 4 m (not the models that move)
    struct Tri {
        float u[3], h[3], w[3];
    };
    std::vector<Tri> road;
    std::vector<uint32_t> patches;   // the patch's vertices
    const float reach = 4.0f;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t first = pass ? world->trans_first : world->opaque_first;
        uint32_t end = std::min<uint32_t>(first + (pass ? world->trans_count : world->opaque_count), uint32_t(vs.size()));
        for (uint32_t i = first; i + 2 < end; i += 3) {
            if (patch(vs[i])) {
                for (uint32_t k = i; k < i + 3; k++) {
                    float u = dot(vs[k].pos, ax) - cu, h = dot(vs[k].pos, up) - low[1], w = dot(vs[k].pos, az) - cw;
                    if (u * u + w * w < 3.5f * 3.5f && std::fabs(h) < 0.6f && !set_down.count(k)) patches.push_back(k);
                }
                continue;
            }
            if (pass || (vs[i].flags & (R3D_ACTOR | R3D_LUMINOUS))) continue;
            Tri t;
            bool in = true;
            for (int k = 0; k < 3; k++) {
                t.u[k] = dot(vs[i + k].pos, ax), t.h[k] = dot(vs[i + k].pos, up), t.w[k] = dot(vs[i + k].pos, az);
            }
            if (std::min({t.u[0], t.u[1], t.u[2]}) > cu + reach || std::max({t.u[0], t.u[1], t.u[2]}) < cu - reach ||
                std::min({t.w[0], t.w[1], t.w[2]}) > cw + reach || std::max({t.w[0], t.w[1], t.w[2]}) < cw - reach)
                in = false;
            if (in) road.push_back(t);
        }
    }
    // The highest of the road below a point (not what is above it: a bridge, a branch)
    auto road_at = [&](float u, float h, float w, float& ground) {
        ground = -1e30f;
        for (const Tri& t : road) {
            float d = (t.w[1] - t.w[2]) * (t.u[0] - t.u[2]) + (t.u[2] - t.u[1]) * (t.w[0] - t.w[2]);
            if (std::fabs(d) < 1e-9f) continue;
            float a = ((t.w[1] - t.w[2]) * (u - t.u[2]) + (t.u[2] - t.u[1]) * (w - t.w[2])) / d;
            float b = ((t.w[2] - t.w[0]) * (u - t.u[2]) + (t.u[0] - t.u[2]) * (w - t.w[2])) / d;
            if (a < 0 || b < 0 || a + b > 1) continue;
            float g = a * t.h[0] + b * t.h[1] + (1 - a - b) * t.h[2];
            if (g <= h + 0.3f) ground = std::max(ground, g);
        }
        return ground > -1e29f;
    };
    // This frame's measure
    float raw = -1;
    std::vector<float> gaps;
    for (size_t i = 0; i < patches.size(); i += 3) {
        const float* p = vs[patches[i]].pos;
        float h = dot(p, up), g;
        if (road_at(dot(p, ax), h, dot(p, az), g)) gaps.push_back(h - g);
    }
    if (gaps.size() >= 8) {
        std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
        raw = gaps[gaps.size() / 2];
    } else if (count >= 90) {
        // No patch: the game lays none under the cars farther away (some 40 m
        // on), which hover all the same, and over a road drawn with less
        // detail there. Those are set down by their tyres: the lowest point of
        // the bodywork at each of its four corners over the road below it, the
        // least of them (no tyre goes into the road). Only what has the
        // footprint of a car (whatever else the game may draw in a viewport
        // like the cars' is not moved)
        double suu = 0, sww = 0, suw = 0;
        auto body = [&](const R3DVertex& x) {
            return (x.flags & R3D_SPECULAR) && !(x.flags & (R3D_TEX_ALPHA | R3D_LUMINOUS | R3D_FIXED_SHADE));
        };
        for (uint32_t i = car->opaque_first; i < car->opaque_first + car->opaque_count; i++) {
            if (!body(vs[i])) continue;
            double u = dot(vs[i].pos, ax) - cu, w = dot(vs[i].pos, az) - cw;
            suu += u * u, sww += w * w, suw += u * w;
        }
        float turn = 0.5f * float(std::atan2(2.0 * suw, suu - sww)), c = std::cos(turn), s = std::sin(turn);
        float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
        for (uint32_t i = car->opaque_first; i < car->opaque_first + car->opaque_count; i++) {
            if (!body(vs[i])) continue;
            float u = dot(vs[i].pos, ax) - cu, w = dot(vs[i].pos, az) - cw;
            float a = u * c + w * s, b = w * c - u * s;
            a0 = std::min(a0, a), a1 = std::max(a1, a), b0 = std::min(b0, b), b1 = std::max(b1, b);
        }
        float length = a1 - a0, width = b1 - b0, am = 0.5f * (a0 + a1), bm = 0.5f * (b0 + b1);
        if (length > 3.2f && length < 5.2f && width > 1.4f && width < 2.5f) {
            float corner[4][3];   // its lowest point: across, height, across
            for (auto& q : corner) q[1] = 1e30f;
            for (uint32_t i = car->opaque_first; i < car->opaque_first + car->opaque_count; i++) {
                if (!body(vs[i])) continue;
                float u = dot(vs[i].pos, ax), h = dot(vs[i].pos, up), w = dot(vs[i].pos, az);
                float a = (u - cu) * c + (w - cw) * s - am, b = (w - cw) * c - (u - cu) * s - bm;
                if (std::fabs(a) < 0.2f * length || std::fabs(b) < 0.2f * width) continue;
                float* q = corner[(a > 0 ? 1 : 0) + (b > 0 ? 2 : 0)];
                if (h < q[1]) q[0] = u, q[1] = h, q[2] = w;
            }
            int found = 0;
            float least = 1e30f;
            for (auto& q : corner) {
                float g;
                if (q[1] > 1e29f || !road_at(q[0], q[1], q[2], g)) continue;
                found++;
                least = std::min(least, q[1] - g);
            }
            if (found >= 3) raw = least;
        }
    }
    bool measured = raw > -0.1f && raw < 0.45f;
    if (measured) patch_seen = out.frame;
    else if (out.frame - patch_seen > 120) car_lift_ = -1;   // (kept through a jump, not for good)
    if (measured) {
        raw = std::max(raw, 0.0f);
        if (car_lift_ < 0) {
            car_lift_ = raw;
            car_lift_skips_ = 0;
        } else if (std::fabs(raw - car_lift_) <= 0.1f) {
            car_lift_ += 0.15f * (raw - car_lift_);
            car_lift_skips_ = 0;
        } else if (++car_lift_skips_ >= 30) {
            // (a kerb, a bump under the patch is not followed, unless it stays)
            car_lift_ = raw;
            car_lift_skips_ = 0;
        }
    }
    if (log)
        printf("[car] frame %llu car %06X patch %zu road %zu raw %.3f lowest %.3f lift %.3f\n",
               (unsigned long long)out.frame, car->node, patches.size(), road.size(), raw, low[1], car_lift_);
    float down = car_lift_;
    if (down <= 0.002f) return;
    auto lower = [&](R3DVertex& x, float by) {
        for (int k = 0; k < 3; k++) x.pos[k] -= by * up[k], x.prev[k] -= by * up[k];
    };
    for (uint32_t i = car->opaque_first; i < car->opaque_first + car->opaque_count && i < vs.size(); i++) lower(vs[i], down);
    for (uint32_t i = car->trans_first; i < car->trans_first + car->trans_count && i < vs.size(); i++) lower(vs[i], down);
    for (uint32_t i = car->back_first; i < car->back_first + car->back_count && i < out.backs.size(); i++) lower(out.backs[i], down);
    // The patch: 3 cm clear of the road (on it, the two would flicker through each other)
    for (uint32_t k : patches) {
        if (down > 0.03f) lower(vs[k], down - 0.03f);
        set_down.insert(k);
    }
    };
    // Every car in a viewport of its own: the player's, the rivals near it
    // (the helicopter's is taken as one too: nowhere near the road, it stays)
    for (const R3DViewport& v : out.viewports) {
        if (v.world || v.opaque_count < 3 || size_t(v.opaque_first) + v.opaque_count > vs.size()) continue;
        if (!(vs[v.opaque_first].flags & R3D_CAR)) continue;
        CarLift& lift = car_lifts_[v.node];
        if (out.frame - lift.frame > 30) lift.height = -1, lift.skips = 0;   // another race, another camera
        lift.frame = out.frame;
        settle(&v, lift.height, lift.skips, lift.patch);
    }
    for (auto it = car_lifts_.begin(); it != car_lifts_.end();)
        it = out.frame - it->second.frame > 30 ? car_lifts_.erase(it) : std::next(it);
}

// Sega Rally 2: the cars are a few hundred flat facets, and their normals
// do not follow the bodywork (the paint mirrored the sky in flat patches).
// The normals are rebuilt from the geometry: each corner gets the average of
// the facets around its position (welded on a 1/256 unit grid, weighted by
// area) that bend from its own by less than 50 degrees, so the panels read
// as curved and the creases (bonnet edge, wheel arches, the spoiler) stay
// sharp. The length of the game's normal is kept (it carries the model's
// scale for the lighting). The player's car (CAR) and the others (ACTOR's
// specular polygons: their paint, decals and glass)
void R3DSceneBuilder::smooth_car_normals(R3DScene& out) {
    struct Face {
        float n[3];   // area weighted (cross product)
    };
    std::vector<uint32_t> tris;   // first vertex of each car triangle
    for (const R3DViewport& v : out.viewports)
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t i = first; i + 2 < first + count && i + 2 < out.vertices.size(); i += 3) {
                uint32_t f = out.vertices[i].flags;
                if ((f & R3D_CAR) || ((f & R3D_ACTOR) && (f & R3D_SPECULAR))) tris.push_back(i);
            }
        }
    if (tris.empty()) return;
    auto key = [](const float* p) {
        int64_t x = int64_t(std::llround(p[0] * 256.0f)), y = int64_t(std::llround(p[1] * 256.0f)),
                z = int64_t(std::llround(p[2] * 256.0f));
        return uint64_t(x * 73856093) ^ uint64_t(y * 19349663) ^ uint64_t(z * 83492791);
    };
    // Every corner with its position's key, sorted so that the corners at one
    // position come together, in the order they were drawn (a table of lists
    // per position, built anew every frame, was a millisecond of allocations)
    struct Ref {
        uint64_t key;
        uint32_t tri, corner;
    };
    struct SmoothUnit {
        float n[3];   // the face's normal, unit length
        bool ok;      // (not a face without area)
    };
    std::vector<Face> faces(tris.size());
    std::vector<SmoothUnit> units(tris.size());
    std::vector<Ref> refs;
    refs.reserve(tris.size() * 3);
    for (size_t t = 0; t < tris.size(); t++) {
        const R3DVertex* v = &out.vertices[tris[t]];
        float a[3], b[3];
        for (int c = 0; c < 3; c++) a[c] = v[1].pos[c] - v[0].pos[c], b[c] = v[2].pos[c] - v[0].pos[c];
        Face& f = faces[t];
        f.n[0] = a[1] * b[2] - a[2] * b[1];
        f.n[1] = a[2] * b[0] - a[0] * b[2];
        f.n[2] = a[0] * b[1] - a[1] * b[0];
        // Facing the game's own normal (the winding is not reliable)
        if (f.n[0] * v[0].nrm[0] + f.n[1] * v[0].nrm[1] + f.n[2] * v[0].nrm[2] < 0)
            for (float& c : f.n) c = -c;
        for (uint32_t k = 0; k < 3; k++) refs.push_back({key(v[k].pos), uint32_t(t), k});
    }
    const float crease = std::cos(50.0f * 3.14159265f / 180.0f);
    auto unit = [](const float* n, float* o) {
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (int c = 0; c < 3; c++) o[c] = l > 0 ? n[c] / l : 0.0f;
        return l > 0;
    };
    for (size_t t = 0; t < tris.size(); t++) units[t].ok = unit(faces[t].n, units[t].n);
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        return a.key != b.key ? a.key < b.key : a.tri != b.tri ? a.tri < b.tri : a.corner < b.corner;
    });
    for (size_t from = 0, to; from < refs.size(); from = to) {
        for (to = from + 1; to < refs.size() && refs[to].key == refs[from].key;) to++;
        for (size_t i = from; i < to; i++) {
            const SmoothUnit& own = units[refs[i].tri];
            if (!own.ok) continue;
            R3DVertex& v = out.vertices[tris[refs[i].tri] + refs[i].corner];
            float sum[3] = {0, 0, 0};
            for (size_t j = from; j < to; j++) {
                const SmoothUnit& u = units[refs[j].tri];
                if (!u.ok || u.n[0] * own.n[0] + u.n[1] * own.n[1] + u.n[2] * own.n[2] < crease) continue;
                for (int c = 0; c < 3; c++) sum[c] += faces[refs[j].tri].n[c];
            }
            float n[3];
            if (!unit(sum, n)) continue;
            float len = std::sqrt(v.nrm[0] * v.nrm[0] + v.nrm[1] * v.nrm[1] + v.nrm[2] * v.nrm[2]);
            if (len <= 0) len = 1;
            for (int c = 0; c < 3; c++) v.nrm[c] = n[c] * len;
        }
    }
}

// Sega Rally 2: a car's see-through layers (the painted shine over the
// bodywork, the glass) are separate polygons laid on it, and do not always
// pair with the previous frame as the bodywork does: between the game's
// frames (120 Hz and up) they stayed behind or moved on their own, a second
// outline beside the car's. Each of their corners takes the motion of the
// nearest corner of the car's solid polygons (within a unit), so the layers
// move with the bodywork they lie on. The player's car (CAR) and the others
// (ACTOR), each among its own
void R3DSceneBuilder::sync_car_layers(R3DScene& out) {
    struct Corner {
        float pos[3], move[3];
    };
    for (uint32_t group : {uint32_t(R3D_CAR), uint32_t(R3D_ACTOR)}) {
        // The other cars by their specular polygons (paint, decals, glass):
        // ACTOR also has the crowds, hundreds of corners close together
        uint32_t need = group == R3D_ACTOR ? uint32_t(R3D_SPECULAR) : ~0u;
        // A corner of the bodywork is shared by the polygons around it, and so
        // are the layers' (some six times each): each place is looked at once,
        // the first corner found there standing for the others (looking from
        // every layer corner at every solid one took 5 ms a frame, half the
        // scene's preparation)
        struct Place {
            uint32_t x, y, z;   // the position's bits
            bool operator==(const Place& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct PlaceHash {
            size_t operator()(const Place& p) const {
                return size_t((uint64_t(p.x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(p.y) * 0xC2B2AE3D27D4EB4Full) ^
                              (uint64_t(p.z) * 0x165667B19E3779F9ull));
            }
        };
        auto place = [](const float* p) {
            Place q;
            memcpy(&q, p, 12);
            return q;
        };
        std::vector<Corner> solid;
        std::vector<uint32_t> layers;
        std::unordered_set<Place, PlaceHash> solid_seen;
        for (const R3DViewport& v : out.viewports) {
            for (uint32_t i = v.opaque_first; i < v.opaque_first + v.opaque_count && i < out.vertices.size(); i++) {
                const R3DVertex& x = out.vertices[i];
                if (!(x.flags & group) || !(x.flags & need)) continue;
                if (!solid_seen.insert(place(x.pos)).second) continue;
                Corner c;
                for (int k = 0; k < 3; k++) c.pos[k] = x.pos[k], c.move[k] = x.pos[k] - x.prev[k];
                solid.push_back(c);
            }
            for (uint32_t i = v.trans_first; i < v.trans_first + v.trans_count && i < out.vertices.size(); i++)
                if ((out.vertices[i].flags & group) && (out.vertices[i].flags & need)) layers.push_back(i);
        }
        if (solid.empty() || layers.empty()) continue;
        // The solid corners in a grid of 1-unit cells: each layer corner looks
        // only in the 27 cells around it, a unit away at most (a search through
        // all of them took 20 ms a frame with the other cars in view; the crowds,
        // left out above, took 100 ms and more). A quarter of a unit was too
        // short: the layers came apart from the bodywork again
        auto cell = [](float v) { return int64_t(std::floor(v)); };
        auto cell_key = [](int64_t x, int64_t y, int64_t z) {
            return uint64_t(x * 73856093) ^ uint64_t(y * 19349663) ^ uint64_t(z * 83492791);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        grid.reserve(solid.size());
        for (uint32_t n = 0; n < solid.size(); n++)
            grid[cell_key(cell(solid[n].pos[0]), cell(solid[n].pos[1]), cell(solid[n].pos[2]))].push_back(n);
        std::unordered_map<Place, const Corner*, PlaceHash> found;   // a layer corner's place -> its solid corner (or none)
        found.reserve(layers.size());
        for (uint32_t i : layers) {
            R3DVertex& x = out.vertices[i];
            float best = 1.0f;
            const Corner* near = nullptr;
            auto known = found.find(place(x.pos));
            if (known != found.end()) {
                if (known->second)
                    for (int k = 0; k < 3; k++) x.prev[k] = x.pos[k] - known->second->move[k];
                continue;
            }
            int64_t cx = cell(x.pos[0]), cy = cell(x.pos[1]), cz = cell(x.pos[2]);
            for (int64_t dx = -1; dx <= 1; dx++)
                for (int64_t dy = -1; dy <= 1; dy++)
                    for (int64_t dz = -1; dz <= 1; dz++) {
                        auto it = grid.find(cell_key(cx + dx, cy + dy, cz + dz));
                        if (it == grid.end()) continue;
                        for (uint32_t n : it->second) {
                            const Corner& c = solid[n];
                            float d = 0;
                            for (int k = 0; k < 3; k++) d += (c.pos[k] - x.pos[k]) * (c.pos[k] - x.pos[k]);
                            if (d < best) best = d, near = &c;
                        }
                    }
            found.emplace(place(x.pos), near);
            if (near)
                for (int k = 0; k < 3; k++) x.prev[k] = x.pos[k] - near->move[k];
        }
    }
}

uint32_t R3DSceneBuilder::instance_key(uint32_t model_addr) {
    uint32_t k = r3d_mix_key(r3d_mix_key(node_key_, model_addr), viewport_key_);
    return r3d_mix_key(k, instance_count_[k]++);   // the same model drawn again through the same path
}

// Copies of one model (the ammo counter's cartridges) are told apart by the
// path and order they are drawn in, and the game reorders its list from frame
// to frame: a cartridge would then be interpolated towards its neighbour and
// bob up and down. Each copy takes the key of the nearest copy of the
// previous frame instead.
void R3DSceneBuilder::pair_copies() {
    auto& now = copies_now_;
    auto& before = copies_before_;
    auto& rekey = rekey_;
    // The lists keep their room; the paths come and go slowly (with the
    // stages), so the tables start over before they fill up with old ones
    if (now.size() > 4096) now.clear();
    if (before.size() > 4096) before.clear();
    for (auto& [path, list] : now) list.clear();
    for (auto& [path, list] : before) list.clear();
    rekey.clear();
    for (auto& list : viewport_instances_)
        for (R3DInstance& in : list) now[in.path].push_back(&in);
    for (const PrevInstance& p : prev_instances_) before[p.path].push_back(&p);
    for (auto& [path, copies] : now) {
        if (copies.empty()) continue;
        auto it = before.find(path);   // (an empty list: the path was not drawn last frame)
        if (it == before.end() || it->second.empty() || (copies.size() < 2 && it->second.size() < 2)) continue;
        if (copies.size() > 64 || it->second.size() > 64) continue;   // scenery repeated everywhere: as drawn
        const auto& old = it->second;
        // Closest pairs first. Moving copies (fence posts as the camera pans)
        // keep their order partner; a swapped list puts a copy right where
        // another one was.
        struct Pair { float d; uint32_t a, b; };
        std::vector<Pair> pairs;
        for (uint32_t a = 0; a < copies.size(); a++)
            for (uint32_t b = 0; b < old.size(); b++) {
                float d = 0;
                for (int c = 0; c < 3; c++) d += std::fabs(copies[a]->origin[c] - old[b]->origin[c]);
                // The partner found by list order wins unless another is clearly closer
                if (copies[a]->key == old[b]->key) d *= 0.5f;
                pairs.push_back({d, a, b});
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.d < y.d; });
        std::vector<uint32_t> key(copies.size(), 0);
        std::vector<bool> paired(copies.size()), taken(old.size());
        std::unordered_set<uint32_t> used;
        for (const Pair& p : pairs)
            if (!paired[p.a] && !taken[p.b]) {
                paired[p.a] = taken[p.b] = true;
                key[p.a] = old[p.b]->key;
                used.insert(old[p.b]->key);
            }
        // Copies without a partner keep a key of the group nobody else has
        uint32_t n = 0;
        for (uint32_t a = 0; a < copies.size(); a++) {
            if (paired[a]) continue;
            uint32_t k;
            do k = r3d_mix_key(path, n++);
            while (used.count(k));
            used.insert(k);
            key[a] = k;
        }
        for (uint32_t a = 0; a < copies.size(); a++)
            if (key[a] != copies[a]->key) {
                rekey[copies[a]->key] = key[a];
                copies[a]->key = key[a];
            }
    }
    // A scenery module the game moved to another viewport (Hoth's base is cut
    // into many, and modules change viewport as the player walks) gets a new
    // path, so it would not be interpolated for one frame while its neighbours
    // are: the joints between them open and flicker. It takes the key of the
    // same model in the previous frame, at the place it was, if nobody else has it.
    {
        std::unordered_set<uint32_t> prev_keys, now_keys;
        for (const PrevInstance& p : prev_instances_) prev_keys.insert(p.key);
        for (const auto& list : viewport_instances_)
            for (const R3DInstance& in : list) now_keys.insert(in.key);
        std::unordered_map<uint32_t, std::vector<const PrevInstance*>> by_model;
        for (const PrevInstance& p : prev_instances_)
            if (!now_keys.count(p.key)) by_model[p.start].push_back(&p);
        if (!by_model.empty()) {
            std::unordered_set<uint32_t> taken;
            // rekey's values back to their keys (a chain is followed in one
            // step: looping over rekey for every module took 20 ms a frame in
            // the Death Star's trench)
            std::unordered_map<uint32_t, uint32_t> back;
            for (auto& [from, to] : rekey) back[to] = from;
            for (auto& list : viewport_instances_)
                for (R3DInstance& in : list) {
                    if (prev_keys.count(in.key)) continue;
                    auto it = by_model.find(in.start);
                    if (it == by_model.end()) continue;
                    const PrevInstance* best = nullptr;
                    float limit = 0.25f * std::max(1.0f, std::fabs(in.origin[2])), best_d = limit * limit;
                    for (const PrevInstance* p : it->second) {
                        if (taken.count(p->key)) continue;
                        float d = 0;
                        for (int c = 0; c < 3; c++) d += (in.origin[c] - p->origin[c]) * (in.origin[c] - p->origin[c]);
                        if (d < best_d) best_d = d, best = p;
                    }
                    if (!best) continue;
                    taken.insert(best->key);
                    auto chain = back.find(in.key);
                    uint32_t from = chain != back.end() ? chain->second : in.key;
                    if (chain != back.end()) back.erase(chain);
                    rekey[from] = best->key;
                    back[best->key] = from;
                    in.key = best->key;
                }
        }
    }
    if (!rekey.empty()) {
        uint32_t last = 0, to = 0;   // a model's vertices come together: one lookup per run
        bool have = false, found = false;
        for (uint64_t& k : keys_) {
            uint32_t hi = uint32_t(k >> 32);
            if (!have || hi != last) {
                auto it = rekey.find(hi);
                found = it != rekey.end();
                if (found) to = it->second;
                last = hi, have = true;
            }
            if (found) k = uint64_t(to) << 32 | (k & 0xFFFFFFFFull);
        }
    }
    prev_instances_.clear();
    for (const auto& list : viewport_instances_)
        for (const R3DInstance& in : list)
            prev_instances_.push_back({in.key, in.path, in.start, {in.origin[0], in.origin[1], in.origin[2]}});
}

// Fills R3DVertex::prev from the previous frame. Vertices that are new, or
// jumped too far (a camera cut, a reused model), are not interpolated.
// Neither are models that turned a lot since the previous frame (wheels):
// blending their vertices in a straight line folds the polygons.
void R3DSceneBuilder::link_previous(R3DScene& out) {
    out.moving = 0;
    pair_copies();
    auto& axes = axes_;
    auto& spinning = spinning_;
    auto& origins = origins_;
    axes.clear();
    spinning.clear();
    origins.clear();
    for (const auto& list : viewport_instances_)
        for (const R3DInstance& in : list) {
            std::array<float, 9>& a = axes[in.key];
            memcpy(a.data(), in.axes, sizeof(in.axes));
            origins[in.key] = {in.origin[0], in.origin[1], in.origin[2]};
            auto it = prev_axes_.find(in.key);
            if (it == prev_axes_.end()) continue;
            // Rotation angle between both frames: trace(prev^T now) = 1 + 2 cos(angle)
            float trace = 0;
            for (int i = 0; i < 9; i++) trace += a[i] * it->second[i];
            if (trace >= 1.0f + 2.0f * 0.866f) continue;   // (up to 30 degrees: its corners are followed one by one)
            // A wheel: between two frames its corners would cut across the
            // circle they turn on (the wheel shrinking and swelling), so it
            // keeps this frame's shape; but it travels with its car, by as
            // much as its origin (its hub) did. Left where this frame has it,
            // it ran ahead of the bodywork drawn between both frames: seen
            // through the wings and the bumper of a car coming fast at a
            // long lens (Sega Rally 2's attract), a metre a frame
            std::array<float, 3> move = {0, 0, 0};
            auto was = prev_origins_.find(in.key);
            if (was != prev_origins_.end()) {
                float d[3] = {in.origin[0] - was->second[0], in.origin[1] - was->second[1], in.origin[2] - was->second[2]};
                float limit = 0.25f * std::max(1.0f, std::fabs(in.origin[2]));
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= limit * limit) move = {d[0], d[1], d[2]};
            }
            spinning[in.key] = move;
        }
    prev_axes_.swap(axes);
    prev_origins_.swap(origins);
    // One entry per polygon (its key without the corner, the low 3 bits:
    // its first vertex), its corners following it; a vertex's partner is
    // checked by its whole key (a per-vertex map took most of the frame
    // with 40000 polygons), in an open-addressing table kept from frame to
    // frame (Lost World's: a new std::unordered_map each frame was most of
    // the preparation's time)
    index_.reset(out.vertices.size() / 3 + 16);
    uint32_t last_hi = 0;
    bool last_spin = false, have_hi = false;
    const float* spin_move = nullptr;
    for (size_t i = 0; i < out.vertices.size(); i++) {
        R3DVertex& v = out.vertices[i];
        memcpy(v.prev, v.pos, sizeof(v.pos));
        uint64_t key = keys_[i], poly = key & ~7ull;
        uint32_t corner = uint32_t(key & 7);
        if (corner == 0) index_.insert(poly, uint32_t(i));
        uint32_t hi = uint32_t(key >> 32);
        if (!have_hi || hi != last_hi) {
            auto spin = spinning.find(hi);
            last_hi = hi, last_spin = spin != spinning.end(), have_hi = true;
            spin_move = last_spin ? spin->second.data() : nullptr;
        }
        if (last_spin) {
            if (spin_move[0] != 0 || spin_move[1] != 0 || spin_move[2] != 0) {
                for (int k = 0; k < 3; k++) v.prev[k] = v.pos[k] - spin_move[k];
                out.moving++;
            }
            continue;
        }
        uint32_t was = prev_index_.find(poly);
        if (was == KeyIndex::EMPTY) continue;
        size_t j = size_t(was) + corner;
        if (j >= prev_positions_.size() || j >= prev_keys_.size() || prev_keys_[j] != key) continue;
        const float* p = prev_positions_[j].data();
        float dx = p[0] - v.pos[0], dy = p[1] - v.pos[1], dz = p[2] - v.pos[2];
        float limit = 0.25f * std::max(1.0f, std::fabs(v.pos[2]));
        if (dx * dx + dy * dy + dz * dz > limit * limit) continue;
        memcpy(v.prev, p, sizeof(v.prev));
        out.moving += dx != 0 || dy != 0 || dz != 0;
    }
    std::swap(prev_index_, index_);
    // Only the positions are kept (a copy of every whole vertex was most of
    // the rest); the vectors keep their room
    prev_positions_.resize(out.vertices.size());
    for (size_t i = 0; i < out.vertices.size(); i++) memcpy(prev_positions_[i].data(), out.vertices[i].pos, 12);
    prev_keys_.assign(keys_.begin(), keys_.end());
}

void R3DSceneBuilder::KeyIndex::reset(size_t count) {
    size_t size = 1024;
    while (size < 2 * count) size *= 2;
    if (vertex.size() != size) {
        keys.resize(size);
        vertex.assign(size, EMPTY);
        mask = uint32_t(size - 1);
    } else {
        std::fill(vertex.begin(), vertex.end(), EMPTY);
    }
}

void R3DSceneBuilder::KeyIndex::insert(uint64_t key, uint32_t v) {
    for (uint32_t at = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; at = (at + 1) & mask) {
        if (vertex[at] == EMPTY) {
            keys[at] = key;
            vertex[at] = v;
            return;
        }
        if (keys[at] == key) return;
    }
}

uint32_t R3DSceneBuilder::KeyIndex::find(uint64_t key) const {
    if (vertex.empty()) return EMPTY;
    for (uint32_t at = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; at = (at + 1) & mask) {
        if (vertex[at] == EMPTY) return EMPTY;
        if (keys[at] == key) return vertex[at];
    }
}

// Marks every textured triangle with its replacement slot (0 = none)
void R3DSceneBuilder::apply_texture_pack(R3DScene& out) {
    frame_slots_.clear();
    for (size_t i = 0; i + 2 < out.vertices.size(); i += 3) {
        R3DVertex* tri = &out.vertices[i];
        if (!(tri[0].flags & R3D_TEXTURED)) continue;
        auto it = frame_slots_.find(tri[0].tex);
        uint32_t slot = it != frame_slots_.end() ? it->second
                                                 : (frame_slots_[tri[0].tex] = pack_->lookup(tri[0].tex, *texture_ram_));
        tri[0].replace = tri[1].replace = tri[2].replace = slot;
        if (tri[0].flags & R3D_CAR) pack_->dump_car(tri[0].tex, *texture_ram_);
    }
}

void R3DSceneBuilder::viewport(uint32_t addr, int depth) {
    if (depth > 64 || !rd_.cull_valid(addr) || !visited_viewports_.insert(addr & 0xFFFFFF).second) return;
    auto vp = [&](uint32_t i) { return rd_.cull(addr + i); };
    auto vpf = [&](uint32_t i) { return rd_.cullf(addr + i); };
    uint32_t w0 = vp(0);
    if (vp(1) == 0) return;   // memory not set up yet

    if (!(w0 & 0x20)) {
        R3DViewport v;
        v.priority = (w0 >> 3) & 3;
        v.node = addr;
        for (int i = 0; i < 12; i++) v.frustum[i] = vp(0x08 + uint32_t(i));
        v.w = float(vp(0x14) & 0xFFFF) / 4.0f;
        v.h = float(vp(0x14) >> 16) / 4.0f;
        v.x = float(vp(0x1A) & 0xFFFF) / 16.0f;
        v.y = float(vp(0x1A) >> 16) / 16.0f;
        // A 496-wide picture drawn at the top of the screen with the rest left
        // black (cutscenes): centred instead, black bars above and below
        if (v.x == 0 && v.y == 0 && v.w >= 495 && v.h >= 200 && v.h < 380) v.y = std::floor((384 - v.h) / 2);

        // Frustum from the viewport coefficients (spec 4.4); Vulkan clip space has Y down
        float cv = vpf(0x08), cw = vpf(0x09), io = vpf(0x0A), jo = vpf(0x0B);
        if (cv == 0 || cw == 0) { cv = 1; cw = 1; io = 0.5f; jo = 0.5f; }
        // (the widescreen culling of runtime.cpp widens only the game's own
        // viewport records: the Real3D nodes keep the 4:3 projection)
        // Now and then the game rebuilds a node from its widened record (the
        // Hoth base, a viewport per stormtrooper): that one is narrowed back
        // to 4:3, or its trooper would be drawn smaller and elsewhere for a
        // frame (a flickering double) and lose the world's lighting
        if (fov_widened_ > 1.001f && v.x == 0 && v.w >= 495 && io > 0 && jo > 0) {
            float ratio = (jo / cv) / (io / cw);   // horizontal / vertical half-angle tangents
            if (ratio > 1.15f * v.w / v.h) cv *= fov_widened_;
        }
        float l = -jo / cv, r = (1 - jo) / cv, b = -(1 - io) / cw, t = io / cw;
        const float near_z = 1e-3f;
        float* p = v.proj;
        memset(p, 0, 64);
        p[0] = 2 / (r - l);
        p[8] = (r + l) / (r - l);
        p[5] = -2 / (t - b);
        p[9] = -(t + b) / (t - b);
        p[14] = near_z;
        p[11] = -1;

        v.sun[0] = vpf(0x05);
        v.sun[1] = -vpf(0x06);
        v.sun[2] = -vpf(0x04);
        v.sun_intensity = std::clamp(vpf(0x07), 0.0f, 1.0f);
        v.ambient = float((vp(0x24) >> 8) & 0xFF) / 255.0f;
        uint32_t fc = vp(0x22);
        v.fog_color[0] = float((fc >> 16) & 0xFF) / 255.0f;
        v.fog_color[1] = float((fc >> 8) & 0xFF) / 255.0f;
        v.fog_color[2] = float(fc & 0xFF) / 255.0f;
        float density = std::fabs(vpf(0x23));
        v.fog_density = density < 99999.0f ? density : 0.0f;
        v.fog_start = float(int16_t(vp(0x25) & 0xFFFF)) / 255.0f;
        v.fog_ambient = float((vp(0x25) >> 16) & 0xFF) / 255.0f;
        static const bool debug_no_fog = getenv("LW_NOFOG") != nullptr;   // debugging aid
        if (debug_no_fog) v.fog_density = v.fog_start = 0;
        v.scroll_fog = float(vp(0x20) & 0xFF) / 255.0f;
        v.scroll_attenuation = float(vp(0x24) & 0xFF) / 255.0f;
        v.fog_attenuation = float((vp(0x24) >> 16) & 0xFF) / 255.0f;
        uint32_t spot_fog = (vp(0x20) >> 8) & 7;   // colour index: bit 2 = R, bit 1 = G, bit 0 = B
        for (int i = 0; i < 3; i++) v.spot_fog_color[i] = float((spot_fog >> (2 - i)) & 1);
        uint32_t spot = (vp(0x20) >> 11) & 7;
        for (int i = 0; i < 3; i++) v.spot_color[i] = float((spot >> (2 - i)) & 1);
        {
            // Centre in screen pixels from the top left, radius 2047 / size pixels per axis
            float cx = float(int16_t(vp(0x1E) & 0xFFFF)) / 8.0f, cy = float(int16_t(vp(0x1D) & 0xFFFF)) / 8.0f;
            float rx = std::round(2047.0f / float(std::max(1u, vp(0x1E) >> 16)));
            float ry = std::round(2047.0f / float(std::max(1u, vp(0x1D) >> 16)));
            // Pixel -> slope through the projection: ndc = p0 * x/-z - p8, ndc_y = p5 * y/-z - p9
            v.spot_center[0] = ((cx - v.x) / v.w * 2.0f - 1.0f + p[8]) / p[0];
            v.spot_center[1] = ((cy - v.y) / v.h * 2.0f - 1.0f + p[9]) / p[5];
            v.spot_radius[0] = rx * 2.0f / (v.w * std::fabs(p[0]));
            v.spot_radius[1] = ry * 2.0f / (v.h * std::fabs(p[5]));
            float start = vpf(0x21);
            v.spot_start = start != 0 && std::isfinite(1.0f / start) ? 1.0f / start : 0.0f;
            float extent = std::fabs(vpf(0x1F));
            v.spot_extent = std::isfinite(extent) ? extent : 0.0f;
        }

        matrix_base_ = vp(0x16) & 0xFFFFFF;
        lod_base_ = vp(0x17) & 0xFFFFFF;

        // Coordinate system: GL view = C * matrix 0 (spec 4.2)
        Mat c{};
        c.m[1] = 0;
        float cm[16] = {0, 0, -1, 0,   // column 0: R.x -> GL -z
                        1, 0, 0, 0,    // column 1: R.y -> GL x
                        0, -1, 0, 0,   // column 2: R.z -> GL -y
                        0, 0, 0, 1};
        memcpy(c.m, cm, 64);
        Mat m0 = matrix(0);
        Mat base{};
        M4::mul(c.m, m0.m, base.m);
        coord_sys_ = base;
        coord_sys_.m[12] = coord_sys_.m[13] = coord_sys_.m[14] = 0;

        opaque_.clear();
        trans_.clear();
        opaque_keys_.clear();
        trans_keys_.clear();
        backs_.clear();
        viewport_key_ = addr;
        instances_.clear();
        node_ptr(vp(0x02), base, Attribs{}, 0);
        v.build_index = uint32_t(viewport_instances_.size());
        viewport_instances_.push_back(std::move(instances_));
        instances_.clear();

        auto& verts = out_->vertices;
        v.opaque_first = uint32_t(verts.size());
        v.opaque_count = uint32_t(opaque_.size());
        verts.insert(verts.end(), opaque_.begin(), opaque_.end());
        keys_.insert(keys_.end(), opaque_keys_.begin(), opaque_keys_.end());
        v.trans_first = uint32_t(verts.size());
        v.trans_count = uint32_t(trans_.size());
        verts.insert(verts.end(), trans_.begin(), trans_.end());
        keys_.insert(keys_.end(), trans_keys_.begin(), trans_keys_.end());
        v.back_first = uint32_t(out_->backs.size());
        v.back_count = uint32_t(backs_.size());
        out_->backs.insert(out_->backs.end(), backs_.begin(), backs_.end());
        out_->viewports.push_back(v);
    }

    uint32_t next = vp(1);
    if (next != 0x01000000 && (next & 0xFFFFFF)) viewport(next & 0xFFFFFF, depth + 1);
}

void R3DSceneBuilder::node_ptr(uint32_t ptr, const Mat& m, const Attribs& a, int depth) {
    uint32_t addr = ptr & 0xFFFFFF;
    if (!addr) return;
    switch ((ptr >> 24) & 5) {
    case 0: node(addr, m, a, depth); break;
    case 1: model(addr, m, a); break;
    case 4: pointer_list(addr, m, a, depth); break;
    default: break;
    }
}

void R3DSceneBuilder::pointer_list(uint32_t addr, const Mat& m, const Attribs& a, int depth) {
    for (uint32_t i = 0; i < 0x10000; i++) {
        uint32_t e = rd_.cull(addr + i);
        if (!(e & 0x01000000)) node(e & 0xFFFFFF, m, a, depth + 1);
        if (e & 0x02000000) break;
    }
}

void R3DSceneBuilder::node(uint32_t addr, const Mat& parent, Attribs a, int depth) {
    if (depth > 1024 || !rd_.cull_valid(addr)) return;
    uint32_t w[10];
    for (int i = 0; i < 10; i++) w[i] = rd_.cull(addr + i);
    if ((w[0] & 3) == 0) return;               // viewport reached as a node
    if ((w[0] & 0x300) == 0x300) return;       // discarded with its children

    // Siblings first (spec 3.2)
    uint32_t sib = w[8] & 0x1FFFFFF;
    bool colour_table_node = (w[0] & 7) == 6, point_ref = w[0] & 0x10000000;
    if ((!colour_table_node || point_ref) && !(sib & 0x1000000) && sib) node(sib, parent, a, depth + 1);

    // Interpolation key of everything below this node
    struct KeyScope {
        uint32_t& key;
        uint32_t saved;
        ~KeyScope() { key = saved; }
    } key_scope{node_key_, node_key_};
    node_key_ = r3d_mix_key(node_key_, addr);

    if (w[0] & 0x04)
        color_table_ = ((w[3] >> 19) | ((w[7] >> 28) << 13) | ((w[8] >> 25) << 17)) & 0x0FFFFF;

    if (w[1] & 1) a.model_scale = as_float(w[1] & ~3u);
    if (w[1] & 2) a.no_cull = true;
    if (w[2] & 0x8000) {
        a.tex_x = 32 * ((w[2] >> 7) & 0x3F);
        a.tex_y = 32 * (w[2] & 0x1F);
        a.page = (w[2] >> 14) & 1;
    }

    Mat m = parent;
    if (w[0] & 0x10) {
        Mat t{};
        M4::identity(t.m);
        t.m[12] = as_float(w[4]);
        t.m[13] = as_float(w[5]);
        t.m[14] = as_float(w[6]);
        M4::mul(parent.m, t.m, m.m);
    } else if (w[3] & 0xFFF) {
        Mat n = matrix(w[3] & 0xFFF);
        M4::mul(parent.m, n.m, m.m);
    }
    if (w[0] & 0x80) {   // reset matrix: keep translation and scale, rotation = coordinate system
        float len[3];
        for (int c = 0; c < 3; c++)
            len[c] = std::sqrt(m.m[c * 4] * m.m[c * 4] + m.m[c * 4 + 1] * m.m[c * 4 + 1] + m.m[c * 4 + 2] * m.m[c * 4 + 2]);
        Mat r = coord_sys_;
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 3; i++) r.m[c * 4 + i] *= len[c];
        r.m[12] = m.m[12];
        r.m[13] = m.m[13];
        r.m[14] = m.m[14];
        m = r;
    }

    // LOD selection from the blend radius and the distance to the camera
    uint32_t lod_index = (w[3] >> 12) & 0x7F;
    bool no_lod = lod_index == 0x7F;
    float dist = std::sqrt(m.m[12] * m.m[12] + m.m[13] * m.m[13] + m.m[14] * m.m[14]);
    float blend_r = float16(w[9] >> 16) * a.model_scale;
    float lod_scale = (a.no_cull || no_lod || dist == 0) ? 1e30f : std::max(0.0f, blend_r / dist);
    float del[4], fac[4];
    for (int k = 0; k < 4; k++) {
        del[k] = rd_.cullf(lod_base_ + lod_index * 8 + k * 2);
        fac[k] = rd_.cullf(lod_base_ + lod_index * 8 + k * 2 + 1);
    }
    static const bool debug_no_lod = getenv("LW_NOLOD") != nullptr;   // debugging aid
    if (!debug_no_lod && !a.no_cull && !no_lod && lod_scale < del[3]) return;   // too far away

    uint32_t child = w[7] & 0x07FFFFFF;
    if (w[0] & 0x08) {   // 4-entry LOD table
        uint32_t table = child & 0xFFFFFF;
        uint32_t entry[4];
        for (int k = 0; k < 4; k++) entry[k] = rd_.cull(table + k);
        int k = 0;
        for (; k < 3; k++)
            if (lod_scale >= del[k] && (entry[k] & 0x01000000)) break;
        float fade = std::clamp(fac[k] * (lod_scale - del[k]), 0.0f, 1.0f);
        if (no_lod || a.no_cull || fade > 31.0f / 32.0f) fade = 1;
        else if (fade < 1.0f / 32.0f) fade = 0;
        float parent_alpha = a.alpha;
        bool models = child & 0x01000000;
        auto visit = [&](uint32_t e, float alpha) {
            Attribs b = a;
            b.alpha = alpha;
            if (models) model(e & 0xFFFFFF, m, b);
            else node(e & 0xFFFFFF, m, b, depth + 1);
        };
        if (fade > 0) visit(entry[k], parent_alpha * fade);
        if (fade < 1 && k != 3) visit(entry[k + 1], parent_alpha * (1 - fade));
    } else {
        if (!no_lod && !a.no_cull) a.alpha *= std::clamp(fac[3] * (lod_scale - del[3]), 0.0f, 1.0f);
        node_ptr(child, m, a, depth + 1);
    }
}

void R3DSceneBuilder::model(uint32_t addr, const Mat& m, const Attribs& a) {
    if (a.alpha <= 0) return;
    // Rotation part used for normals; normals are divided by the model scale
    float inv_scale = a.model_scale != 0 ? 1.0f / a.model_scale : 1.0f;
    uint32_t end = (addr & 0xFFFFFF) < 0x100000 ? 0x100000 : 0x1000000;
    int tex_w = 32, tex_h = 32;
    float uv_scale = 1.0f / 8;
    uint32_t path = r3d_mix_key(addr, viewport_key_);
    uint64_t instance = uint64_t(instance_key(addr)) << 32;
    uint32_t drawn = 0;
    const uint32_t start_addr = addr;
    uint32_t opaque_from = uint32_t(opaque_.size()), trans_from = uint32_t(trans_.size()), back_from = uint32_t(backs_.size());

    for (int guard = 0; guard < 65536; guard++) {
        uint32_t h[7];
        for (int i = 0; i < 7; i++) h[i] = model_word(addr + i);
        if (h[6] == 0) break;
        int nverts = (h[0] & 0x40) ? 4 : 3;
        int shared = __popcnt(h[0] & 0xF);
        if (addr + 7 + 4 * uint32_t(std::max(0, nverts - shared)) > end) break;

        // Texture state of this polygon (affects reused vertices' UVs as well)
        int wcode = (h[3] >> 3) & 7, hcode = h[3] & 7;
        if (wcode > 5) wcode = 0;
        if (hcode > 5) hcode = 0;
        tex_w = 32 << wcode;
        tex_h = 32 << hcode;
        uv_scale = (h[1] & 0x40) ? 1.0f : 1.0f / 8;

        bool smooth = h[1] & 0x08;
        bool fixed_shading = (h[1] & 0x20) && !smooth;
        bool specular = h[0] & 0x80;
        float face[3] = {fixed_normal(h[1]), fixed_normal(h[2]), fixed_normal(h[3])};

        PrevVertex v[4];
        int j = 0;
        for (int i = 0; i < 4; i++)
            if ((h[0] >> i) & 1) v[j++] = prev_[i];
        uint32_t p = addr + 7;
        for (; j < nverts; j++) {
            uint32_t x = model_word(p), y = model_word(p + 1), z = model_word(p + 2), t = model_word(p + 3);
            p += 4;
            v[j].pos[0] = float(int32_t(x) >> 8) / 2048.0f;
            v[j].pos[1] = float(int32_t(y) >> 8) / 2048.0f;
            v[j].pos[2] = float(int32_t(z) >> 8) / 2048.0f;
            v[j].nrm[0] = vertex_normal(x);
            v[j].nrm[1] = vertex_normal(y);
            v[j].nrm[2] = vertex_normal(z);
            v[j].shade = specular ? float(x & 0xFF) / 255.0f : vertex_normal(x);
            v[j].u = uint16_t(t >> 16);
            v[j].v = uint16_t(t & 0xFFFF);
        }
        if (!smooth)
            for (int i = 0; i < nverts; i++) memcpy(v[i].nrm, face, 12);
        for (int i = 0; i < 4; i++) prev_[i] = v[i];
        addr = p;

        bool last = h[1] & 0x04;
        bool discard = (h[0] & 0x300) == 0x300;
        if (!discard) {
            out_->polygons++;
            drawn++;
            // Colour and alpha
            uint32_t r, g, b;
            if (h[1] & 0x02) {
                r = h[4] >> 24;
                g = (h[4] >> 16) & 0xFF;
                b = (h[4] >> 8) & 0xFF;
            } else {
                uint32_t c = rd_.poly(color_table_ + ((h[4] >> 8) & 0xFFF));
                r = (c >> 16) & 0xFF;
                g = (c >> 8) & 0xFF;
                b = c & 0xFF;
            }
            bool poly_alpha = !(h[6] & 0x800000);
            float alpha = poly_alpha ? float((h[6] >> 18) & 0x3F) / 32.0f : 1.0f;
            if (h[0] & 0x200) alpha *= 0.5f;
            alpha = std::min(1.0f, alpha * a.alpha);
            if (alpha > 0) {
                uint32_t color = r | g << 8 | b << 16 | uint32_t(alpha * 255.0f + 0.5f) << 24;

                bool textured = h[6] & 0x400;
                uint32_t fmt = (h[6] >> 7) & 7;
                bool contour = h[6] & 0x80000000;
                if (contour && fmt >= 1 && fmt <= 4) fmt += 7;
                int tx = 32 * (int(((h[4] & 0x1F) << 1) | ((h[5] >> 7) & 1))) + a.tex_x;
                int ty = 32 * int(h[5] & 0x1F) + a.tex_y;
                int page = int((h[4] >> 6) & 1) ^ a.page;
                uint32_t tex = uint32_t(tx & 2047) | uint32_t((ty + 1024 * page) & 2047) << 11 |
                               uint32_t(wcode) << 22 | uint32_t(hcode) << 25 | fmt << 28;

                bool tex_alpha = (h[6] & 7) != 0;
                uint32_t flags = 0;
                if (textured) flags |= R3D_TEXTURED;
                if (contour) flags |= R3D_ALPHA_TEST;
                if (h[2] & 2) flags |= R3D_MIRROR_U;
                if (h[2] & 1) flags |= R3D_MIRROR_V;
                if (h[3] & 0x80) flags |= R3D_SMOOTH_U;
                if (h[3] & 0x40) flags |= R3D_SMOOTH_V;
                if (h[6] & 0x10000) flags |= R3D_LUMINOUS;
                if (fixed_shading) flags |= R3D_FIXED_SHADE;
                if (specular) flags |= R3D_SPECULAR;
                if (tex_alpha) flags |= R3D_TEX_ALPHA;
                flags |= ((h[6] >> 5) & 3) << R3D_SHININESS_SHIFT;
                if (((h[6] >> 24) & 0x7F) == 2) flags |= R3D_INVERT_RGB;
                if (h[4] & 0x80) flags |= R3D_TRANSLATOR;
                float fog = float((h[6] >> 11) & 0x1F) / 16.0f;
                float spec_coef = float(h[0] >> 26) / 63.0f;

                // To view space
                R3DVertex out[4];
                for (int i = 0; i < nverts; i++) {
                    M4::point(m.m, v[i].pos, out[i].pos);
                    float n[3];
                    M4::vec(m.m, v[i].nrm, n);
                    for (int k = 0; k < 3; k++) out[i].nrm[k] = n[k] * inv_scale;
                    out[i].uv[0] = float(v[i].u) * uv_scale / float(tex_w);
                    out[i].uv[1] = float(v[i].v) * uv_scale / float(tex_h);
                    out[i].color = color;
                    out[i].tex = tex;
                    out[i].flags = flags;
                    out[i].shade = v[i].shade;
                    out[i].fog = fog;
                    out[i].spec = spec_coef;
                    out[i].replace = 0;
                }
                // Back-face test with the face normal (spec 5.4)
                float fn[3];
                M4::vec(m.m, face, fn);
                float facing = out[0].pos[0] * fn[0] + out[0].pos[1] * fn[1] + out[0].pos[2] * fn[2];
                bool double_sided = h[1] & 0x10;
                // A solid polygon with texture alpha goes to both passes: its
                // solid texels are drawn opaque (and occlude by depth), the
                // others translucent (spec 7.2)
                bool translucent = poly_alpha || a.alpha < 1.0f;
                uint64_t poly_key = instance | (uint64_t(guard) << 3);
                auto emit_to = [&](std::vector<R3DVertex>& list, std::vector<uint64_t>& keys, const R3DVertex* q,
                                   uint64_t key) {
                    list.push_back(q[0]);
                    list.push_back(q[1]);
                    list.push_back(q[2]);
                    if (nverts == 4) {
                        list.push_back(q[0]);
                        list.push_back(q[2]);
                        list.push_back(q[3]);
                    }
                    for (int c = 0; c < (nverts == 4 ? 6 : 3); c++) keys.push_back(key | uint64_t(c));
                };
                auto emit = [&](R3DVertex* q) {
                    if (!translucent) emit_to(opaque_, opaque_keys_, q, poly_key);
                    if (translucent || tex_alpha) emit_to(trans_, trans_keys_, q, poly_key | (1ull << 31));
                };
                if (facing <= 0) emit(out);
                else if (!double_sided) {
                    if (!translucent) {
                        backs_.insert(backs_.end(), out, out + 3);
                        if (nverts == 4) backs_.insert(backs_.end(), {out[0], out[2], out[3]});
                    }
                } else {
                    for (int i = 0; i < nverts; i++)
                        for (int k = 0; k < 3; k++) out[i].nrm[k] = -out[i].nrm[k];
                    emit(out);
                }
            }
        }
        if (last) break;
    }
    if (drawn) {   // world tracking: where this model is and how it is turned
        R3DInstance in{uint32_t(instance >> 32), addr, {}, {m.m[12], m.m[13], m.m[14]}, drawn,
                    {opaque_from, uint32_t(opaque_.size())}, {trans_from, uint32_t(trans_.size())}};
        for (int c = 0; c < 3; c++) {
            const float* col = m.m + c * 4;
            float len = std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]);
            for (int i = 0; i < 3; i++) in.axes[c * 3 + i] = len > 0 ? col[i] / len : 0;
        }
        in.path = path;
        in.start = start_addr;
        in.back[0] = back_from, in.back[1] = uint32_t(backs_.size());
        instances_.push_back(in);
    }
}
