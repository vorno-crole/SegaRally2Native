#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_TRACING
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#endif
// Real3D polygons. Vertices arrive already in view space (GL convention);
// this only projects them with the viewport's reverse-Z frustum. Built with
// RAY_TRACING it also traces the light of the dust (dust_light), once for
// each corner of a puff, not for each of its pixels; without ray tracing the
// sun's shadow map gives the dust its shade instead (dust_shadow_map).
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;   // RGBA8 unorm
layout(location = 4) in uint in_tex;
layout(location = 5) in uint in_flags;
layout(location = 6) in vec3 in_params;  // fixed shade, fog intensity, specular coefficient
layout(location = 7) in uint in_replace; // texture pack slot (0 = original texture)
layout(location = 8) in vec3 in_prev;    // position in the previous game frame

layout(push_constant) uniform Viewport {
    mat4 proj;
    vec4 sun;        // xyz direction (view space), w intensity
    vec4 fog_color;  // rgb, a = fog ambient
    vec4 params;     // ambient, fog density, fog start, interpolation of the previously
                     // displayed image (motion vectors for DLAA)
    vec4 quality;    // MSAA samples (negative in the translucent pass, -1000 in its depth
                     // pre-pass), max anisotropic taps
                     // (0 = no mipmaps), HUD shift (NDC), interpolation between the previous
                     // and current game frame
} vp;

layout(location = 0) out vec3 view_pos;
layout(location = 1) out vec3 normal;
layout(location = 2) out vec2 uv;
layout(location = 3) out vec4 color;
layout(location = 4) flat out uint tex;
layout(location = 5) flat out uint flags;
layout(location = 6) out vec3 params;
layout(location = 7) flat out uint replace;
layout(location = 8) out vec4 clip_now;    // this image and the previous one, for motion vectors
layout(location = 9) out vec4 clip_before;
layout(location = 10) out vec4 dust_light; // DUST: the light bounced onto it (rgb), how much of the sun reaches it (a)

#define ATMOSPHERE_BINDING 2
layout(set = 0, binding = 4) uniform sampler2D shadow_map;   // sun shadows (depth seen from the sun)
#ifdef RAY_TRACING
layout(set = 0, binding = 0) uniform usampler2D texture_ram;
layout(set = 0, binding = 1) uniform sampler2D replacements[];
#include "atmosphere.glsl"
#define RT_SET 0
#include "raytrace.glsl"
#else
#include "atmosphere.glsl"
#include "r3d_texture.glsl"
#endif

// The middle of a puff of dust (its four corners all give the same one, so
// it is lit evenly), and its half side; a clod or a stone is small: itself
vec3 dust_centre(vec3 pos, out float size) {
    size = 0.1;
    if ((in_tex & 3u) != 0u) return pos;
    vec3 up = normalize(atm.axes[1].xyz);
    vec3 d = normalize(in_nrm);   // a puff: upright, facing the eye, in_params.z its half side
    vec3 r = normalize(cross(up, d) + vec3(1e-5, 0.0, 0.0)), u = cross(d, r);
    size = in_params.z;
    return pos - (r * in_uv.x + u * in_uv.y) * size;
}

// Without ray tracing: how much of the sun reaches the middle of the puff,
// from the shadow map the world's sun shadows use. Under the trees and
// behind a car the dust is in their shade. A wide, soft look-up (4x4 texels,
// two apart): a puff is a cloud, not a surface, so its shade has no sharp edge
vec4 dust_shadow_map(vec3 pos) {
    vec4 light = vec4(0.0, 0.0, 0.0, 1.0);
    if (atm.shadow.x <= 0.0 || vp.sun.w <= 0.0) return light;
    float size;
    vec3 centre = dust_centre(pos, size);
    vec3 L = normalize(vp.sun.xyz);
    vec4 lc = atm.shadow_matrix * vec4(centre + L * (0.05 + 0.3 * size), 1.0);
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || lc.z <= 0.0) return light;
    ivec2 tex_size = textureSize(shadow_map, 0);
    ivec2 c = ivec2(uv * vec2(tex_size));
    float lit = 0.0;
    for (int y = -3; y <= 3; y += 2)
        for (int x = -3; x <= 3; x += 2) {
            float d = texelFetch(shadow_map, clamp(c + ivec2(x, y), ivec2(0), tex_size - 1), 0).r;
            lit += d > lc.z + atm.shadow.w ? 0.0 : 1.0;
        }
    vec2 edge = min(uv, 1.0 - uv);   // fades out towards the edge of the shadow map, as r3d.frag's
    light.a = mix(1.0, lit / 16.0, atm.shadow.x * smoothstep(0.0, 0.08, min(edge.x, edge.y)));
    return light;
}

