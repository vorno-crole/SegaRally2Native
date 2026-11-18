#include "view_math.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "tilegen.h"   // SCREEN_W

namespace {

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const float* a, const float* b, float* out) {
    float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(out, r, sizeof(r));
}
inline bool normalize3(float* v) {
    float len = std::sqrt(dot3(v, v));
    if (len < 1e-20f) return false;
    for (int i = 0; i < 3; i++) v[i] /= len;
    return true;
}
// v turned about a unit axis (Rodrigues)
void turn3(const float* axis, float angle, float* v) {
    float c = std::cos(angle), s = std::sin(angle), d = dot3(axis, v), x[3];
    cross3(axis, v, x);
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + axis[i] * d * (1 - c);
}

// A viewport's place in the 3D view and the x terms of its projection as
// R3DRenderer::record_world draws it: full-screen viewports take the whole
// widened view, the others keep their place in the 4:3 area
struct Place {
    float x, w;     // Model 3 pixels inside the 3D view
    float p0, p8;   // proj[0], proj[8]
};
Place place_of(const R3DViewport& v, const ViewLayout& lay) {
    bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
    float widen = lay.view_w / SCREEN_W;
    if (full) return {0.0f, lay.view_w, v.proj[0] / widen, v.proj[8] / widen};
    return {v.x + lay.game_x0, v.w, v.proj[0], v.proj[8]};
}

// How much of the view a window pixel covers at a unit from the eye (the lines' width)
float pixel_slope(const R3DViewport& v, const ViewLayout& lay) {
    float tall = v.h * lay.scale * std::fabs(v.proj[5]);
    return tall > 1e-6f ? 2.0f / tall : 0.0f;
}

constexpr float WIRE_NEAR = 0.02f;
// RGBA8, R in the low byte (R3DVertex::color)
constexpr uint32_t WIRE_OPAQUE = 0xFFD8D8D8u, WIRE_TRANS = 0xFFE8C060u, WIRE_BACK = 0xFF707070u,
                   WIRE_SELECTED = 0xFF30D8FFu, TINT_SELECTED = 0x0040B0FFu;

