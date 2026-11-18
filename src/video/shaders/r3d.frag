#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_TRACING
#extension GL_EXT_ray_query : require
#endif
// Real3D polygon shading: texture decode from the raw 16-bit texture RAM,
// sun + ambient lighting, fixed shading, specular and fog
// (docs/real3d_spec.md sections 6 and 7). Textures are filtered with the
// mip levels the game uploads (trilinear) plus anisotropic taps.
layout(location = 0) in vec3 view_pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 4) flat in uint tex;
layout(location = 5) flat in uint flags;
layout(location = 6) in vec3 params;
layout(location = 7) flat in uint replace;
layout(location = 8) in vec4 clip_now;
layout(location = 9) in vec4 clip_before;
layout(location = 10) in vec4 dust_light;   // DUST: the light bounced onto it (rgb) and how much of the sun reaches it (a), r3d.vert

layout(set = 0, binding = 0) uniform usampler2D texture_ram;   // 2048x2048 R16_UINT
layout(set = 0, binding = 1) uniform sampler2D replacements[];   // texture pack, RGBA8 with mips

layout(push_constant) uniform Viewport {
    mat4 proj;
    vec4 sun;
    vec4 fog_color;
    vec4 params;
    vec4 quality;
} vp;

layout(set = 0, binding = 4) uniform sampler2D shadow_map;     // sun shadows (depth seen from the sun)
layout(set = 0, binding = 5) uniform sampler2D scene_depth;    // the opaque world's depth (soft particles)

#define ATMOSPHERE_BINDING 2
#include "atmosphere.glsl"

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_motion;   // DLAA only: pixels from here to the previous image
layout(location = 2) out vec4 out_glow;     // bloom: the bright part of luminous world surfaces

#include "r3d_texture.glsl"
#ifdef RAY_TRACING
#define RT_SET 0
#include "raytrace.glsl"
#endif

// Sun shadows (not in the original): how much of the sun reaches this point
// of the game world, 0..1, from the shadow map drawn from the sun this frame
bool sun_shadowed() {
    return atm.shadow.x > 0.0 && vp.sun.w > 0.0 && (flags & (WORLD | NO_SHADOW)) == WORLD;
}
#ifdef RAY_TRACING
// Whether they are traced here (traced_shadow) instead
// (rt.x 2: on the left half only, for comparing: LW_RTSPLIT)
bool sun_traced() {
    return sun_shadowed() && (atm.rt.x > 1.5 ? gl_FragCoord.x * atm.backdrop.x < 0.5 : atm.rt.x > 0.0);
}
#endif
float sun_shadow(vec3 n) {
    if (!sun_shadowed()) return 1.0;
    float nl = length(n);
    vec3 pos = view_pos + (nl > 0.0 ? n / nl : vec3(0.0)) * atm.shadow.z;
    vec4 lc = atm.shadow_matrix * vec4(pos, 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || lc.z <= 0.0) return 1.0;
    // 4x4 texels around the point, compared one by one and weighted by where
    // the point falls between them: a 3x3 box over bilinear comparisons (a
    // tent), so the edge slides smoothly from texel to texel. (An even 4x4
    // average was constant within each texel: the edges came out as stairs
    // the size of a texel, in bands of grey, without ray tracing.)
    vec2 size = vec2(1.0 / atm.shadow.y);
    vec2 st = uv * size - 0.5;
    vec2 b = floor(st), f = st - b;
    float wx[4] = float[4](1.0 - f.x, 1.0, 1.0, f.x);   // texels b-1 .. b+2
    float wy[4] = float[4](1.0 - f.y, 1.0, 1.0, f.y);
    float shade = 0.0;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++) {
            // The 2x2 texels from (b-1, b-1) + 2 * (x, y): gathered at their shared corner
            vec4 d = textureGather(shadow_map, (b + 2.0 * vec2(x, y)) / size, 0);
            vec4 hit = vec4(greaterThan(d, vec4(lc.z + atm.shadow.w)));
            // textureGather's order: x (0,1), y (1,1), z (1,0), w (0,0)
            shade += hit.w * wx[2 * x] * wy[2 * y] + hit.z * wx[2 * x + 1] * wy[2 * y] +
                     hit.x * wx[2 * x] * wy[2 * y + 1] + hit.y * wx[2 * x + 1] * wy[2 * y + 1];
        }
    float lit = 1.0 - shade / 9.0;
    // Fades out towards the edge of the shadow map
    vec2 edge = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    return mix(1.0, lit, atm.shadow.x * fade);
}