#ifdef RAY_TRACING

// Sega Rally 2, DUST: ENHANCED (r3d_dust.cpp): the particles in the light of
// the place they are in (not in the original). With the traced sun shadows,
// a ray towards the sun from the middle of the puff: under the trees and
// behind a car the dust is in their shade. With the global illumination,
// four rays down and out to the sides: what the ground and the boards
// around throw back. The four corners of a puff trace the same rays (its
// middle is worked out from each), so it is lit evenly
vec4 traced_dust_light(vec3 pos) {
    vec3 up = normalize(atm.axes[1].xyz);
    float size;
    vec3 centre = dust_centre(pos, size);
    vec3 L = normalize(vp.sun.xyz);
    // (traced shadows off: the shadow map's shade)
    vec4 light = atm.rt.x > 0.0 ? vec4(0.0, 0.0, 0.0, 1.0) : dust_shadow_map(pos);
    // (from a little way towards the sun: the puff's middle may be in the ground)
    vec3 from = centre + L * (0.05 + 0.3 * size);
    if (atm.rt.x > 0.0 && vp.sun.w > 0.0)
        light.a = mix(1.0, rt_light(from, L, 2000.0, RT_LOW | RT_ACTOR), atm.shadow.x);
    if (atm.gi.y > 0.0) {
        float sun = max(vp.sun.w, 0.0) * (atm.sunlight.z > 0.0 ? atm.sunlight.x : 1.0);
        float ambient = vp.params.x * (atm.sunlight.z > 0.0 ? atm.sunlight.y : 1.0);
        vec3 x = normalize(cross(abs(up.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), up)), z = cross(up, x);
        vec3 above = centre + up * (0.05 + 0.3 * size);
        for (int i = 0; i < 4; i++) {
            float a = (float(i) + 0.5) * 1.5707963;
            light.rgb += rt_bounce(above, normalize(x * cos(a) + z * sin(a) - up * 0.7), atm.gi.w, L, sun, ambient);
        }
        light.rgb *= 0.6 * atm.gi.y / 4.0;
    }
    return light;
}
#endif
// The translucent pass must land exactly on the depth its pre-pass wrote
invariant gl_Position;

void main() {
    // Displays faster than the game: positions in between its frames
    vec3 pos = mix(in_prev, in_pos, vp.quality.w);
    view_pos = pos;
    normal = in_nrm;
    uv = in_uv;
    color = in_color;
    tex = in_tex;
    flags = in_flags;
    params = in_params;
    replace = in_replace;
    gl_Position = vp.proj * vec4(pos, 1.0);
    vec4 before = vp.proj * vec4(mix(in_prev, in_pos, vp.params.w), 1.0);
    clip_now = gl_Position;
    clip_before = before;
    dust_light = vec4(0.0, 0.0, 0.0, 1.0);
    // (in the translucent pass, where it is drawn: not in its depth pre-pass)
    if ((in_flags & DUST) != 0u && vp.quality.x < 0.0 && vp.quality.x > -1000.0 && vp.fog_color.w >= 0.0)
#ifdef RAY_TRACING
        dust_light = traced_dust_light(pos);
#else
        dust_light = dust_shadow_map(pos);
#endif
}