// One edge as a thin quad turned to the eye, self-lit and untextured: `half`
// its half width in window pixels
void add_edge(std::vector<R3DVertex>& out, const float* a_in, const float* b_in, uint32_t color, float slope, float half) {
    float a[3] = {a_in[0], a_in[1], a_in[2]}, b[3] = {b_in[0], b_in[1], b_in[2]};
    if (a[2] > -WIRE_NEAR && b[2] > -WIRE_NEAR) return;   // behind the eye
    if (a[2] > -WIRE_NEAR || b[2] > -WIRE_NEAR) {         // crosses the near plane: cut there
        float* behind = a[2] > -WIRE_NEAR ? a : b;
        const float* ahead = a[2] > -WIRE_NEAR ? b : a;
        float t = (-WIRE_NEAR - ahead[2]) / (behind[2] - ahead[2]);
        for (int i = 0; i < 3; i++) behind[i] = ahead[i] + (behind[i] - ahead[i]) * t;
    }
    float along[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, mid[3] = {a[0] + b[0], a[1] + b[1], a[2] + b[2]}, side[3];
    cross3(along, mid, side);
    if (!normalize3(side)) return;   // seen end on, or no length
    float wa = half * slope * -a[2], wb = half * slope * -b[2];
    R3DVertex v{};
    v.nrm[2] = 1;
    v.color = color;
    v.flags = R3D_LUMINOUS;
    auto put = [&](const float* p, float w) {
        // (a hair nearer than the surface it runs along, so the outline of a selected model shows)
        for (int i = 0; i < 3; i++) v.pos[i] = v.prev[i] = (p[i] + side[i] * w) * 0.9985f;
        out.push_back(v);
    };
    put(a, -wa), put(a, wa), put(b, wb);
    put(a, -wa), put(b, wb), put(b, -wb);
}

// The edges of the triangles in [first, first + count). The builder cuts a
// four-sided polygon into (0, 1, 2) and (0, 2, 3): its diagonal is left out
void add_wire(std::vector<R3DVertex>& out, const std::vector<R3DVertex>& src, size_t first, size_t count, uint32_t color,
              float slope, float half) {
    if (first >= src.size()) return;
    size_t end = std::min(src.size(), first + count);
    auto same = [](const R3DVertex& p, const R3DVertex& q) { return memcmp(p.pos, q.pos, sizeof(p.pos)) == 0; };
    for (size_t i = first; i + 2 < end;) {
        const R3DVertex* t = &src[i];
        if (i + 5 < end && same(t[0], t[3]) && same(t[2], t[4])) {
            add_edge(out, t[0].pos, t[1].pos, color, slope, half);
            add_edge(out, t[1].pos, t[2].pos, color, slope, half);
            add_edge(out, t[4].pos, t[5].pos, color, slope, half);
            add_edge(out, t[5].pos, t[3].pos, color, slope, half);
            i += 6;
        } else {
            add_edge(out, t[0].pos, t[1].pos, color, slope, half);
            add_edge(out, t[1].pos, t[2].pos, color, slope, half);
            add_edge(out, t[2].pos, t[0].pos, color, slope, half);
            i += 3;
        }
    }
}

// Copies src[first, first + count) to the end of out; returns where it starts there
size_t append(std::vector<R3DVertex>& out, const std::vector<R3DVertex>& src, size_t first, size_t count) {
    size_t at = out.size();
    if (first >= src.size()) return at;
    size_t end = std::min(src.size(), first + count);
    out.insert(out.end(), src.begin() + ptrdiff_t(first), src.begin() + ptrdiff_t(end));
    return at;
}

// The selected model's own colour: self-lit orange over its textures
void tint(std::vector<R3DVertex>& out, size_t list_start, size_t list_size, const uint32_t range[2]) {
    size_t end = std::min<size_t>(range[1], list_size);
    for (size_t i = range[0]; i < end; i++) {
        R3DVertex& x = out[list_start + i];
        x.color = (x.color & 0xFF000000u) | TINT_SELECTED;
        x.flags = (x.flags | R3D_LUMINOUS) & ~uint32_t(R3D_FIXED_SHADE);
    }
}

// Ray from the origin against a triangle, from either side: the distance along dir, or 0
float ray_triangle(const float* dir, const float* a, const float* b, const float* c) {
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float p[3], q[3];
    cross3(dir, e2, p);
    float det = dot3(e1, p);
    if (std::fabs(det) < 1e-20f) return 0;
    float inv = 1.0f / det;
    float from[3] = {-a[0], -a[1], -a[2]};
    float u = dot3(from, p) * inv;
    if (u < 0 || u > 1) return 0;
    cross3(from, e1, q);
    float v = dot3(dir, q) * inv;
    if (v < 0 || u + v > 1) return 0;
    float t = dot3(e2, q) * inv;
    return t > 1e-3f ? t : 0;
}

}  // namespace

bool ViewCamera::moved() const {
    static const ViewCamera still;
    return memcmp(pos, still.pos, sizeof(pos)) != 0 || memcmp(rot, still.rot, sizeof(rot)) != 0 || lens != still.lens;
}

void ViewCamera::turn(const float* up, float yaw, float pitch) {
    float *right = rot, *above = rot + 3, *back = rot + 6;
    for (float* axis : {right, above, back}) turn3(up, yaw, axis);
    float about[3] = {right[0], right[1], right[2]};
    turn3(about, pitch, above);
    turn3(about, pitch, back);
    // (the rounding of many small turns: square and unit again)
    normalize3(back);
    cross3(above, back, right);
    if (!normalize3(right)) return reset();
    cross3(back, right, above);
}

void ViewCamera::move(const float* up, float right, float rise, float forward) {
    for (int i = 0; i < 3; i++) pos[i] += rot[i] * right + up[i] * rise - rot[6 + i] * forward;
}

void ViewCamera::orbit(const float* up, float yaw, float pitch, float distance) {
    float pivot[3];
    for (int i = 0; i < 3; i++) pivot[i] = pos[i] - rot[6 + i] * distance;
    turn(up, yaw, pitch);
    for (int i = 0; i < 3; i++) pos[i] = pivot[i] + rot[6 + i] * distance;
}

void ViewCamera::track(float right, float rise) {
    for (int i = 0; i < 3; i++) pos[i] += rot[i] * right + rot[3 + i] * rise;
}

