#include "r3d_dust.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

std::atomic<bool> g_enhanced{true};

constexpr float FRAME_S = 1.0f / 57.524f;   // one game frame
constexpr size_t MAX_PUFFS = 1800, MAX_CLODS = 3000;
constexpr size_t MAX_EMITTERS = 64;
constexpr float GRAVITY = 9.8f * 1.2f;   // units (metres) per second squared; a little heavy reads better at this scale
constexpr float RIBBON_FRAMES = 11.8f;   // the game's fan is this many frames of the wheel's way long

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a, V3 or_else = {0, 0, 1}) {
    float l = length(a);
    return l > 1e-9f ? a * (1.0f / l) : or_else;
}
V3 load(const float* p) { return {p[0], p[1], p[2]}; }
void store(float* p, V3 a) { p[0] = a.x, p[1] = a.y, p[2] = a.z; }

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

struct Rng {
    uint32_t s;
    float f() { return float((s = hash32(s + 0x9E3779B9u)) >> 8) * (1.0f / 16777216.0f); }
    float r(float a, float b) { return a + (b - a) * f(); }
};

// The world in view space: where its origin is and how its axes lie (y is
// up). The game builds the fans of dust in world coordinates, so the model
// that holds one gives it exactly
struct Frame {
    V3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    bool ok = false;
    V3 dir(V3 w) const { return x * w.x + y * w.y + z * w.z; }
    V3 view(V3 w) const { return o + dir(w); }
    V3 local_dir(V3 v) const { return {dot(v, x), dot(v, y), dot(v, z)}; }
    V3 world(V3 v) const { return local_dir(v - o); }
};

enum Kind : uint32_t { PUFF = 0, CLOD = 1 };

struct Particle {
    Kind kind = PUFF;
    V3 p, v;          // world; units per second
    V3 before;        // where it was when last drawn (world)
    float age = 0, life = 1;
    float size = 0.3f, grow = 1.0f;   // a puff's radius at first and how much it gains; a clod's half size
    float drag = 1.5f, lift = 0;
    float alpha = 0.4f;
    float ground = 0;   // world height of the road where it was thrown up
    uint32_t color = 0xFFFFFFFFu, seed = 0;
    int bounces = 0;
    bool drop = false;   // a drop of water: gone when it lands
    bool snow = false;   // raised from snow: white powder and clumps of snow
    bool own = false;    // raised by the player's car
};

// A wheel raising dust: the tip of one of the game's fans, followed from frame to frame
struct Emitter {
    uint32_t id = 0;
    V3 tip, vel;        // world; units per game frame
    float carry = 0;    // way gone since its last puff
    float clods = 0;    // clods owed (fractions carried over)
    float drops = 0;    // ... and drops of water
    bool matched = false;
    int unseen = 0;
};

// One of the game's fans as drawn this frame
struct Ribbon {
    uint32_t viewport = 0;
    std::vector<uint32_t> tris;   // first vertex of each of its triangles
    std::vector<V3> path;         // along the ground from the wheel back (world)
    V3 rgb;                       // its tint, 0..1
    float strength = 1;           // its alpha at the wheel
    float length = 0;
    bool full = false;            // drawn out to its whole length (it grows from the wheel when the dirt begins)
    uint32_t debris = 0;          // the texture of the flying bits drawn with it (0: none)
    V3 tip_view;                  // the wheel's end in view space
    bool wet = false;             // the wheel is in a puddle: the game draws its splash sprites at it
    bool snow = false;            // raised from snow (is_snow)
    bool own = false;             // the player's car, a few metres in front of the chase camera
};

std::vector<Particle> g_parts;
std::vector<Emitter> g_emitters;
Frame g_before;            // the world in the previous frame's view
V3 g_delta;                // the fans' world minus the tracker's (to go on while no fan is drawn)
V3 g_cam_prev;             // the camera last frame (world), to tell a cut
bool g_cam_ok = false;
float g_px_per_unit = 0;     // native pixels a unit across at a unit from the eye (the world's lens)
uint32_t g_bit_model = 0x714041, g_splash_model = 0x7143f7, g_snow_bit_model = 0x714550;   // the VROM models of the flying bits, the splash and the snow (as last seen in a car's viewport)
bool g_have_delta = false;
uint64_t g_last_frame = 0;
uint32_t g_next_id = 1;

// The colours of a debris sprite's solid texels (ARGB1555), to colour the clods with
struct Bits {
    uint64_t generation = 0;
    std::vector<uint32_t> colors;
};
std::unordered_map<uint32_t, Bits> g_bits;

uint32_t rgba(V3 c, float a = 1) {
    auto q = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(c.x) | q(c.y) << 8 | q(c.z) << 16 | q(a) << 24;
}

const std::vector<uint32_t>& debris_colors(uint32_t tex, R3DTextureRam* ram) {
    static const std::vector<uint32_t> none;
    if (!ram || (tex >> 28) != 0) return none;
    int x = int(tex & 2047), y = int((tex >> 11) & 2047);
    int w = 32 << int((tex >> 22) & 7), h = 32 << int((tex >> 25) & 7);
    if (g_bits.size() > 64 && !g_bits.count(tex)) g_bits.clear();
    Bits& b = g_bits[tex];
    uint64_t gen = ram->texture_generation(x, y, w, h);
    if (b.generation != gen || b.colors.empty()) {
        std::vector<uint16_t> texels;
        b.generation = ram->read_texture(x, y, w, h, texels);
        b.colors.clear();
        for (uint16_t t : texels)
            if (!(t & 0x8000))
                b.colors.push_back(rgba({float((t >> 10) & 31) / 31.0f, float((t >> 5) & 31) / 31.0f, float(t & 31) / 31.0f}));
    }
    return b.colors;
}

