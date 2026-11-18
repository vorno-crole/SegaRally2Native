#include "capture.h"

#include <cstdio>

#include "tilegen.h"

namespace {
constexpr uint32_t SNAPSHOT_MAGIC = 0x3153574C;   // "LWS1", as app_files.cpp
constexpr size_t TEXELS = 2048 * 2048;
// The three Real3D regions hold 144 pages; anything far above that is not a snapshot
constexpr uint32_t MAX_PAGES = 4096;
}  // namespace

bool load_capture(const std::string& path, Capture& out, std::string& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    uint32_t magic = 0, count = 0;
    auto state = std::make_shared<R3DFrameState>();
    bool ok = fread(&magic, 4, 1, f) == 1;
    if (!ok || magic != SNAPSHOT_MAGIC) {
        fclose(f);
        error = path + " is not a snapshot (F9 in the game writes them to out/snap)";
        return false;
    }
    ok = fread(&state->frame, 8, 1, f) == 1 && fread(&count, 4, 1, f) == 1 && count <= MAX_PAGES;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t key = 0;
        auto page = std::make_shared<R3DPage>();
        ok = fread(&key, 4, 1, f) == 1 && fread(page->data(), 1, page->size(), f) == page->size();
        if (ok) state->pages[key] = std::move(page);
    }
    std::vector<uint16_t> texels(TEXELS);
    ok = ok && fread(texels.data(), 2, texels.size(), f) == texels.size();
    std::vector<uint8_t> vram(TILEGEN_SIZE);
    ok = ok && fread(vram.data(), 1, vram.size(), f) == vram.size();
    fclose(f);
    if (!ok) {
        error = path + " stops short: not a whole snapshot";
        return false;
    }
    out.state = std::move(state);
    out.texels = std::move(texels);
    out.vram = std::move(vram);
    out.path = path;
    return true;
}

bool save_capture(const std::string& path, const Capture& c) {
    if (!c.state || c.texels.size() != TEXELS || c.vram.size() != TILEGEN_SIZE) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t count = uint32_t(c.state->pages.size());
    bool ok = fwrite(&SNAPSHOT_MAGIC, 4, 1, f) == 1 && fwrite(&c.state->frame, 8, 1, f) == 1 && fwrite(&count, 4, 1, f) == 1;
    for (const auto& [key, page] : c.state->pages)
        ok = ok && fwrite(&key, 4, 1, f) == 1 && fwrite(page->data(), 1, page->size(), f) == page->size();
    ok = ok && fwrite(c.texels.data(), 2, c.texels.size(), f) == c.texels.size();
    ok = ok && fwrite(c.vram.data(), 1, c.vram.size(), f) == c.vram.size();
    return fclose(f) == 0 && ok;
}