void ViewCamera::to_view(const float* point, float* out) const {
    float d[3] = {point[0] - pos[0], point[1] - pos[1], point[2] - pos[2]};
    rotate(d, out);
}

void ViewCamera::rotate(const float* dir, float* out) const {
    float r[3] = {dot3(rot, dir), dot3(rot + 3, dir), dot3(rot + 6, dir)};
    memcpy(out, r, sizeof(r));
}

void view_world_up(const R3DScene& scene, float up[3]) {
    up[0] = 0, up[1] = 1, up[2] = 0;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) {
            float axis[3] = {v.world_axes[3], v.world_axes[4], v.world_axes[5]};
            if (normalize3(axis)) memcpy(up, axis, sizeof(axis));
        }
}

std::vector<uint8_t> view_follows(const R3DScene& scene) {
    const R3DViewport* world = nullptr;
    for (const R3DViewport& v : scene.viewports)
        if (v.world) world = &v;
    std::vector<uint8_t> follows(scene.viewports.size(), 1);
    if (!world) return follows;
    for (size_t i = 0; i < scene.viewports.size(); i++) {
        const R3DViewport& v = scene.viewports[i];
        bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
        follows[i] = v.world || (full && v.priority == world->priority);
    }
    return follows;
}

void view_move(const R3DScene& base, const ViewCamera& cam, const std::vector<uint8_t>& follows, R3DScene& out) {
    out.vertices = base.vertices;
    out.backs = base.backs;
    out.viewports = base.viewports;
    out.frame = base.frame;
    out.polygons = base.polygons;
    out.moving = 0;
    auto move_range = [&](std::vector<R3DVertex>& list, size_t first, size_t count) {
        if (first >= list.size()) return;
        size_t end = std::min(list.size(), first + count);
        for (size_t i = first; i < end; i++) {
            R3DVertex& x = list[i];
            cam.to_view(x.pos, x.pos);
            cam.rotate(x.nrm, x.nrm);
        }
    };
    for (size_t i = 0; i < out.viewports.size(); i++) {
        if (i >= follows.size() || !follows[i]) continue;
        R3DViewport& v = out.viewports[i];
        move_range(out.vertices, v.opaque_first, v.opaque_count);
        move_range(out.vertices, v.trans_first, v.trans_count);
        move_range(out.backs, v.back_first, v.back_count);
        // (the off-centre terms are ratios of the frustum's sides: they stay)
        if (cam.lens > 0) v.proj[0] /= cam.lens, v.proj[5] /= cam.lens;
        // The camera in the tracked world frame, before its axes are turned
        for (int k = 0; k < 3; k++) v.camera[k] += dot3(v.world_axes + 3 * k, cam.pos);
        cam.rotate(v.sun, v.sun);
        for (int k = 0; k < 3; k++) cam.rotate(v.world_axes + 3 * k, v.world_axes + 3 * k);
    }
    // A still picture: nothing is in between two game frames
    for (R3DVertex& x : out.vertices) memcpy(x.prev, x.pos, sizeof(x.pos));
    for (R3DVertex& x : out.backs) memcpy(x.prev, x.pos, sizeof(x.pos));
}