#ifdef RAY_TRACING
// Sun shadows traced through the world: a few rays spread over the sun's
// disc, so the shadow is sharp where the caster touches the ground and
// softens with distance from it. Most pixels are fully lit or fully in
// shadow: only the penumbra takes them all. It is found with four rays at
// the rim of the disc, a quarter turn apart: where they all agree the point
// is fully lit or fully in shadow. (Two rays from the middle of the disc, as
// at first, cut the penumbra short: ragged, grainy edges.) Each pixel traces
// two of the four, opposite each other, and its neighbours to the side and
// above or below the other two (sun_probe); main() compares them across each
// block of 2x2 pixels, so half the rays find the same penumbra
const float SUN_RANGE = 2000.0;   // how far a sun ray is followed (view units)
struct SunRays {
    vec3 origin, near, L, X, Y;
    float turn;
    bool apart;
};
SunRays sun_rays(vec3 n) {
    SunRays r;
    float nl = length(n);
    vec3 N = nl > 0.0 ? n / nl : vec3(0.0);
    r.L = normalize(vp.sun.xyz);
    // Off the surface by a little more as it gets farther (depth precision),
    // and more on surfaces the sun grazes (boards and walls side-on to it), whose
    // rays otherwise met the surface itself: dark specks
    float graze = 1.0 + 3.0 * (1.0 - clamp(abs(dot(N, r.L)), 0.0, 1.0));
    vec3 off = (N * 1.5 * graze + r.L * 0.5) * (0.02 + 0.0015 * length(view_pos));
    r.origin = view_pos + off;
    // That lift grows to half a metre and more on the road 100 m away under a
    // low sun, above the underside of a car standing there: the far cars cast
    // no shadow around their tyres, or none at all, and seemed to hover. Where
    // it is more than 15 cm the models that move (the cars, the people) are
    // traced apart, from 15 cm off the surface: nothing of the scenery is
    // among them to meet the ray at once. (Not for their own surfaces, which would)
    float reach = length(off);
    r.apart = reach > 0.15 && (flags & (ACTOR | CAR)) == 0u;
    r.near = view_pos + off * (0.15 / max(reach, 0.15));
    r.X = normalize(cross(abs(r.L.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), r.L));
    r.Y = cross(r.L, r.X);
    // The same spread for every pixel (turned each image only with DLAA,
    // which averages it): turned per pixel, the penumbra was a pattern of dots
    // fixed to the screen, which crawled over the scenery as the camera moved
    r.turn = atm.rt.w * 6.2831853;
    return r;
}
// How much of the sun's light comes from the point of its disc at the angle
// a, the share `radius` of the way out from its middle: 1, 0 behind something
// solid, in between through foliage
float sun_ray(SunRays r, float a, float radius) {
    vec3 dir = normalize(r.L + (r.X * cos(a) + r.Y * sin(a)) * radius);
    if (!r.apart) return rt_light(r.origin, dir, SUN_RANGE, RT_LOW | RT_ACTOR);
    float light = rt_light(r.origin, dir, SUN_RANGE, RT_LOW);
    return light > 0.0 ? light * rt_light(r.near, dir, min(SUN_RANGE, 60.0), RT_ACTOR) : 0.0;
}
// (A fractional part in rt.z: every pixel traces the four rim rays itself, as before, for comparing)
bool sun_probe_alone() { return fract(atm.rt.z) > 0.4; }
// (a smaller one: only the rays inside the disc are every pixel's own; without DLAA, see sun_disc)
bool sun_disc_alone() { return fract(atm.rt.z) > 0.1; }
float sun_pair() { return float((int(gl_FragCoord.x) + int(gl_FragCoord.y)) & 1); }
// The light of this pixel's two rim rays: x their sum (0 to 2), y how far apart they are
vec2 sun_probe(vec3 n) {
    SunRays r = sun_rays(n);
    float a = r.turn + sun_pair() * 1.5707963;
    float one = sun_ray(r, a, atm.rt.y), other = sun_ray(r, a + 3.1415927, atm.rt.y);
    if (!sun_probe_alone()) return vec2(one + other, abs(one - other));
    float third = sun_ray(r, a + 1.5707963, atm.rt.y), fourth = sun_ray(r, a + 4.7123890, atm.rt.y);
    return vec2(0.5 * (one + other + third + fourth),
                max(max(one, other), max(third, fourth)) - min(min(one, other), min(third, fourth)));
}
// +1: this pixel's neighbour in its block of 2x2 is the next pixel, -1: the one before
vec2 sun_first() { return vec2(1.0) - 2.0 * vec2(ivec2(gl_FragCoord.xy) & 1); }
// The rays over the inside of the disc (a golden-angle spiral), in the
// penumbra only. With DLAA they too are shared across each block of 2x2
// pixels: every pixel traces a quarter of them, a different one each, and
// main() adds the quarters of the pixel beside it and of the one above or
// below (the dappled shade of a row of trees is all penumbra: every pixel
// tracing them all, the start of the mountain took half as long again to
// draw as the rest of it). Each pixel of the block then has three quarters,
// not the same three: DLAA, which turns the rays every image, evens them
// out. Without it every pixel traces them all (shared, the penumbra was a
// pattern of dots). (All four quarters for the whole block, through the
// pair's sum: its shadows' sharp edges came out in steps of two pixels)
// probe: sun_probe here; across: its sum's change to the pixel beside and to
// the one above or below in the block of 2x2
// x: the light of this pixel's quarter, y: how many rays that is (where the
// rim rays agree: their mean, as that many), z: 1 in the penumbra, w: the
// light of the four rim rays
vec4 sun_disc(vec3 n, vec2 probe, vec2 across) {
    int quarter = max((int(atm.rt.z) - 4) / 4, 0);
    // The four rim rays (two here, two each in the neighbours) all but agree:
    // lit, in shadow, or in the even half-light under foliage; their mean
    if (sun_probe_alone()) across = vec2(0.0);
    if (max(probe.y, max(abs(across.x), abs(across.y))) <= 0.12) {
        float mean = (3.0 * probe.x + dot(sun_first(), across)) / 6.0;
        return vec4(mean * float(quarter), float(quarter), 0.0, mean * 4.0);
    }
    // The penumbra: the other two rim rays, then its quarter of the disc
    SunRays r = sun_rays(n);
    float rim = probe.x;
    if (sun_probe_alone()) {
        rim *= 2.0;
    } else {
        float a = r.turn + (1.0 - sun_pair()) * 1.5707963;
        rim += sun_ray(r, a, atm.rt.y) + sun_ray(r, a + 3.1415927, atm.rt.y);
    }
    bool alone = sun_disc_alone();
    ivec2 at = ivec2(gl_FragCoord.xy) & 1;
    int from = alone ? 0 : at.x + 2 * at.y, stride = alone ? 1 : 4, count = quarter * 4;
    float lit = 0.0;
    for (int i = from; i < count; i += stride)
        lit += sun_ray(r, float(i) * 2.3999632 + r.turn, sqrt((float(i) + 0.5) / float(count)) * atm.rt.y);
    return vec4(alone ? lit * 0.25 : lit, float(quarter), 1.0, rim);
}
// disc: sun_disc here; block: the light and the count of the disc's rays of
// this pixel and its two neighbours in the block of 2x2 (a pixel the sun's
// rays were not traced for has none)
float traced_shadow(vec4 disc, vec2 block) {
    int rays = int(atm.rt.z);
    if (disc.z == 0.0 || rays < 8) return mix(1.0, disc.w * 0.25, atm.shadow.x);
    vec2 all = sun_disc_alone() ? disc.xy : block;
    // (the rim's four rays count as four of them all, as when one pixel traced them)
    return mix(1.0, (disc.w + all.x / max(all.y, 1.0) * float(rays - 4)) / float(rays), atm.shadow.x);
}
#endif