bool is_fan(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_TEX_ALPHA | R3D_FIXED_SHADE;
    return (x.flags & (need | R3D_SPECULAR | R3D_LUMINOUS)) == need;
}

// A puddle's splash: a self-lit, alpha tested sprite of drops, many along the water's flight
bool is_splash(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_ALPHA_TEST | R3D_LUMINOUS;
    return (x.flags & (need | R3D_SPECULAR | R3D_TEX_ALPHA | R3D_FIXED_SHADE)) == need;
}

bool is_bit(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_ALPHA_TEST;
    return (x.flags & (need | R3D_SPECULAR | R3D_LUMINOUS | R3D_TEX_ALPHA | R3D_FIXED_SHADE)) == need;
}

// The dust's own colour from the game's tint of its cloud texture: the
// ground's colour, paler and less saturated, as dust in the air is
V3 dust_color(V3 tint) {
    float luma = dot(tint, V3{0.2126f, 0.7152f, 0.0722f});
    V3 c = tint * 0.42f + V3{luma, luma, luma} * 0.58f;
    float peak = std::max(c.x, std::max(c.y, c.z));
    float gain = peak > 1e-3f ? std::clamp(0.72f / peak, 1.0f, 1.7f) : 1.0f;
    c = c * gain;
    return c * 0.7f + V3{0.80f, 0.74f, 0.64f} * 0.3f;
}

// Snow: only on the Snowy course (the game's course of the race, set by the
// runtime from RAM), where the game tints its fan a bluish white (0.85 0.89
// 0.94) or, the dirty snow, a neutral grey (0.5 0.5 0.5): dark lumps and a grey
// cloud read wrong on it. The tint alone does not tell: Riviera's pale gravel
// is the same bluish white (0.82 0.88 0.94)
std::atomic<bool> g_snowy{false};
bool is_snow(V3 c) {
    float luma = 0.3f * c.x + 0.59f * c.y + 0.11f * c.z;
    return g_snowy && luma > 0.4f && c.z >= c.x - 0.02f;
}
uint64_t g_fan_frame[64] = {};   // the last frame each viewport had a fan (its bits may outlast it a moment)

