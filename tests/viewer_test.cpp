// The capture viewer (src/viewer) without a window: a snapshot written and
// read back, and a frame made by hand (a viewport, two copies of a model of
// two polygons) taken through the game's own scene builder and then through
// the viewer's free camera, picking, view modes and texture listing.
// On Windows it also checks that the viewer names a texture as the texture
// packs do, in each of the twelve texel formats.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

#include "capture.h"
#include "r3d_dust.h"
#include "r3d_scene.h"
#include "textures.h"
#include "tilegen.h"
#include "view_math.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "texture_pack.h"
#endif

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAILED line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

static bool close_to(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// Real3D memory written word by word, as the game's bus does
struct Memory {
    std::map<uint32_t, std::shared_ptr<R3DPage>> pages;
    void write(uint32_t ppc_addr, uint32_t v) {
        auto& page = pages[ppc_addr >> 16];
        if (!page) {
            page = std::make_shared<R3DPage>();
            page->fill(0);
        }
        memcpy(page->data() + (ppc_addr & 0xFFFC), &v, 4);
    }
    // Culling RAM (word addresses: low below 0x100000, high from 0x800000) and polygon RAM
    void cull(uint32_t addr, uint32_t v) {
        if (addr >= 0x800000) write(0x8E000000 + (addr & 0x3FFFF) * 4, v);
        else write(0x8C000000 + addr * 4, v);
    }
    void cullf(uint32_t addr, float f) {
        uint32_t v;
        memcpy(&v, &f, 4);
        cull(addr, v);
    }
    void poly(uint32_t addr, uint32_t v) { write(0x98000000 + addr * 4, v); }
    std::shared_ptr<R3DFrameState> state(uint64_t frame) const {
        auto s = std::make_shared<R3DFrameState>();
        for (const auto& [key, page] : pages) s->pages[key] = page;
        s->frame = frame;
        return s;
    }
};

// 24-bit fixed point in bits 8-31: a vertex coordinate (11 fraction bits), a face normal (22)
static uint32_t coord(float v) { return uint32_t(int32_t(std::lround(v * 2048.0f))) << 8; }
static uint32_t normal(float v) { return uint32_t(int32_t(std::lround(v * 4194304.0f))) << 8; }

constexpr uint32_t VIEWPORT = 0x800000, MATRICES = 0x100, LODS = 0x200, NODE_A = 0x300, NODE_B = 0x340, MODEL = 0x400;
constexpr uint32_t TEX_X = 32, TEX_Y = 64;

// One viewport over the whole screen with a square 90 degree view; two nodes,
// each placing the same model 20 units ahead (B 6 units to the right of A).
// The model: a 2 x 2 textured quad facing the camera and, below it, a
// triangle facing away (one-sided: the back-face test leaves it out).
// Real3D axes: x ahead, y to the right, z down (docs/real3d_spec.md 4.2)
static Memory test_frame() {
    Memory m;
    m.cull(VIEWPORT + 0x00, 0);            // priority 0
    m.cull(VIEWPORT + 0x01, 0x01000000);   // no next viewport
    m.cull(VIEWPORT + 0x02, NODE_A);
    m.cullf(VIEWPORT + 0x04, -1.0f);       // the sun, towards the camera
    m.cullf(VIEWPORT + 0x07, 1.0f);
    m.cullf(VIEWPORT + 0x08, 0.5f);
    m.cullf(VIEWPORT + 0x09, 0.5f * 496.0f / 384.0f);
    m.cullf(VIEWPORT + 0x0A, 0.5f);
    m.cullf(VIEWPORT + 0x0B, 0.5f);
    m.cull(VIEWPORT + 0x14, (496 * 4) | (384 * 4) << 16);
    m.cull(VIEWPORT + 0x16, MATRICES);
    m.cull(VIEWPORT + 0x17, LODS);
    m.cull(VIEWPORT + 0x24, 0x8000);       // ambient
    // Matrix 0, the coordinate system: no translation, rows of the identity
    m.cullf(MATRICES + 3, 1.0f);
    m.cullf(MATRICES + 7, 1.0f);
    m.cullf(MATRICES + 11, 1.0f);
    auto node = [&](uint32_t addr, float right, uint32_t sibling) {
        m.cull(addr + 0, 0x11);            // a node with a translation
        m.cull(addr + 3, 0x7F << 12);      // no LOD
        m.cullf(addr + 4, 20.0f);
        m.cullf(addr + 5, right);
        m.cullf(addr + 6, 0.0f);
        m.cull(addr + 7, 0x01000000 | MODEL);
        m.cull(addr + 8, sibling);
    };
    node(NODE_A, 0.0f, NODE_B);
    node(NODE_B, 6.0f, 0);
    uint32_t p = MODEL;
    auto vertex = [&](float x, float y, float z, uint32_t u, uint32_t v) {
        m.poly(p++, coord(x));
        m.poly(p++, coord(y));
        m.poly(p++, coord(z));
        m.poly(p++, u << 16 | v);
    };
    // The quad: its normal towards the camera, colour in the header, a 32 x 32 texture
    m.poly(p++, 0x40);
    m.poly(p++, normal(-1.0f) | 0x02);
    m.poly(p++, 0);
    m.poly(p++, 0);
    m.poly(p++, 0xFF804000);
    m.poly(p++, 0x80 | (TEX_Y / 32));
    m.poly(p++, 0x800000 | 0x400);
    vertex(0, -1, -1, 0, 0);
    vertex(0, 1, -1, 256, 0);
    vertex(0, 1, 1, 256, 256);
    vertex(0, -1, 1, 0, 256);
    // The triangle: its normal away from the camera, untextured, the last polygon
    m.poly(p++, 0);
    m.poly(p++, normal(1.0f) | 0x02 | 0x04);
    m.poly(p++, 0);
    m.poly(p++, 0);
    m.poly(p++, 0x20FF2000);
    m.poly(p++, 0);
    m.poly(p++, 0x800000);
    vertex(0, -1, 2, 0, 0);
    vertex(0, 1, 2, 0, 0);
    vertex(0, 0, 3, 0, 0);
    return m;
}

// Where the renderer draws a point of a viewport (R3DRenderer::record_world)
static void to_window(const R3DViewport& v, const ViewLayout& lay, const float* p, float& x, float& y) {
    bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
    float widen = full ? lay.view_w / SCREEN_W : 1.0f;
    float vx = full ? 0.0f : v.x + lay.game_x0, vw = full ? lay.view_w : v.w;
    float cx = v.proj[0] / widen * p[0] + v.proj[8] / widen * p[2], cy = v.proj[5] * p[1] + v.proj[9] * p[2], cw = -p[2];
    x = lay.ox + (vx + (cx / cw + 1) / 2 * vw) * lay.scale * lay.px_aspect;
    y = lay.oy + (v.y + (cy / cw + 1) / 2 * v.h) * lay.scale;
}

static void test_capture_file() {
    std::mt19937 rng(77);
    Capture c;
    Memory m = test_frame();
    c.state = m.state(123456789012ull);
    c.texels.resize(2048 * 2048);
    for (auto& t : c.texels) t = uint16_t(rng());
    c.vram.resize(TILEGEN_SIZE);
    for (auto& b : c.vram) b = uint8_t(rng());
    const char* path = "viewer_test_capture.bin";
    CHECK(save_capture(path, c));
    Capture back;
    std::string error;
    CHECK(load_capture(path, back, error));
    if (back.state) {
        CHECK(back.state->frame == c.state->frame);
        CHECK(back.state->pages.size() == c.state->pages.size());
        for (const auto& [key, page] : c.state->pages) {
            auto it = back.state->pages.find(key);
            CHECK(it != back.state->pages.end() && *it->second == *page);
        }
        CHECK(back.texels == c.texels);
        CHECK(back.vram == c.vram);
        CHECK(back.path == path);
    }
    // Not a snapshot, a snapshot cut short and no file at all
    if (FILE* f = fopen(path, "wb")) {
        fwrite("not a snapshot at all", 1, 21, f);
        fclose(f);
    }
    CHECK(!load_capture(path, back, error) && !error.empty());
    if (FILE* f = fopen(path, "wb")) {
        uint32_t head[4] = {0x3153574C, 1, 0, 3};
        fwrite(head, 4, 4, f);
        fclose(f);
    }
    CHECK(!load_capture(path, back, error));
    remove(path);
    CHECK(!load_capture(path, back, error));
}

static void test_camera() {
    ViewCamera cam;
    CHECK(!cam.moved());
    const float up[3] = {0, 1, 0};
    float p[3] = {1, 2, -3}, q[3];
    cam.to_view(p, q);
    CHECK(close_to(q[0], 1) && close_to(q[1], 2) && close_to(q[2], -3));
    // Forward is -Z; right and up are +X and +Y
    cam.move(up, 2, 3, 5);
    CHECK(cam.moved() && close_to(cam.pos[0], 2) && close_to(cam.pos[1], 3) && close_to(cam.pos[2], -5));
    cam.to_view(p, q);
    CHECK(close_to(q[0], -1) && close_to(q[1], -1) && close_to(q[2], 2));
    // A quarter turn to the left: what was to the left is now ahead
    cam.reset();
    cam.turn(up, 1.5707963f, 0);
    float left[3] = {-10, 0, 0};
    cam.to_view(left, q);
    CHECK(close_to(q[0], 0) && close_to(q[1], 0) && close_to(q[2], -10));
    // Looking up: what was above is now ahead
    cam.reset();
    cam.turn(up, 0, 1.5707963f);
    float above[3] = {0, 10, 0};
    cam.to_view(above, q);
    CHECK(close_to(q[0], 0) && close_to(q[1], 0) && close_to(q[2], -10));
    // An orbit keeps the point it turns about straight ahead and as far away
    cam.reset();
    cam.move(up, 1, 2, 3);
    float pivot[3] = {1, 2, -3 - 7};
    for (int i = 0; i < 300; i++) cam.orbit(up, 0.021f, i % 3 ? 0.004f : -0.011f, 7);
    cam.to_view(pivot, q);
    CHECK(close_to(q[0], 0, 1e-2f) && close_to(q[1], 0, 1e-2f) && close_to(q[2], -7, 1e-2f));
    // Dragged to the right, the camera goes round to the left of what it looks at
    cam.reset();
    cam.orbit(up, -1.5707963f, 0, 5);
    CHECK(close_to(cam.pos[0], -5) && close_to(cam.pos[1], 0) && close_to(cam.pos[2], -5));
    // A track slides it along its own right and up, whatever the world's up is
    cam.reset();
    cam.turn(up, 0, 1.5707963f);   // looking straight up: its own up is now +Z
    cam.track(2, 3);
    CHECK(close_to(cam.pos[0], 2) && close_to(cam.pos[1], 0) && close_to(cam.pos[2], 3));
    // Many small turns leave its axes square and of unit length
    cam.reset();
    for (int i = 0; i < 5000; i++) cam.turn(up, 0.013f, i % 2 ? 0.007f : -0.0065f);
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            float d = cam.rot[a * 3] * cam.rot[b * 3] + cam.rot[a * 3 + 1] * cam.rot[b * 3 + 1] + cam.rot[a * 3 + 2] * cam.rot[b * 3 + 2];
            CHECK(close_to(d, a == b ? 1.0f : 0.0f, 1e-4f));
        }
}