vec4 fetch(ivec2 base, ivec2 size, ivec2 c, uint fmt) {
    c.x = wrap(c.x, size.x, (flags & MIRROR_U) != 0u);
    c.y = wrap(c.y, size.y, (flags & MIRROR_V) != 0u);
    ivec2 p = ivec2((base.x + c.x) & 2047, (base.y & 1024) | ((base.y + c.y) & 1023));
    vec4 t = decode(texelFetch(texture_ram, p, 0).r, fmt);
    // Polygons without texture alpha show every texel, including the ones
    // marked transparent: their colour must survive the premultiplied filter
    if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
    return t;
}

// Mip level positions inside a texture page (spec 6.4)
const int MIP_X[12] = int[](0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047);
const int MIP_Y[11] = int[](0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023);

// Bilinear sample of one mip level; st in level-0 texels. Premultiplied
// result: the colour of transparent texels (often sky blue in the texture
// data) must not bleed into the edges of foliage.
vec4 bilinear(ivec2 base, ivec2 size, vec2 st, int level, uint fmt) {
    ivec2 b = ivec2(MIP_X[level] + (base.x >> level), (base.y & 1024) | (MIP_Y[level] + ((base.y & 1023) >> level)));
    ivec2 s = max(size >> level, ivec2(1));
    vec2 p = st / float(1 << level) - 0.5;
    ivec2 i0 = ivec2(floor(p));
    vec2 f = p - vec2(i0);
    ivec2 i1 = i0 + 1;
    // Axes without wrap smoothing (and all of an alpha-tested texture) repeat
    // without blending across the edge: both taps stay in the repetition the
    // sample falls in (spec 6.6)
    bvec2 smooth_edge = bvec2((flags & SMOOTH_U) != 0u, (flags & SMOOTH_V) != 0u);
    if ((flags & ALPHA_TEST) != 0u) smooth_edge = bvec2(false);
    ivec2 first = ivec2(floor((p + 0.5) / vec2(s))) * s;
    for (int k = 0; k < 2; k++)
        if (!smooth_edge[k]) {
            i0[k] = clamp(i0[k], first[k], first[k] + s[k] - 1);
            i1[k] = clamp(i1[k], first[k], first[k] + s[k] - 1);
        }
    vec4 a = fetch(b, s, i0, fmt);
    vec4 bb = fetch(b, s, ivec2(i1.x, i0.y), fmt);
    vec4 c = fetch(b, s, ivec2(i0.x, i1.y), fmt);
    vec4 d = fetch(b, s, i1, fmt);
    a.rgb *= a.a;
    bb.rgb *= bb.a;
    c.rgb *= c.a;
    d.rgb *= d.a;
    return mix(mix(a, bb, f.x), mix(c, d, f.x), f.y);
}

// duv_dx/duv_dy: screen-space derivatives of uv, taken in uniform control flow
vec4 sample_texture(vec2 duv_dx, vec2 duv_dy) {
    if (replace != 0u) {   // texture pack image: hardware trilinear + anisotropic filtering
        vec2 u = uv;
        if ((flags & MIRROR_U) != 0u) { float m = mod(u.x, 2.0); u.x = m > 1.0 ? 2.0 - m : m; }
        if ((flags & MIRROR_V) != 0u) { float m = mod(u.y, 2.0); u.y = m > 1.0 ? 2.0 - m : m; }
        vec4 c = textureGrad(replacements[nonuniformEXT(replace & 0xFFFFu)], u, duv_dx, duv_dy);
        uint dirt = replace >> 16;   // the game's dirt over it (texture_pack.h): colour and amount
        if (dirt != 0u) {
            vec4 d = textureGrad(replacements[nonuniformEXT(dirt)], u, duv_dx, duv_dy);
            c.rgb = mix(c.rgb, d.rgb, d.a);
        }
        return c;
    }
    ivec2 base = ivec2(int(tex & 2047u), int((tex >> 11) & 2047u));
    ivec2 size = ivec2(32 << int((tex >> 22) & 7u), 32 << int((tex >> 25) & 7u));
    uint fmt = (tex >> 28) & 15u;
    vec2 st = uv * vec2(size);
    vec4 sum;
    float max_aniso = vp.quality.y;
    if (max_aniso < 1.0) {
        sum = bilinear(base, size, st, 0, fmt);   // original look: base level only
    } else {
        // Footprint of the pixel in level-0 texels: the long axis gets several
        // taps, the short one picks the mip level
        vec2 dx = duv_dx * vec2(size), dy = duv_dy * vec2(size);
        float lx = length(dx), ly = length(dy);
        float pmax = max(lx, ly), pmin = max(min(lx, ly), 1e-6);
        float taps = clamp(ceil(pmax / pmin), 1.0, max_aniso);
        // Levels are stored while both sides are >= 4 before halving
        int max_level = max(findMSB(min(size.x, size.y)) - 1, 0);
        float lod = clamp(log2(max(pmax / taps, 1e-6)), 0.0, float(max_level));
        int l0 = int(floor(lod));
        int l1 = min(l0 + 1, max_level);
        float lf = lod - float(l0);
        vec2 axis = lx > ly ? dx : dy;
        sum = vec4(0.0);
        int n = int(taps);
        for (int i = 0; i < n; i++) {
            vec2 p = st + axis * ((float(i) + 0.5) / taps - 0.5);
            vec4 s = bilinear(base, size, p, l0, fmt);
            if (lf > 0.0 && l1 != l0) s = mix(s, bilinear(base, size, p, l1, fmt), lf);
            sum += s;
        }
        sum /= taps;
    }
    sum.rgb = sum.a > 0.0 ? sum.rgb / sum.a : vec3(0.0);
    return sum;
}

