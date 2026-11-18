// The textures a model uses, read back from texture RAM: decoded to RGBA and
// named as the texture packs name them (texture_pack.h: a hash of the decoded
// picture, textures/<hash>.png replaces it).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "r3d_scene.h"

// A polygon texture word (R3DVertex::tex) taken apart
struct TextureWord {
    int x, y;          // in the 2048 x 2048 texture RAM (the page folded into y)
    int w, h;
    uint32_t format;   // texel format, 0-11 (docs/real3d_spec.md 6.5)
};
TextureWord texture_word(uint32_t tex);

// One texel to 8-bit RGBA: the decoding of r3d.frag and of texture_pack.cpp
void decode_texel(uint16_t texel, uint32_t format, uint8_t* rgba);

// The texture as RGBA (w x h x 4) and its texture pack hash
uint64_t decode_texture(uint32_t tex, R3DTextureRam& ram, std::vector<uint8_t>& rgba);

// "<16 hex digits>.png": its file name in a texture pack
std::string texture_file_name(uint64_t hash);

struct TextureUse {
    uint32_t tex = 0;
    TextureWord word{};
    uint64_t hash = 0;
    uint32_t triangles = 0;      // of the model that use it
    bool in_pack = false;        // the texture pack has a picture for it (set by the caller)
    std::vector<uint8_t> rgba;
};
// The textures of one model drawn in a viewport, most used first
std::vector<TextureUse> model_textures(const R3DScene& scene, const R3DViewport& v, const R3DInstance& in,
                                       R3DTextureRam& ram);
