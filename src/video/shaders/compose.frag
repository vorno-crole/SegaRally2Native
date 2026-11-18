#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// Final composition: background 2D layers, 3D image and foreground 2D layers,
// with black bars to preserve the Model 3 aspect ratio.
// The 2D layers are upscaled with xBR (Hyllian's xBR-lv2), which rebuilds
// smooth edges from the pixel art instead of blurring or blocking it; with the
// HD HUD off (or --pixel2d) as plain, even pixels (sharp_pixels).
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D scene3d;
layout(set = 0, binding = 1) uniform sampler2D layer2d_front;
layout(set = 0, binding = 2) uniform sampler2D layer2d_back;
layout(set = 0, binding = 3) uniform sampler2D overlays[256];   // 2D replacement pictures (pack2d.h)
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"
layout(set = 0, binding = 5) uniform sampler2D text_atlas;   // high-resolution text (text_hd.h)
layout(set = 0, binding = 6) uniform sampler2D text_cells;
layout(set = 0, binding = 8) uniform sampler2D bloom;   // blurred glow of luminous surfaces (bloom.frag)
layout(set = 0, binding = 7) uniform sampler2D scene3d_alpha;   // alpha of the 3D image, in red
layout(set = 0, binding = 9) uniform sampler2D sky_coords;   // backdrop replacements (sky_pack.h, tilegen.h)
layout(set = 0, binding = 12) uniform sampler2D diffusion;   // the picture behind the HUD, blurred (bloom.frag)
layout(set = 0, binding = 11) uniform sampler2D exposure_log;   // 1x1: average log brightness of the 3D image, eased
layout(set = 0, binding = 13) uniform sampler2D bloom_tight;   // the same glow, blurred only a little (halos)

layout(push_constant) uniform Compose {
    vec2 scale;       // image size relative to the window
    vec2 offset;      // top-left corner
    vec2 sharpen;     // x: sharpening of the 3D image (0 = off)
    vec2 window;      // window size in pixels
    vec2 layer2d;     // x offset and width of the 4:3 2D area inside the 3D view
    vec2 filter2d;    // x: 1 = xBR, 0 = plain pixels (sharp_pixels); y: window pixels per 2D pixel
    vec4 hud;         // x: shift of the race HUD's corners towards the screen edges (2D pixels, 0 = none; at most a 16:9 view's); y: 1 = HDR lighting;
                      // z: brightness offset of the foreground layers (fades), applied to the overlays;
                      // w: 1 = background layers continue beside the 4:3 area
    vec4 sky;         // y > 0: glow mode, for the bloom: only the bright part of the sky, times y;
                      // w: 2 = measure mode: average log brightness of the 3D image;
                      // 3 = scene mode: the picture behind the HUD (backdrop and 3D), for the diffusion;
                      // 4 = a black picture
    vec4 effects;     // x: bloom strength (0 = off); y: colour saturation (1 = original);
                      // z: gamma (1 = original, higher = brighter mid-tones)
    vec4 motion;      // x: interpolation of the back layers: the fraction of each row's scroll
                      // since the previous game frame (in sky_coords) to draw them back by;
                      // w: where the race HUD's bands split (R3DRenderer::compose_push)
} pc;

const ivec2 SIZE_2D = ivec2(496, 384);
const int BACK_MARGIN = 440;   // tilegen.h: the back layers extend beyond both sides
const ivec2 SIZE_BACK = ivec2(496 + 2 * BACK_MARGIN, 384);

// ---- xBR-lv2 ---------------------------------------------------------------

const vec4 Ao = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bo = vec4(1.0, 1.0, -1.0, -1.0);
const vec4 Co = vec4(1.5, 0.5, -0.5, 0.5);
const vec4 Ax = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 Bx = vec4(0.5, 2.0, -0.5, -2.0);
const vec4 Cx = vec4(1.0, 1.0, -0.5, 0.0);
const vec4 Ay = vec4(1.0, -1.0, -1.0, 1.0);
const vec4 By = vec4(2.0, 0.5, -2.0, -0.5);
const vec4 Cy = vec4(2.0, 0.0, -1.0, 0.5);
const vec4 Ci = vec4(0.25);
const float EQ_THRESHOLD = 15.0;
const float LV2_COEFFICIENT = 2.0;

