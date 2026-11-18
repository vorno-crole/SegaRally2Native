// Replacements for 2D graphics (tilegen layers above the 3D image), such as
// the route map shown before each stage.
//
// The foreground 2D image is split into 8x8-pixel cells. A replacement is a
// rectangle of cells with the hash each cell had when it was captured, plus a
// picture to show there. While most of those cells are on screen unchanged the
// picture replaces them; cells that differ (the route drawn so far, the hint
// text) keep the game's own pixels on top, so live information is not lost.
//
// Files in textures/2d/: <name>.txt (made by tools/make_2d_replacement.py from
// a capture) and <name>.png (the new picture, any resolution, with alpha).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "image_io.h"

class Pack2D {
public:
    static constexpr int CELLS_X = 62, CELLS_Y = 48;   // 496x384 in 8x8 cells
    // Pictures on the GPU (compose.frag overlays[], slot 0 = none). A slot
    // number is one byte in the text cells (the list and the lit marks), so
    // 256 is the most there can be
    static constexpr uint32_t MAX_SLOTS = 256;
    static_assert(MAX_SLOTS <= 256, "slots are written as one byte in the text cells");

    // Pictures use the GPU slots first_slot..last_slot (the foreground and the
    // layers below the 3D image each have their own Pack2D and range). When a
    // session meets more different pictures than that, a new one takes the slot
    // of the one not shown for longest (its file is read again if it comes back)
    void init(const std::string& dir, uint32_t first_slot = 1, uint32_t last_slot = MAX_SLOTS - 1);
    bool empty() const { return entries_.empty(); }

    // Hashes of all the cells of a 496x384 BGRA image
    static void hash_cells(const uint32_t* front, std::vector<uint64_t>& out);
    // Capture for tools/make_2d_replacement.py: <path>.png and <path>.cells
    static void save_capture(const uint32_t* front, const std::string& path);

    static constexpr int MAX_ACTIVE = 8;   // pictures on screen at once, per Pack2D
    // Finds the replacements on screen (anywhere, at any brightness: cells are
    // compared on `raw`, the 496x384 layers without fade offsets), clears their
    // unchanged cells in `target` (stride pixels per row) and, for the
    // foreground, in the high-resolution text cells so the pictures show
    // through, and adds them to the list in the text cells' last row (see
    // text_hd.h), marked as below the 3D image when `below_3d`. Returns how many.
    // raw_top: `raw` with only the top layer pair, where texts are drawn; a
    // reference cell captured that way matches whatever is behind it
    // under (optional, 496 pixels per row): what the cleared cells show
    // instead of nothing (the picture behind a replaced text), copied into
    // under_dst (under_stride pixels per row; by default `target` itself).
    // The pictures are drawn above the 3D image and below the foreground, so
    // for the foreground it goes into the layers below the 3D image
    int apply(uint32_t* target, int stride, const uint32_t* raw, const uint32_t* raw_top, uint32_t* text_cells,
              bool below_3d = false, const uint32_t* under = nullptr, uint32_t* under_dst = nullptr,
              int under_stride = 0);

    // Pictures loaded since the last call, to be uploaded by the renderer (a
    // reused slot comes again with its new picture, which replaces the old one)
    std::vector<RgbaImage> take_new_images();

private:
    struct Entry {
        std::string name;
        int cx = 0, cy = 0, cw = 0, ch = 0;   // rectangle in cells
        std::vector<uint64_t> cells;          // reference hash per cell (0 = ignored)
        uint32_t slot = 0;                    // 0 = picture not loaded (yet, or its slot was reused)
        bool failed = false;                  // its file could not be read: not tried again
        int min_share = 50;   // "match P" line: P% of the reference cells must be unchanged (titles that differ
                              // only in a digit, STAGE1 and STAGE5, need nearly all of them)
        bool cover = false;   // "cover" line: the picture hides every cell of its box (glints over a text, the
                              // picture behind it, letters wider than the old ones)
        // A second state of the same cells (<name>_lit.txt / .png: a bar graph
        // lit up): a cell showing it gets that picture instead (drawn over the
        // first one, masked per cell through the text cells, see compose.frag)
        std::vector<uint64_t> lit;
        std::string lit_name;
        uint32_t lit_slot = 0;
    };
    // Who holds each slot of the range, and the last apply() that showed it
    struct SlotUse {
        int entry = -1;   // index in entries_ (-1 = free)
        bool lit = false;
        uint64_t shown = 0;
    };
    std::string dir_;
    uint32_t first_slot_ = 1, last_slot_ = MAX_SLOTS - 1;
    std::vector<Entry> entries_;
    std::vector<SlotUse> slots_;
    uint64_t frame_ = 0;   // apply() calls
    bool reuse_told_ = false;
    // A slot for picture `entry` (its lit state when `lit`): a free one, else
    // the one not shown for longest; 0 when all were shown in the last frames
    uint32_t take_slot(int entry, bool lit);
    void mark_shown(uint32_t slot) { slots_[slot - first_slot_].shown = frame_; }
    std::vector<RgbaImage> pending_;
    std::vector<uint64_t> hashes_, hashes_top_;
    int matches(const Entry& e, int cx, int cy, int& total) const;
    bool same(int cy, int cx, uint64_t ref) const {
        return hashes_[cy * CELLS_X + cx] == ref || hashes_top_[cy * CELLS_X + cx] == ref;
    }
    static uint64_t empty_cell_hash();
};
