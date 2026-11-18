#include "ui_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "backdrop.frag.h"
#include "compose.vert.h"
#include "ui.frag.h"

namespace {
VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

struct BackdropParams {   // backdrop.frag's push constants
    float t, screen_aspect, pic_aspect, anchor_x, height, blurred, dust;
};
}  // namespace

bool UiOverlay::init(VkPresenter& vk) {
    vk_ = &vk;
    dev_ = vk.device();
    start_ = std::chrono::steady_clock::now();
    if (const char* t = getenv("SR2_MENU_TIME")) time_offset_ = atof(t);
    if (const char* d = getenv("SR2_MENU_DUST")) dust_ = atoi(d) != 0;
    if (const char* f = getenv("SR2_MENU_FPS")) fixed_fps_ = atof(f);

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &set_layout_);
    const uint32_t sets = 1 + BACKDROPS + 1;   // + the frozen game's
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = sets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_);
    VkDescriptorSet* set_of[sets] = {&ui_.set, &back_[0].set, &back_[1].set, &frost_set_};
    for (VkDescriptorSet* set : set_of) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        vkAllocateDescriptorSets(dev_, &ai, set);
    }
    for (Layer& l : back_) l.keep_pending = false;

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout_;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_);
    VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BackdropParams)};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &back_layout_);

    pipeline_ = make_pipeline(spv_ui_frag, sizeof(spv_ui_frag), layout_, true);
    back_pipeline_ = make_pipeline(spv_backdrop_frag, sizeof(spv_backdrop_frag), back_layout_, false);
    if (!pipeline_) fprintf(stderr, "[vulkan] could not create the menu pipeline\n");
    if (!back_pipeline_) fprintf(stderr, "[vulkan] could not create the menu backdrop pipeline\n");
    return pipeline_ != VK_NULL_HANDLE;
}

// A fullscreen triangle with this fragment shader; blend: premultiplied
// alpha over what is there, else opaque
VkPipeline UiOverlay::make_pipeline(const uint32_t* fs_code, size_t fs_bytes, VkPipelineLayout layout, bool blend) {
    VkShaderModule vs = make_module(dev_, spv_compose_vert, sizeof(spv_compose_vert));
    VkShaderModule fs = make_module(dev_, fs_code, fs_bytes);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = blend ? VK_TRUE : VK_FALSE;   // premultiplied alpha
    cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkFormat format = vk_->swap_format();
    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rend;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    return p;
}

void UiOverlay::set_image(const uint32_t* bgra, uint32_t w, uint32_t h) {
    ui_.pending.assign(bgra, bgra + size_t(w) * h);
    ui_.pending_w = w;
    ui_.pending_h = h;
    ui_.dirty = visible_ = true;
}

void UiOverlay::set_backdrop(int slot, const uint32_t* bgra, uint32_t w, uint32_t h, float anchor_x) {
    if (slot < 0 || slot >= BACKDROPS || !bgra || !w || !h) return;
    Layer& l = back_[slot];
    l.pending.resize(size_t(w) * h);
    for (size_t i = 0; i < l.pending.size(); i++) l.pending[i] = bgra[i] | 0xFF000000u;   // opaque
    l.pending_w = w;
    l.pending_h = h;
    l.dirty = true;
    back_anchor_[slot] = anchor_x;
}

void UiOverlay::destroy(Layer& l) {
    if (l.view) vkDestroyImageView(dev_, l.view, nullptr);
    if (l.image) vkDestroyImage(dev_, l.image, nullptr);
    if (l.memory) vkFreeMemory(dev_, l.memory, nullptr);
    if (l.staging) {
        vkDestroyBuffer(dev_, l.staging, nullptr);
        vkFreeMemory(dev_, l.staging_memory, nullptr);
    }
    l.view = VK_NULL_HANDLE;
    l.image = VK_NULL_HANDLE;
    l.memory = VK_NULL_HANDLE;
    l.staging = VK_NULL_HANDLE;
    l.staging_memory = VK_NULL_HANDLE;
    l.mapped = nullptr;
    l.staging_size = 0;
    l.w = l.h = 0;
}

