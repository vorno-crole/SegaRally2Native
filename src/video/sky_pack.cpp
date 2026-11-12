#include "sky_pack.h"

#include "../common/fnv1a.h"

#if defined(__APPLE__)
#include "mac_win32.h"   // the Win32 calls used here, on macOS
#else
#include <windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "tilegen.h"

namespace {

void save_plane(const std::string& dir, uint64_t hash, const std::vector<uint32_t>& plane) {
    CreateDirectoryA(dir.c_str(), nullptr);
    std::vector<uint8_t> rgba(plane.size() * 4);
    for (size_t i = 0; i < plane.size(); i++) {
        uint32_t c = plane[i];
        rgba[i * 4 + 0] = uint8_t(c >> 16);
        rgba[i * 4 + 1] = uint8_t(c >> 8);
        rgba[i * 4 + 2] = uint8_t(c);
        rgba[i * 4 + 3] = uint8_t(c >> 24);
    }
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.png", (unsigned long long)hash);
    png_save(dir + name, SkyPack::SIZE, SkyPack::SIZE, rgba.data());
    printf("[sky] backdrop saved to %s%s\n", dir.c_str(), name);
}
}  // namespace

void SkyPack::init(const std::string& dir, const std::string& dump_dir) {
    dir_ = dir;
    dump_dir_ = dump_dir;
    pictures_.reserve(LAST_SLOT - FIRST_SLOT + 1);   // find() hands out pointers into it
    capture_dir_ = dump_dir.empty() ? "out/sky" : dump_dir;
    list_hashes(dir, available_);
    if (!dump_dir_.empty()) list_hashes(dump_dir_, dumped_);   // already written by earlier runs
    if (!available_.empty()) printf("[sky] %zu backdrop replacements\n", available_.size());
}

const SkyPack::Picture* SkyPack::find(const uint8_t* vram, int layer, bool four_bpp) {
    // The picture is only rebuilt and hashed when the layer's data changes
    uint64_t key = TileGen::layer_key(vram, layer, four_bpp);
    auto it = hashes_.find(key);
    bool rendered = false, seen = it != hashes_.end();
    uint64_t hash;
    if (seen) {
        hash = it->second;
    } else {
        if (hashes_.size() > 4096) hashes_.clear();   // animated title screens
        TileGen::layer_plane(vram, layer, four_bpp, plane_);
        rendered = true;
        hash = fnv1a_words(plane_.data(), plane_.size());
        hashes_[key] = hash;
    }
    bool dump = !dump_dir_.empty() && !seen && dumped_.insert(hash).second;
    if (dump || capture_) {
        if (!rendered) TileGen::layer_plane(vram, layer, four_bpp, plane_);
        save_plane(capture_ ? capture_dir_ : dump_dir_, hash, plane_);
    }
    return available_.count(hash) ? load(hash) : nullptr;
}

void SkyPack::frame_done() {
    capture_ = false;
    frame_++;
}

const SkyPack::Picture* SkyPack::load(uint64_t hash) {
    auto it = loaded_.find(hash);
    if (it != loaded_.end()) {
        pictures_[it->second].used = frame_;
        return &pictures_[it->second];
    }
    // A free slot, or else the one of the picture unused longest (a screen
    // with many states, like the stage select, cycles through its pictures)
    int index = int(pictures_.size());
    uint32_t capacity = LAST_SLOT - FIRST_SLOT + 1;
    if (pictures_.size() >= capacity) {
        index = -1;
        for (int i = 0; i < int(pictures_.size()); i++)
            if (pictures_[i].used < frame_ && (index < 0 || pictures_[i].used < pictures_[index].used)) index = i;
    }
    uint32_t slot = index < 0 ? LAST_SLOT + 1 : index < int(pictures_.size()) ? pictures_[index].slot
                                                                                : FIRST_SLOT + uint32_t(index);
    RgbaImage img;
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.png", (unsigned long long)hash);
    if (index < 0) return nullptr;   // every slot is on screen this frame: the original stays
    if (slot > LAST_SLOT || !png_load(dir_ + name, img.w, img.h, img.rgba)) {
        fprintf(stderr, "[sky] could not load %s%s\n", dir_.c_str(), name);
        available_.erase(hash);
        return nullptr;
    }
    img.slot = slot;
    // Reduction to the layer's own resolution: average of the source area
    // behind each pixel, opaque where most of it is
    Picture p;
    p.slot = slot;
    p.low.resize(SIZE * SIZE);
    for (int y = 0; y < SIZE; y++) {
        int y0 = y * img.h / SIZE, y1 = (std::max)(y0 + 1, (y + 1) * img.h / SIZE);
        for (int x = 0; x < SIZE; x++) {
            int x0 = x * img.w / SIZE, x1 = (std::max)(x0 + 1, (x + 1) * img.w / SIZE);
            uint64_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const uint8_t* s = &img.rgba[(size_t(sy) * img.w + sx) * 4];
                    for (int c = 0; c < 3; c++) sum[c] += uint64_t(s[c]) * s[3];
                    sum[3] += s[3];
                }
            uint32_t n = uint32_t((x1 - x0) * (y1 - y0));
            if (sum[3] * 2 < uint64_t(n) * 255) continue;
            p.low[y * SIZE + x] = 0xFF000000u | uint32_t(sum[0] / sum[3]) << 16 | uint32_t(sum[1] / sum[3]) << 8 |
                                  uint32_t(sum[2] / sum[3]);
        }
    }
    printf("[sky] replacement %s%s (%dx%d)\n", dir_.c_str(), name, img.w, img.h);
    pending_.push_back(std::move(img));
    p.hash = hash;
    p.used = frame_;
    if (index < int(pictures_.size())) {   // takes over a slot
        loaded_.erase(pictures_[index].hash);
        pictures_[index] = std::move(p);
    } else {
        pictures_.push_back(std::move(p));
    }
    loaded_[hash] = index;
    return &pictures_[index];
}

std::vector<RgbaImage> SkyPack::take_new_images() {
    std::vector<RgbaImage> out;
    out.swap(pending_);
    return out;
}