// Global illumination (not in the original): the colour of the ambient
// light on a surface facing `n`. It is the light of the sky: blue from
// straight above, the pale colour of the horizon from the sides, and from
// below what the ground sends up, warmer. As bright as white (it changes
// the hue of the light, not how much there is): what the sun does not reach
// turns towards blue, as shadows do outdoors on a clear day, against the
// warm light of the sun
vec3 sky_ambient(vec3 n) {
    vec3 up = normalize(atm.axes[1].xyz);
    float h = length(n) > 0.0 ? dot(normalize(n), up) : 0.5;
    vec3 horizon = dot(atm.haze.rgb, vec3(1.0)) > 0.05 ? atm.haze.rgb : vec3(0.85, 0.9, 1.0);
    vec3 zenith = horizon * vec3(0.5, 0.72, 1.15);
    vec3 ground = horizon * vec3(1.0, 0.86, 0.66);
    vec3 c = h >= 0.0 ? mix(horizon, zenith, h) : mix(horizon, ground, -h);
    c /= max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-3);
    // A third of the way to that colour: in full, the ambient light being
    // much of the light even in the sun, the grey road was blue all over.
    // (The fogged and dark scenes keep the game's light)
    return mix(vec3(1.0), c, 0.35 * atm.sunlight.z);
}

// Spotlight of the original (spec 7.1): a lobe around a point of the view
// (spot_lobe), fading with distance past the extent. x: lobe on the light,
// y: lobe on the fog
vec2 spotlight() {
    float z = -view_pos.z;
    if (z <= 0.0 || atm.spot.z <= 0.0) return vec2(0.0);
    float ellipse = spot_lobe(view_pos.xy / z);
    float extent = atm.spot_fog.w, range = 0.0;
    if (extent > 0.0) {
        float d = min(0.0, atm.spot_color.w + extent - z);
        float k = d / (1.0 + extent) - 1.0;
        range = 1.0 / (k * k);
    }
    return vec2(range, z >= atm.spot_color.w ? 1.0 : 0.0) * ellipse;
}

// Sega Rally 2, DUST: ENHANCED (r3d_dust.cpp, not in the original): the
// particles that take the place of the game's flat fans of dust behind the
// wheels. tex: kind in bits 0-1, a seed in bits 8-23; params.x: age 0..1;
// params.z: a puff's radius, a clod's length over its width. The normal is
// the direction to the eye (they face it)
float dust_fbm(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; i++) {
        s += a * atm_noise(p);
        p = p * 2.07 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s / 0.875;
}

vec4 dust() {
    uint kind = tex & 3u;
    float seed = float((tex >> 8) & 0xFFFFu) / 65535.0;
    float t = params.x;
    vec3 eye = normalize(normal);
    vec3 up = normalize(atm.axes[1].xyz);
    vec3 sun_dir = length(vp.sun.xyz) > 0.01 ? normalize(vp.sun.xyz) : up;
    float sun = vp.sun.w, ambient = vp.params.x;
    if (atm.sunlight.z > 0.0) {   // clear outdoor scenes (SUN), as the world is lit
        sun *= atm.sunlight.x;
        ambient *= atm.sunlight.y;
    }
    vec2 q = uv;
    vec4 c = vec4(0.0);
    if (kind == 0u) {
        // A puff: a ragged ball of dust that thins out into wisps as it ages.
        // Its pattern is its own (seed), turning slowly and churning with age
        float r = length(q);
        if (r >= 1.0) return c;
        float turn = seed * 6.2831853 + (seed - 0.5) * t * 2.5;
        vec2 qr = mat2(cos(turn), sin(turn), -sin(turn), cos(turn)) * q;
        float n = dust_fbm(vec3(qr * 1.5, seed * 40.0 + t * 1.3));
        float n2 = dust_fbm(vec3(qr * 3.3 + 7.0, seed * 13.0 + t * 0.9));
        float body = (1.0 - smoothstep(0.35, 1.0, r + (n - 0.5) * 0.9)) * (1.0 - smoothstep(0.75, 1.0, r));
        float wisp = smoothstep(0.65 * t - 0.2, 0.65 * t + 0.3, n2 + (1.0 - r) * 0.35);
        // Lit as a ball would be, roughened by its billows; the light wraps
        // far round it (dust lets it through), and against the sun the whole
        // cloud glows
        vec3 rx = normalize(cross(up, eye) + vec3(1e-5, 0.0, 0.0)), ux = cross(eye, rx);
        vec2 lump = q + (vec2(n, n2) - 0.5) * 0.7;
        vec3 N = normalize(rx * lump.x + ux * lump.y + eye * sqrt(max(1.0 - r * r, 0.0)) * 0.8 + eye * 0.15);
        float wrap = 0.5 + 0.5 * dot(N, sun_dir);
        float through = pow(max(dot(normalize(view_pos), sun_dir), 0.0), 5.0);
        // (the sun, where it reaches the puff: under the trees and behind a
        // car the dust is in their shade, with what the ground throws back)
        // (and not all of it: the low cloud glared in full sun)
        vec3 light = vec3(ambient) + dust_light.rgb +
                     sun * ((tex & 8u) != 0u ? 1.0 : 0.6) * dust_light.a * (vec3(1.0, 0.97, 0.91) * (0.3 + 0.8 * wrap * wrap) + 0.45 * through);
        light *= mix(1.0, 0.86, body * body);   // darker in its thick middle
        c.rgb = color.rgb * light * mix(0.82, 1.1, n2) * (0.9 + 0.2 * fract(seed * 7.31));
        c.a = body * wisp * mix(0.7, 1.0, smoothstep(0.3, 0.7, n2)) * color.a * pow(1.0 - t, 1.3);
    } else {
        // A clod or a stone in flight: a short rounded streak, lit from above
        float aspect = max(params.z, 1.0);
        vec2 p = vec2(max(abs(q.x) * aspect - (aspect - 1.0), 0.0), q.y);
        float d = length(p);
        c.rgb = color.rgb * (ambient + dust_light.rgb + sun * dust_light.a * 0.85) * (1.0 - 0.35 * d * d);
        if ((tex & 4u) != 0u) {   // (bit 2: all but a puddle's drops)
            // A solid lump of earth: opaque, with a ragged edge of its own
            // (round its outline from its seed, and finer bumps), dark and
            // dull in the light, and it shrinks away at the end rather than
            // fading into a smudge
            c.rgb = color.rgb * (ambient + dust_light.rgb + sun * dust_light.a * 0.4) * (0.75 - 0.25 * d);
            if ((tex & 8u) != 0u)   // a clump of snow: bright, catching the sun
                c.rgb = color.rgb * (ambient + dust_light.rgb + sun * dust_light.a * 0.9) * (0.95 - 0.15 * d);
            vec2 dir = d > 1e-4 ? p / d : vec2(1.0, 0.0);
            float lump = atm_noise(vec3(dir * 1.6, seed * 57.0)) - 0.5;
            float bump = atm_noise(vec3(q * 4.0, seed * 31.0 + 5.0)) - 0.5;
            float edge = 0.72 - 0.3 * smoothstep(0.7, 1.0, t) + 0.45 * lump + 0.12 * bump;
            float aa = max(fwidth(d), 0.02);
            c.a = 1.0 - smoothstep(edge - aa, edge + aa, d);
        } else {
            c.a = (1.0 - smoothstep(0.55, 1.0, d)) * (1.0 - smoothstep(0.75, 1.0, t)) / mix(1.0, aspect, 0.5);
        }
    }
    return c;
}