static void test_follows() {
    R3DScene s;
    auto add = [&](int priority, float x, float w, bool world) {
        R3DViewport v;
        v.priority = priority;
        v.x = x;
        v.w = w;
        v.world = world;
        memset(v.proj, 0, sizeof(v.proj));
        s.viewports.push_back(v);
    };
    add(1, 0, 496, false);   // no world yet: everything follows
    add(2, 16, 96, false);
    std::vector<uint8_t> f = view_follows(s);
    CHECK(f.size() == 2 && f[0] && f[1]);
    float up[3];
    view_world_up(s, up);
    CHECK(up[0] == 0 && up[1] == 1 && up[2] == 0);
    s.viewports.clear();
    add(1, 0, 496, true);    // the world
    add(1, 0, 496, false);   // the player's car, drawn with it
    add(1, 16, 96, false);   // a gauge of the HUD
    add(2, 0, 496, false);   // the countdown, over everything
    s.viewports[0].world_axes[3] = 0, s.viewports[0].world_axes[4] = 0, s.viewports[0].world_axes[5] = 2;
    f = view_follows(s);
    CHECK(f.size() == 4 && f[0] && f[1] && !f[2] && !f[3]);
    view_world_up(s, up);
    CHECK(close_to(up[0], 0) && close_to(up[1], 0) && close_to(up[2], 1));
    ViewOptions opt;
    CHECK(opt.shows(0, f) && opt.shows(3, f));
    opt.overlays = false;
    CHECK(opt.shows(1, f) && !opt.shows(2, f) && !opt.shows(3, f));
    opt.solo = 1;
    CHECK(!opt.shows(0, f) && opt.shows(1, f));
}