void emit_puff(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, float lod, Rng& rng) {
    if (g_parts.size() >= MAX_PUFFS + MAX_CLODS) return;
    size_t puffs = 0;
    for (const Particle& p : g_parts) puffs += p.kind == PUFF;
    if (puffs >= MAX_PUFFS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);   // 0 crawling .. 1 at 108 km/h and over
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = PUFF;
    p.ground = at.y;
    // A low cloud right behind the wheel that is gone in a moment (the dirt
    // thrown up is what shows, the dust only goes with it): kicked out
    // sideways and hardly up, it stays under the car's roof
    p.size = rng.r(0.35f, 0.55f) * (0.7f + 0.3f * fast) * lod;
    p.grow = rng.r(1.0f, 1.8f) * (0.45f + 0.55f * fast) * lod;
    p.p = at + V3{rng.r(-0.15f, 0.15f), p.size * 0.45f, rng.r(-0.15f, 0.15f)};
    p.v = wheel_vel * rng.r(0.3f, 0.7f) + side * rng.r(-3.0f, 3.0f) * (0.4f + 0.6f * fast) +
          V3{0, rng.r(0.3f, 1.2f) * (0.4f + 0.6f * fast), 0};
    p.drag = rng.r(3.5f, 5.0f);
    p.lift = 0;
    p.life = rng.r(0.9f, 1.7f) * (0.5f + 0.5f * fast);
    // A slow wheel raises little (the game's fan is short and faint then): thin puffs,
    // or they pile up where the car crawls into a wall of dust
    p.alpha = r.strength * rng.r(0.8f, 1.0f) * std::clamp(speed / 28.0f, 0.12f, 1.0f) * 0.7f;   // (a thin haze you see the road through)
    V3 c = dust_color(r.rgb) * rng.r(0.9f, 1.06f);
    c = (c * 0.5f + r.rgb * 0.5f) * 0.82f;   // nearer the ground's own colour: the pale cloud glared in the sun
    // Only over earth and sand (a warm tint): on a road or pale gravel the game's
    // fan is a cool or neutral pale and its cloud, made more of, was white smoke
    p.own = r.own && r.rgb.x > r.rgb.z + 0.08f;
    // A neutral grey tint (no colour to it: tarmac, a road) raises hardly any
    // dust, for any car: there the cloud read as white smoke
    float chroma = std::max(r.rgb.x, std::max(r.rgb.y, r.rgb.z)) - std::min(r.rgb.x, std::min(r.rgb.y, r.rgb.z));
    if (chroma < 0.08f && !r.snow) p.alpha *= 0.4f;
    if (p.own) {
        // The chase camera is a few metres behind it and goes through what it
        // leaves on the road in a moment: drawn along in its wake a while longer
        // (the car's own cloud, around and just behind it), and a little thicker
        p.v = p.v + wheel_vel * rng.r(0.25f, 0.4f);
        p.drag = rng.r(1.8f, 2.6f);
        p.size *= 1.15f;
        p.alpha *= 1.4f;
    }
    if (r.snow) {
        // Powder snow: a bright white spray, thicker than dust (white on white
        // it is the light and shade on it that show), up to about the roof
        p.snow = true;
        c = (r.rgb * 0.35f + V3{0.96f, 0.98f, 1.0f} * 0.65f) * rng.r(0.96f, 1.03f);
        p.alpha *= 1.3f;
        p.v.y *= 1.4f;
        p.life *= 0.85f;
    }
    p.color = rgba(c);
    if (r.wet) {
        // Spray: a pale mist thrown wide and high that falls back and is gone in a
        // moment, with a little of the puddle's mud in it. A slow wheel splashes too
        p.size = rng.r(0.25f, 0.45f) * lod;
        p.grow = rng.r(0.8f, 1.5f) * (0.5f + 0.5f * fast) * lod;
        p.v = wheel_vel * rng.r(0.3f, 0.8f) + side * rng.r(-4.5f, 4.5f) +
              V3{0, rng.r(1.5f, 5.0f) * (0.45f + 0.55f * fast), 0};
        p.drag = rng.r(3.0f, 4.5f);
        p.lift = -rng.r(0.5f, 1.2f);
        p.life = rng.r(0.5f, 1.0f);
        p.alpha = r.strength * rng.r(0.5f, 0.7f) * std::clamp(speed / 18.0f, 0.3f, 1.0f);
        V3 w = V3{0.84f, 0.86f, 0.88f} * 0.58f + dust_color(r.rgb) * 0.42f;
        p.color = rgba(w * rng.r(0.92f, 1.05f));
    }
    p.seed = hash32(rng.s);
    p.age = 0;
    // Its share of the frame already gone by
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

void emit_clod(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, const std::vector<uint32_t>& colors, Rng& rng) {
    size_t clods = 0;
    for (const Particle& p : g_parts) clods += p.kind == CLOD;
    if (clods >= MAX_CLODS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = CLOD;
    p.ground = at.y;
    // Lumps of earth big enough to see from the chase camera, now and then a bigger one
    p.size = rng.r(0.03f, 0.08f);
    if (rng.f() < 0.15f) p.size *= 1.8f;
    p.p = at + V3{rng.r(-0.1f, 0.1f), 0.04f, rng.r(-0.1f, 0.1f)};
    // A rooster tail: thrown hard back and up out of the tread, fanning out
    // wide to the sides past the body; some carried along with the car a moment
    p.v = wheel_vel * rng.r(0.05f, 0.45f) - fwd * rng.r(2.0f, 9.0f) * (0.4f + 0.6f * fast) + side * rng.r(-5.5f, 5.5f) +
          V3{0, rng.r(1.5f, 5.0f) * (0.5f + 0.5f * fast), 0};
    p.drag = 0.25f;
    p.life = rng.r(0.55f, 1.25f);
    p.alpha = 1;
    // Dark, wet-looking earth: the ground's own colour, much darker (the
    // flying bits' texture is grass on some ground and read as pale olive)
    V3 k = r.rgb * rng.r(0.28f, 0.5f);
    if (!colors.empty()) {
        uint32_t t = colors[size_t(rng.f() * float(colors.size())) % colors.size()];
        k = k * 0.8f + V3{float(t & 255) / 255.0f, float((t >> 8) & 255) / 255.0f, float((t >> 16) & 255) / 255.0f} * 0.08f;
    }
    if (r.snow) {   // clumps of snow, white and a little smaller
        p.snow = true;
        p.size *= 0.8f;
        k = (r.rgb * 0.45f + V3{0.95f, 0.97f, 1.0f} * 0.55f) * rng.r(0.9f, 1.02f);
    }
    p.color = rgba(k);
    p.seed = hash32(rng.s);
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

// A drop of a puddle's water: thrown up and out to both sides by the wheel,
// carried along with the car, falling in an arc
void emit_drop(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, Rng& rng) {
    size_t clods = 0;
    for (const Particle& p : g_parts) clods += p.kind == CLOD;
    if (clods >= MAX_CLODS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = CLOD;
    p.drop = true;
    p.ground = at.y;
    p.size = rng.r(0.006f, 0.017f);
    p.p = at + V3{rng.r(-0.12f, 0.12f), 0.05f, rng.r(-0.12f, 0.12f)};
    float out = rng.r(-1.0f, 1.0f);
    p.v = wheel_vel * rng.r(0.2f, 0.85f) - fwd * rng.r(0.0f, 3.0f) + side * (out * rng.r(2.0f, 7.5f)) +
          V3{0, rng.r(1.5f, 7.5f) * (0.6f + 0.4f * fast), 0};
    p.drag = 0.7f;
    p.life = rng.r(0.5f, 1.1f);
    // Muddy water catching the light: pale, some drops almost white
    V3 w = V3{0.74f, 0.76f, 0.77f} * 0.55f + dust_color(r.rgb) * 0.45f;
    p.color = rgba(w * rng.r(0.6f, 1.1f));
    p.seed = hash32(rng.s);
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

void step(Particle& p, float dt) {
    p.age += dt;
    if (p.kind == PUFF) {
        p.v = p.v * std::exp(-p.drag * dt);
        p.p = p.p + (p.v + V3{0, p.lift, 0}) * dt;
        // Dust is heavy: what is thrown up settles back, the cloud rides under
        // the car's roof, and as it grows it spreads along the ground rather
        // than sinking into it
        float s = p.size + p.grow * (1.0f - std::exp(-3.0f * p.age));
        float top = p.ground + (p.snow ? 0.7f + 0.3f * s : 0.45f + 0.2f * s), low = p.ground + 0.45f * s;
        if (p.p.y > top) {
            p.p.y += (top - p.p.y) * (1.0f - std::exp(-2.5f * dt));
            p.v.y = std::min(p.v.y, 0.0f);
        }
        if (p.p.y < low) p.p.y = low;
        return;
    }
    p.v = p.v * std::exp(-p.drag * dt);
    p.v.y -= GRAVITY * dt;
    p.p = p.p + p.v * dt;
    float floor = p.ground + p.size;
    if (p.p.y < floor && p.v.y < 0) {   // the road: a bounce or two, then it rolls to a stop
        p.p.y = floor;
        if (p.drop) p.life = std::min(p.life, p.age);
        p.bounces++;
        p.v = V3{p.v.x * 0.55f, -p.v.y * (p.bounces > 2 ? 0.0f : 0.32f), p.v.z * 0.55f};
        if (p.bounces > 2) p.life = std::min(p.life, p.age + 0.25f);
    }
}

// The wheel's way over one frame, and what it throws up along it. a: where
// the wheel was `frames` game frames ago, b: where it is (world)
void emit_along(Emitter& e, const Ribbon& r, V3 a, V3 b, float frames, float dist_eye, R3DTextureRam* ram, Rng& rng) {
    float dt = frames * FRAME_S;
    if (dt <= 0) return;
    V3 move = b - a;
    float dist = length(move);
    V3 vel = move * (1.0f / dt);   // units per second
    float speed = dist / dt;
    if (speed < 1.0f || speed > 150.0f) return;   // standing, or not one wheel's way (a cut)
    if (r.wet && dist_eye < 90.0f) {
        e.drops += r.strength * std::clamp(speed * 9.0f, 110.0f, 380.0f) * dt;
        while (e.drops >= 1.0f) {
            e.drops -= 1.0f;
            float f = rng.f();
            emit_drop(r, a + move * f, vel, speed, (1.0f - f) * dt, rng);
        }
    }
    // Far away, fewer and bigger puffs
    float lod = std::clamp(dist_eye / 45.0f, 1.0f, 3.0f);
    // (water comes up thick; the low cloud needs fewer, overlapping less where
    // it is raised: thinner there, and less to draw with the camera in it)
    float spacing = (r.wet ? 0.42f : 0.8f) * lod;
    e.carry += dist;
    while (e.carry >= spacing) {
        e.carry -= spacing;
        float f = dist > 1e-6f ? 1.0f - e.carry / dist : 1.0f;   // how far along a -> b
        f = std::clamp(f, 0.0f, 1.0f);
        emit_puff(r, a + move * f, vel, speed, (1.0f - f) * dt, std::sqrt(lod), rng);
    }
    // Lumps of earth wherever a wheel raises dust (tinted by the game's flying
    // bits where it draws them, in the ground's colour where it does not)
    // (none from the player's car: right in front of the chase camera they were
    // a spray of black dots over a cloud mostly faded out there)
    if (dist_eye < 90.0f && !r.own) {
        static const std::vector<uint32_t> no_colors;
        const std::vector<uint32_t>& colors = r.debris ? debris_colors(r.debris, ram) : no_colors;
        e.clods += r.strength * std::clamp(speed * 8.0f, 0.0f, 300.0f) * (r.snow ? 0.5f : 1.0f) * dt;
        while (e.clods >= 1.0f) {
            e.clods -= 1.0f;
            float f = rng.f();
            emit_clod(r, a + move * f, vel, speed, (1.0f - f) * dt, colors, rng);
        }
    }
}

R3DVertex vertex(V3 pos, V3 prev, V3 nrm, float u, float v, const Particle& p, float t, float extra, uint32_t color) {
    R3DVertex o{};
    store(o.pos, pos);
    store(o.prev, prev);
    store(o.nrm, nrm);
    o.uv[0] = u, o.uv[1] = v;
    o.color = color;
    o.tex = uint32_t(p.kind) | (p.drop ? 0u : 4u) | (p.snow ? 8u : 0u) | (p.own ? 16u : 0u) | (p.seed & 0xFFFFu) << 8;   // bit 2: a solid lump or the low cloud (not a drop's soft streak), bit 3: snow, bit 4: the player's car
    o.flags = R3D_DUST | R3D_WORLD | R3D_NO_SHADOW;
    o.shade = t;
    o.fog = 1.0f;
    o.spec = extra;
    o.replace = 0;
    return o;
}

void draw(const Frame& f, const Frame& before, std::vector<R3DVertex>& out) {
    // The camera's own speed through the world (units per second)
    V3 eye_vel = before.ok ? (f.world(V3{}) - before.world(V3{})) * (1.0f / FRAME_S) : V3{};
    if (length(eye_vel) > 150.0f) eye_vel = {};   // a cut
    struct Order {
        float z;
        uint32_t i;
    };
    std::vector<Order> order;
    order.reserve(g_parts.size());
    for (uint32_t i = 0; i < g_parts.size(); i++) {
        V3 c = f.view(g_parts[i].p);
        if (c.z > -0.05f || c.z < -500.0f) continue;
        if (g_parts[i].kind == PUFF && c.z > -0.8f) continue;   // all faded out in the eye's face (r3d.frag)
        order.push_back({c.z, i});
    }
    // Farthest first: they are blended over each other
    std::sort(order.begin(), order.end(), [](const Order& a, const Order& b) { return a.z < b.z; });
    const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (const Order& o : order) {
        const Particle& p = g_parts[o.i];
        V3 c = f.view(p.p);
        V3 was = before.ok ? before.view(p.before) : c;
        V3 delta = c - was;
        if (length(delta) > 0.25f * std::max(1.0f, -c.z)) delta = {};   // a cut: not blended from where it was
        float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        V3 d = normalize(c * -1.0f);
        R3DVertex q[4];
        if (p.kind == PUFF) {
            V3 r = normalize(cross(f.y, d), {1, 0, 0}), u = cross(d, r);   // upright in the world
            float s = p.size + p.grow * (1.0f - std::exp(-3.0f * p.age));
            // (wider than tall: it spreads along the ground, flattening as it grows)
            float sv = s * (0.65f - 0.15f * std::clamp(p.age / p.life, 0.0f, 1.0f));
            V3 corner[4] = {c - r * s - u * sv, c + r * s - u * sv, c + r * s + u * sv, c - r * s + u * sv};
            const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            // Thick where the wheel raises it, thinner as it spreads (and it does not pop up under the wheel)
            float a = p.alpha * std::pow(p.size / s, 0.4f) * std::clamp(p.age / 0.12f, 0.0f, 1.0f);
            // (it comes up over a moment rather than as a thick blob at the
            // wheel, and one the chase camera is in, all but faded out by the
            // shader, is not drawn: a puff about the eye is the whole picture
            // over and over, which cost a hitch driving through the cloud)
            float nearf = p.own ? std::clamp((-c.z - 0.5f) / 2.0f, 0.0f, 1.0f) : std::clamp((-c.z - 0.8f) / 3.2f, 0.0f, 1.0f);   // (as r3d.frag)
            nearf = nearf * nearf * (3.0f - 2.0f * nearf);
            if (a * nearf < 0.02f) continue;
            uint32_t color = (p.color & 0x00FFFFFFu) | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
            for (int k = 0; k < 4; k++) q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], p, t, s, color);
        } else {
            // A short streak along its way (what the eye makes of a stone in flight), at least its own size
            // (as the eye moving with the camera sees it)
            float shutter = p.drop ? 0.008f : 0.004f, longest = p.drop ? 0.3f : 0.25f;   // (water draws out into threads)
            if (!p.drop) shutter = 0.0012f, longest = 0.05f;   // a lump of earth: hardly drawn out (not a capsule)
            V3 along = f.dir(p.v - eye_vel) * shutter;
            float len = length(along);
            if (len > longest) along = along * (longest / len), len = longest;
            V3 tail = c - along;
            V3 a = len > 1e-5f ? along * (1.0f / len) : V3{0, 1, 0};
            V3 side = normalize(cross(a, d), {1, 0, 0});
            float w = std::max(p.size, -c.z * 0.0016f);   // about a pixel at least: smaller ones flicker
            // and not a blob filling the picture as it flies past the camera, nor a
            // boulder in the attract demo's long lenses: at most a few pixels across
            w = std::min(w, -c.z * (g_px_per_unit > 1.0f ? 3.2f / g_px_per_unit : 0.008f));
            V3 head = c + a * w, foot = tail - a * w;
            V3 corner[4] = {foot - side * w, head - side * w, head + side * w, foot + side * w};
            const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            float aspect = (len + 2 * w) / (2 * w);
            for (int k = 0; k < 4; k++) q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], p, t, aspect, p.color);
        }
        for (int k : corners) out.push_back(q[k]);
    }
}

}  // namespace

