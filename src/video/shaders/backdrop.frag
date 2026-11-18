#version 450
// The menu's backdrop, moving under the menu layer (ui_overlay.cpp): the
// picture covering the window with a breathing zoom and a drift, faint shafts
// of light falling from the top left with a brighter one sweeping across every
// 6 s, a low haze of dust rolling over the ground and a few soft motes that
// only show where the light catches them. Behind the settings it is the
// blurred picture, a little darker, sampled smoothly (a sixteenth of the window)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D pic;
layout(push_constant) uniform Params {
    float t;               // seconds
    float screen_aspect;   // width / height of the window
    float pic_aspect;      // ... of the picture
    float anchor_x;        // the picture's crop when the window is narrower: 0 keeps its left side
    float height;          // window height in pixels
    float blurred;         // 1: the blurred picture behind the settings; 2: the frozen game (pause settings), still
    float dust;            // 1: the haze and the motes (SR2_MENU_DUST=0: off)
} pc;

const float TAU = 6.2831853;
const float BLUR_DIM = 0.3;   // the blurred picture this much darker
const float PAUSE_DIM = 0.35;  // the frozen game a little more (the race HUD is bright)

// The picture covering the window (cut at the sides or at the top and bottom)
vec2 cover(vec2 s) {
    float sa = pc.screen_aspect, pa = pc.pic_aspect;
    if (sa > pa) return vec2(s.x, 0.5 + (s.y - 0.5) * pa / sa);
    return vec2(pc.anchor_x * (1.0 - sa / pa) + s.x * sa / pa, s.y);
}

// Smooth (B-spline) filtering from four bilinear taps: plain bilinear would
// show the small blurred picture's grid while it moves
vec4 cubic(float v) {
    vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
    vec4 s = n * n * n;
    float x = s.x, y = s.y - 4.0 * s.x, z = s.z - 4.0 * s.y + 6.0 * s.x;
    return vec4(x, y, z, 6.0 - x - y - z) / 6.0;
}
vec3 smooth_sample(vec2 p) {
    vec2 size = vec2(textureSize(pic, 0));
    p = p * size - 0.5;
    vec2 f = fract(p);
    p -= f;
    vec4 xc = cubic(f.x), yc = cubic(f.y);
    vec4 c = p.xxyy + vec2(-0.5, 1.5).xyxy;
    vec4 s = vec4(xc.xz + xc.yw, yc.xz + yc.yw);
    vec4 o = (c + vec4(xc.yw, yc.yw) / s) / size.xxyy;
    vec3 s0 = texture(pic, o.xz).rgb, s1 = texture(pic, o.yz).rgb;
    vec3 s2 = texture(pic, o.xw).rgb, s3 = texture(pic, o.yw).rgb;
    float sx = s.x / (s.x + s.y), sy = s.z / (s.z + s.w);
    return mix(mix(s3, s2, sx), mix(s1, s0, sx), sy);
}

vec3 hash3(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yxz + 33.33);
    return fract((q.xxy + q.yzz) * q.zyx);
}

// Smooth value noise and a few octaves of it (each octave turned, so no grid shows)
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash3(i).x, b = hash3(i + vec2(1, 0)).x, c = hash3(i + vec2(0, 1)).x, d = hash3(i + vec2(1, 1)).x;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    const mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) {
        v += a * vnoise(p);
        p = turn * p * 2.03 + 11.7;
        a *= 0.5;
    }
    return v / 0.9375;
}

// One depth of motes: at most one per cell (few cells hold one), a soft
// out-of-focus disc a little brighter at its rim, drifting slowly
float motes(vec2 q, float t, float cells, vec2 vel, float density, float seed) {
    vec2 g = (q - vel * t) * cells + seed * 17.0;
    vec3 h = hash3(floor(g) + seed * 31.0);
    if (h.z > density) return 0.0;
    vec2 c = 0.3 + 0.4 * h.xy + 0.08 * vec2(sin(t * 0.5 + h.x * TAU), cos(t * 0.4 + h.y * TAU));
    float r = 0.13 + 0.07 * fract(h.z * 7.31);
    float d = length(fract(g) - c);
    return smoothstep(r, r * 0.72, d) * (0.7 + 0.3 * smoothstep(r * 0.4, r * 0.85, d)) * (0.6 + 0.4 * h.y);
}

