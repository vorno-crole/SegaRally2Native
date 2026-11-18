// Ray traced effects (r3d_raytrace.h): the opaque world of this image in an
// acceleration structure, and the texture and colour of its triangles. The
// includer defines RT_SET (the descriptor set of the 3D pass, whose bindings
// 6 and 7 these are) and declares texture_ram and replacements in it.
#include "r3d_texture.glsl"

const uint RT_LOW = 1u, RT_HIGH = 2u, RT_ACTOR = 4u;   // R3DRayTracer instance masks: casters near the ground, what is higher up, the models that move

layout(set = RT_SET, binding = 6) uniform accelerationStructureEXT world_rays;
struct Cutout {
    vec2 uv[3];
    uint tex, flags, replace;
    uint color;   // RGBA8, alpha in the top byte
};
// Every triangle, in the order of the positions: each level's solid ones,
// then its cut-outs; solid_count[level] tells them apart
layout(std430, set = RT_SET, binding = 7) readonly buffer Cutouts {
    uvec4 solid_count;
    Cutout cutouts[];
};

// The texture of a triangle where a ray crossed it, nearest texel of mip
// level `level` (0 for the cut-outs' alpha test; a smaller one for what a
// reflection shows)
vec4 surface_texel(Cutout c, vec2 bary, int level) {
    vec2 uv = c.uv[0] * (1.0 - bary.x - bary.y) + c.uv[1] * bary.x + c.uv[2] * bary.y;
    if (c.replace != 0u) {
        if ((c.flags & MIRROR_U) != 0u) { float m = mod(uv.x, 2.0); uv.x = m > 1.0 ? 2.0 - m : m; }
        if ((c.flags & MIRROR_V) != 0u) { float m = mod(uv.y, 2.0); uv.y = m > 1.0 ? 2.0 - m : m; }
        return textureLod(replacements[nonuniformEXT(c.replace & 0xFFFFu)], uv, float(level));
    }
    ivec2 base = ivec2(int(c.tex & 2047u), int((c.tex >> 11) & 2047u));
    ivec2 size = ivec2(32 << int((c.tex >> 22) & 7u), 32 << int((c.tex >> 25) & 7u));
    // A mip level has its own place in the texture page (spec 6.4), as in
    // r3d.frag's bilinear(); levels stop while both sides are >= 4
    const int MX[4] = int[](0, 1024, 1536, 1792), MY[4] = int[](0, 512, 768, 896);
    int l = clamp(level, 0, min(3, max(findMSB(min(size.x, size.y)) - 1, 0)));
    ivec2 b = ivec2(MX[l] + (base.x >> l), (base.y & 1024) | (MY[l] + ((base.y & 1023) >> l)));
    ivec2 s = max(size >> l, ivec2(1));
    ivec2 t = ivec2(floor(uv * vec2(s)));
    t.x = wrap(t.x, s.x, (c.flags & MIRROR_U) != 0u);
    t.y = wrap(t.y, s.y, (c.flags & MIRROR_V) != 0u);
    ivec2 p = ivec2((b.x + t.x) & 2047, (b.y & 1024) | ((b.y + t.y) & 1023));
    return decode(texelFetch(texture_ram, p, 0).r, (c.tex >> 28) & 15u);
}

// Whether a ray stops on a cut-out triangle where it crossed it: its texture's
// alpha there, nearest texel of the top level, as the alpha test would see it
bool cutout_blocks(uint index, vec2 bary) {
    Cutout c = cutouts[index];
    float a = surface_texel(c, bary, 0).a;
    // Alpha test: what the 3D pass keeps; texture alpha: only its solid texels
    // are in the opaque pass
    return (c.flags & ALPHA_TEST) != 0u ? a >= 32.0 / 255.0 : a * float(c.color >> 24) / 255.0 >= 0.5;
}