// The layer's new pixels to its image (a new image and staging buffer when
// the size changed)
void UiOverlay::upload(VkCommandBuffer cmd, Layer& l) {
    size_t bytes = l.pending.size() * 4;
    if (l.pending_w != l.w || l.pending_h != l.h || bytes > l.staging_size) {
        vkDeviceWaitIdle(dev_);
        destroy(l);
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_B8G8R8A8_UNORM;
        ii.extent = {l.pending_w, l.pending_h, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vkCreateImage(dev_, &ii, nullptr, &l.image);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, l.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev_, &mai, nullptr, &l.memory);
        vkBindImageMemory(dev_, l.image, l.memory, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = l.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ii.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev_, &vci, nullptr, &l.view);
        l.w = l.pending_w;
        l.h = l.pending_h;

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(dev_, &bi, nullptr, &l.staging);
        vkGetBufferMemoryRequirements(dev_, l.staging, &req);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = vk_->find_memory(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev_, &mai, nullptr, &l.staging_memory);
        vkBindBufferMemory(dev_, l.staging, l.staging_memory, 0);
        vkMapMemory(dev_, l.staging_memory, 0, VK_WHOLE_SIZE, 0, &l.mapped);
        l.staging_size = bytes;

        VkDescriptorImageInfo info{sampler_, l.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = l.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    }
    // The previous frame has finished (the presenter waits for it), so
    // the staging buffer is free to rewrite
    memcpy(l.mapped, l.pending.data(), bytes);
    vk_image_barrier(cmd, l.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {l.w, l.h, 1};
    vkCmdCopyBufferToImage(cmd, l.staging, l.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vk_image_barrier(cmd, l.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    l.dirty = false;
    if (!l.keep_pending) std::vector<uint32_t>().swap(l.pending);
}

// The game as it is in the swapchain image (before the menu goes over it),
// halved five times by blits: a 1/32 picture, smoothed when drawn (the
// backdrop shader's B-spline), so the HUD's figures become smudges
void UiOverlay::frost(VkCommandBuffer cmd, VkExtent2D extent) {
    VkImage target = vk_->current_image();
    if (!target || extent.width < 64 || extent.height < 64) return;
    constexpr uint32_t LEVELS = 5;
    if (frost_extent_.width != extent.width || frost_extent_.height != extent.height || !frost_image_) {
        vkDeviceWaitIdle(dev_);
        if (frost_view_) vkDestroyImageView(dev_, frost_view_, nullptr);
        if (frost_image_) vkDestroyImage(dev_, frost_image_, nullptr);
        if (frost_memory_) vkFreeMemory(dev_, frost_memory_, nullptr);
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = vk_->swap_format();
        ii.extent = {std::max(1u, extent.width / 2), std::max(1u, extent.height / 2), 1};
        ii.mipLevels = LEVELS;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(dev_, &ii, nullptr, &frost_image_) != VK_SUCCESS) { frost_image_ = VK_NULL_HANDLE; return; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, frost_image_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev_, &mai, nullptr, &frost_memory_);
        vkBindImageMemory(dev_, frost_image_, frost_memory_, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = frost_image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ii.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, LEVELS - 1, 1, 0, 1};
        vkCreateImageView(dev_, &vci, nullptr, &frost_view_);
        VkDescriptorImageInfo info{sampler_, frost_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = frost_set_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
        frost_extent_ = extent;
        frost_levels_ = LEVELS;
    }
    auto barrier = [&](VkImage image, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    };
    auto blit = [&](VkImage from, uint32_t from_level, int32_t fw, int32_t fh, uint32_t to_level, int32_t tw, int32_t th) {
        VkImageBlit r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, from_level, 0, 1};
        r.srcOffsets[1] = {fw, fh, 1};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, to_level, 0, 1};
        r.dstOffsets[1] = {tw, th, 1};
        vkCmdBlitImage(cmd, from, from == frost_image_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       frost_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
    };
    barrier(target, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(frost_image_, 0, frost_levels_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int32_t w = int32_t(extent.width), h = int32_t(extent.height);
    int32_t lw = std::max(1, w / 2), lh = std::max(1, h / 2);
    blit(target, 0, w, h, 0, lw, lh);
    for (uint32_t i = 1; i < frost_levels_; i++) {
        barrier(frost_image_, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        int32_t nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        blit(frost_image_, i - 1, lw, lh, i, nw, nh);
        lw = nw, lh = nh;
    }
    barrier(frost_image_, 0, frost_levels_ - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(frost_image_, frost_levels_ - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(target, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    frost_ok_ = true;
}

void UiOverlay::record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    if (!visible_ || !pipeline_) return;
    if (ui_.dirty) upload(cmd, ui_);
    if (!ui_.image) return;
    Layer* back = motion_ >= 0 && motion_ < BACKDROPS && back_pipeline_ ? &back_[motion_] : nullptr;
    if (back && back->dirty) upload(cmd, *back);
    if (back && !back->image) back = nullptr;
    bool frozen = false;   // the pause settings: the blurred game under them
    if (freeze_ && back_pipeline_ && !back) {
        if (!frost_ok_ || frost_extent_.width != extent.width || frost_extent_.height != extent.height) frost(cmd, extent);
        frozen = frost_ok_;
    }

    VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ta.imageView = target;
    ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ta.loadOp = back || frozen ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;   // the backdrop covers it all
    ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ta;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport full{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &full);
    VkRect2D all{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &all);
    if (back) {
        // the clock wraps every 2 hours (a jump then) so the shader's floats stay precise
        double t = fixed_fps_ > 0 ? double(back_frames_++) / fixed_fps_ + time_offset_
                                  : std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count() + time_offset_;
        BackdropParams p{float(std::fmod(t, 7200.0)),
                         float(extent.width) / float(std::max(1u, extent.height)),
                         float(back->w) / float(back->h),
                         back_anchor_[motion_],
                         float(extent.height),
                         motion_ == BACKDROP_BLURRED ? 1.0f : 0.0f,
                         dust_ ? 1.0f : 0.0f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, back_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, back_layout_, 0, 1, &back->set, 0, nullptr);
        vkCmdPushConstants(cmd, back_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (frozen) {
        float aspect = float(extent.width) / float(std::max(1u, extent.height));
        BackdropParams p{0, aspect, aspect, 0, float(extent.height), 2.0f, 0.0f};   // blurred 2: still, no light nor dust
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, back_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, back_layout_, 0, 1, &frost_set_, 0, nullptr);
        vkCmdPushConstants(cmd, back_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &ui_.set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void UiOverlay::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    destroy(ui_);
    for (Layer& l : back_) destroy(l);
    if (frost_view_) vkDestroyImageView(dev_, frost_view_, nullptr);
    if (frost_image_) vkDestroyImage(dev_, frost_image_, nullptr);
    if (frost_memory_) vkFreeMemory(dev_, frost_memory_, nullptr);
    if (pipeline_) vkDestroyPipeline(dev_, pipeline_, nullptr);
    if (back_pipeline_) vkDestroyPipeline(dev_, back_pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
    if (back_layout_) vkDestroyPipelineLayout(dev_, back_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev_, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
}
