// Full-window menu layer: a BGRA picture with premultiplied alpha, drawn on
// the CPU (ui/frontend.cpp) and blended over the composed frame. Under it,
// when the menu leaves its backdrop transparent, the backdrop picture drawn
// on the GPU with some movement (shaders/backdrop.frag)
#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

#include "vk_presenter.h"

class UiOverlay {
public:
    bool init(VkPresenter& vk);
    void shutdown();

    // New picture (w x h BGRA, premultiplied); uploaded with the next record
    void set_image(const uint32_t* bgra, uint32_t w, uint32_t h);
    void hide() { visible_ = false; }
    bool visible() const { return visible_; }

    // The backdrops the menu can stand on (opaque BGRA, uploaded once):
    // BACKDROP the picture, BACKDROP_BLURRED the same blurred (behind the
    // settings). anchor_x: which side stays when a narrow window crops it
    enum { BACKDROP, BACKDROP_BLURRED, BACKDROPS };
    void set_backdrop(int slot, const uint32_t* bgra, uint32_t w, uint32_t h, float anchor_x);
    // The one under the current menu picture, or -1 (the picture is opaque)
    void set_motion(int slot) { motion_ = slot; }
    // The pause settings: the frozen game under them, blurred (copied once
    // from the swapchain image, halved a few times) and a little darker
    void set_freeze(bool on) {
        if (!on) frost_ok_ = false;
        freeze_ = on;
    }
    bool can_freeze() const { return back_pipeline_ != VK_NULL_HANDLE; }

    // After the main renderer: the moving backdrop (if any), then the picture
    // blended over the swapchain image
    void record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

private:
    struct Layer {   // an image the CPU fills, its staging buffer and descriptor set
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t w = 0, h = 0;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t staging_size = 0;
        std::vector<uint32_t> pending;
        uint32_t pending_w = 0, pending_h = 0;
        bool dirty = false;
        bool keep_pending = true;   // false: the pixels are dropped once uploaded
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    void upload(VkCommandBuffer cmd, Layer& l);
    void frost(VkCommandBuffer cmd, VkExtent2D extent);   // the blurred copy of the game
    void destroy(Layer& l);
    VkPipeline make_pipeline(const uint32_t* fs, size_t fs_bytes, VkPipelineLayout layout, bool blend);

    VkPresenter* vk_ = nullptr;
    VkDevice dev_ = VK_NULL_HANDLE;
    Layer ui_;
    Layer back_[BACKDROPS];
    float back_anchor_[BACKDROPS] = {};
    int motion_ = -1;
    bool visible_ = false;
    std::chrono::steady_clock::time_point start_;
    double time_offset_ = 0;   // SR2_MENU_TIME: the backdrop's clock starts there (screenshots)
    bool dust_ = true;         // SR2_MENU_DUST=0: the backdrop without its haze and motes
    double fixed_fps_ = 0;     // SR2_MENU_FPS=N: the clock goes 1/N s a frame (films at any speed)
    uint64_t back_frames_ = 0;

    bool freeze_ = false, frost_ok_ = false;
    VkImage frost_image_ = VK_NULL_HANDLE;
    VkDeviceMemory frost_memory_ = VK_NULL_HANDLE;
    VkImageView frost_view_ = VK_NULL_HANDLE;   // its smallest level
    VkDescriptorSet frost_set_ = VK_NULL_HANDLE;
    VkExtent2D frost_extent_{};
    uint32_t frost_levels_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout back_layout_ = VK_NULL_HANDLE;   // with the push constants of backdrop.frag
    VkPipeline back_pipeline_ = VK_NULL_HANDLE;
};