// How solid a cut-out triangle is where a ray crossed it, 0..1: its texture's
// alpha blended between the four texels around the point (top level), as the
// 3D pass filters it. For the sun's rays (rt_light)
// `level`: the mip level (the farther the caster, the wider the sun's disc
// blurs its outline, and the coarser the level that will do)
float cutout_solid(uint index, vec2 bary, int level) {
    Cutout c = cutouts[index];
    vec2 uv = c.uv[0] * (1.0 - bary.x - bary.y) + c.uv[1] * bary.x + c.uv[2] * bary.y;
    float a;
    if (c.replace != 0u) {
        if ((c.flags & MIRROR_U) != 0u) { float m = mod(uv.x, 2.0); uv.x = m > 1.0 ? 2.0 - m : m; }
        if ((c.flags & MIRROR_V) != 0u) { float m = mod(uv.y, 2.0); uv.y = m > 1.0 ? 2.0 - m : m; }
        a = textureLod(replacements[nonuniformEXT(c.replace & 0xFFFFu)], uv, float(level)).a;
    } else {
        ivec2 base = ivec2(int(c.tex & 2047u), int((c.tex >> 11) & 2047u));
        ivec2 size = ivec2(32 << int((c.tex >> 22) & 7u), 32 << int((c.tex >> 25) & 7u));
        uint fmt = (c.tex >> 28) & 15u;
        // (a level's place in the texture page: as in surface_texel)
        const int MX[4] = int[](0, 1024, 1536, 1792), MY[4] = int[](0, 512, 768, 896);
        int l = clamp(level, 0, min(3, max(findMSB(min(size.x, size.y)) - 1, 0)));
        ivec2 b = ivec2(MX[l] + (base.x >> l), (base.y & 1024) | (MY[l] + ((base.y & 1023) >> l)));
        ivec2 sz = max(size >> l, ivec2(1));
        vec2 p = uv * vec2(sz) - 0.5;
        ivec2 i0 = ivec2(floor(p));
        vec2 f = p - vec2(i0);
        float s[4];
        for (int k = 0; k < 4; k++) {
            ivec2 t = i0 + ivec2(k & 1, k >> 1);
            t.x = wrap(t.x, sz.x, (c.flags & MIRROR_U) != 0u);
            t.y = wrap(t.y, sz.y, (c.flags & MIRROR_V) != 0u);
            s[k] = decode(texelFetch(texture_ram, ivec2((b.x + t.x) & 2047, (b.y & 1024) | ((b.y + t.y) & 1023)), 0).r, fmt).a;
        }
        a = mix(mix(s[0], s[1], f.x), mix(s[2], s[3], f.x), f.y);
    }
    // Alpha test: the 3D pass keeps a pixel as often as its alpha says (smooth
    // outlines); texture alpha: its solid texels in the opaque pass, the
    // others blended over it
    return (c.flags & ALPHA_TEST) != 0u ? a : a * float(c.color >> 24) / 255.0;
}

// How much light comes along `dir` from within `range` to `origin`, 0..1:
// nothing past a solid surface, and past cut-outs (foliage, fences) what
// their texture lets through, smoothly. (Each ray stopped or not by the
// nearest texel, the shadow of a tree was made of the squares of its texture,
// one copy for every ray of the sun's disc, and where the rays turn from image
// to image for DLAA the squares came and went: dots that flickered)
float rt_light(vec3 origin, vec3 dir, float range, uint mask) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, world_rays, gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, 0.0, dir, range);
    float light = 1.0;
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint level = uint(rayQueryGetIntersectionInstanceIdEXT(q, false));
            uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)) + solid_count[level] +
                         uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
            // The disc (some 0.03 across) blurs a caster 6 units away over a
            // texel of foliage or so: a level coarser every time that doubles
            int mip = int(log2(max(rayQueryGetIntersectionTEXT(q, false) / 6.0, 1.0)) + 0.5);
            if (glow_sprite(cutouts[index].flags)) continue;   // (a camera flash: no shadow)
            light *= 1.0 - cutout_solid(index, rayQueryGetIntersectionBarycentricsEXT(q, false), mip);
            if (light < 0.02) rayQueryConfirmIntersectionEXT(q);
        }
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 0.0 : light;
}

