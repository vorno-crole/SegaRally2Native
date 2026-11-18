// Replacements for the 2D layers below the 3D image: the sky and the other
// backdrops behind the scenery.
//
// Each of those layers is a 512x512 picture that wraps around as the game
// scrolls it. It is identified by a hash of that picture (without the
// brightness offsets of fades), so the same sky is recognised in every stage.
// F12 in the game (and --dump-textures, for every new one) saves the
// backdrops on screen as out/sky/<hash>.png; a file with the same name in
// textures/sky/ replaces it, at any square resolution (2048x2048 for 4x
// detail) and with its own alpha. The picture is drawn at full resolution
// following the game's scroll; a 512x512 reduction of it stands in for the
// layer everywhere else (the distance haze, the horizon colour).
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "image_io.h"

class SkyPack {
public:
    // Slots of the 2D replacement pictures (compose.frag overlays[]) used for
    // backdrops; Pack2D fills the ones below
    static constexpr uint32_t FIRST_SLOT = 248, LAST_SLOT = 255;   // (Pack2D::MAX_SLOTS - 8)
    static constexpr int SIZE = 512;

    // dir: folder with replacement PNGs; dump_dir: where every new backdrop
    // is written (empty = only on capture())
    void init(const std::string& dir, const std::string& dump_dir);
    // Something to do: replacement files, or backdrops to save
    bool active() const { return !available_.empty() || !dump_dir_.empty() || capture_; }
    // Saves the backdrops of the next frame composed to out/sky
    void capture() { capture_ = true; }

    struct Picture {
        uint32_t slot;
        std::vector<uint32_t> low;   // SIZE x SIZE BGRA (alpha 0 or 255), for the layer's own pixels
        uint64_t hash = 0;                // replacement file it was loaded from
        uint64_t used = 0;                // frame it was last shown in (frame_done counts)
    };
    // Replacement for a layer below the 3D image (TileGen::compose), nullptr = none
    const Picture* find(const uint8_t* vram, int layer, bool four_bpp);
    // End of a composed frame (ends a capture)
    void frame_done();

    // Pictures loaded since the last call, to be uploaded by the renderer
    std::vector<RgbaImage> take_new_images();

private:
    const Picture* load(uint64_t hash);

    std::string dir_, dump_dir_, capture_dir_;
    bool capture_ = false;
    std::unordered_map<uint64_t, uint64_t> hashes_;      // layer contents key -> picture hash
    std::unordered_set<uint64_t> available_;             // hashes with a replacement file
    std::unordered_set<uint64_t> dumped_;
    std::unordered_map<uint64_t, int> loaded_;           // hash -> picture
    std::vector<Picture> pictures_;
    std::vector<RgbaImage> pending_;
    std::vector<uint32_t> plane_;
    uint64_t frame_ = 1;   // frames composed, to give the slot of the picture unused longest to a new one
};