void view_display(const R3DScene& moved, const std::vector<std::vector<R3DInstance>>& instances,
                  const std::vector<uint8_t>& follows, const ViewOptions& opt, const ViewLayout& lay, R3DScene& out) {
    out.vertices.clear();
    out.backs.clear();
    out.viewports.clear();
    out.frame = moved.frame;
    out.polygons = moved.polygons;
    out.moving = 0;
    out.vertices.reserve(moved.vertices.size() + moved.backs.size());
    for (size_t i = 0; i < moved.viewports.size(); i++) {
        const R3DViewport& src = moved.viewports[i];
        R3DViewport v = src;
        v.back_first = v.back_count = 0;
        v.opaque_first = v.trans_first = uint32_t(out.vertices.size());
        v.opaque_count = v.trans_count = 0;
        if (!opt.shows(i, follows)) {
            out.viewports.push_back(v);
            continue;
        }
        const R3DInstance* sel = nullptr;
        if (opt.sel_viewport == int(i) && src.build_index < instances.size() && opt.sel_instance >= 0 &&
            size_t(opt.sel_instance) < instances[src.build_index].size())
            sel = &instances[src.build_index][size_t(opt.sel_instance)];
        const bool backs = opt.backs && i < follows.size() && follows[i];
        const float slope = pixel_slope(src, lay);
        auto outline = [&](float half) {
            if (!sel) return;
            auto range = [&](const std::vector<R3DVertex>& list, size_t first, size_t count, const uint32_t r[2]) {
                size_t from = std::min<size_t>(r[0], count), to = std::min<size_t>(r[1], count);
                if (to > from) add_wire(out.vertices, list, first + from, to - from, WIRE_SELECTED, slope, half);
            };
            range(moved.vertices, src.opaque_first, src.opaque_count, sel->opaque);
            range(moved.vertices, src.trans_first, src.trans_count, sel->trans);
            if (backs) range(moved.backs, src.back_first, src.back_count, sel->back);
        };
        if (opt.mode == ViewMode::Wire) {
            add_wire(out.vertices, moved.vertices, src.opaque_first, src.opaque_count, WIRE_OPAQUE, slope, 0.6f);
            if (backs) add_wire(out.vertices, moved.backs, src.back_first, src.back_count, WIRE_BACK, slope, 0.6f);
            add_wire(out.vertices, moved.vertices, src.trans_first, src.trans_count, WIRE_TRANS, slope, 0.6f);
            outline(1.4f);
            v.opaque_count = uint32_t(out.vertices.size()) - v.opaque_first;
            v.trans_first = uint32_t(out.vertices.size());
            out.viewports.push_back(v);
            continue;
        }
        size_t at = append(out.vertices, moved.vertices, src.opaque_first, src.opaque_count);
        if (sel) tint(out.vertices, at, out.vertices.size() - at, sel->opaque);
        if (backs) {
            at = append(out.vertices, moved.backs, src.back_first, src.back_count);
            if (sel) tint(out.vertices, at, out.vertices.size() - at, sel->back);
        }
        outline(1.0f);
        v.opaque_count = uint32_t(out.vertices.size()) - v.opaque_first;
        v.trans_first = uint32_t(out.vertices.size());
        at = append(out.vertices, moved.vertices, src.trans_first, src.trans_count);
        if (sel) tint(out.vertices, at, out.vertices.size() - at, sel->trans);
        v.trans_count = uint32_t(out.vertices.size()) - v.trans_first;
        if (opt.mode == ViewMode::Flat)
            for (size_t k = v.opaque_first; k < out.vertices.size(); k++) {
                R3DVertex& x = out.vertices[k];
                x.flags &= ~uint32_t(R3D_TEXTURED | R3D_ALPHA_TEST | R3D_TEX_ALPHA | R3D_INVERT_RGB);
                x.replace = 0;
            }
        out.viewports.push_back(v);
    }
}

bool view_ray(const R3DViewport& v, const ViewLayout& lay, float win_x, float win_y, float dir[3]) {
    if (lay.scale <= 0 || lay.px_aspect <= 0 || v.h <= 0) return false;
    Place p = place_of(v, lay);
    if (p.w <= 0 || std::fabs(p.p0) < 1e-9f || std::fabs(v.proj[5]) < 1e-9f) return false;
    // Window pixels to Model 3 pixels of the 3D view, then to the viewport's own -1..1
    float px = (win_x - lay.ox) / (lay.scale * lay.px_aspect), py = (win_y - lay.oy) / lay.scale;
    float nx = (px - p.x) / p.w * 2 - 1, ny = (py - v.y) / v.h * 2 - 1;
    if (nx < -1 || nx > 1 || ny < -1 || ny > 1) return false;
    // clip x = p0 x + p8 z, clip y = p5 y + p9 z, clip w = -z
    dir[0] = (nx + p.p8) / p.p0;
    dir[1] = (ny + v.proj[9]) / v.proj[5];
    dir[2] = -1;
    return true;
}

void view_angles(const R3DViewport& v, const ViewLayout& lay, float& across, float& down) {
    float p0 = std::fabs(place_of(v, lay).p0), p5 = std::fabs(v.proj[5]);
    across = p0 > 1e-9f ? 1.0f / p0 : 0.0f;
    down = p5 > 1e-9f ? 1.0f / p5 : 0.0f;
}