void r3d_set_sr2_dust(bool enhanced) { g_enhanced = enhanced; }
void r3d_set_sr2_snowy(bool snowy) { g_snowy = snowy; }

void r3d_sr2_dust(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances, R3DTextureRam* textures) {
    static const bool off = getenv("SR2_DUST") && atoi(getenv("SR2_DUST")) == 0;
    static FILE* log = getenv("SR2_DUSTLOG") ? fopen(getenv("SR2_DUSTLOG"), "w") : nullptr;
    if (!g_enhanced || off) {
        g_parts.clear();
        g_emitters.clear();
        g_before.ok = false;
        g_have_delta = false;
        g_last_frame = 0;
        g_cam_ok = false;
        return;
    }
    auto began = std::chrono::steady_clock::now();
    int steps = 1;
    if (g_last_frame) steps = int(std::clamp<int64_t>(int64_t(out.frame) - int64_t(g_last_frame), 0, 8));
    g_last_frame = out.frame;

    // The viewport of the world: the particles are drawn at the end of its see-through list
    int world = -1;
    for (uint32_t i = 0; i < out.viewports.size(); i++)
        if (out.viewports[i].world) world = int(i);

    // The game's fans of dust: models it builds in polygon RAM, in world
    // coordinates (so their node's matrix is the camera's), drawn in the cars'
    // viewports beside the world's. A ridge (texture v = 1) between two edges
    // on the ground (v = 0), from the wheel (the highest u) back
    Frame f;
    std::vector<Ribbon> ribbons;
    std::vector<bool> has_fan(out.viewports.size(), false);
    std::vector<uint32_t> stray;   // the game's fans too short to be followed, taken out all the same
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        const R3DViewport& v = out.viewports[vi];
        // (the cars' viewports, and the world's too: a car far off, drawn in the
        // world as the attract demo draws them all, has its fan there; left out,
        // its old flat fan showed and its dust began only once it was near)
        if (v.build_index >= instances.size() || v.x > 0.5f || v.w < 495.5f) continue;
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) >= 0x100000 || in.trans[1] < in.trans[0] + 3) continue;
            Ribbon r;
            r.viewport = vi;
            Frame own;
            own.o = load(in.origin), own.x = load(in.axes), own.y = load(in.axes + 3), own.z = load(in.axes + 6);
            own.ok = true;
            struct Rung {
                float u;
                V3 sum;
                int n;
                V3 seen[4];
            };
            std::vector<Rung> rungs;
            float alpha = 0, newest = -1e30f;
            bool shaped = true;   // every corner on the ground or on the ridge, as a fan's are
            for (uint32_t i = in.trans[0]; i + 2 < in.trans[1]; i += 3) {
                uint32_t k = v.trans_first + i;
                if (k + 2 >= out.vertices.size()) break;
                const R3DVertex* x = &out.vertices[k];
                if (!is_fan(x[0])) continue;
                r.tris.push_back(k);
                for (int c = 0; c < 3; c++) shaped &= x[c].uv[1] < 0.02f || x[c].uv[1] > 0.98f;
                alpha = std::max(alpha, float(x[0].color >> 24) / 255.0f);
                // (each stretch keeps the colour of the ground it was raised from: the wheel's is the newest)
                float u = std::max(x[0].uv[0], std::max(x[1].uv[0], x[2].uv[0]));
                if (u >= newest) {
                    newest = u;
                    r.rgb = {float(x[0].color & 255) / 255.0f, float((x[0].color >> 8) & 255) / 255.0f, float((x[0].color >> 16) & 255) / 255.0f};
                }
                for (int c = 0; c < 3; c++) {
                    if (x[c].uv[1] > 0.5f) continue;   // the ridge
                    V3 w = own.world(load(x[c].pos));
                    Rung* rung = nullptr;
                    for (Rung& g : rungs)
                        if (std::fabs(g.u - x[c].uv[0]) < 0.02f) rung = &g;
                    if (!rung) {
                        rungs.push_back({x[c].uv[0], {}, 0, {}});
                        rung = &rungs.back();
                    }
                    bool dup = false;
                    for (int s = 0; s < rung->n; s++) dup |= length(rung->seen[s] - w) < 1e-3f;
                    if (dup || rung->n >= 4) continue;
                    rung->seen[rung->n++] = w;
                    rung->sum = rung->sum + w;
                }
            }
            if (r.tris.size() < 8 || rungs.size() < 3) {
                // (the first stretches of a fan, as a wheel runs onto the dirt: the
                // game's flat fan showed there on the ground under the new dust)
                if (!r.tris.empty() && shaped && in.trans[1] - in.trans[0] <= 3 * 64) {
                    if (rungs.empty()) {
                        stray.insert(stray.end(), r.tris.begin(), r.tris.end());
                        has_fan[vi] = true;
                        continue;
                    }
                    // (and its wheel raises dust all the same: a car some way ahead
                    // has only these stubs for seconds on end, the game starting its
                    // fan over and over, and went without dust until it was near)
                } else {
                    continue;
                }
            }
            std::sort(rungs.begin(), rungs.end(), [](const Rung& a, const Rung& b) { return a.u > b.u; });
            for (const Rung& g : rungs) r.path.push_back(g.sum * (1.0f / float(g.n)));
            for (size_t i = 1; i < r.path.size(); i++) r.length += length(r.path[i] - r.path[i - 1]);
            r.strength = alpha;
            r.tip_view = own.view(r.path.front());
            r.full = rungs.size() >= 11;
            r.snow = is_snow(r.rgb);
            r.own = !v.world && length(r.tip_view) < 12.0f;
            if (!f.ok) f = own;
            has_fan[vi] = true;
            ribbons.push_back(std::move(r));
        }
    }
    // The flying bits drawn with them: one small alpha tested sprite of the
    // VROM facing the eye, many times over along the wheels' way
    std::vector<uint32_t> bits;                              // first vertex of each sprite triangle
    std::vector<uint32_t> bits_tex(out.viewports.size(), 0);
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        const R3DViewport& v = out.viewports[vi];
        if (has_fan[vi] && vi < 64) g_fan_frame[vi] = out.frame;
        // (also a car's viewport whose fan went this very moment: as a wheel runs
        // off the dirt the game drops the fan a few frames before its bits, which
        // then showed; nothing in a car's viewport is touched otherwise)
        bool lately = vi < 64 && g_fan_frame[vi] && out.frame - g_fan_frame[vi] <= 10;
        if (v.build_index >= instances.size() || v.x > 0.5f || v.w < 495.5f || !(has_fan[vi] || (lately && !v.world))) continue;
        // On snow the game throws self-lit sprites of snow instead (another
        // model): taken out too, the snow clumps take their place. Left in, with
        // the game's white fan gone, the ray traced sun drew each one's square
        // shadow on the snow behind the car
        bool snow = !has_fan[vi] && g_snowy;
        for (const Ribbon& r : ribbons) snow |= r.viewport == vi && r.snow;
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) < 0x100000 || in.polygons != 1 || in.opaque[1] != in.opaque[0] + 6 || in.axes[8] < 0.999f) continue;
            uint32_t k = v.opaque_first + in.opaque[0];
            if (k + 5 >= out.vertices.size()) continue;
            bool snowbit = snow && is_splash(out.vertices[k]);
            if (!is_bit(out.vertices[k]) && !snowbit) continue;
            if (length(load(out.vertices[k].pos) - load(out.vertices[k + 1].pos)) > 1.0f) continue;
            // (in the world, where other sprites are drawn too, only the model the cars' bits are)
            uint32_t model = in.start & 0xFFFFFF;
            uint32_t& known = snowbit ? g_snow_bit_model : g_bit_model;
            bool learn = !v.world && has_fan[vi];
            if (!learn && model != known) continue;
            if (learn) known = model;
            bits.push_back(k), bits.push_back(k + 3);
            if (!snowbit) bits_tex[vi] = out.vertices[k].tex;
        }
    }
    for (Ribbon& r : ribbons) r.debris = bits_tex[r.viewport];
    // A puddle's splash sprites (drawn in the see-through list): the fan of the
    // wheel they start at is water, not dust. They go when a wheel takes their place
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        if (!has_fan[vi]) continue;
        const R3DViewport& v = out.viewports[vi];
        std::vector<uint32_t> splash;
        bool wet = false;
        V3 last;
        float pace = 0;
        int rows = 0;
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) < 0x100000 || in.polygons != 1 || in.trans[1] != in.trans[0] + 6 || in.axes[8] < 0.999f) continue;
            uint32_t k = v.trans_first + in.trans[0];
            if (k + 5 >= out.vertices.size() || !is_splash(out.vertices[k])) continue;
            if (length(load(out.vertices[k].pos) - load(out.vertices[k + 1].pos)) > 1.5f) continue;
            uint32_t model = in.start & 0xFFFFFF;
            if (v.world && model != g_splash_model) continue;
            if (!v.world) g_splash_model = model;
            splash.push_back(k), splash.push_back(k + 3);
            // Each row of them is the flight of the water from one wheel, newest
            // first: a row begins where the place jumps. Its first is at the wheel
            // (a little ahead of it at speed); the rest trail away, past the other wheel too
            V3 at = load(in.origin);
            float jump = length(at - last);
            bool head = !rows || (jump > 0.6f && jump > 2.5f * pace);
            pace = head ? 0.0f : (pace > 0 ? 0.5f * (pace + jump) : jump);
            last = at;
            rows++;
            if (!head) continue;
            Ribbon* near = nullptr;
            float best = 1.4f;
            for (Ribbon& r : ribbons) {
                float d = length(r.tip_view - load(in.origin));
                if (r.viewport == vi && d < best) best = d, near = &r;
            }
            if (near) near->wet = wet = true;
        }
        if (wet) bits.insert(bits.end(), splash.begin(), splash.end());
    }
    // SR2_DUSTDUMP (with SR2_DUSTLOG): the other effect models drawn beside the fans, to tell what they are
    static const bool dump = getenv("SR2_DUSTDUMP") != nullptr;
    if (log && dump)
        for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
            if (!has_fan[vi]) continue;
            const R3DViewport& v = out.viewports[vi];
            for (const R3DInstance& in : instances[v.build_index])
                for (int pass = 0; pass < 2; pass++) {
                    const uint32_t* r = pass ? in.trans : in.opaque;
                    uint32_t k = (pass ? v.trans_first : v.opaque_first) + r[0];
                    if (r[1] <= r[0] || k + 2 >= out.vertices.size()) continue;
                    const R3DVertex* x = &out.vertices[k];
                    if (!(x->flags & R3D_TEXTURED) || (x->flags & R3D_SPECULAR)) continue;
                    fprintf(log, "  model %06x polys %u %c tris %u flags %x tex %08x color %08x origin %.2f %.2f %.2f z axis %.2f %.2f %.2f: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n",
                            in.start, in.polygons, pass ? 'T' : 'O', (r[1] - r[0]) / 3, x->flags, x->tex, x->color, in.origin[0], in.origin[1],
                            in.origin[2], in.axes[6], in.axes[7], in.axes[8], x[0].pos[0], x[0].pos[1], x[0].pos[2], x[1].pos[0], x[1].pos[1],
                            x[1].pos[2], x[2].pos[0], x[2].pos[1], x[2].pos[2]);
                }
        }

    // Without a fan this frame the world goes on from the tracker's camera
    const R3DViewport* w = world >= 0 ? &out.viewports[size_t(world)] : nullptr;
    if (w) {
        Frame t;
        t.x = load(w->world_axes), t.y = load(w->world_axes + 3), t.z = load(w->world_axes + 6);
        V3 cam = load(w->camera);
        if (f.ok) {
            g_delta = f.world(V3{}) - cam;
            g_have_delta = true;
        } else if (g_have_delta) {
            t.o = t.dir(cam + g_delta) * -1.0f;
            t.ok = true;
            f = t;
        }
    }
    if (!f.ok) {   // nowhere to put them: they wait (and age)
        for (Particle& p : g_parts) p.age += FRAME_S * float(steps);
        g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Particle& p) { return p.age >= p.life; }), g_parts.end());
        g_emitters.clear();
        g_before.ok = false;
        g_cam_ok = false;
        return;
    }

    for (Particle& p : g_parts) p.before = p.p;
    for (Particle& p : g_parts)
        for (int s = 0; s < steps; s++) step(p, FRAME_S);

    // Each fan to the wheel it was last frame, or a new one
    Rng rng{hash32(uint32_t(out.frame) * 2654435761u + 17u)};
    // A cut to another camera (or the first frame): only then is a fan seen
    // already drawn out given the dust it has left so far. A car coming into
    // view (the game draws a far one's fan only from close enough) raises its
    // dust from there on: made up, it popped in already spread out
    V3 cam_now = f.world(V3{});
    bool cut = !g_cam_ok || length(cam_now - g_cam_prev) > 3.0f * float(std::max(steps, 1));
    g_cam_prev = cam_now, g_cam_ok = true;
    for (Emitter& e : g_emitters) e.matched = false;
    for (const Ribbon& r : ribbons) {
        V3 tip = r.path.front();
        float dist_eye = length(f.view(tip));
        int best = -1;
        float best_d = 1e30f;
        for (int i = 0; i < int(g_emitters.size()); i++) {
            const Emitter& e = g_emitters[size_t(i)];
            if (e.matched) continue;
            // (one unseen a few frames, its fan flickering out, is looked for where it has gone on to)
            float d = length(e.tip + e.vel * float(std::max(steps, 1) + e.unseen) - tip);
            if (d < 1.5f + 4.0f * float(std::max(steps, 1)) + 0.15f * float(e.unseen) && d < best_d) best = i, best_d = d;
        }
        if (best >= 0) {
            Emitter& e = g_emitters[size_t(best)];
            e.matched = true;
            if (steps > 0 && e.unseen > 0) {
                // Back after its fan went out for a moment (the game drops a far
                // car's fan now and then, and a wheel in the air raises none): the
                // dust starts again from where it is, as a wheel would raise it,
                // not made up along the gap
                if (log) fprintf(log, "  emitter %u back after %d frames, eye %.1f full %d\n", e.id, e.unseen, dist_eye, int(r.full));
                // (a short gap, the game's fan flickering, is filled in: the wheel
                // was on the ground all along; after a long one, a jump, it is not)
                if (e.unseen <= 8) emit_along(e, r, e.tip, tip, float(steps + e.unseen), dist_eye, textures, rng);
                e.vel = (tip - e.tip) * (1.0f / float(steps + e.unseen));
                e.tip = tip;
                e.unseen = 0;
                continue;
            }
            e.unseen = 0;
            if (steps > 0) {
                emit_along(e, r, e.tip, tip, float(steps), dist_eye, textures, rng);
                e.vel = (tip - e.tip) * (1.0f / float(steps));
                e.tip = tip;
            }
            continue;
        }
        if (g_emitters.size() >= MAX_EMITTERS) continue;
        Emitter e;
        e.id = g_next_id++;
        e.tip = tip;
        e.matched = true;
        // A fan already drawn out when first seen (a car coming into view, a
        // change of camera): the dust it has left on its way so far is made up,
        // back along the fan and straight on behind it, so the cloud does not
        // grow out of nothing. The fan is about a dozen frames of the wheel's way
        float per_frame = r.length / RIBBON_FRAMES;
        V3 heading = r.path.size() > 1 ? normalize(r.path.front() - r.path[1], {0, 0, 0}) : V3{};
        e.vel = heading * per_frame;
        if (r.full && per_frame > 0.05f && cut) {
            auto along = [&](float s) {   // the point a way s back from the wheel
                for (size_t i = 1; i < r.path.size(); i++) {
                    float seg = length(r.path[i] - r.path[i - 1]);
                    if (s <= seg || i + 1 == r.path.size()) return r.path[i - 1] + (r.path[i] - r.path[i - 1]) * (seg > 1e-6f ? s / seg : 0.0f);
                    s -= seg;
                }
                return r.path.back();
            };
            const int frames = 75;   // (not all of it: it would go straight on through a bend's inside)
            size_t from = g_parts.size();
            for (int k = frames; k >= 1; k--) {
                for (size_t i = from; i < g_parts.size(); i++) step(g_parts[i], FRAME_S);
                emit_along(e, r, along(per_frame * float(k)), along(per_frame * float(k - 1)), 1.0f, dist_eye, textures, rng);
            }
            for (size_t i = from; i < g_parts.size(); i++) g_parts[i].before = g_parts[i].p - g_parts[i].v * FRAME_S;
        }
        if (log) fprintf(log, "  new emitter %u vp %u eye %.1f m rungs %zu length %.2f full %d cut %d\n", e.id, r.viewport, dist_eye, r.path.size(), r.length, int(r.full), int(cut));
        g_emitters.push_back(e);
    }
    for (Emitter& e : g_emitters)
        if (!e.matched) e.unseen += std::max(steps, 1);
    g_emitters.erase(std::remove_if(g_emitters.begin(), g_emitters.end(), [](const Emitter& e) { return e.unseen > 40; }), g_emitters.end());
    g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Particle& p) { return p.age >= p.life; }), g_parts.end());

    // The game's own fans and bits are taken out: folded to a point (the ray
    // tracer takes the cars' polygons as they are) and fully see-through
    auto fold = [&](uint32_t k) {
        V3 at = load(out.vertices[k].pos), was = load(out.vertices[k].prev);
        for (int j = 0; j < 3; j++) {
            R3DVertex& x = out.vertices[k + j];
            store(x.pos, at), store(x.prev, was);
            x.color &= 0x00FFFFFFu;
            x.flags &= ~(R3D_TEXTURED | R3D_TEX_ALPHA | R3D_ALPHA_TEST);
        }
    };
    for (const Ribbon& r : ribbons)
        for (uint32_t k : r.tris) fold(k);
    for (uint32_t k : stray) fold(k);
    for (uint32_t k : bits) fold(k);

    if (log) {
        size_t puffs = 0;
        for (const Particle& p : g_parts) puffs += p.kind == PUFF;
        fprintf(log, "frame %llu steps %d fans %zu bits %zu emitters %zu puffs %zu clods %zu world %d ms %.3f\n", (unsigned long long)out.frame, steps,
                ribbons.size(), bits.size() / 2, g_emitters.size(), puffs, g_parts.size() - puffs, world,
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count());
        for (const Ribbon& r : ribbons)
            fprintf(log, "  fan vp %u tris %zu tip %.2f %.2f %.2f tail %.2f %.2f %.2f length %.2f strength %.2f rgb %.2f %.2f %.2f debris %08x wet %d\n",
                    r.viewport, r.tris.size(), r.path.front().x, r.path.front().y, r.path.front().z, r.path.back().x, r.path.back().y,
                    r.path.back().z, r.length, r.strength, r.rgb.x, r.rgb.y, r.rgb.z, r.debris, int(r.wet));
        fflush(log);
    }

    if (world < 0 || g_parts.empty()) {
        g_before = f;
        return;
    }
    {
        const R3DViewport& wv = out.viewports[size_t(world)];
        g_px_per_unit = std::fabs(wv.proj[5]) * wv.h * 0.5f;
    }
    std::vector<R3DVertex> add;
    add.reserve(g_parts.size() * 6);
    draw(f, g_before, add);
    g_before = f;
    if (add.empty()) return;
    // A viewport of their own: the world's (its camera, light and fog), drawn
    // after the last one of its priority, so they come over the cars' own
    // see-through polygons too (the shadow the game lays under the car is one,
    // in the car's viewport: drawn after the dust, it cut its shape out of it).
    // It is not the world again (nothing of it is solid, lit or tracked)
    R3DViewport own = out.viewports[size_t(world)];
    own.world = false;
    own.opaque_first = own.trans_first = uint32_t(out.vertices.size());
    own.opaque_count = 0;
    own.trans_count = uint32_t(add.size());
    own.back_first = own.back_count = 0;
    own.scroll_fog = 0;
    out.vertices.insert(out.vertices.end(), add.begin(), add.end());
    size_t at = size_t(world) + 1;
    while (at < out.viewports.size() && out.viewports[at].priority == own.priority) at++;
    out.viewports.insert(out.viewports.begin() + ptrdiff_t(at), own);
}