static size_t count_flag(const R3DScene& s, uint32_t flag) {
    size_t n = 0;
    for (const R3DVertex& x : s.vertices) n += (x.flags & flag) != 0;
    return n;
}

static void test_scene() {
    r3d_set_sr2_dust(false);   // as the viewer
    Memory m = test_frame();
    auto state = m.state(42);
    R3DTextureRam ram;
    std::vector<uint16_t> texels(2048 * 2048);
    std::mt19937 rng(5);
    for (auto& t : texels) t = uint16_t(rng());
    ram.load_all(texels);

    R3DSceneBuilder builder;
    builder.set_texture_pack(nullptr, &ram);
    R3DScene base;
    builder.build(*state, base);
    // What the viewer relies on: the builder's lists and its record of each model
    CHECK(base.viewports.size() == 1);
    CHECK(base.polygons == 4);
    CHECK(base.vertices.size() == 12 && base.backs.size() == 6);
    if (base.viewports.size() != 1 || base.vertices.size() != 12 || base.backs.size() != 6) return;
    const R3DViewport& v = base.viewports[0];
    CHECK(v.opaque_first == 0 && v.opaque_count == 12 && v.trans_count == 0 && v.back_first == 0 && v.back_count == 6);
    CHECK(close_to(v.w, 496) && close_to(v.h, 384) && close_to(v.proj[0], 1) && close_to(v.proj[8], 0) && close_to(v.proj[9], 0));
    const auto& instances = builder.instances();
    CHECK(instances.size() == 1 && v.build_index == 0);
    if (instances.size() != 1 || instances[0].size() != 2) {
        CHECK(!"two models");
        return;
    }
    // (siblings are drawn first: B, then A)
    const R3DInstance &b = instances[0][0], &a = instances[0][1];
    CHECK(a.start == MODEL && b.start == MODEL && a.polygons == 2 && a.key != b.key);
    CHECK(b.opaque[0] == 0 && b.opaque[1] == 6 && a.opaque[0] == 6 && a.opaque[1] == 12);
    CHECK(b.back[0] == 0 && b.back[1] == 3 && a.back[0] == 3 && a.back[1] == 6);
    CHECK(close_to(a.origin[0], 0) && close_to(a.origin[1], 0) && close_to(a.origin[2], -20) && close_to(b.origin[0], 6));
    for (const R3DVertex& x : base.vertices) CHECK(close_to(x.pos[2], -20) && (x.flags & R3D_TEXTURED));
    const uint32_t tex = base.vertices[0].tex;
    CHECK(tex == (TEX_X | TEX_Y << 11));

    std::vector<uint8_t> follows = view_follows(base);
    CHECK(follows.size() == 1 && follows[0]);
    ViewBounds bounds = view_bounds(base, v, a);
    CHECK(bounds.vertices == 9 && bounds.radius > 1.0f && bounds.radius < 3.0f && close_to(bounds.centre[0], 0) && close_to(bounds.centre[2], -20));

    // Picking from the game's camera, in a 4:3 window twice the Model 3 screen
    ViewLayout lay{496, 2, 1, 0, 0, 0};
    ViewOptions opt;
    R3DScene moved, shown;
    ViewCamera cam;
    view_move(base, cam, follows, moved);
    CHECK(moved.vertices.size() == 12 && moved.backs.size() == 6);
    for (size_t i = 0; i < moved.vertices.size(); i++) {
        CHECK(memcmp(moved.vertices[i].pos, base.vertices[i].pos, 12) == 0);
        CHECK(memcmp(moved.vertices[i].prev, moved.vertices[i].pos, 12) == 0);
    }
    float dir[3];
    CHECK(view_ray(v, lay, 496, 384, dir) && close_to(dir[0], 0) && close_to(dir[1], 0) && dir[2] == -1);
    CHECK(!view_ray(v, lay, -5, 384, dir) && !view_ray(v, lay, 496, 800, dir));
    // A pixel of the window, as the rays of two rows one apart tell it
    float lower[3];
    CHECK(view_ray(v, lay, 496, 300, dir) && view_ray(v, lay, 496, 301, lower));
    CHECK(view_pixel_size(v, lay) > 0 && close_to(std::fabs(lower[1] - dir[1]), view_pixel_size(v, lay), 1e-6f));
    // A wider lens: the same pixel covers that much more, and the ray to the
    // edge of the picture leans out that much more
    {
        ViewCamera wide;
        CHECK(!wide.moved());
        wide.lens = 2;
        CHECK(wide.moved());
        R3DScene wider;
        view_move(base, wide, follows, wider);
        const R3DViewport& w = wider.viewports[0];
        CHECK(close_to(view_pixel_size(w, lay), 2 * view_pixel_size(v, lay), 1e-6f));
        float across, down, across2, down2, edge[3], edge2[3];
        view_angles(v, lay, across, down);
        view_angles(w, lay, across2, down2);
        CHECK(across > 0 && down > 0 && close_to(across2, 2 * across) && close_to(down2, 2 * down));
        CHECK(view_ray(v, lay, 496, 1, edge) && view_ray(w, lay, 496, 1, edge2));
        CHECK(close_to(edge2[1], 2 * edge[1]) && close_to(std::fabs(edge[1]), down, 0.01f));
        for (size_t i = 0; i < wider.vertices.size(); i++) CHECK(memcmp(wider.vertices[i].pos, base.vertices[i].pos, 12) == 0);
    }
    float x, y;
    to_window(v, lay, a.origin, x, y);
    CHECK(close_to(x, 496) && close_to(y, 384));
    ViewPick pick = view_pick(moved, instances, follows, opt, lay, x, y);
    CHECK(pick.hit() && pick.viewport == 0 && pick.instance == 1 && pick.list == 0 && close_to(pick.distance, 20));
    to_window(v, lay, b.origin, x, y);
    pick = view_pick(moved, instances, follows, opt, lay, x, y);
    CHECK(pick.hit() && pick.instance == 0 && pick.list == 0);
    // The triangle facing away, below A: there only with the culled faces shown
    const float under_a[3] = {0, -2.3f, -20};
    to_window(v, lay, under_a, x, y);
    pick = view_pick(moved, instances, follows, opt, lay, x, y);
    CHECK(pick.hit() && pick.instance == 1 && pick.list == 2);
    opt.backs = false;
    CHECK(!view_pick(moved, instances, follows, opt, lay, x, y).hit());
    opt.backs = true;
    CHECK(!view_pick(moved, instances, follows, opt, lay, 20, 20).hit());
    opt.solo = 3;   // a viewport that is not there: nothing is shown
    to_window(v, lay, a.origin, x, y);
    CHECK(!view_pick(moved, instances, follows, opt, lay, x, y).hit());
    opt.solo = -1;

    // The camera 6 units to the right and turned a little: B is where its ray says
    float up[3];
    view_world_up(base, up);
    cam.move(up, 6, 0, 0);
    view_move(base, cam, follows, moved);
    pick = view_pick(moved, instances, follows, opt, lay, 496, 384);
    CHECK(pick.hit() && pick.instance == 0 && close_to(pick.distance, 20));
    cam.turn(up, 0.2f, -0.1f);
    cam.move(up, 0.5f, -0.25f, 3);
    view_move(base, cam, follows, moved);
    float seen[3];
    cam.to_view(a.origin, seen);
    to_window(v, lay, seen, x, y);
    pick = view_pick(moved, instances, follows, opt, lay, x, y);
    CHECK(pick.hit() && pick.instance == 1 && close_to(pick.distance, -seen[2], 1e-2f));
    // ...and in a 16:9 window, where the renderer widens the full-screen viewports
    ViewLayout wide{384.0f * 16 / 9, 2.5f, 1, 0, 0, (384.0f * 16 / 9 - 496) / 2};
    to_window(v, wide, seen, x, y);
    pick = view_pick(moved, instances, follows, opt, wide, x, y);
    CHECK(pick.hit() && pick.instance == 1);
    cam.to_view(b.origin, seen);
    to_window(v, wide, seen, x, y);
    pick = view_pick(moved, instances, follows, opt, wide, x, y);
    CHECK(pick.hit() && pick.instance == 0);
    // The sun turns with the view: from the camera's own back axis it is straight behind
    ViewCamera round;
    round.turn(up, 3.1415927f, 0);
    view_move(base, round, follows, moved);
    CHECK(close_to(moved.viewports[0].sun[2], -base.viewports[0].sun[2]) && close_to(moved.vertices[0].pos[2], 20));

    // What the renderer is given
    cam.reset();
    view_move(base, cam, follows, moved);
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.viewports.size() == 1 && shown.vertices.size() == 18 && shown.backs.empty());
    CHECK(shown.viewports[0].opaque_first == 0 && shown.viewports[0].opaque_count == 18 && shown.viewports[0].trans_count == 0);
    CHECK(shown.viewports[0].back_count == 0 && count_flag(shown, R3D_LUMINOUS) == 0);
    opt.backs = false;
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.vertices.size() == 12 && shown.viewports[0].opaque_count == 12);
    opt.backs = true;
    // A selected: its 9 vertices tinted, and its outline (4 edges of the quad, 3 of the triangle, 6 vertices each)
    opt.sel_viewport = 0, opt.sel_instance = 1;
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.vertices.size() == 18 + 7 * 6 && shown.viewports[0].opaque_count == 18 + 7 * 6);
    CHECK(count_flag(shown, R3D_LUMINOUS) == 9 + 7 * 6);
    for (size_t i = 0; i < 6; i++) CHECK(!(shown.vertices[i].flags & R3D_LUMINOUS) && (shown.vertices[6 + i].flags & R3D_LUMINOUS));
    opt.sel_instance = 7;   // a model that is not there: no selection
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.vertices.size() == 18 && count_flag(shown, R3D_LUMINOUS) == 0);
    opt.sel_viewport = opt.sel_instance = -1;
    opt.mode = ViewMode::Flat;
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.vertices.size() == 18 && count_flag(shown, R3D_TEXTURED) == 0);
    // The wire frame: 2 quads and 2 triangles, lines as wide as 1.2 pixels of the window
    opt.mode = ViewMode::Wire;
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.vertices.size() == (2 * 4 + 2 * 3) * 6 && shown.viewports[0].opaque_count == shown.vertices.size());
    CHECK(count_flag(shown, R3D_LUMINOUS) == shown.vertices.size() && count_flag(shown, R3D_TEXTURED) == 0);
    {
        // (the first edge runs along the top of B's quad: its two sides 1.2 pixels apart)
        float x0, y0, x1, y1;
        to_window(v, lay, shown.vertices[0].pos, x0, y0);
        to_window(v, lay, shown.vertices[1].pos, x1, y1);
        CHECK(close_to(std::hypot(x1 - x0, y1 - y0), 1.2f, 0.05f));
    }
    opt.solo = 3;
    view_display(moved, instances, follows, opt, lay, shown);
    CHECK(shown.viewports.size() == 1 && shown.vertices.empty() && shown.viewports[0].opaque_count == 0);
    opt = ViewOptions{};

    // The model's textures
    TextureWord word = texture_word(tex);
    CHECK(word.x == int(TEX_X) && word.y == int(TEX_Y) && word.w == 32 && word.h == 32 && word.format == 0);
    std::vector<TextureUse> uses = model_textures(base, v, a, ram);
    CHECK(uses.size() == 1);
    if (uses.size() == 1) {
        CHECK(uses[0].tex == tex && uses[0].triangles == 2 && uses[0].rgba.size() == 32 * 32 * 4);
        uint8_t first[4];
        decode_texel(texels[TEX_Y * 2048 + TEX_X], 0, first);
        CHECK(memcmp(first, uses[0].rgba.data(), 4) == 0);
        CHECK(texture_file_name(uses[0].hash).size() == 20);
    }

#ifdef _WIN32
    // The viewer's name for a texture is the texture packs': a pack that
    // dumps writes <hash>.png for each texture it is asked about
    const char* dump = "viewer_test_textures";
    TexturePack pack;
    pack.init("viewer_test_no_such_folder", dump);
    for (uint32_t format = 0; format < 12; format++) {
        // (other sizes and places too, across the edge of a texture page)
        uint32_t t = ((TEX_X + 96 * format) & 2047) | ((1000 + 40 * format) & 2047) << 11 | (format % 4) << 22 | (format % 3) << 25 | format << 28;
        std::vector<uint8_t> rgba;
        uint64_t hash = decode_texture(t, ram, rgba);
        pack.lookup(t, ram);
        std::string file = std::string(dump) + "/" + texture_file_name(hash);
        bool there = GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!there) printf("format %u: the pack did not write %s\n", format, file.c_str());
        CHECK(there);
        DeleteFileA(file.c_str());
    }
    RemoveDirectoryA(dump);
#endif
}

int main() {
    test_capture_file();
    test_camera();
    test_follows();
    test_scene();
    if (g_failed) return 1;
    printf("viewer: capture file, camera, picking, view modes and textures all as expected\n");
    return 0;
}