// (view_ray's dir[1] from one row of the window to the next)
float view_pixel_size(const R3DViewport& v, const ViewLayout& lay) { return pixel_slope(v, lay); }

ViewPick view_pick(const R3DScene& moved, const std::vector<std::vector<R3DInstance>>& instances,
                   const std::vector<uint8_t>& follows, const ViewOptions& opt, const ViewLayout& lay, float win_x,
                   float win_y) {
    ViewPick best;
    const auto& vps = moved.viewports;
    // Priority by priority, the last drawn first: each starts with a cleared
    // depth buffer, so whatever it draws covers the ones before
    for (size_t j = vps.size(); j > 0 && !best.hit();) {
        size_t i = j;
        while (i > 0 && vps[i - 1].priority == vps[j - 1].priority) i--;
        for (size_t k = i; k < j; k++) {
            const R3DViewport& v = vps[k];
            float dir[3];
            if (!opt.shows(k, follows) || !view_ray(v, lay, win_x, win_y, dir)) continue;
            auto test = [&](const std::vector<R3DVertex>& list, size_t first, size_t count, int which) {
                if (first >= list.size()) return;
                size_t end = std::min(list.size(), first + count);
                for (size_t t = first; t + 2 < end; t += 3) {
                    const R3DVertex* tri = &list[t];
                    if (which == 1 && (tri[0].color >> 24) < 16) continue;   // all but invisible (fades)
                    float d = ray_triangle(dir, tri[0].pos, tri[1].pos, tri[2].pos);
                    if (d <= 0 || (best.hit() && d >= best.distance)) continue;
                    best.viewport = int(k);
                    best.list = which;
                    best.vertex = uint32_t(t - first);
                    best.distance = d;
                    for (int c = 0; c < 3; c++) best.point[c] = dir[c] * d;
                }
            };
            test(moved.vertices, v.opaque_first, v.opaque_count, 0);
            test(moved.vertices, v.trans_first, v.trans_count, 1);
            if (opt.backs && k < follows.size() && follows[k]) test(moved.backs, v.back_first, v.back_count, 2);
        }
        j = i;
    }
    if (!best.hit()) return best;
    // The model that polygon belongs to
    const R3DViewport& v = vps[size_t(best.viewport)];
    if (v.build_index < instances.size()) {
        const auto& list = instances[v.build_index];
        for (size_t n = 0; n < list.size(); n++) {
            const uint32_t* r = best.list == 0 ? list[n].opaque : best.list == 1 ? list[n].trans : list[n].back;
            if (best.vertex >= r[0] && best.vertex < r[1]) {
                best.instance = int(n);
                break;
            }
        }
    }
    return best;
}

ViewBounds view_bounds(const R3DScene& scene, const R3DViewport& v, const R3DInstance& in) {
    ViewBounds b;
    double sum[3] = {0, 0, 0};
    auto each = [&](const std::vector<R3DVertex>& list, size_t first, size_t count, const uint32_t r[2], auto&& fn) {
        size_t from = std::min<size_t>(r[0], count), to = std::min<size_t>(r[1], count);
        for (size_t i = first + from; i < first + to && i < list.size(); i++) fn(list[i]);
    };
    auto all = [&](auto&& fn) {
        each(scene.vertices, v.opaque_first, v.opaque_count, in.opaque, fn);
        each(scene.vertices, v.trans_first, v.trans_count, in.trans, fn);
        each(scene.backs, v.back_first, v.back_count, in.back, fn);
    };
    all([&](const R3DVertex& x) {
        for (int c = 0; c < 3; c++) sum[c] += x.pos[c];
        b.vertices++;
    });
    if (!b.vertices) return b;
    for (int c = 0; c < 3; c++) b.centre[c] = float(sum[c] / b.vertices);
    all([&](const R3DVertex& x) {
        float d[3] = {x.pos[0] - b.centre[0], x.pos[1] - b.centre[1], x.pos[2] - b.centre[2]};
        b.radius = std::max(b.radius, std::sqrt(dot3(d, d)));
    });
    return b;
}
