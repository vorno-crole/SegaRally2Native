#include "textures.h"

#include <algorithm>
#include <cstdio>

#include "../common/fnv1a.h"

TextureWord texture_word(uint32_t tex) {
    return {int(tex & 2047), int((tex >> 11) & 2047), 32 << ((tex >> 22) & 7), 32 << ((tex >> 25) & 7), tex >> 28};
}

// (kept the same as decode() in src/video/texture_pack.cpp, which is private
// to it: tests/viewer_test.cpp checks that both give a texture the same name)
void decode_texel(uint16_t t, uint32_t fmt, uint8_t* o) {
    auto lum = [&](uint32_t l, uint32_t max, bool opaque, uint32_t a) {
        o[0] = o[1] = o[2] = uint8_t((l * 255 + max / 2) / max);
        o[3] = opaque ? uint8_t(a) : uint8_t(0);
    };
    auto l4 = [&](uint32_t l, uint32_t a) { lum(l, 15, true, (a * 255 + 7) / 15); };
    switch (fmt) {
    case 0:
        o[0] = uint8_t((((t >> 10) & 31) * 255 + 15) / 31);
        o[1] = uint8_t((((t >> 5) & 31) * 255 + 15) / 31);
        o[2] = uint8_t(((t & 31) * 255 + 15) / 31);
        o[3] = (t & 0x8000) ? 0 : 255;
        break;
    case 1: l4(t & 15, (t >> 4) & 15); break;
    case 2: l4((t >> 4) & 15, t & 15); break;
    case 3: l4((t >> 8) & 15, (t >> 12) & 15); break;
    case 4: l4((t >> 12) & 15, (t >> 8) & 15); break;
    case 5: lum(t & 255, 255, (t & 255) != 255, 255); break;
    case 6: lum((t >> 8) & 255, 255, ((t >> 8) & 255) != 255, 255); break;
    case 7:
        o[0] = uint8_t(((t >> 12) & 15) * 17);
        o[1] = uint8_t(((t >> 8) & 15) * 17);
        o[2] = uint8_t(((t >> 4) & 15) * 17);
        o[3] = uint8_t((t & 15) * 17);
        break;
    default: {
        uint32_t n = (t >> (4 * (fmt - 8))) & 15;
        lum(n, 15, n != 15, 255);
    }
    }
}

uint64_t decode_texture(uint32_t tex, R3DTextureRam& ram, std::vector<uint8_t>& rgba) {
    TextureWord w = texture_word(tex);
    std::vector<uint16_t> texels;
    ram.read_texture(w.x, w.y, w.w, w.h, texels);
    rgba.resize(texels.size() * 4);
    for (size_t i = 0; i < texels.size(); i++) decode_texel(texels[i], w.format, &rgba[i * 4]);
    uint32_t dims[2] = {uint32_t(w.w), uint32_t(w.h)};
    return fnv1a_bytes(rgba.data(), rgba.size(), fnv1a_bytes(dims, sizeof(dims)));
}

std::string texture_file_name(uint64_t hash) {
    char name[32];
    snprintf(name, sizeof(name), "%016llx.png", (unsigned long long)hash);
    return name;
}

std::vector<TextureUse> model_textures(const R3DScene& scene, const R3DViewport& v, const R3DInstance& in,
                                       R3DTextureRam& ram) {
    std::vector<TextureUse> uses;
    auto count = [&](const std::vector<R3DVertex>& list, size_t first, size_t size, const uint32_t r[2]) {
        size_t from = std::min<size_t>(r[0], size), to = std::min<size_t>(r[1], size);
        for (size_t i = first + from; i + 2 < first + to && i + 2 < list.size(); i += 3) {
            const R3DVertex& x = list[i];
            if (!(x.flags & R3D_TEXTURED)) continue;
            auto it = std::find_if(uses.begin(), uses.end(), [&](const TextureUse& u) { return u.tex == x.tex; });
            if (it == uses.end()) {
                uses.emplace_back();
                it = uses.end() - 1;
                it->tex = x.tex;
            }
            it->triangles++;
        }
    };
    count(scene.vertices, v.opaque_first, v.opaque_count, in.opaque);
    count(scene.vertices, v.trans_first, v.trans_count, in.trans);
    count(scene.backs, v.back_first, v.back_count, in.back);
    for (TextureUse& u : uses) {
        u.word = texture_word(u.tex);
        u.hash = decode_texture(u.tex, ram, u.rgba);
    }
    std::stable_sort(uses.begin(), uses.end(), [](const TextureUse& a, const TextureUse& b) { return a.triangles > b.triangles; });
    return uses;
}