void main() {
    if (pc.blurred > 1.5) {   // the pause settings: the game blurred and darker, nothing moving
        color = vec4(smooth_sample(uv) * (1.0 - PAUSE_DIM), 1.0);
        return;
    }
    float t = pc.t;
    bool blurred = pc.blurred > 0.5;
    vec2 q = vec2(uv.x * pc.screen_aspect, uv.y);   // window height = 1, y down

    // 1. The picture, never showing past its edges
    vec3 col;
    if (blurred) {
        // a zoom breathing every 15 s and a drift (about 20 s), as far as the zoom leaves room
        float zoom = 1.08 + 0.045 * sin(TAU * t / 15.0);
        float room = (1.0 - 1.0 / zoom) * 0.5 * 0.9;
        vec2 drift = room * vec2(sin(TAU * t / 22.5), sin(TAU * t / 18.5 + 1.3));
        col = smooth_sample(cover((uv - 0.5) / zoom + 0.5 + drift)) * (1.0 - BLUR_DIM);
    } else {
        // zoomed about the car and the bottom of the picture (its dark bottom,
        // under the credits, stays in the window), breathing every 19 s
        float zoom = 1.07 + 0.025 * sin(TAU * t / 19.0);
        vec2 pan = vec2(0.012 * sin(TAU * t / 23.5), -0.006 * (0.5 + 0.5 * cos(TAU * t / 20.5)));
        vec2 centre = vec2(0.3, 0.92);
        col = texture(pic, cover(centre + (uv - centre) / zoom + pan)).rgb;
    }
    // (the main menu's bottom a little darker, under the credits: after the haze)
    float darker = blurred ? 1.0 : 1.0 - 0.12 * smoothstep(0.8, 1.0, uv.y);

    // 2. Light: faint shafts falling from the top left that sway and breathe,
    // and a brighter one sweeping across every 6 s
    vec2 dir = normalize(vec2(0.45, 1.0));
    float across = q.x * dir.y - q.y * dir.x;
    float rays = 0.6 * pow(0.5 + 0.5 * sin(across * 7.0 + t * 0.26), 6.0) +
                 0.4 * pow(0.5 + 0.5 * sin(across * 13.0 - t * 0.18 + 1.7), 8.0);
    rays *= 0.65 + 0.35 * sin(t * 0.46 + across * 2.0);
    float phase = fract(t / 6.0);
    float lo = -dir.x - 0.3, hi = pc.screen_aspect * dir.y + 0.3;
    float sweep = exp(-pow((across - mix(lo, hi, phase)) / 0.07, 2.0)) *
                  smoothstep(0.0, 0.15, phase) * smoothstep(1.0, 0.85, phase);
    float from_top = 1.0 - 0.75 * clamp(q.y, 0.0, 1.0);
    float light = (rays * 0.7 + sweep * 1.6) * from_top;   // how lit the air is here

    if (pc.dust > 0.5) {
        // 3. A low haze of dust over the ground: two depths of soft noise rolling
        // to the right (the nearer faster), in the colour of the picture's ground
        // made warmer and lighter, over its bottom third; brighter in the light
        vec3 ground = (texture(pic, cover(vec2(0.15, 0.84))).rgb + texture(pic, cover(vec2(0.45, 0.88))).rgb +
                       texture(pic, cover(vec2(0.75, 0.84))).rgb) / 3.0;
        vec3 tone = mix(ground * 1.8 + 0.08, vec3(0.78, 0.66, 0.50), 0.55);
        float low = smoothstep(0.62, 0.86, uv.y) * (blurred ? 1.0 : 1.0 - 0.5 * smoothstep(0.9, 1.0, uv.y));
        float far = fbm(vec2(q.x * 1.7 - t * 0.035, q.y * 4.5));
        float near = fbm(vec2(q.x * 0.9 - t * 0.07, q.y * 2.6) + 5.3);
        float haze = low * (0.45 * smoothstep(0.3, 0.8, far) + 0.55 * smoothstep(0.35, 0.85, near));
        col = mix(col, tone, haze * (blurred ? 0.14 : 0.30) * (0.75 + 0.5 * light));
    }
    col *= darker;

    col += vec3(1.0, 0.86, 0.62) * (rays * 0.07 + sweep * 0.1) * from_top;

    if (pc.dust > 0.5) {
        // 4. Motes, few and out of focus, seen only where the light catches them:
        // a glint when the sweep goes by
        float m = motes(q, t, 2.6, vec2(0.030, -0.012), 0.22, 1.0) + 0.7 * motes(q, t, 4.2, vec2(0.020, -0.008), 0.18, 2.0);
        col += vec3(1.0, 0.88, 0.68) * m * light * (blurred ? 0.30 : 0.50);
    }

    // a little noise so the slow gradients do not band
    col += (hash3(gl_FragCoord.xy + fract(t) * 91.0).x - 0.5) / 255.0;
    color = vec4(col, 1.0);
}
