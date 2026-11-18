// Shared by r3d.frag and the ray traced effects (raytrace.glsl): the
// polygon flags (R3D_* in r3d_scene.h) and the texel formats of the Real3D
// texture RAM (docs/real3d_spec.md section 6).
#ifndef R3D_TEXTURE_GLSL
#define R3D_TEXTURE_GLSL

const uint TEXTURED = 1u, ALPHA_TEST = 2u, MIRROR_U = 4u, MIRROR_V = 8u, SMOOTH_U = 16u, SMOOTH_V = 32u,
           LUMINOUS = 64u, FIXED_SHADE = 128u, SPECULAR = 256u, TEX_ALPHA = 512u, INVERT_RGB = 4096u,
           TRANSLATOR = 8192u, WORLD = 65536u, CAR = 131072u, ACTOR = 262144u, NO_SHADOW = 1048576u,
           DUST = 8388608u;

// Sega Rally 2: the spectators' camera flashes (and the like: self-lit
// pictures with an alpha of their own, alpha tested, not shiny; the cars'
// glass is shiny and not alpha tested) are light, not things: they cast no
// sun shadows (the dark discs they left on the ground and the barriers)
bool glow_sprite(uint flags) {
    return (flags & (TEXTURED | ALPHA_TEST | LUMINOUS | TEX_ALPHA | SPECULAR)) == (TEXTURED | ALPHA_TEST | LUMINOUS | TEX_ALPHA);
}

vec4 decode(uint t, uint fmt) {
    switch (fmt) {
    case 0u: return vec4(float((t >> 10) & 31u), float((t >> 5) & 31u), float(t & 31u), 0.0) / 31.0
                    + vec4(0.0, 0.0, 0.0, (t & 0x8000u) != 0u ? 0.0 : 1.0);
    case 1u: return vec4(vec3(float(t & 15u)), float((t >> 4) & 15u)) / 15.0;
    case 2u: return vec4(vec3(float((t >> 4) & 15u)), float(t & 15u)) / 15.0;
    case 3u: return vec4(vec3(float((t >> 8) & 15u)), float((t >> 12) & 15u)) / 15.0;
    case 4u: return vec4(vec3(float((t >> 12) & 15u)), float((t >> 8) & 15u)) / 15.0;
    case 5u: { uint l = t & 255u; return vec4(vec3(float(l) / 255.0), l == 255u ? 0.0 : 1.0); }
    case 6u: { uint l = (t >> 8) & 255u; return vec4(vec3(float(l) / 255.0), l == 255u ? 0.0 : 1.0); }
    case 7u: return vec4(float((t >> 12) & 15u), float((t >> 8) & 15u), float((t >> 4) & 15u), float(t & 15u)) / 15.0;
    default: {
        uint n = (t >> (4u * (fmt - 8u))) & 15u;
        return vec4(vec3(float(n) / 15.0), n == 15u ? 0.0 : 1.0);
    }
    }
}

// Wraps a texel coordinate inside the texture rectangle (repeat or mirror)
int wrap(int c, int size, bool mirror) {
    int period = mirror ? size * 2 : size;
    int m = ((c % period) + period) % period;
    return (mirror && m >= size) ? period - 1 - m : m;
}

#endif