void main() {
    // (quality.z: with DLSS upscaling, the size of the 3D image over that of
    // the picture it makes: the textures are filtered for the latter)
    float lod_scale = vp.quality.z > 0.0 ? vp.quality.z : 1.0;
    vec2 duv_dx = dFdx(uv) * lod_scale, duv_dy = dFdy(uv) * lod_scale;
    // Motion vector in pixels; NDC is linear across the screen, so its
    // derivatives give the viewport size without passing it in
    vec2 ndc = clip_now.xy / clip_now.w;
    vec2 px_per_ndc = 1.0 / vec2(dFdx(ndc.x), dFdy(ndc.y));
    out_motion = (clip_before.xy / clip_before.w - ndc) * px_per_ndc;
    gl_SampleMask[0] = -1;   // a statically written mask must be set on every path
    vec4 col = color;
    if ((flags & DUST) != 0u) {   // only in the translucent pass
        if (vp.quality.x <= -1000.0 || vp.quality.x > 0.0 || vp.fog_color.w < 0.0) discard;
        bool puff = (tex & 3u) == 0u;
        // Not in the eye's face: the cloud the camera drives through thins out
        // over the last few metres before it (it would wall the picture off)
        // (the player's own car's cloud, tex bit 4, a little closer: it is only
        // a few metres in front of the chase camera)
        float fade = puff ? ((tex & 16u) != 0u ? smoothstep(0.5, 2.5, -view_pos.z) : smoothstep(0.8, 4.0, -view_pos.z))
                          : smoothstep(0.8, 2.2, -view_pos.z);
        if (atm.soft.x > 0.0) {   // soft where it meets the road and what stands behind, over its size
            float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
            // (a puff's own soft edge is short: the cloud lies low over the road,
            // and over its whole size it faded into nothing against it)
            float soft = puff ? clamp(params.z * 0.2, 0.1, 0.35) : 0.05;
            if (d > 0.0) fade *= clamp((view_pos.z + 1e-3 / d) / soft, 0.0, 1.0);
        }
        // Where it will not show whatever its pattern is (in the eye's face, at
        // the road, its faded last moments), the pattern is not worked out:
        // a cloud about the camera is many puffs, each over much of the picture
        if (fade * (puff ? color.a * (1.0 - params.x) : 1.0) < 1.0 / 255.0) discard;
        vec4 c = dust();
        c.a *= fade;
        if (c.a < 1.0 / 255.0) discard;
        c.rgb = min(c.rgb, vec3(atm.soft.z > 0.5 ? 8.0 : 1.0));
        float fog = params.y * clamp(vp.params.z + (-view_pos.z) * vp.params.y, 0.0, 1.0);
        if (fog > 0.0) c.rgb = mix(c.rgb, vp.fog_color.rgb * vp.fog_color.a * mist_light(view_pos), fog);
        if (atm.haze.a > 0.0) c.rgb = distance_haze(c.rgb, view_pos, fog);
        out_color = c;
        out_glow = vec4(0.0, 0.0, 0.0, c.a);
        return;
    }
    if ((flags & TRANSLATOR) != 0u) col.rgb *= 16.0;

    // Sun shadow pass (fog_color.w < 0): only what stands near the ground casts
    // shadows; canopies and roofs high above would put whole scenes in the dark
    if (vp.fog_color.w < 0.0 && dot(view_pos, vp.sun.xyz) + vp.fog_color.x > vp.fog_color.y) discard;
    if (vp.fog_color.w < 0.0 && glow_sprite(flags)) discard;   // (a camera flash casts no shadow)

    vec4 t = vec4(1.0);
    if ((flags & TEXTURED) != 0u) {
        t = sample_texture(duv_dx, duv_dy);
        if ((flags & INVERT_RGB) != 0u) t.rgb = 1.0 - t.rgb;
        if ((flags & (TEX_ALPHA | ALPHA_TEST)) == 0u) t.a = 1.0;
        if ((flags & ALPHA_TEST) != 0u && t.a < 32.0 / 255.0) discard;
    }

    float ambient = vp.params.x;
    float fixed_shade = params.x;
    // Self-lit polygons as the game colours them (the menus, the selection
    // screens, the cars' painted shine and glass), brighter only with a
    // fixed shade
    if ((flags & LUMINOUS) != 0u && (flags & FIXED_SHADE) != 0u) col.rgb *= 1.0 + fixed_shade + ambient;

    vec4 c = t * col;
    if (c.a < 1.0 / 32.0) discard;
    // Depth of the nearest see-through surface (spec 7.2); effects stay out of
    // it. Not the game's black patch under each car (its shadow: untextured,
    // self-lit): it is made of several shapes laid over each other, which
    // blended one over another came out nearly black; only the nearest shows
    if (vp.quality.x <= -1000.0) {
        bool blob = (flags & (WORLD | LUMINOUS | TEXTURED)) == (WORLD | LUMINOUS) && max(col.r, max(col.g, col.b)) == 0.0;
        if ((flags & (TEX_ALPHA | ALPHA_TEST | LUMINOUS)) != 0u && !blob) discard;
        return;
    }
    // Texture alpha (spec 7.2): solid texels belong to the opaque pass, the
    // see-through ones to the translucent pass
    if ((flags & TEX_ALPHA) != 0u && (vp.quality.x > 0.0 ? c.a < 1.0 : c.a >= 1.0)) discard;

    vec3 n = normal;
    float s = 0.0;
    bool sunlit = false;      // the sun lights it: its shadows are looked for
    vec2 sun_count = vec2(-1.0, 0.0);   // traced shadows: this pixel's two rays (sun_probe), -1 where there are none
    if ((flags & LUMINOUS) == 0u) {
        s = (flags & FIXED_SHADE) != 0u ? fixed_shade : dot(vp.sun.xyz, n);
        s = clamp(s, -1.0, 1.0);
        s = max(s, 0.0);
        sunlit = (flags & FIXED_SHADE) == 0u && s > 0.0;
#ifdef RAY_TRACING
        if (sunlit && sun_traced()) sun_count = sun_probe(n);
#endif
    }
    // Its neighbours' counts in the block of 2x2 pixels, taken here where
    // every pixel of the block comes by (a pixel that traced none differs)
    vec2 sun_across = vec2(dFdx(sun_count.x), dFdy(sun_count.x));
#ifdef RAY_TRACING
    // The penumbra's rays over the disc, a quarter here; with them, those of
    // the pixel beside it and of the one above or below
    vec4 sun_part = vec4(0.0);
    if (sun_count.x >= 0.0) sun_part = sun_disc(n, sun_count, sun_across);
    vec2 sun_block = 3.0 * sun_part.xy + sun_first().x * dFdxFine(sun_part.xy) + sun_first().y * dFdyFine(sun_part.xy);
#endif
    if ((flags & LUMINOUS) == 0u) {
        if (sunlit) {
#ifdef RAY_TRACING
            s *= sun_count.x >= 0.0 ? traced_shadow(sun_part, sun_block) : sun_shadow(n);
#else
            s *= sun_shadow(n);
#endif
        }
        vec3 light = vec3(s * vp.sun.w + ambient);
        // Clear outdoor scenes (SUN): a stronger, warmer sun and a softer,
        // slightly bluer ambient (light from the sky) for more contrast
        if (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u) {
            float w = atm.sunlight.z;
            light = s * vp.sun.w * atm.sunlight.x * vec3(1.0, 1.0 - 0.05 * w, 1.0 - 0.14 * w) +
                    ambient * atm.sunlight.y * vec3(1.0 - 0.06 * w, 1.0, 1.0 + 0.08 * w);
        }
        // Global illumination: the ambient light in the colour of the sky
        if (atm.gi.y > 0.0 && atm.sunlight.z > 0.0) {
            bool clear = (flags & WORLD) != 0u;
            float w = atm.sunlight.z;
            float a = ambient * (clear ? atm.sunlight.y : 1.0);
            light += a * (sky_ambient(n) - (clear ? vec3(1.0 - 0.06 * w, 1.0, 1.0 + 0.08 * w) : vec3(1.0)));
        }
#ifdef RAY_TRACING
        // Global illumination (not in the original; GLOBAL ILLUMINATION): the
        // player's car takes the light the ground and the scenery around it
        // throw back (the rest of the world gets it with the ambient occlusion,
        // ao.frag: this car, in a viewport of its own, is drawn after it. Not
        // the other cars and the people, ACTOR, which are in the world's and
        // have it from there: given both, the flagman at the start, with the
        // sunlit ground all round him, shone like plastic). A few rays over the hemisphere
        // above the bodywork, the same spread for every pixel and every image
        // (smooth over the smoothed normals, and steady)
        if (atm.gi.y > 0.0 && (flags & CAR) != 0u && length(normal) > 0.0) {
            vec3 gn = normalize(normal);
            if (dot(gn, view_pos) > 0.0) gn = -gn;
            vec3 from = view_pos + gn * (0.03 + 0.002 * length(view_pos));
            vec3 gt = normalize(cross(abs(gn.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), gn));
            vec3 gb = cross(gn, gt);
            float world_sun = max(vp.sun.w, 0.0) * (atm.sunlight.z > 0.0 ? atm.sunlight.x : 1.0);
            float world_ambient = ambient * (atm.sunlight.z > 0.0 ? atm.sunlight.y : 1.0);
            vec3 back = vec3(0.0);
            int RAYS = int(atm.gi.z);
            for (int i = 0; i < RAYS; i++) {
                float u = (float(i) + 0.5) / float(RAYS);
                float a = float(i) * 0.618034 * 6.2831853;
                float r = sqrt(u);
                vec3 dir = normalize(gt * (r * cos(a)) + gb * (r * sin(a)) + gn * sqrt(1.0 - u));
                back += rt_bounce(from, dir, atm.gi.w, normalize(vp.sun.xyz), world_sun, world_ambient);
            }
            // (0.6 of it: in full, a white car on the yellow dirt came out cream all over)
            light += back * (0.6 * atm.gi.y / float(RAYS));
        }
#endif
        // Without the traced bounce above (no ray tracing, or GLOBAL
        // ILLUMINATION off) the player's car had only the game's ambient on
        // the sides away from the sun (the other cars, in the world, miss the
        // bounce of the ambient occlusion pass too): beside the arcade, and beside the same
        // car with ray tracing, its white paint came out a dull grey. A fixed
        // fill (0.2 of the sun; 0.1 with the bounce) in the colour of the sunlit dirt road, stronger on the faces
        // turned to the ground, stands in for the light the ground throws back
        // (and the other cars, ACTOR with specular paint, as in the replays
        // and the attract: not the people, who are self-lit cards)
        bool car_body = (flags & CAR) != 0u || (flags & (ACTOR | SPECULAR)) == (ACTOR | SPECULAR);
        // (with the traced bounce, a smaller share: the cars in the world are
        // also darkened by the occlusion pass, and the bounce alone left them
        // darker than without ray tracing)
        float fill = atm.gi.y > 0.0 ? atm.mirror.x : atm.gi.x;
        if (fill > 0.0 && car_body && length(normal) > 0.0) {
            vec3 fn = normalize(normal);
            if (dot(fn, view_pos) > 0.0) fn = -fn;
            float down = 0.5 - 0.5 * dot(fn, normalize(atm.axes[1].xyz));
            float world_sun = max(vp.sun.w, 0.0) * (atm.sunlight.z > 0.0 ? atm.sunlight.x : 1.0);
            light += vec3(1.0, 0.92, 0.80) * world_sun * fill * (0.6 + 0.4 * down);
        }
        // The spotlight: a flat lobe, as in the original
        if ((flags & WORLD) != 0u && atm.spot.z > 0.0) light += atm.spot_color.rgb * spotlight().x;
        c.rgb *= light;
        if ((flags & SPECULAR) != 0u) {
            uint shin = (flags >> 10) & 3u;
            float powers[4] = float[](8.0, 16.0, 32.0, 64.0);
            float scales[4] = float[](1.6, 1.6, 2.4, 3.2);
            float f = pow(max(s, 0.0), powers[shin]) * scales[shin] * params.z * vp.sun.w *
                      (atm.sunlight.z > 0.0 && (flags & WORLD) != 0u ? atm.sunlight.x : 1.0);
            c.rgb += f;
        }
    }
    // Sega Rally 2 (not in the original): clear coat on the cars' paint, and
    // their glass. The game marks both specular; the cars are the player's
    // (CAR: its own viewport) and the others (ACTOR), whose paint is
    // specular. The player's car is in layers: the paint (untextured), its
    // decals, a see-through self-lit shine over the bodywork (with texture
    // alpha: the original's painted reflection, left as it is) and the glass,
    // a see-through self-lit texture without texture alpha over a black base.
    // The sky and the ground are mirrored with the Fresnel rise at grazing
    // angles, and the sun as a glint
    bool shine_layer = (flags & (LUMINOUS | TEX_ALPHA)) == (LUMINOUS | TEX_ALPHA);
    if ((flags & (CAR | ACTOR)) != 0u && (flags & SPECULAR) != 0u && !shine_layer && length(normal) > 0.0) {
        // The cars' normals rebuilt smooth over the bodywork
        // (R3DSceneBuilder::smooth_car_normals): the reflection glides over
        // the panels instead of jumping from facet to facet
        vec3 nn = normalize(normal), eye = normalize(-view_pos);
        if (dot(nn, eye) < 0.0) nn = -nn;
        bool glass = (flags & (LUMINOUS | TEXTURED)) == (LUMINOUS | TEXTURED);
        float ndv = clamp(dot(nn, eye), 0.0, 1.0);
        // Rally cars are not showroom cars: a satin, dusty paint. Less of a
        // mirror (the Fresnel rise held back too), and dirtier low down and on
        // the sides, where the wheels throw the dust, than on the roof and bonnet
        vec3 world_up = normalize(atm.axes[1].xyz);
        float clean = mix(0.35, 1.0, smoothstep(-0.3, 0.7, dot(nn, world_up)));
        // The glass: blotches of dried mud and dust, fixed to it (its texture
        // coordinates), that break the reflection up and veil it, heavier towards
        // its lower edge
        float grime = 0.0;
        if (glass) {
            vec2 g = uv * vec2(9.0, 5.0);
            float blotch = atm_noise(vec3(g, 0.0)) * 0.6 + atm_noise(vec3(g * 3.1, 1.7)) * 0.3 + atm_noise(vec3(g * 9.0, 3.3)) * 0.1;
            grime = smoothstep(0.30, 0.62, blotch + 0.25 * (1.0 - clamp(uv.y, 0.0, 1.0)));
            clean = mix(1.0, 0.15, grime);
        }
        float f0 = glass ? 0.12 : 0.06;
        float fres = (f0 + (0.55 - f0) * pow(1.0 - ndv, 5.0)) * clean;
        vec3 r = reflect(-eye, nn);
        float up = dot(r, world_up);
        // Rough: a soft horizon and sky (the glass sharper)
        float soft_h = glass ? 0.04 : 0.18;
        vec3 sky = mix(vec3(0.90, 0.92, 0.95), vec3(0.40, 0.56, 0.85), smoothstep(0.0, 0.6, up));
        vec3 env = mix(vec3(0.42, 0.37, 0.30), sky, smoothstep(-soft_h, soft_h, up));   // below: the sunlit dirt road
        // What the paint mirrors, as bright as it is (multiplied by the
        // car's own light, sun plus an ambient of 1, it laid a white veil over it)
        vec3 refl = env * 0.85;
#ifdef RAY_TRACING
        // With ray tracing the world itself: the scenery, the stands and the
        // other cars mirrored where the reflected ray meets them, lit by the
        // sun and the ambient as the scene is, fading into the sky with distance
        if (atm.rt.x > 0.0 && atm.mirror.w > 0.5) {
            vec3 hit_colour;
            float hit_dist;
            vec3 from = view_pos + nn * (0.03 + 0.002 * length(view_pos));
            if (rt_trace_surface(from, r, 250.0, RT_LOW | RT_HIGH | RT_ACTOR, hit_colour, hit_dist)) {
                vec3 lit = hit_colour * clamp(0.45 + 0.55 * max(vp.sun.w, 0.0), 0.0, 1.0);
                // The paint blurs what it mirrors (half the sharp image, half
                // the soft surroundings); the glass keeps it
                lit = mix(lit, refl, glass ? 0.15 : 0.5);
                refl = mix(lit, refl, smoothstep(60.0, 250.0, hit_dist));
            }
        }
#endif
        c.rgb = mix(c.rgb, refl, clamp(fres, 0.0, 1.0));
        if (glass)   // the dust itself: a dull, light brown film, lit like the car
            c.rgb = mix(c.rgb, vec3(0.55, 0.48, 0.38) * (ambient + 0.6 * max(vp.sun.w, 0.0)), 0.6 * grime);
        if (length(vp.sun.xyz) > 0.01) {
            // a sharp glint on the glass, a broad sheen on the satin paint
            float glint = pow(max(dot(r, normalize(vp.sun.xyz)), 0.0), glass ? 900.0 : 60.0);
            c.rgb += vec3(1.0, 0.97, 0.9) * glint * (glass ? 2.0 : 0.35) * clean * vp.sun.w;
        }
    }
    // The original clips the light at white; with HDR lighting the highlights
    // (sunlit surfaces, fire, specular) keep going and the composition's tone
    // curve rolls them off
    c.rgb = min(c.rgb, vec3(atm.soft.z > 0.5 ? 8.0 : 1.0));

    float fog = params.y * clamp(vp.params.z + (-view_pos.z) * vp.params.y, 0.0, 1.0);
    vec3 fog_color = vp.fog_color.rgb * vp.fog_color.a;
    if ((flags & WORLD) != 0u && fog > 0.0) fog_color = fog_color * mist_light(view_pos) + atm.spot_fog.rgb * spotlight().y;
    c.rgb = mix(c.rgb, fog_color, fog);
    // Optional distance haze
    if ((flags & WORLD) != 0u && atm.haze.a > 0.0)
        c.rgb = distance_haze(c.rgb, view_pos, fog);
    // Opaque pass: the colour written is always solid, since a filtered alpha
    // would let the 2D background show through. With MSAA, alpha-tested edges
    // become partial sample coverage instead (smooth foliage outlines).
    if (vp.quality.x > 0.0) {
        int samples = int(vp.quality.x);
        // DLAA: per-frame noise seed in the fractional part (0 = none)
        float seed = fract(vp.quality.x);
        if (samples > 1 && (flags & ALPHA_TEST) != 0u) {
            int covered = clamp(int(t.a * float(samples) + 0.5), 0, samples);
            if (covered == 0) discard;
            gl_SampleMask[0] = (1 << covered) - 1;
        } else if (seed > 0.0 && (flags & ALPHA_TEST) != 0u) {
            // The same partial coverage spread over frames: the pixel is kept
            // with probability alpha and DLAA averages the frames into a
            // smooth outline. Interleaved gradient noise, shifted every frame.
            float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            if (t.a < fract(noise + seed)) discard;
        }
        c.a = 1.0;
    }
    // Soft particles: smoke, fire and other translucent surfaces of the world
    // fade out where they meet what stands behind them, instead of a hard line.
    // Textured ones only: the game's untextured black patch under each car (its
    // shadow) lies 3 cm over the road once the car is set down on it
    // (settle_player_car), and faded over 2 m it was gone
    if (vp.quality.x < 0.0 && atm.soft.x > 0.0 && (flags & (WORLD | TEXTURED)) == (WORLD | TEXTURED)) {
        float d = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
        if (d > 0.0) c.a *= clamp((view_pos.z + 1e-3 / d) / atm.soft.y, 0.0, 1.0);
    }
    out_color = c;
    // Bloom source: fire, explosions and lights are drawn unlit (luminous);
    // only their bright part glows. Everything else writes no glow, so it
    // hides the glow of what is behind it.
    float peak = max(c.r, max(c.g, c.b));
    bool glows = (flags & (LUMINOUS | WORLD)) == (LUMINOUS | WORLD);
    // ... and, more gently, whatever else of the world is very bright (sunlit
    // surfaces and highlights above white with HDR lighting)
    vec3 glow = glows ? c.rgb * smoothstep(0.6, 1.0, peak)
                      : (flags & WORLD) != 0u ? c.rgb * smoothstep(0.75, 1.6, peak) * 0.6 : vec3(0.0);
    out_glow = vec4(glow, c.a);
}