// Distance to the first surface along `dir` (view space, normalised) from
// `origin` within `range`, or a negative number if there is none. mask: the
// instances to look at (RT_LOW, RT_HIGH); any: the first surface found will
// do (is it hidden?) rather than the nearest one
float rt_trace(vec3 origin, vec3 dir, float range, uint mask, bool any) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, world_rays, any ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT, mask, origin,
                          0.0, dir, range);
    while (rayQueryProceedEXT(q)) {
        // Only cut-out triangles are candidates: after the level's solid ones
        // (a camera flash hides nothing: not in the occlusion either)
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint level = uint(rayQueryGetIntersectionInstanceIdEXT(q, false));
            uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)) + solid_count[level] +
                         uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
            if (!glow_sprite(cutouts[index].flags) && cutout_blocks(index, rayQueryGetIntersectionBarycentricsEXT(q, false)))
                rayQueryConfirmIntersectionEXT(q);
        }
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT
               ? rayQueryGetIntersectionTEXT(q, true)
               : -1.0;
}

// The nearest surface along `dir` and its colour there (texture times vertex
// colour, unlit), for reflections and bounced light; false if there is none
// within `range`. mask: the instances to look at
bool rt_trace_surface(vec3 origin, vec3 dir, float range, uint mask, out vec3 colour, out float dist) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, world_rays, gl_RayFlagsNoneEXT, mask, origin, 0.0, dir, range);
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint level = uint(rayQueryGetIntersectionInstanceIdEXT(q, false));
            uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)) + solid_count[level] +
                         uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
            if (cutout_blocks(index, rayQueryGetIntersectionBarycentricsEXT(q, false))) rayQueryConfirmIntersectionEXT(q);
        }
    }
    colour = vec3(0.0);
    dist = -1.0;
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;
    dist = rayQueryGetIntersectionTEXT(q, true);
    uint level = uint(rayQueryGetIntersectionInstanceIdEXT(q, true));
    uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)) +
                 (rayQueryGetIntersectionGeometryIndexEXT(q, true) == 1 ? solid_count[level] : 0u) +
                 uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    Cutout c = cutouts[index];
    vec3 vc = vec3(float(c.color & 255u), float((c.color >> 8) & 255u), float((c.color >> 16) & 255u)) / 255.0;
    colour = (c.flags & TEXTURED) != 0u ? surface_texel(c, rayQueryGetIntersectionBarycentricsEXT(q, true), 3).rgb * vc : vc;
    return true;
}

// Global illumination (not in the original): the light that comes back
// along one ray from the first surface it meets within `reach`: that
// surface's colour, lit by the sun where a ray from there towards it (L)
// gets through, and by half the ambient light (the surface's own surroundings
// hide some of the sky from it). Nothing from the open sky: that is the
// ambient light already. The cars and the people are left out (the cars'
// own rays would meet their own bodywork)
vec3 rt_bounce(vec3 origin, vec3 dir, float reach, vec3 L, float sun, float ambient) {
    vec3 colour;
    float dist;
    if (!rt_trace_surface(origin, dir, reach, RT_LOW | RT_HIGH, colour, dist)) return vec3(0.0);
    // Back from the surface met, on the side the ray came from: where the
    // sun is behind it, the ray towards the sun meets it again
    vec3 at = origin + dir * (dist - 0.03 - 0.004 * dist);
    float lit = sun > 0.0 ? rt_light(at, L, 2000.0, RT_LOW | RT_ACTOR) : 0.0;
    // Half its colour: the game's ground is a strong yellow, and in full its
    // light turned every white thing beside the road (the boards, the
    // crowd's shirts, the cars) cream: a yellow cast over the whole picture
    colour = mix(vec3(dot(colour, vec3(0.2126, 0.7152, 0.0722))), colour, 0.5);
    // (fading out towards the end of the reach: nothing pops in or out)
    return colour * ((sun * lit + 0.5 * ambient) * (1.0 - smoothstep(0.6 * reach, reach, dist)));
}