// Edge-detection value of a texel: luma, with transparency far from any colour
float val(vec4 c) {
    return c.a < 0.5 ? -128.0 : dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)) * 255.0;
}
vec4 df(vec4 a, vec4 b) { return abs(a - b); }
bvec4 eq(vec4 a, vec4 b) { return lessThan(df(a, b), vec4(EQ_THRESHOLD)); }
bvec4 neq(vec4 a, vec4 b) { return not(eq(a, b)); }
bvec4 band(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
bvec4 bor(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
vec4 wdist(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {
    return df(a, b) + df(a, c) + df(d, e) + df(d, f) + 4.0 * df(g, h);
}
float cdist(vec4 a, vec4 b) { return dot(abs(a - b), vec4(1.0)); }

vec4 texel(sampler2D layer, ivec2 size, ivec2 p) { return texelFetch(layer, clamp(p, ivec2(0), size - 1), 0); }

vec4 xbr(sampler2D layer, vec2 q, ivec2 size) {
    vec2 pos = q * vec2(size);
    ivec2 e0 = ivec2(floor(pos));
    vec2 fp = pos - vec2(e0);
    //    A1 B1 C1
    // A0 A  B  C  C4
    // D0 D  E  F  F4
    // G0 G  H  I  I4
    //    G5 H5 I5
    vec4 A1 = texel(layer, size, e0 + ivec2(-1, -2)), B1 = texel(layer, size, e0 + ivec2(0, -2)), C1 = texel(layer, size, e0 + ivec2(1, -2));
    vec4 A0 = texel(layer, size, e0 + ivec2(-2, -1)), A = texel(layer, size, e0 + ivec2(-1, -1)), B = texel(layer, size, e0 + ivec2(0, -1));
    vec4 C = texel(layer, size, e0 + ivec2(1, -1)), C4 = texel(layer, size, e0 + ivec2(2, -1));
    vec4 D0 = texel(layer, size, e0 + ivec2(-2, 0)), D = texel(layer, size, e0 + ivec2(-1, 0)), E = texel(layer, size, e0);
    vec4 F = texel(layer, size, e0 + ivec2(1, 0)), F4 = texel(layer, size, e0 + ivec2(2, 0));
    vec4 G0 = texel(layer, size, e0 + ivec2(-2, 1)), G = texel(layer, size, e0 + ivec2(-1, 1)), H = texel(layer, size, e0 + ivec2(0, 1));
    vec4 I = texel(layer, size, e0 + ivec2(1, 1)), I4 = texel(layer, size, e0 + ivec2(2, 1));
    vec4 G5 = texel(layer, size, e0 + ivec2(-1, 2)), H5 = texel(layer, size, e0 + ivec2(0, 2)), I5 = texel(layer, size, e0 + ivec2(1, 2));

    // The four corner rotations are evaluated at once in the vector lanes
    vec4 b = vec4(val(B), val(D), val(H), val(F));
    vec4 c = vec4(val(C), val(A), val(G), val(I));
    vec4 e = vec4(val(E));
    vec4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;
    vec4 i4 = vec4(val(I4), val(C1), val(A0), val(G5));
    vec4 i5 = vec4(val(I5), val(C4), val(A1), val(G0));
    vec4 h5 = vec4(val(H5), val(F4), val(B1), val(D0));
    vec4 f4 = h5.yzwx;

    // Straight-line inequations for 45, 30 and 60 degree edges
    vec4 fx = Ao * fp.y + Bo * fp.x;
    vec4 fx_left = Ax * fp.y + Bx * fp.x;
    vec4 fx_up = Ay * fp.y + By * fp.x;

    bvec4 restriction_lv1 = band(neq(e, f), neq(e, h));
    bvec4 lv2_extra = bor(bor(band(neq(f, b), neq(f, c)), band(neq(h, d), neq(h, g))),
                          bor(band(eq(e, i), bor(band(neq(f, f4), neq(f, i4)), band(neq(h, h5), neq(h, i5)))),
                              bor(eq(e, g), eq(e, c))));
    restriction_lv1 = band(restriction_lv1, lv2_extra);
    bvec4 restriction_left = band(neq(e, g), neq(d, g));
    bvec4 restriction_up = band(neq(e, c), neq(b, c));

    // Edge smoothing about one window pixel wide
    vec4 delta = vec4(1.0 / max(pc.filter2d.y, 1.0));
    vec4 deltaL = vec4(0.5, 1.0, 0.5, 1.0) * delta;
    vec4 deltaU = deltaL.yxwz;
    vec4 fx45i = clamp((fx + delta - Co - Ci) / (2.0 * delta), 0.0, 1.0);
    vec4 fx45 = clamp((fx + delta - Co) / (2.0 * delta), 0.0, 1.0);
    vec4 fx30 = clamp((fx_left + deltaL - Cx) / (2.0 * deltaL), 0.0, 1.0);
    vec4 fx60 = clamp((fx_up + deltaU - Cy) / (2.0 * deltaU), 0.0, 1.0);

    vec4 wd1 = wdist(e, c, g, i, h5, f4, h, f);
    vec4 wd2 = wdist(h, d, i5, f, i4, b, e, i);
    bvec4 edri = band(lessThanEqual(wd1, wd2), restriction_lv1);
    bvec4 edr = band(lessThan(wd1, wd2), restriction_lv1);
    bvec4 edr_left = band(band(lessThanEqual(LV2_COEFFICIENT * df(f, g), df(h, c)), restriction_left), edr);
    bvec4 edr_up = band(band(greaterThanEqual(df(f, g), LV2_COEFFICIENT * df(h, c)), restriction_up), edr);

    fx45 = vec4(edr) * fx45;
    fx30 = vec4(edr_left) * fx30;
    fx60 = vec4(edr_up) * fx60;
    fx45i = vec4(edri) * fx45i;

    bvec4 px = lessThanEqual(df(e, f), df(e, h));
    vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    vec4 res1 = E;
    res1 = mix(res1, px.x ? F : H, maximos.x);
    res1 = mix(res1, px.z ? D : B, maximos.z);
    vec4 res2 = E;
    res2 = mix(res2, px.y ? B : F, maximos.y);
    res2 = mix(res2, px.w ? H : D, maximos.w);
    return cdist(E, res2) >= cdist(E, res1) ? res2 : res1;
}

// Plain pixels, all the same size: each 2D pixel a solid block, blended with
// its neighbour over one window pixel at its edges. Plain nearest sampling at a
// scale that is not whole (3.75 at 1440p) gives rows and columns of uneven
// width, which makes the letters wobble
vec4 sharp_pixels(sampler2D layer, vec2 q, ivec2 size) {
    vec2 pos = q * vec2(size) - 0.5;
    vec2 i = floor(pos);
    vec2 f = clamp((pos - i - 0.5) * max(pc.filter2d.y, 1.0) + 0.5, 0.0, 1.0);
    ivec2 p = ivec2(i);
    vec4 top = mix(texel(layer, size, p), texel(layer, size, p + ivec2(1, 0)), f.x);
    vec4 bottom = mix(texel(layer, size, p + ivec2(0, 1)), texel(layer, size, p + ivec2(1, 1)), f.x);
    return mix(top, bottom, f.y);   // premultiplied: blends cleanly into transparency
}

vec4 layer(sampler2D tex, vec2 q, ivec2 size) {
    return pc.filter2d.x > 0.5 ? xbr(tex, q, size) : sharp_pixels(tex, q, size);
}

// ---- Backdrop replacements ---------------------------------------------------
// The back layer pixels that show a replaced backdrop say which picture and
// which point of its 512x512 layer they show; the picture is sampled in
// between at full resolution, following the game's scroll.

const uint SKY_FIRST_SLOT = 248u;       // SkyPack::FIRST_SLOT

// Which replacement picture and point each back layer pixel shows: sampled
// in between. dx, dy: screen derivatives of qb * SIZE_BACK / 512 (explicit,
// as the position wraps every 512 pixels)
void sky_layer(vec2 qb, vec2 dx, vec2 dy, inout vec4 back) {
    vec2 pos = qb * vec2(SIZE_BACK);
    ivec2 t = clamp(ivec2(floor(pos)), ivec2(0), SIZE_BACK - 1);
    vec4 v = texelFetch(sky_coords, t, 0);
    if (v.a < 0.5) return;
    uvec4 b = uvec4(round(v * 255.0));
    vec2 at = vec2(b.r | ((b.b & 1u) << 8), b.g | (((b.b >> 1) & 1u) << 8)) + clamp(pos - vec2(t), 0.0, 1.0);
    uint slot = SKY_FIRST_SLOT + ((b.b >> 2) & 7u);
    vec4 s = textureGrad(overlays[nonuniformEXT(slot)], fract(at / 512.0), dx, dy);
    // Fade offset of the layer pair, in the last row
    vec3 offset = (texelFetch(sky_coords, ivec2((b.b >> 5) & 1u, SIZE_BACK.y), 0).rgb * 255.0 - 128.0) * (2.0 / 255.0);
    back = vec4(clamp(s.rgb + offset, 0.0, 1.0) * s.a, s.a);
}

// ---- High-resolution text ----------------------------------------------------
// Cells that hold known letters were left out of the foreground layer; the
// letter is drawn here from the vector-font atlas with the game's colours.

const int TEXT_CELL_TEXELS = 4;
const int OVERLAY_ROW = 48;          // after the cells: the replacement pictures on screen

// The 2D replacement pictures (pack2d.h) of the foreground or of the layers
// below the 3D image over `under` (premultiplied), at `pos` in 2D pixels.
// Listed in the last row of the text cells (text_hd.h): rectangle in cells,
// then slot in R and 1 in G for the layers below the 3D image.
vec4 replacement_pictures(vec4 under, vec2 pos, bool below_3d) {
    int pictures = int(texelFetch(text_cells, ivec2(0, OVERLAY_ROW), 0).r * 255.0 + 0.5);
    for (int i = 0; i < pictures; i++) {
        vec4 info = texelFetch(text_cells, ivec2(2 + 2 * i, OVERLAY_ROW), 0);
        if ((info.g > 0.5) != below_3d) continue;
        if (info.b * 255.0 > 0.5) {
            // a lit state (pack2d.h): only in the cells marked with its slot
            ivec2 cell = clamp(ivec2(pos / 8.0), ivec2(0), SIZE_2D / 8 - 1);
            vec4 mark = texelFetch(text_cells, ivec2(cell.x * TEXT_CELL_TEXELS + 1, cell.y), 0);
            if (mark.a < 0.5 || abs(mark.r - info.r) > 0.5 / 255.0) continue;
        }
        vec4 rect = round(texelFetch(text_cells, ivec2(1 + 2 * i, OVERLAY_ROW), 0) * 255.0) * 8.0;
        vec2 r = (pos - rect.xy) / (rect.zw - rect.xy);
        if (all(greaterThanEqual(r, vec2(0.0))) && all(lessThan(r, vec2(1.0)))) {
            vec4 o = texture(overlays[nonuniformEXT(uint(info.r * 255.0 + 0.5))], r);
            under = under * (1.0 - o.a) + vec4(clamp(o.rgb + pc.hud.z, 0.0, 1.0) * o.a, o.a);
        }
    }
    return under;
}
const int ATLAS_COLS = 16;           // tools/make_text_atlas.py
const float ATLAS_SLOT = 80.0;       // (8 + 2 * PAD) * SCALE
const float ATLAS_PAD = 8.0;         // PAD * SCALE
const float ATLAS_CELL = 64.0;       // 8 * SCALE

vec4 front_layer(vec2 q) {
    vec4 f = layer(layer2d_front, q, SIZE_2D);
    vec2 pos = q * vec2(SIZE_2D) / 8.0;
    ivec2 cell = clamp(ivec2(pos), ivec2(0), SIZE_2D / 8 - 1);
    ivec2 t = ivec2(cell.x * TEXT_CELL_TEXELS, cell.y);
    vec4 info = texelFetch(text_cells, t, 0);
    if (info.a < 0.5) return f;
    int slot = int(info.r * 255.0 + 0.5) + 256 * int(info.g * 255.0 + 0.5);
    vec2 local = clamp(pos - vec2(cell), 0.0, 1.0);
    vec2 corner = vec2(slot % ATLAS_COLS, slot / ATLAS_COLS) * ATLAS_SLOT + ATLAS_PAD;
    // Explicit mip level: screen derivatives jump at the cell edges, where the
    // atlas coordinates change slot
    float lod = max(log2(ATLAS_CELL / 8.0 / max(pc.filter2d.y, 1e-3)), 0.0);
    vec4 g = textureLod(text_atlas, (corner + local * ATLAS_CELL) / vec2(textureSize(text_atlas, 0)), lod);
    vec4 top = texelFetch(text_cells, t + ivec2(1, 0), 0);
    vec4 bottom = texelFetch(text_cells, t + ivec2(2, 0), 0);
    vec4 edge = texelFetch(text_cells, t + ivec2(3, 0), 0);
    float ea = g.g * edge.a, fa = g.r * top.a;
    f = f * (1.0 - ea) + vec4(edge.rgb, 1.0) * ea;   // premultiplied, like the layer
    f = f * (1.0 - fa) + vec4(mix(top.rgb, bottom.rgb, g.b), 1.0) * fa;
    return f;
}

// HDR lighting: below the knee the image is left as the original drew it;
// above it the highlights roll off smoothly towards white instead of clipping
vec3 tone_curve(vec3 x) {
    // (0.8 until the comparison with the arcade: its white paint and sunlit
    // whites came out light grey, 0.93 at most; at 0.9, 0.96)
    float knee = atm.soft.w > 0.0 ? atm.soft.w : 0.8;
    vec3 over = max(x - knee, 0.0);
    return min(x, vec3(knee)) + (1.0 - knee) * (1.0 - exp(-over / (1.0 - knee)));
}

// How much the eased average brightness opens or closes the exposure: gently,
// within a narrow range, so dark scenes stay dark and bright ones bright
float exposure() {
    // Menus and selection screens (no game world): as the original drew them
    if (atm.view.x == 0.0) return 1.0;
    float average = exp(texelFetch(exposure_log, ivec2(0), 0).r);
    // (a bright stage used to be closed down to 0.9: in the sunlit desert,
    // already brighter with SUN, everything came out darker than the arcade)
    float least = atm.sunlight.w > 0.0 ? atm.sunlight.w : 0.9;
    return clamp(pow(0.17 / max(average, 1e-3), 0.35), least, max(least, 1.15));
}

void main() {
    if (pc.sky.w > 3.5) {   // nothing to show (R3DRenderer::sr2_between_screens)
        color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (pc.sky.w > 1.5 && pc.sky.w < 2.5) {   // measure mode (one pixel): log-average brightness of the 3D image
        float sum = 0.0, n = 0.0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                vec2 q = (vec2(x, y) + 0.5) / 16.0;
                vec4 s = texture(scene3d, q);
                if (texture(scene3d_alpha, q).r < 0.5) continue;
                sum += log(max(dot(s.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-3));
                n += 1.0;
            }
        color = vec4(n > 0.0 ? sum / n : log(0.17));
        return;
    }
    vec2 p = (uv - pc.offset) / pc.scale;
    vec3 c = vec3(0.0);
    if (p.x >= 0.0 && p.y >= 0.0 && p.x <= 1.0 && p.y <= 1.0) {
        vec4 s = texture(scene3d, p);
        // With DLAA the alpha comes from its own anti-aliasing pass, steady
        // from frame to frame like the colour
        s.a = texture(scene3d_alpha, p).r;
        // Sharpening (SHARPEN), contrast adaptive like AMD's CAS: detail comes
        // back after the anti-aliasing and the diffusion, flat areas and
        // strong edges are left alone (no halos)
        if (pc.sharpen.x > 0.0 && s.a > 0.0) {
            vec2 t = 1.0 / vec2(textureSize(scene3d, 0));
            vec3 a = texture(scene3d, p - vec2(0.0, t.y)).rgb, b = texture(scene3d, p - vec2(t.x, 0.0)).rgb;
            vec3 d = texture(scene3d, p + vec2(t.x, 0.0)).rgb, e = texture(scene3d, p + vec2(0.0, t.y)).rgb;
            vec3 lo = min(s.rgb, min(min(a, b), min(d, e))), hi = max(s.rgb, max(max(a, b), max(d, e)));
            vec3 amp = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, vec3(1e-4)), 0.0, 1.0));
            vec3 w = -amp * mix(0.125, 0.2, pc.sharpen.x);
            s.rgb = max((a + b + d + e) * w + s.rgb, 0.0) / (1.0 + 4.0 * w);
        }
        if (pc.hud.y > 0.5 && s.a > 0.0) {   // HDR lighting: exposure and tone curve (premultiplied)
            vec3 lit = s.rgb / max(s.a, 1e-4) * exposure();
            s.rgb = tone_curve(lit) * s.a;
        }
        vec2 q = vec2((p.x - pc.layer2d.x) / pc.layer2d.y, p.y);
        // The back layers (sky) were drawn wider than the screen; beyond that
        // and for the front layers the outermost column is repeated
        vec2 qb = vec2((q.x * float(SIZE_2D.x) + float(BACK_MARGIN)) / float(SIZE_BACK.x), q.y);
        // Between game frames the back layers (sky) go part of the way back to
        // where they were, like the 3D image in front of them
        if (pc.motion.x > 0.0) {
            vec4 m = texelFetch(sky_coords, ivec2(clamp(int(q.y * 384.0), 0, 383), SIZE_BACK.y + 1), 0);
            if (m.b > 0.5) qb -= (m.rg * 255.0 - 128.0) * pc.motion.x / vec2(SIZE_BACK);
        }
        qb.x = clamp(qb.x, 0.5 / float(SIZE_BACK.x), 1.0 - 0.5 / float(SIZE_BACK.x));
        q.x = clamp(q.x, 0.5 / 496.0, 1.0 - 0.5 / 496.0);
        bool inside_43 = p.x >= pc.layer2d.x && p.x <= pc.layer2d.x + pc.layer2d.y;
        vec2 sky_dx = dFdx(qb * vec2(SIZE_BACK)) / 512.0, sky_dy = dFdy(qb * vec2(SIZE_BACK)) / 512.0;
        vec4 back = vec4(0.0);
        if (pc.hud.w > 0.5 || inside_43) {
            back = layer(layer2d_back, qb, SIZE_BACK);
            sky_layer(qb, sky_dx, sky_dy, back);
        }
        // 2D replacement pictures of the layers below the 3D image (title and
        // tutorial screens), inside the 4:3 area
        if (inside_43) back = replacement_pictures(back, q * vec2(SIZE_2D), true);
        vec4 front;
        float shift = pc.hud.x;
        float row = q.y * 384.0;
        // Where the foreground's replacement pictures are looked up (2D pixels):
        // moved with the HUD like the foreground itself (-1: none there)
        vec2 pic_pos = vec2((p.x - pc.layer2d.x) / pc.layer2d.y * 496.0, p.y * 384.0);
        if (shift > 0.0) {
            // The race HUD to the corners: on top the times (left)
            // and the position / stage top 3 (right), the time left and the
            // co-driver's arrow staying in the middle; below the rev counter
            // and the speed (left) and the car / driver (right). The splits
            // are empty columns found in each band (app.cpp, sr2_hud_splits)
            uint split = uint(pc.motion.w);
            float x = (p.x - pc.layer2d.x) / pc.layer2d.y * 496.0;
            float xl = x + shift, xr = x - shift;
            // (the top left goes down to row 200: practice's list of laps)
            bool bottom = row >= 264.0;
            float to_left = row < 200.0 ? float(split & 0xFFu) * 2.0 : bottom ? float(split >> 16 & 0xFFu) * 2.0 : 0.0;
            float to_right = row < 128.0 ? float(split >> 8 & 0xFFu) * 2.0 : bottom ? to_left : 0.0;
            if (to_right <= 0.0) to_right = 496.0;
            if (to_left <= 0.0 && to_right >= 496.0) {
                front = front_layer(q);
            } else if (xl >= 0.0 && xl < to_left) {
                front = front_layer(vec2(max(xl, 0.5) / 496.0, q.y));
                pic_pos = vec2(xl, row);
            } else if (xr >= to_right && xr < 496.0) {
                front = front_layer(vec2(min(xr, 495.5) / 496.0, q.y));
                pic_pos = vec2(xr, row);
            } else if (x >= to_left && x < to_right) {
                front = front_layer(q);
            } else {
                front = vec4(0.0);
                pic_pos = vec2(-1.0);
            }
        } else {
            front = front_layer(q);
        }
        // The original's scroll fog veils the 2D layers behind the 3D image
        vec2 ndc_view = p * 2.0 - 1.0;
        vec3 dir_view = vec3((ndc_view.x + atm.view.z) / atm.view.x, (ndc_view.y + atm.view.w) / atm.view.y, -1.0);
        back.rgb = scroll_fog(back.rgb, p, atm.view.x != 0.0 && atm.view.y != 0.0 ? mist_light(dir_view)
                                                                               : vec3(1.0));
        // The sky behind a game world gets the same exposure as the 3D image
        // in front of it, or fully fogged scenery (the same fog colour as the
        // fogged sky) would stand out as a slightly darker silhouette
        if (pc.hud.y > 0.5 && atm.view.x != 0.0 && back.a > 0.0)
            back.rgb = tone_curve(back.rgb / back.a * exposure()) * back.a;
        // Glow mode (r3d_renderer.cpp, bloom): the sky's bright part where the 3D leaves it visible
        if (pc.sky.y > 0.0) {
            // A sky hidden by the scroll fog is just fog: it glows no more than
            // the fogged scenery in front of it
            vec3 b = back.rgb * (1.0 - s.a) * (1.0 - scroll_fog_amount(p));
            float peak = max(b.r, max(b.g, b.b));
            // Mostly around the sun: a bright sky all over (Hoth) glowing
            // everywhere only washed the picture out in white
            float near_sun = 0.2;
            if (atm.view.x != 0.0 && atm.view.y != 0.0) {
                vec2 ndc = p * 2.0 - 1.0;
                vec3 dir = normalize(vec3((ndc.x + atm.view.z) / atm.view.x, (ndc.y + atm.view.w) / atm.view.y, -1.0));
                near_sun = mix(0.2, 1.0, pow(max(dot(dir, atm.sun.xyz), 0.0), 8.0));
            }
            color = vec4(b * smoothstep(0.55, 1.0, peak) * near_sun * pc.sky.y, 0.0);
            return;
        }
        // Transparent 2D texels are all zero, so filtered 2D colours are
        // premultiplied by their alpha
        c = back.rgb;
        // The 3D image is premultiplied too: it starts cleared to zero, MSAA
        // averages partly covered edges with that zero, and the translucent
        // pass blends over it
        c = c * (1.0 - s.a) + s.rgb;
        if (pc.sky.w > 2.5) {   // scene mode
            color = vec4(c, 0.0);
            return;
        }
        // Diffusion (part of BLOOM): the picture mixed with a blurred copy of
        // itself, light spilling into the dark around it more than the other
        // way round, like a soft-focus filter: the hard contrasts soften while
        // the overall brightness stays where it was
        // Neither on the menus and selection screens (no game world): their
        // bright art and white cards glowed into a burnt-looking picture
        float bloom_strength = atm.view.x != 0.0 ? pc.effects.x : 0.0;
        if (bloom_strength > 0.0) {
            vec3 soft = texture(diffusion, p).rgb;
            float k = 0.3 * bloom_strength * (0.45 + 0.55 * dot(soft, vec3(0.2126, 0.7152, 0.0722)));
            c = mix(c, soft, clamp(k, 0.0, 0.5));
        }
        // Glow of fire, explosions and lights over the 3D image and the backdrop
        // (less the part every pixel of a bright scene shares: a sunlit
        // snowfield glows all over, which only veiled the picture in white)
        if (bloom_strength > 0.0) {
            // and less what is already bright around it: glow should spill
            // into the dark, a lit explosion over sunlit snow needs none
            float average = exp(texelFetch(exposure_log, ivec2(0), 0).r);
            // In a dark scene (space) the glow of engines, lasers and
            // explosions is the light that matters: up to twice as strong,
            // and only a bright scene loses the part its surroundings share
            float dark = 0.35 * (1.0 - smoothstep(0.04, 0.25, average));
            vec3 around = texture(diffusion, p).rgb * (1.0 - dark);
            vec3 cut = vec3(0.5 * average) + 0.7 * around;
            vec3 wide = max(texture(bloom, p).rgb - cut, vec3(0.0));
            // ... and a tight halo hugging lasers, engines and explosions,
            // the coloured glow of the films, strongest in the dark of space
            vec3 tight = max(texture(bloom_tight, p).rgb - cut, vec3(0.0));
            c += (wide * 0.5 * (1.0 + dark) + tight * (0.3 + 1.7 * dark)) * bloom_strength;
        }
        // Sega Rally 2's stages: a little more contrast (a gentle S-curve about
        // the mid-tones). The haze, the diffusion and the lifted shadows of
        // the added lighting each take a little of it, and beside the
        // arcade's hard, clean picture the stages looked flat. Under the HUD;
        // not the menus and selection screens (no game world)
        if (atm.view.x != 0.0) {
            vec3 s_curve = clamp(c, 0.0, 1.0);
            s_curve = s_curve * s_curve * (3.0 - 2.0 * s_curve);
            c = mix(c, s_curve, 0.22);
        }
        // 2D replacement pictures of the foreground: above the 3D image, below
        // what changed in the 2D layer
        vec4 pictures = replacement_pictures(vec4(c, 1.0), pic_pos, false);
        c = pictures.rgb;
        c = c * (1.0 - front.a) + front.rgb;
    }
    // Picture settings from the menu: saturation, then gamma
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(luma), c, pc.effects.y), 0.0, 1.0);
    c = pow(c, vec3(1.0 / pc.effects.z));
    color = vec4(c, 1.0);
}
