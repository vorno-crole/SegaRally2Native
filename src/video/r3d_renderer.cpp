#include "r3d_renderer.h"

#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ao.frag.h"
#include "ao_rt.frag.h"
#include "ao_apply.frag.h"
#include "bloom.frag.h"
#include "compose.frag.h"
#include "compose.vert.h"
#include "r3d.frag.h"
#include "r3d_rt.frag.h"
#include "r3d.vert.h"
#include "r3d_rt.vert.h"
#include "tilegen.h"

namespace {

constexpr size_t TEXT_CELLS_BYTES = TextHD::ROW_TEXELS * TextHD::ROWS * 4;
constexpr size_t SKY_COORDS_BYTES = size_t(TileGen::SKY_COORDS_W) * TileGen::SKY_COORDS_H * 4;

struct ViewportPush {
    float proj[16];
    float sun[4];
    float fog_color[4];
    float params[4];    // ambient, fog density, fog start, interpolation of the previous image
    float quality[4];   // MSAA samples (negative: translucent pass; fraction: DLAA alpha noise seed),
                        // max anisotropic taps
};
static_assert(sizeof(ViewportPush) <= 128, "push constant budget");

// Uniform block of atmosphere.glsl
struct AtmosphereUniform {
    float axes[12];
    float camera[4];
    float sun[4];
    float view[4];
    float haze[4];
    float range[4];
    float backdrop[4];
    float scroll_fog[4];
    float scroll_rect[4];
    float shadow_matrix[16];
    float shadow[4];
    float soft[4];
    float mist[4];
    float sunlight[4];
    float spot[4];
    float spot_color[4];
    float spot_fog[4];
    float gi[4];
    float mirror[4];
    float rt[4];
};

// The cars, stands and poles are a few units big, so the map covers little
// ground at a fine texel (at 220 units and 2048 texels the shadows without
// ray tracing were blotchy and thin ones vanished)
constexpr uint32_t SHADOW_SIZE = 4096;   // shadow map, square
constexpr float SHADOW_RANGE = 100.0f;   // how far in front of the camera shadows are drawn (view units)

VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

}  // namespace

// Push constants of compose.frag (layout(push_constant) uniform Compose), in
// its order; what each field means is documented there
struct R3DRenderer::ComposePush {
    float scale[2], offset[2], sharpen[2], window[2], layer2d[2], filter2d[2];
    float hud[4], sky[4], effects[4], motion[4];
};

ScreenLayout compute_layout(float win_w, float win_h, float max_aspect) {
    ScreenLayout l{};
    float aspect = win_h > 0 ? win_w / win_h : 4.0f / 3.0f;
    float max_w = SCREEN_H * std::max(max_aspect, float(SCREEN_W) / SCREEN_H);
    l.view_w = std::clamp(SCREEN_H * aspect, float(SCREEN_W), max_w);
    l.scale = std::min(win_w / l.view_w, win_h / SCREEN_H);
    l.ox = (win_w - l.view_w * l.scale) / 2;
    l.oy = (win_h - SCREEN_H * l.scale) / 2;
    l.game_x0 = (l.view_w - SCREEN_W) / 2;
    return l;
}

bool R3DRenderer::create_buffer(Buffer& b, size_t size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(dev_, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    b.size = size;
    return true;
}

void R3DRenderer::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer{};
}

bool R3DRenderer::create_image(Image& img, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                               VkImageAspectFlags aspect, VkSampleCountFlagBits samples) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(dev_, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(dev_, &vi, nullptr, &img.view);
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.w = w;
    img.h = h;
    return true;
}

void R3DRenderer::destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(dev_, img.view, nullptr);
    if (img.alpha_view) vkDestroyImageView(dev_, img.alpha_view, nullptr);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.memory) vkFreeMemory(dev_, img.memory, nullptr);
    img = Image{};
}

void R3DRenderer::transition(VkCommandBuffer cmd, Image& img, VkImageLayout to, VkImageAspectFlags aspect) {
    if (img.layout == to) return;
    vk_image_barrier(cmd, img.image, img.layout, to, aspect);
    img.layout = to;
}

bool R3DRenderer::init(VkPresenter& vk, float supersample, int msaa, int aniso, bool dlaa, bool dlss_quality) {
    vk_ = &vk;
    dev_ = vk.device();
    supersample_ = std::clamp(supersample, 0.5f, 4.0f);
    aniso_ = float(std::clamp(aniso, 0, 16));
    // DLAA does the anti-aliasing itself from aliased single-sample input
    if (dlaa && dlaa_.init(vk.instance(), vk.gpu(), dev_)) {
        msaa = 1;
        upscale_ = dlss_quality ? 1.5f : 1.0f;   // DLSS's quality setting: two thirds of the size each way
    }
    // Highest supported MSAA level not above the requested one
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu(), &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples_ = VK_SAMPLE_COUNT_1_BIT;
    for (int s = 2; s <= std::min(msaa, 8); s *= 2)
        if (counts & s) samples_ = VkSampleCountFlagBits(s);

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!create_image(texture_ram_, 2048, 2048, VK_FORMAT_R16_UINT, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(back_, BACK_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_image(front_, SCREEN_W, SCREEN_H, VK_FORMAT_B8G8R8A8_UNORM, sampled, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!create_buffer(texture_staging_, 2048 * 2048 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    if (!create_image(text_cells_, TextHD::ROW_TEXELS, TextHD::ROWS, VK_FORMAT_B8G8R8A8_UNORM,
                      sampled, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_image(sky_coords_, TileGen::SKY_COORDS_W, TileGen::SKY_COORDS_H, VK_FORMAT_B8G8R8A8_UNORM, sampled,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!create_buffer(layer_staging_, (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES + SKY_COORDS_BYTES,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    if (!create_buffer(vertices_, 8u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
    if (!create_buffer(atmosphere_uniform_, sizeof(AtmosphereUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    memset(atmosphere_uniform_.mapped, 0, sizeof(AtmosphereUniform));

    // Clear the texture RAM and the 2D layers once
    if (!create_image(shadow_map_, SHADOW_SIZE, SHADOW_SIZE, VK_FORMAT_D32_SFLOAT,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    if (!create_image(exposure_, 1, 1, VK_FORMAT_R16_SFLOAT,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    vk.run_once([&](VkCommandBuffer cmd) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkClearColorValue start{};
        start.float32[0] = std::log(0.17f);   // a typical scene: exposure 1
        VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, exposure_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &start, 1, &one);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_}) {
            transition(cmd, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            transition(cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &nearest_);
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(dev_, &si, nullptr, &linear_);
    // Replacement textures: trilinear + anisotropic, repeating (mirroring is done in the shader)
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = std::min(16.0f, props.limits.maxSamplerAnisotropy);
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev_, &si, nullptr, &replace_sampler_);
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &si, nullptr, &overlay_sampler_);

    // Ray tracing, when the GPU has it: its acceleration structure is built once
    // empty here, so the descriptors always point at a valid one
    if (rt_.init(vk)) printf("[video] ray tracing available (sun shadows)\n");

    // Descriptor sets: texture RAM for the 3D pass; 3D image + two 2D layers for composition;
    // both also see the atmosphere parameters
    // (what the traced rays need is also the vertex shader's: it traces the
    // light of each puff of dust, r3d.vert)
    const VkShaderStageFlags rt_stages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    // Texture pack slots: as many as the GPU allows in one shader stage, up to
    // TexturePack::MAX_SLOTS (Intel drivers allow few without update-after-bind)
    const bool uab = vk.update_after_bind();
    {
        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &p12;
        vkGetPhysicalDeviceProperties2(vk.gpu(), &p2);
        const VkPhysicalDeviceLimits& l = p2.properties.limits;
        uint64_t limit = uab ? std::min({p12.maxPerStageDescriptorUpdateAfterBindSamplers,
                                         p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                         p12.maxDescriptorSetUpdateAfterBindSamplers,
                                         p12.maxDescriptorSetUpdateAfterBindSampledImages,
                                         p12.maxPerStageUpdateAfterBindResources})
                             : std::min({l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages,
                                         l.maxDescriptorSetSamplers, l.maxDescriptorSetSampledImages,
                                         l.maxPerStageResources});
        // (16: room for the stage's other textures, buffers and attachments)
        texture_slots_ = uint32_t(std::clamp<int64_t>(int64_t(limit) - 16, 2, TexturePack::MAX_SLOTS));
        if (texture_slots_ < TexturePack::MAX_SLOTS)
            printf("[video] texture pack limited to %u images by the GPU (%s limit %llu)\n", texture_slots_,
                   uab ? "update-after-bind" : "descriptor", (unsigned long long)limit);
    }
    VkDescriptorSetLayoutBinding b0[7] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, rt_stages},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_slots_, rt_stages},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, rt_stages},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // sun shadow map (no 3)
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},   // world depth (soft particles)
        {6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, rt_stages},   // ray tracing: the world
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, rt_stages},   // ray tracing: cut-out triangles
    };
    const uint32_t r3d_bindings = rt_.ready() ? 7 : 5;
    // Replacement slots are filled while earlier frames may still be in flight
    const VkDescriptorBindingFlags pending =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags bflags[7] = {
        0, pending | (uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0u), 0, 0, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = r3d_bindings;
    bfi.pBindingFlags = bflags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.flags = uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0u;
    li.bindingCount = r3d_bindings;
    li.pBindings = b0;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &r3d_set_layout_);
    li.flags = 0;
    li.pNext = nullptr;
    // 5: high-resolution text atlas (only bound when there is one), 6: text cells,
    // 7: alpha of the 3D image (in red), 8: bloom, 9: backdrop replacement coordinates,
    // 11: exposure, 12: diffusion, 13: tight bloom (no 10)
    VkDescriptorSetLayoutBinding cb[13];
    for (uint32_t i = 0; i < 3; i++)
        cb[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Pack2D::MAX_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[10] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[11] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    cb[12] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorBindingFlags cflags[13] = {0, 0, 0, pending, 0, pending, 0, 0, 0, 0, 0, 0, 0};
    bfi.bindingCount = 13;
    bfi.pBindingFlags = cflags;
    li.pNext = &bfi;
    li.bindingCount = 13;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &compose_set_layout_);
    li.pNext = nullptr;
    // Bloom passes: one source image each
    li.bindingCount = 1;
    li.pBindings = cb;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &bloom_set_layout_);
    // Ambient occlusion: world depth and the raw occlusion
    li.bindingCount = 2;
    vkCreateDescriptorSetLayout(dev_, &li, nullptr, &ao_set_layout_);

    VkDescriptorPoolSize ps[4] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 36 + texture_slots_ + Pack2D::MAX_SLOTS},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 10;
    if (uab) pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.poolSizeCount = rt_.ready() ? 4 : 2;
    pi.pPoolSizes = ps;
    vkCreateDescriptorPool(dev_, &pi, nullptr, &pool_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &r3d_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &r3d_set_);
    ai.pSetLayouts = &compose_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &compose_set_);
    ai.pSetLayouts = &bloom_set_layout_;
    for (VkDescriptorSet& s : bloom_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    for (VkDescriptorSet& s : diff_sets_) vkAllocateDescriptorSets(dev_, &ai, &s);
    ai.pSetLayouts = &ao_set_layout_;
    vkAllocateDescriptorSets(dev_, &ai, &ao_set_);

    VkDescriptorImageInfo tex{nearest_, texture_ram_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = r3d_set_;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &tex;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo shadow{nearest_, shadow_map_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 4;
    w.pImageInfo = &shadow;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    if (rt_.ready()) {
        VkAccelerationStructureKHR tlas = rt_.tlas();
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet aw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        aw.pNext = &as;
        aw.dstSet = r3d_set_;
        aw.dstBinding = 6;
        aw.descriptorCount = 1;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        aw.pNext = nullptr;
        aw.dstBinding = 7;
        aw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        aw.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &aw, 0, nullptr);
        rt_generation_ = rt_.generation();
    }
    w.dstBinding = 0;
    VkDescriptorBufferInfo atmosphere_info{atmosphere_uniform_.buffer, 0, sizeof(AtmosphereUniform)};
    VkWriteDescriptorSet fw[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    for (int i = 0; i < 2; i++) {
        fw[i].dstSet = i == 0 ? r3d_set_ : compose_set_;
        fw[i].dstBinding = i == 0 ? 2 : 4;
        fw[i].descriptorCount = 1;
        fw[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        fw[i].pBufferInfo = &atmosphere_info;
    }
    vkUpdateDescriptorSets(dev_, 2, fw, 0, nullptr);
    VkDescriptorImageInfo exposure{nearest_, exposure_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 11;
    w.pImageInfo = &exposure;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo cells{nearest_, text_cells_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstSet = compose_set_;
    w.dstBinding = 6;
    w.pImageInfo = &cells;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkDescriptorImageInfo sky{nearest_, sky_coords_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    w.dstBinding = 9;
    w.pImageInfo = &sky;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);

    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ViewportPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r3d_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &r3d_layout_);
    VkPushConstantRange cpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ComposePush)};
    pli.pSetLayouts = &compose_set_layout_;
    pli.pPushConstantRanges = &cpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &compose_layout_);
    VkPushConstantRange bpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    pli.pSetLayouts = &bloom_set_layout_;
    pli.pPushConstantRanges = &bpr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &bloom_layout_);
    VkPushConstantRange apr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96};
    // Traced, the ambient occlusion also sees the 3D pass's set (the world's rays)
    VkDescriptorSetLayout ao_sets[2] = {ao_set_layout_, r3d_set_layout_};
    pli.setLayoutCount = rt_.ready() ? 2 : 1;
    pli.pSetLayouts = ao_sets;
    pli.pPushConstantRanges = &apr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &ao_layout_);

    return create_pipelines();
}

VkPipeline R3DRenderer::make_pipeline(const PipelineDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.fs, "main"};

    VkVertexInputBindingDescription binding{0, sizeof(R3DVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[9] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, nrm)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(R3DVertex, uv)},
        {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(R3DVertex, color)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, tex)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, flags)},
        {6, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, shade)},
        {7, 0, VK_FORMAT_R32_UINT, offsetof(R3DVertex, replace)},
        {8, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(R3DVertex, prev)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (d.vertex_input) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 9;
        vi.pVertexAttributeDescriptions = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // facing is decided on the CPU with the face normal
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth;
    ds.depthWriteEnable = d.depth_write;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse Z
    if (d.once) {
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER;
    }
    if (d.bias != 0) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = d.bias;
    }
    VkPipelineColorBlendAttachmentState cbas[3]{};
    VkPipelineColorBlendAttachmentState& cba = cbas[0];
    cba.colorWriteMask = 0xF;
    // Motion vectors: written by the opaque pass only, translucent surfaces keep what is behind
    cbas[1].colorWriteMask = d.depth_write ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT : 0;
    if (d.blend) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.additive) {   // colour = colour + output (the sky's glow added to the bloom)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (d.ease > 0) {   // colour moves a fraction of the way towards the output (the exposure)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    }
    if (d.multiply) {   // colour = colour * output (ambient occlusion); alpha kept
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    }
    // Bloom source: blended like the colour. With it the motion slot is always
    // there (without an image when DLAA is off)
    cbas[2] = cba;
    if (d.depth_only)
        for (auto& c : cbas) c.colorWriteMask = 0;
    uint32_t attachments = d.glow ? 3 : d.motion ? 2 : 1;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = attachments;
    cb.blendConstants[3] = d.ease;
    cb.pAttachments = cbas;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rend{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat formats[3] = {d.color, d.motion ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_R8G8B8A8_UNORM};
    rend.colorAttachmentCount = attachments;
    rend.pColorAttachmentFormats = formats;
    rend.depthAttachmentFormat = d.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

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
    gp.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &p) != VK_SUCCESS)
        fprintf(stderr, "[vulkan] could not create a pipeline\n");
    return p;
}

bool R3DRenderer::create_pipelines() {
    VkShaderModule r3d_vs = rt_.ready() ? make_module(dev_, spv_r3d_rt_vert, sizeof(spv_r3d_rt_vert))
                                        : make_module(dev_, spv_r3d_vert, sizeof(spv_r3d_vert));
    // With ray queries the 3D pass can also trace the sun shadows
    VkShaderModule r3d_fs = rt_.ready() ? make_module(dev_, spv_r3d_rt_frag, sizeof(spv_r3d_rt_frag))
                                        : make_module(dev_, spv_r3d_frag, sizeof(spv_r3d_frag));
    VkShaderModule c_vs = make_module(dev_, spv_compose_vert, sizeof(spv_compose_vert));
    VkShaderModule c_fs = make_module(dev_, spv_compose_frag, sizeof(spv_compose_frag));
    VkShaderModule b_fs = make_module(dev_, spv_bloom_frag, sizeof(spv_bloom_frag));
    VkShaderModule ao_fs = rt_.ready() ? make_module(dev_, spv_ao_rt_frag, sizeof(spv_ao_rt_frag))
                                       : make_module(dev_, spv_ao_frag, sizeof(spv_ao_frag));
    VkShaderModule aoa_fs = make_module(dev_, spv_ao_apply_frag, sizeof(spv_ao_apply_frag));
    bool motion = motion_pass_ = dlaa_.ready();
    // The world: 3D geometry into the HDR image, with the motion and glow slots
    PipelineDesc world{.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                       .depth = true, .depth_write = true, .vertex_input = true, .samples = samples_,
                       .motion = motion, .glow = true};
    opaque_ = make_pipeline(world);
    PipelineDesc translucent = world;
    translucent.depth_write = false;
    translucent.blend = true;
    translucent_ = make_pipeline(translucent);
    PipelineDesc trans_depth = world;
    trans_depth.depth_only = true;
    trans_depth_ = make_pipeline(trans_depth);
    // The game's dark patch under each car (its shadow), made of several
    // shapes laid over each other: blended over one another they came out
    // nearly black. It is blended once at each pixel instead: its nearest depth
    // goes first a hair farther (blob_depth_; 1 unit, the least that also
    // leaves no seam where two of its shapes cross), then the first of it to
    // pass, nearer than that, takes the pixel (blob_)
    PipelineDesc blob = translucent;
    blob.once = true;
    blob_ = make_pipeline(blob);
    PipelineDesc blob_depth = trans_depth;
    blob_depth.bias = -1.0f;
    blob_depth_ = make_pipeline(blob_depth);
    shadow_ = make_pipeline({.vs = r3d_vs, .fs = r3d_fs, .layout = r3d_layout_, .color = VK_FORMAT_UNDEFINED,
                             .depth = true, .depth_write = true, .vertex_input = true});
    // Full-screen passes: a triangle over the target, no vertex input
    compose_format_ = vk_->swap_format();
    PipelineDesc compose{.vs = c_vs, .fs = c_fs, .layout = compose_layout_, .color = compose_format_};
    compose_ = make_pipeline(compose);
    PipelineDesc blur{.vs = c_vs, .fs = b_fs, .layout = bloom_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM};
    bloom_ = make_pipeline(blur);
    // A twentieth of the way per frame: the eye adapts over about a second
    PipelineDesc measure = compose;
    measure.color = VK_FORMAT_R16_SFLOAT;
    measure.ease = 0.05f;
    compose_measure_ = make_pipeline(measure);
    PipelineDesc glow = compose;
    glow.color = VK_FORMAT_R8G8B8A8_UNORM;
    glow.additive = true;
    compose_glow_ = make_pipeline(glow);
    ao_pass_ = make_pipeline({.vs = c_vs, .fs = ao_fs, .layout = ao_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT});
    ao_apply_ = make_pipeline({.vs = c_vs, .fs = aoa_fs, .layout = ao_layout_, .color = VK_FORMAT_R16G16B16A16_SFLOAT,
                               .samples = samples_, .multiply = true});
    ao_apply_glow_ = make_pipeline({.vs = c_vs, .fs = aoa_fs, .layout = ao_layout_, .color = VK_FORMAT_R8G8B8A8_UNORM,
                                    .samples = samples_, .multiply = true});
    for (auto m : {r3d_vs, r3d_fs, c_vs, c_fs, b_fs, ao_fs, aoa_fs}) vkDestroyShaderModule(dev_, m, nullptr);
    return opaque_ && translucent_ && blob_ && blob_depth_ && trans_depth_ && shadow_ && compose_measure_ && compose_ && bloom_ && ao_pass_ && ao_apply_ && ao_apply_glow_;
}

// w, h: the 3D image as drawn; up_w, up_h: as DLSS hands it on (the same but with DLSS upscaling)
void R3DRenderer::ensure_targets(uint32_t w, uint32_t h, uint32_t up_w, uint32_t up_h) {
    if (color_.image && color_.w == w && color_.h == h && (!dlaa_out_.image || (dlaa_out_.w == up_w && dlaa_out_.h == up_h)))
        return;
    vkDeviceWaitIdle(dev_);
    destroy_image(color_);
    destroy_image(color_ms_);
    destroy_image(depth_);
    destroy_image(motion_);
    destroy_image(dlaa_out_);
    destroy_image(dlaa_alpha_);
    for (Image* img : {&glow_, &glow_ms_, &bloom_a_, &bloom_b_, &bloom_c_, &depth_resolved_, &ao_, &diff_a_, &diff_b_})
        destroy_image(*img);
    create_image(color_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo avi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    avi.image = color_.image;
    avi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    avi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    avi.components = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE};
    avi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev_, &avi, nullptr, &color_.alpha_view);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        create_image(color_ms_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    bool dlaa = motion_pass_;
    create_image(depth_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT, samples_);
    // The world's depth, kept after its opaque surfaces (sample 0 of it with
    // MSAA, a copy without) for the ambient occlusion and the soft particles;
    // and the raw occlusion
    create_image(depth_resolved_, w, h, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The occlusion at half resolution (one pixel in four): ao_apply.frag
    // brings it back to full size along the depth edges. (Its colour: the
    // bounced light of the global illumination)
    create_image(ao_, (w + 1) / 2, (h + 1) / 2, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[2] = {
            {nearest_, depth_resolved_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_, ao_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet aw[2];
        for (int i = 0; i < 2; i++) {
            aw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            aw[i].dstSet = ao_set_;
            aw[i].dstBinding = uint32_t(i);
            aw[i].descriptorCount = 1;
            aw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            aw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 2, aw, 0, nullptr);
        // ... and for the soft particles in the 3D pass
        VkWriteDescriptorSet dw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        dw.dstSet = r3d_set_;
        dw.dstBinding = 5;
        dw.descriptorCount = 1;
        dw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dw.pImageInfo = &src[0];
        vkUpdateDescriptorSets(dev_, 1, &dw, 0, nullptr);
    }
    if (dlaa) {
        create_image(motion_, w, h, VK_FORMAT_R16G16_SFLOAT,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_out_, up_w, up_h, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        create_image(dlaa_alpha_, up_w, up_h, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // Bloom: glow written by the 3D pass, then reduced to a quarter and blurred
    const VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create_image(glow_, w, h, VK_FORMAT_R8G8B8A8_UNORM, target, VK_IMAGE_ASPECT_COLOR_BIT);
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        create_image(glow_ms_, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, samples_);
    for (Image* img : {&bloom_a_, &bloom_b_, &bloom_c_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[6] = {{linear_, glow_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_c_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, bloom_c_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet bw[6];
        for (int i = 0; i < 6; i++) {
            bw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            bw[i].dstSet = i < 4 ? bloom_sets_[i] : compose_set_;
            bw[i].dstBinding = i < 4 ? 0 : i == 4 ? 8 : 13;
            bw[i].descriptorCount = 1;
            bw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 6, bw, 0, nullptr);
    }
    // Diffusion: the picture behind the HUD and its blur, a quarter of the size
    for (Image* img : {&diff_a_, &diff_b_})
        create_image(*img, std::max(1u, w / 4), std::max(1u, h / 4), VK_FORMAT_R8G8B8A8_UNORM, target,
                     VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkDescriptorImageInfo src[3] = {{linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_b_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {linear_, diff_a_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet dw[3];
        for (int i = 0; i < 3; i++) {
            dw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            dw[i].dstSet = i < 2 ? diff_sets_[i] : compose_set_;
            dw[i].dstBinding = i < 2 ? 0 : 12;
            dw[i].descriptorCount = 1;
            dw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            dw[i].pImageInfo = &src[i];
        }
        vkUpdateDescriptorSets(dev_, 3, dw, 0, nullptr);
    }
    composed_view_ = VK_NULL_HANDLE;
    if (dlaa_.ready()) bind_scene_image(dlaa_out_.view, dlaa_alpha_.view);
    else bind_scene_image(color_.view, color_.alpha_view);
    VkDescriptorImageInfo infos[3] = {
        {linear_, color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, front_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {nearest_, back_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w3[3];
    for (uint32_t i = 1; i < 3; i++) {
        w3[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w3[i].dstSet = compose_set_;
        w3[i].dstBinding = i;
        w3[i].descriptorCount = 1;
        w3[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w3[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev_, 2, w3 + 1, 0, nullptr);
}

// Bindings 0 and 7 of the composition: the plain 3D image or the DLAA output,
// and its alpha
void R3DRenderer::bind_scene_image(VkImageView view, VkImageView alpha) {
    if (view == composed_view_) return;
    VkDescriptorImageInfo info[2] = {{linear_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                     {linear_, alpha, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2];
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = compose_set_;
        w[i].dstBinding = i ? 7 : 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &info[i];
    }
    if (composed_view_) vkDeviceWaitIdle(dev_);   // the previous frame may still be reading it
    vkUpdateDescriptorSets(dev_, 2, w, 0, nullptr);
    composed_view_ = view;
}

void R3DRenderer::set_layers(const uint32_t* back, const uint32_t* front) {
    uint8_t* dst = static_cast<uint8_t*>(layer_staging_.mapped);
    memcpy(dst, back, BACK_W * SCREEN_H * 4);
    memcpy(dst + BACK_W * SCREEN_H * 4, front, SCREEN_W * SCREEN_H * 4);
    layers_dirty_ = true;
    // Average colour of each row of the back layer, for the horizon colour
    for (int y = 0; y < SCREEN_H; y++) {
        float sum[3] = {0, 0, 0}, n = 0;
        for (int x = 0; x < BACK_W; x += 2) {
            uint32_t p = back[y * BACK_W + x];
            float a = float(p >> 24);
            if (a < 128) continue;
            sum[0] += float((p >> 16) & 255) / a;   // premultiplied
            sum[1] += float((p >> 8) & 255) / a;
            sum[2] += float(p & 255) / a;
            n++;
        }
        for (int i = 0; i < 3; i++) back_rows_[y * 4 + i] = sum[i];
        back_rows_[y * 4 + 3] = n;
    }
    {   // how bright the backdrop is on the whole (a snowfield's pale sky is all bright)
        double l = 0, n = 0;
        for (int y = 0; y < SCREEN_H; y++) {
            l += 0.2126 * back_rows_[y * 4] + 0.7152 * back_rows_[y * 4 + 1] + 0.0722 * back_rows_[y * 4 + 2];
            n += back_rows_[y * 4 + 3];
        }
        back_luma_ = n > 0 ? float(l / n) : 0.0f;
    }
}

void R3DRenderer::set_text_cells(const uint32_t* cells) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4, cells,
           TEXT_CELLS_BYTES);
}

void R3DRenderer::set_sky_coords(const uint32_t* coords) {
    memcpy(static_cast<uint8_t*>(layer_staging_.mapped) + (BACK_W + SCREEN_W) * SCREEN_H * 4 + TEXT_CELLS_BYTES, coords,
           SKY_COORDS_BYTES);
}

void R3DRenderer::set_text_atlas(const TextHD& text) {
    if (text.atlas_rgba.empty()) return;
    upload_slot(compose_set_, 5, overlay_sampler_, 0, text.atlas_w, text.atlas_h, text.atlas_rgba);
}

void R3DRenderer::upload_texture_rows(const std::vector<uint16_t>& rows, int y0, int y1) {
    memcpy(static_cast<uint8_t*>(texture_staging_.mapped) + size_t(y0) * 2048 * 2, rows.data(), rows.size() * 2);
    if (tex_y0_ >= tex_y1_) {
        tex_y0_ = y0;
        tex_y1_ = y1;
    } else {
        tex_y0_ = std::min(tex_y0_, y0);
        tex_y1_ = std::max(tex_y1_, y1);
    }
}

void R3DRenderer::upload_slot(VkDescriptorSet set, uint32_t binding, VkSampler sampler, uint32_t slot, int w, int h,
                              const std::vector<uint8_t>& rgba, bool replace) {
    {
        uint32_t mips = 1;
        while ((std::max(w, h) >> mips) > 0) mips++;
        Image img;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {uint32_t(w), uint32_t(h), 1};
        ii.mipLevels = mips;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(dev_, &ii, nullptr, &img.image) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev_, img.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev_, &ai, nullptr, &img.memory) != VK_SUCCESS) {
            vkDestroyImage(dev_, img.image, nullptr);
            return;
        }
        vkBindImageMemory(dev_, img.image, img.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &img.view);
        img.w = uint32_t(w);
        img.h = uint32_t(h);

        Buffer staging;
        if (!create_buffer(staging, rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return;
        memcpy(staging.mapped, rgba.data(), rgba.size());
        vk_->run_once([&](VkCommandBuffer cmd) {
            auto barrier = [&](uint32_t level, VkImageLayout from, VkImageLayout to) {
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = img.image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &b);
            };
            barrier(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {img.w, img.h, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            // Mip chain: each level is a filtered blit of the previous one
            for (uint32_t l = 1; l < mips; l++) {
                barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                barrier(l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                blit.srcOffsets[1] = {int32_t(std::max(1u, img.w >> (l - 1))), int32_t(std::max(1u, img.h >> (l - 1))), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                blit.dstOffsets[1] = {int32_t(std::max(1u, img.w >> l)), int32_t(std::max(1u, img.h >> l)), 1};
                vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            barrier(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        destroy_buffer(staging);
        img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo info{sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.dstBinding = binding;
        wr.dstArrayElement = slot;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &info;
        if (replace) {   // the slot's previous picture is no longer needed
            auto old = overlay_images_.find(slot);
            if (old != overlay_images_.end()) {
                vkDeviceWaitIdle(dev_);   // frames in flight may still read it
                destroy_image(old->second);
            }
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
            overlay_images_[slot] = img;
            return;
        }
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        replacements_.push_back(img);
    }
}

void R3DRenderer::add_replacements(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(r3d_set_, 1, replace_sampler_, src.slot, src.w, src.h, src.rgba);
}

void R3DRenderer::add_overlays(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, true);
}

void R3DRenderer::add_sky_pictures(const std::vector<RgbaImage>& images) {
    for (const RgbaImage& src : images)
        upload_slot(compose_set_, 3, overlay_sampler_, src.slot, src.w, src.h, src.rgba, true);
}

// The background layers continue beside the 4:3 area only while the 3D
// scene is widened too; a small 3D window (camera effects) keeps black sides
bool R3DRenderer::back_layer_extended() const {
    if (!scene_ || scene_->viewports.empty()) return true;
    int scene_priority = scene_->viewports.front().priority;
    for (const R3DViewport& v : scene_->viewports)
        if (v.priority == scene_priority && v.x <= 0.5f && v.w >= SCREEN_W - 0.5f) return true;
    return false;
}

// Scroll fog (spec 7.4): from the lowest priority that has any, the viewport
// with the strongest scroll fog. It is drawn only when a viewport with the
// same fog colour actually fogs something.
static const R3DViewport* scroll_fog_viewport(const std::vector<R3DViewport>& vps) {
    const R3DViewport* best = nullptr;
    for (const R3DViewport& v : vps) {   // sorted by priority
        if (best && v.priority != best->priority) break;
        if (v.scroll_fog > 0 && (!best || v.scroll_fog > best->scroll_fog)) best = &v;
    }
    if (!best) return nullptr;
    auto same = [&](const float* c) { return c[0] == best->fog_color[0] && c[1] == best->fog_color[1] && c[2] == best->fog_color[2]; };
    for (const R3DViewport& v : vps) {
        bool spot = v.fog_attenuation > 0 && (v.spot_fog_color[0] + v.spot_fog_color[1] + v.spot_fog_color[2]) > 0;
        if (same(v.fog_color) && (v.fog_density > 0 || v.fog_start > 0 || v.scroll_attenuation > 0 || spot)) return best;
    }
    return nullptr;
}

// The world viewport's camera between the previous game frame and the latest
// one, as the scenery is drawn (the interpolation); as in the latest frame
// after a cut (a jump of the camera, a sudden turn)
R3DRenderer::WorldPose R3DRenderer::world_pose(const R3DViewport* w) {
    if (pose_serial_ != scene_serial_) {
        pose_serial_ = scene_serial_;
        pose_prev_ = pose_now_;
        pose_now_.valid = w != nullptr;
        if (w) {
            memcpy(pose_now_.axes, w->world_axes, sizeof(pose_now_.axes));
            memcpy(pose_now_.camera, w->camera, sizeof(pose_now_.camera));
            float sl = std::sqrt(w->sun[0] * w->sun[0] + w->sun[1] * w->sun[1] + w->sun[2] * w->sun[2]);
            for (int i = 0; i < 3; i++) pose_now_.sun[i] = sl > 0 ? w->sun[i] / sl : 0;
        }
    }
    const WorldPose &a = pose_prev_, &b = pose_now_;
    auto dot = [](const float* p, const float* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
    float d[3] = {b.camera[0] - a.camera[0], b.camera[1] - a.camera[1], b.camera[2] - a.camera[2]};
    pose_cut_ = !a.valid || !b.valid || dot(d, d) > 10.0f * 10.0f || dot(a.axes + 6, b.axes + 6) < 0.9f ||
                dot(a.axes + 3, b.axes + 3) < 0.9f;
    if (pose_cut_ || interp_ >= 1.0f) return b;
    WorldPose p = b;
    float t = interp_;
    for (int i = 0; i < 9; i++) p.axes[i] = a.axes[i] + (b.axes[i] - a.axes[i]) * t;
    for (int i = 0; i < 3; i++) p.camera[i] = a.camera[i] + (b.camera[i] - a.camera[i]) * t;
    for (int i = 0; i < 3; i++) p.sun[i] = a.sun[i] + (b.sun[i] - a.sun[i]) * t;
    for (float* v : {p.axes, p.axes + 3, p.axes + 6, p.sun}) {   // back to unit length
        float l = std::sqrt(dot(v, v));
        if (l > 0)
            for (int i = 0; i < 3; i++) v[i] /= l;
    }
    return p;
}

// Atmosphere parameters for this frame (atmosphere.glsl)
void R3DRenderer::update_atmosphere(const ScreenLayout& lay, uint32_t target_w, uint32_t target_h) {
    AtmosphereUniform u{};
    rt_frame_ = rt_ao_ = rt_gi_ = false;
    u.backdrop[0] = 1.0f / float(target_w);
    u.backdrop[1] = 1.0f / float(target_h);
    // The original scroll fog, laid over the 2D layers behind the 3D image
    const R3DViewport* sf = scene_ && !scene_->viewports.empty() ? scroll_fog_viewport(scene_->viewports) : nullptr;
    if (sf) {
        for (int i = 0; i < 3; i++) u.scroll_fog[i] = sf->fog_color[i] * sf->fog_ambient;
        u.scroll_fog[3] = sf->scroll_fog;
        // Its rectangle in the 3D image, widened like the viewport when it fills the screen
        bool full = sf->x <= 0.5f && sf->w >= SCREEN_W - 0.5f;
        float x0 = full ? 0.0f : sf->x + lay.game_x0, w = full ? lay.view_w : sf->w;
        u.scroll_rect[0] = x0 / lay.view_w;
        u.scroll_rect[1] = sf->y / SCREEN_H;
        u.scroll_rect[2] = (x0 + w) / lay.view_w;
        u.scroll_rect[3] = (sf->y + sf->h) / SCREEN_H;
    }
    const R3DViewport* w = nullptr;
    if (scene_)   // the sun direction is also used by the sky's sun
        for (const R3DViewport& v : scene_->viewports)
            if (v.world) w = &v;
    WorldPose pose = world_pose(w);
    if (w) {
        // Game time (about 57.5 frames per second), so a replay drifts the same way
        float time = (float(scene_->frame % 100000) + interp_) / 57.5f;
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 3; i++) u.axes[k * 4 + i] = pose.axes[k * 3 + i];
        u.camera[0] = pose.camera[0];
        u.camera[1] = pose.camera[1];
        u.camera[2] = pose.camera[2];
        u.camera[3] = time;
        for (int i = 0; i < 3; i++) u.sun[i] = pose.sun[i];
        u.sun[3] = w->sun_intensity;
        static const char* force_sun = getenv("LW_FOGSUN");   // debugging aid: "x,y,z" in view space
        if (force_sun) {
            float f[3] = {0, 0, -1};
            sscanf(force_sun, "%f,%f,%f", &f[0], &f[1], &f[2]);
            float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            for (int i = 0; i < 3; i++) u.sun[i] = f[i] / fl;
            u.sun[3] = 1;
        }
        // Projection as used for the world viewport (widened when it fills the screen)
        bool full = w->x <= 0.5f && w->w >= SCREEN_W - 0.5f;
        float widen = full ? lay.view_w / SCREEN_W : 1.0f;
        u.view[0] = w->proj[0] / widen;
        u.view[1] = w->proj[5];
        u.view[2] = w->proj[8] / widen;
        u.view[3] = w->proj[9];

        // Distance haze. Horizon colour: a pale haze, tinted by the back
        // layer just above the horizon (where a level view direction meets
        // the screen), which is the sky or the painted far landscape
        float haze[3] = {0.60f, 0.66f, 0.64f};
        float target[3] = {haze[0], haze[1], haze[2]};
        const float* up = pose.axes + 3;
        if (std::fabs(up[1]) > 0.2f && u.view[0] != 0 && u.view[1] != 0) {
            float ndc_y = (up[2] - up[0] * u.view[2] / u.view[0]) * u.view[1] / up[1] - u.view[3];
            int row = int((ndc_y * 0.5f + 0.5f) * SCREEN_H);
            float sum[3] = {0, 0, 0}, n = 0;
            for (int y = std::max(row - 48, 0); y < std::min(row - 4, SCREEN_H); y++) {
                for (int i = 0; i < 3; i++) sum[i] += back_rows_[y * 4 + i];
                n += back_rows_[y * 4 + 3];
            }
            if (n > 500)
                for (int i = 0; i < 3; i++) {
                    float sky = sum[i] / n;
                    sky += (u.scroll_fog[i] - sky) * u.scroll_fog[3];
                    target[i] = 0.3f * sky + 0.7f * haze[i];
                }
        }
        for (int i = 0; i < 3; i++) haze_color_[i] += 0.05f * (target[i] - haze_color_[i]);
        for (int i = 0; i < 3; i++) u.haze[i] = haze_color_[i];
        u.haze[3] = haze_strength_;
        u.range[1] = std::max(w->scenery_far, 50.0f);
        u.range[0] = 0.1f * u.range[1];   // the tint starts past the near scenery
        u.range[2] = 0.6f;          // desaturation

        // World up and the camera's height above the ground: what may cast
        // sun shadows (the shadow map, the traced ones)
        for (int i = 0; i < 3; i++) shadow_up_[i] = pose.axes[3 + i];
        shadow_height_ = pose.camera[1] - w->ground;
        // Ray tracing: the ambient occlusion through the world (all of it)
        if (ray_tracing_ >= 2 && rt_.ready() && ao_strength_ > 0) {
            rt_frame_ = rt_ao_ = true;
            rt_world_ = w;
        }
        // ... and the global illumination: the sun's light bounced off it
        // (where there is a sun)
        if (gi_ && ray_tracing_ >= 1 && rt_.ready() && u.sun[3] > 0) {
            rt_frame_ = rt_gi_ = true;
            rt_world_ = w;
        }
        // Sun shadows: an orthographic view from the sun covering what the
        // camera sees up to SHADOW_RANGE, plus whatever stands between it and the sun
        // (not indoors: a small world under a roof has no sun)
        bool indoors = w->scenery_far > 0 && w->scenery_far < 120.0f;
        shadow_frame_ = shadow_strength_ > 0 && u.sun[3] > 0 && u.view[0] != 0 && u.view[1] != 0 && !indoors;
        if (shadow_frame_) {
            const float* L = u.sun;   // towards the sun (view space)
            float sun_up[3] = {0, 1, 0};   // any axis not parallel to the sun
            if (std::fabs(L[1]) > 0.95f) sun_up[0] = 1, sun_up[1] = 0;
            auto cross = [](const float* a, const float* b, float* o) {
                o[0] = a[1] * b[2] - a[2] * b[1];
                o[1] = a[2] * b[0] - a[0] * b[2];
                o[2] = a[0] * b[1] - a[1] * b[0];
            };
            float X[3], Y[3];
            cross(sun_up, L, X);
            float xl = std::sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]);
            for (float& v : X) v /= xl;
            cross(L, X, Y);
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (float z : {1.0f, SHADOW_RANGE})
                for (float nx : {-1.0f, 1.0f})
                    for (float ny : {-1.0f, 1.0f}) {
                        float p[3] = {z * (nx + u.view[2]) / u.view[0], z * (ny + u.view[3]) / u.view[1], -z};
                        float l[3] = {p[0] * X[0] + p[1] * X[1] + p[2] * X[2], p[0] * Y[0] + p[1] * Y[1] + p[2] * Y[2],
                                      p[0] * L[0] + p[1] * L[1] + p[2] * L[2]};
                        for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], l[i]), hi[i] = std::max(hi[i], l[i]);
                    }
            // Square, on whole texels, so the shadows do not crawl as the camera turns
            float size = std::max(hi[0] - lo[0], hi[1] - lo[1]);
            float texel = size / float(SHADOW_SIZE);
            float cx = std::floor((lo[0] + hi[0]) * 0.5f / texel) * texel, cy = std::floor((lo[1] + hi[1]) * 0.5f / texel) * texel;
            float zmin = lo[2], zmax = hi[2] + 400.0f, zr = zmax - zmin;
            float* m = shadow_matrix_;   // column-major
            for (int i = 0; i < 3; i++) {
                m[i * 4 + 0] = X[i] * 2.0f / size;
                m[i * 4 + 1] = Y[i] * 2.0f / size;
                m[i * 4 + 2] = L[i] / zr;
                m[i * 4 + 3] = 0;
            }
            m[12] = -cx * 2.0f / size;
            m[13] = -cy * 2.0f / size;
            m[14] = -zmin / zr;
            m[15] = 1;
            memcpy(u.shadow_matrix, m, sizeof(u.shadow_matrix));
            // (All of the sun's light is cut; 0.75 of it at first: beside the arcade, whose shadow under the car is black,
            // the sun's shadows were faint, more so with the light the global
            // illumination puts back into them. SR2_SHADOW_DEPTH: debugging aid)
            static const float depth = getenv("SR2_SHADOW_DEPTH") ? float(atof(getenv("SR2_SHADOW_DEPTH"))) : 1.0f;
            u.shadow[0] = std::min(shadow_strength_, 1.0f) * depth;
            u.shadow[1] = 1.0f / float(SHADOW_SIZE);
            u.shadow[2] = 2.5f * texel;   // normal offset
            u.shadow[3] = 1.5f * texel / zr;   // depth bias
            // Ray tracing: the same shadows traced through the world instead (no map drawn)
            if (ray_tracing_ >= 1 && rt_.ready()) {
                rt_frame_ = true;
                rt_world_ = w;
                static const bool split = getenv("LW_RTSPLIT");   // debugging aid: traced left, shadow map right
                shadow_frame_ = split;
                u.rt[0] = split ? 2.0f : 1.0f;
                // A little more than the real sun's 0.27 degrees: softer edges. Only
                // the penumbra traces them all (r3d.frag traced_shadow): 4 left its
                // edges grainy, 16 grade them smoothly
                u.rt[1] = 0.016f;
                static const bool probe_alone = getenv("SR2_SHADOW_FOUR") != nullptr;   // debugging aid: every pixel its own four rim rays
                static const float sun_rays = getenv("SR2_SHADOW_RAYS") ? float(atoi(getenv("SR2_SHADOW_RAYS"))) : 24.0f;   // debugging aid
                // (.5: debugging; .25: the rays inside the disc every pixel's own. Shared
                // across blocks of 2x2 pixels, they need DLAA to tell the pixels apart again)
                static const bool disc_shared = !getenv("SR2_SHADOW_DISC_ALONE");   // debugging aid
                u.rt[2] = sun_rays + (probe_alone ? 0.5f : dlaa_.ready() && disc_shared ? 0.0f : 0.25f);
                // With DLAA the spread of the rays turns every image, and DLAA averages them
                float golden = float(jitter_index_) * 0.618034f;
                u.rt[3] = dlaa_.ready() ? golden - std::floor(golden) : 0.0f;
            }
        }
        u.soft[0] = soft_particles_ ? 1.0f : 0.0f;
        u.soft[1] = 2.0f;
        // Light in the game's fog, with the distance haze: strength, brighter
        // upwards, drifting banks
        if (w->fog_density > 0) {
            u.mist[0] = std::min(haze_strength_, 1.0f);
            u.mist[1] = 0.45f;
            u.mist[2] = 0.12f;
            static const char* tune = getenv("LW_MIST");   // debugging aid: "upwards,banks"
            if (tune) sscanf(tune, "%f,%f", &u.mist[1], &u.mist[2]);
        }
    } else {
        shadow_frame_ = false;
    }
    u.soft[2] = hdr_ ? 1.0f : 0.0f;
    // HDR tone curve (compose.frag): where the roll-off of the highlights
    // starts, and the least exposure a bright scene is given.
    // SR2_TONE=knee,min to compare (the old ones: 0.8,0.9)
    static float tone[2] = {0.9f, 1.0f};
    static const bool tone_read = [] {
        if (const char* e = getenv("SR2_TONE")) sscanf(e, "%f,%f", &tone[0], &tone[1]);
        return true;
    }();
    (void)tone_read;
    u.soft[3] = tone[0];
    u.sunlight[3] = tone[1];
    // SUN: more contrast in clear outdoor scenes (sunlit, little of the game's
    // own fog); the fogged, dark and indoor scenes keep the game's light
    if (w && sun_ && w->sun_intensity > 0.3f) {
        float fogged = std::clamp(w->scroll_fog * 2.0f + w->fog_density * 300.0f, 0.0f, 1.0f);
        float k = 1.0f - fogged;
        u.sunlight[0] = 1.0f + 0.45f * k;
        u.sunlight[1] = 1.0f - 0.4f * k;
        u.sunlight[2] = k;
    }
    u.mirror[3] = reflections_ ? 1.0f : 0.0f;   // r3d.frag: ray traced reflections on the cars
    // The world viewport's spotlight (spec 7.1), applied to the game world
    if (w && w->spot_radius[0] > 0 && w->spot_radius[1] > 0) {
        u.spot[0] = w->spot_center[0];
        u.spot[1] = w->spot_center[1];
        u.spot[2] = 1.0f / w->spot_radius[0];
        u.spot[3] = 1.0f / w->spot_radius[1];
        for (int i = 0; i < 3; i++) {
            u.spot_color[i] = w->spot_color[i];
            u.spot_fog[i] = w->spot_fog_color[i] * w->fog_attenuation * w->fog_color[i];
        }
        u.spot_color[3] = w->spot_start;
        u.spot_fog[3] = w->spot_extent;
    }
    // Fill light on the player's car where the traced bounce is not there
    // (r3d.frag; SR2_CAR_FILL=without,with to compare, 0 = none)
    // (0.3 matched the arcade's Lancer at the start, but on the Corolla's
    // side in the shade its whites came out brighter than the arcade's)
    static float car_fill[2] = {0.2f, 0.1f};
    static const bool fill_read = [] {
        if (const char* e = getenv("SR2_CAR_FILL")) sscanf(e, "%f,%f", &car_fill[0], &car_fill[1]);
        return true;
    }();
    (void)fill_read;
    u.gi[0] = car_fill[0];
    u.mirror[0] = car_fill[1];
    if (rt_gi_ && gi_tune().strength > 0) {   // the player's car's and the dust's share of the global illumination (r3d.frag, r3d.vert)
        u.gi[1] = gi_tune().strength;
        u.gi[2] = 2.0f * gi_tune().rays;   // the car's rays: four, eight on HIGH
        u.gi[3] = gi_tune().reach;
    }
    memcpy(atmosphere_uniform_.mapped, &u, sizeof(u));
}

// What every pass of a frame needs
struct R3DRenderer::Frame {
    VkCommandBuffer cmd;
    VkExtent2D extent;       // the window
    ScreenLayout lay;        // placement of the game screen in it
    uint32_t out_w, out_h;   // internal 3D resolution
    uint32_t up_w, up_h;     // ... and what DLSS makes of it (the same, but with DLSS upscaling)
    size_t vcount;           // vertices of the scene
    float before;            // motion vectors: how far back the previously displayed image was
    float jitter_x, jitter_y, alpha_seed;   // DLAA: sub-pixel shift and alpha-test noise seed
};

// Texture RAM rows, 2D layers and the scene's vertices changed since the last frame
void R3DRenderer::record_uploads(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    // Uploads
    if (tex_y0_ < tex_y1_) {
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(tex_y0_) * 2048 * 2;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {0, tex_y0_, 0};
        c.imageExtent = {2048, uint32_t(tex_y1_ - tex_y0_), 1};
        vkCmdCopyBufferToImage(cmd, texture_staging_.buffer, texture_ram_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, texture_ram_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tex_y0_ = tex_y1_ = 0;
    }
    if (layers_dirty_) {
        Image* imgs[2] = {&back_, &front_};
        for (int i = 0; i < 2; i++) {
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.bufferOffset = VkDeviceSize(i) * BACK_W * SCREEN_H * 4;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {imgs[i]->w, SCREEN_H, 1};
            vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            transition(cmd, *imgs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = VkDeviceSize(BACK_W + SCREEN_W) * SCREEN_H * 4;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {text_cells_.w, text_cells_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, text_cells_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, text_cells_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        c.bufferOffset += TEXT_CELLS_BYTES;
        c.imageExtent = {sky_coords_.w, sky_coords_.h, 1};
        vkCmdCopyBufferToImage(cmd, layer_staging_.buffer, sky_coords_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(cmd, sky_coords_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        layers_dirty_ = false;
    }
    size_t total = vcount;
    if (total * sizeof(R3DVertex) > vertices_.size) {
        vkDeviceWaitIdle(dev_);
        destroy_buffer(vertices_);
        create_buffer(vertices_, total * sizeof(R3DVertex) * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    if (vcount) memcpy(vertices_.mapped, scene_->vertices.data(), vcount * sizeof(R3DVertex));
}

// Global illumination: the rays for every pixel of the half-size image (16
// times as many for each block of 4x4 put together, ao_apply.frag) and how
// far they reach. LOW: two, 30 units (the walls, the banks and the trees
// beside the road); HIGH: four, 60 units (the rock faces and the far banks
// too). SR2_GI="strength,rays,reach": debugging aid (a negative strength
// shows the bounced light alone)
R3DRenderer::GiTune R3DRenderer::gi_tune() const {
    static const char* env = getenv("SR2_GI");
    GiTune t = gi_ >= 2 ? GiTune{1.0f, 4.0f, 60.0f} : GiTune{1.0f, 2.0f, 30.0f};
    if (env) sscanf(env, "%f,%f,%f", &t.strength, &t.rays, &t.reach);
    return t;
}

// Its rays keep the same spread in every image. (Turned every image for DLAA
// to average, as the sun shadows' are, the cars flickered: four rays each,
// and what one of them brings back differs too much from image to image.)
// SR2_GI_TURN: debugging aid, turned all the same
float R3DRenderer::gi_turn() const {
    static const bool turning = getenv("SR2_GI_TURN") != nullptr;
    float turn = float(jitter_index_) * 0.618034f;
    return dlaa_.ready() && turning ? turn - std::floor(turn) : 0.0f;
}

// Ray tracing: this image's world into the acceleration structure the 3D pass
// traces the sun shadows through. Before anything binds the 3D pass's
// descriptors, which may have to point at a new cut-out buffer
void R3DRenderer::record_ray_tracing(const Frame& f) {
    if (!rt_frame_ || !rt_world_) return;
    // the high level too: the cars' reflections (r3d.frag) see the canopies and roofs
    rt_.prepare(*scene_, *rt_world_, interp_, shadow_up_, shadow_height_, 35.0f, true);
    if (rt_.generation() != rt_generation_) {
        rt_generation_ = rt_.generation();
        VkDescriptorBufferInfo cut{rt_.cutouts(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = r3d_set_;
        w.dstBinding = 7;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &cut;
        vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    }
    rt_.record(f.cmd);
}

void R3DRenderer::record_shadows(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const size_t vcount = f.vcount;
    const float before = f.before;
    // Sun shadows: the opaque game world drawn from the sun, depth only
    if (shadow_frame_ && vcount) {
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo none{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};   // no colour image
        VkRenderingAttachmentInfo sd{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sd.imageView = shadow_map_.view;
        sd.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sd.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sd.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sd.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo sr{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sr.renderArea = {{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        sr.layerCount = 1;
        sr.colorAttachmentCount = 1;
        sr.pColorAttachments = &none;
        sr.pDepthAttachment = &sd;
        vkCmdBeginRendering(cmd, &sr);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_);
        VkViewport sv{0, 0, float(SHADOW_SIZE), float(SHADOW_SIZE), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &sv);
        VkRect2D ss{{0, 0}, {SHADOW_SIZE, SHADOW_SIZE}};
        vkCmdSetScissor(cmd, 0, 1, &ss);
        // The world. The cars drawn in viewports of their own (CAR: the player's,
        // the rivals near it) do not cast here: without ray tracing their shadow
        // is the game's own dark patch under each car, as in the arcade
        // (SR2_CAR_SHADOWMAP=1 casts them as well, to compare)
        static const bool cars = getenv("SR2_CAR_SHADOWMAP") && atoi(getenv("SR2_CAR_SHADOWMAP")) == 1;
        for (const R3DViewport& v : scene_->viewports) {
            if (!v.opaque_count || size_t(v.opaque_first) >= scene_->vertices.size()) continue;
            bool car = !v.world && cars && (scene_->vertices[v.opaque_first].flags & R3D_CAR);
            if (!v.world && !car) continue;
            ViewportPush pc{};
            memcpy(pc.proj, shadow_matrix_, 64);
            pc.params[3] = before;
            pc.quality[0] = 1.0f;   // opaque rules (alpha test), no MSAA coverage
            pc.quality[1] = aniso_;
            pc.quality[3] = interp_;   // sun.w = 0: no lighting, so no shadow lookup while drawing it
            // Shadow pass marker, and only casters up to this height above the ground
            for (int i = 0; i < 3; i++) pc.sun[i] = shadow_up_[i];
            pc.fog_color[0] = shadow_height_;
            pc.fog_color[1] = 35.0f;
            pc.fog_color[3] = -1.0f;
            vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(cmd, v.opaque_count, 1, v.opaque_first, 0);
        }
        vkCmdEndRendering(cmd);
        transition(cmd, shadow_map_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
}

// The game world and HUD into color_ (and glow_, motion_, depth_)
void R3DRenderer::record_world(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const size_t vcount = f.vcount;
    const float before = f.before, jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    const bool sr2_on = sr2_hud_on();
    // 3D pass
    bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    color_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // contents are cleared every frame
    color_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    motion_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa) transition(cmd, color_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (motion_pass_) transition(cmd, motion_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    glow_.layout = glow_ms_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cmd, glow_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (msaa) transition(cmd, glow_ms_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo cas[3] = {{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
                                        {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    VkRenderingAttachmentInfo& ca = cas[0];
    cas[1].imageView = motion_pass_ ? motion_.view : VK_NULL_HANDLE;
    cas[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cas[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cas[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.imageView = msaa ? color_ms_.view : color_.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // The ambient occlusion splits the 3D pass in two: the first part must keep everything
    bool ao_on = (ao_strength_ > 0 || rt_gi_) && scene_ &&
                 std::any_of(scene_->viewports.begin(), scene_->viewports.end(), [](const R3DViewport& v) { return v.world; });
    // Soft particles need the world's depth too, so they split it as well
    bool split = ao_on || (soft_particles_ && scene_ &&
                           std::any_of(scene_->viewports.begin(), scene_->viewports.end(),
                                       [](const R3DViewport& v) { return v.world; }));
    ca.storeOp = msaa && !split ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    if (msaa) {   // samples are averaged into the single-sample image at the end of the pass
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = color_.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    ca.clearValue.color = {{0, 0, 0, 0}};
    cas[2] = ca;   // bloom source, cleared to no glow
    cas[2].imageView = msaa ? glow_ms_.view : glow_.view;
    if (msaa) cas[2].resolveImageView = glow_.view;
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth_.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = motion_pass_ || split ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (split && msaa) {   // sample 0 of the depth, readable without multisampling
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = depth_resolved_.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    da.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {out_w, out_h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = cas;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);

    if (vcount) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
        float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
        float widen = lay.view_w / SCREEN_W;
        const auto& vps = scene_->viewports;
        size_t i = 0;
        bool first_priority = true;
        while (i < vps.size()) {
            size_t j = i;
            while (j < vps.size() && vps[j].priority == vps[i].priority) j++;
            if (!first_priority) {   // each priority starts with a cleared depth buffer
                VkClearAttachment clr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
                clr.clearValue.depthStencil = {0.0f, 0};
                VkClearRect rect{{{0, 0}, {out_w, out_h}}, 0, 1};
                vkCmdClearAttachments(cmd, 1, &clr, 1, &rect);
#if defined(__APPLE__)
                // MoltenVK clears by drawing with buffers of its own (its clear
                // colours go in fragment buffer 0) and does not bind the
                // descriptor set back: the next 3D draw read its 65600-byte set
                // from a 160-byte buffer and the GPU hung (Metal validation)
                vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0, nullptr);
#endif
            }
            first_priority = false;
            bool world_here = false;
            for (size_t k = i; k < j; k++) world_here |= vps[k].world;
            // Steps: opaque, the occlusion, translucent
            for (int step = 0; step < 3; step++) {
                int pass = step == 2 ? 1 : 0;
                // Between the opaque and the translucent surfaces of the world:
                // the ambient occlusion, from its depth (the HUD clears it later)
                if (step == 1 && split && world_here) {
                    const R3DViewport* w = nullptr;
                    for (size_t k = i; k < j; k++)
                        if (vps[k].world) w = &vps[k];
                    vkCmdEndRendering(cmd);
                    if (gpu_times_ && !gpu_ao_marked_) gpu_time_mark(cmd, GPU_AO_FROM);
                    record_world_depth(f, *w, msaa, ao_on);
                    if (gpu_times_ && !gpu_ao_marked_) gpu_time_mark(cmd, GPU_AO_TO);
                    gpu_ao_marked_ = true;
                    // The 3D pass goes on where it was; the world depth stays
                    // readable (no resolve into it any more this frame)
                    VkRenderingAttachmentInfo again[3] = {cas[0], cas[1], cas[2]};
                    VkRenderingAttachmentInfo dagain = da;
                    for (auto& a : again) a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    dagain.resolveMode = VK_RESOLVE_MODE_NONE;
                    dagain.resolveImageView = VK_NULL_HANDLE;
                    VkRenderingInfo rag = ri;
                    rag.pColorAttachments = again;
                    rag.pDepthAttachment = &dagain;
                    vkCmdBeginRendering(cmd, &rag);
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r3d_layout_, 0, 1, &r3d_set_, 0,
                                            nullptr);
                }
                if (step == 1) continue;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? opaque_ : translucent_);
                for (size_t k = i; k < j; k++) {
                    const R3DViewport& v = vps[k];
                    uint32_t count = pass == 0 ? v.opaque_count : v.trans_count;
                    if (!count) continue;
                    // Full-screen viewports are widened (wider horizontal field of
                    // view, same vertical one); smaller ones keep their 4:3 place
                    bool full = v.x <= 0.5f && v.w >= SCREEN_W - 0.5f;
                    float vx = full ? 0.0f : v.x + lay.game_x0, vw = full ? lay.view_w : v.w;
                    // A small viewport of the HUD (the rev counter's needle)
                    // goes with its corner of the 2D HUD
                    if (!full && sr2_on) {
                        const int* s = sr2_split_;
                        bool top = v.y + v.h <= SR2_HUD_TOP_ROWS, bottom = v.y >= SR2_HUD_BOTTOM_ROW - 8;
                        bool top_left = v.y + v.h <= SR2_HUD_LEFT_ROWS;
                        if ((top_left && v.x + v.w <= s[0]) || (bottom && s[2] > 0 && v.x + v.w <= s[2])) vx = v.x;
                        else if ((top && s[1] > 0 && v.x >= s[1]) || (bottom && s[2] > 0 && v.x >= s[2]))
                            vx = v.x + 2.0f * lay.game_x0;
                    }
                    VkViewport viewport{vx * sx + jitter_x, v.y * sy + jitter_y, vw * sx, v.h * sy, 0.0f, 1.0f};
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    int32_t x0 = std::max(0, int32_t(vx * sx)), y0 = std::max(0, int32_t(v.y * sy));
                    VkRect2D sc{{x0, y0},
                                {std::min(out_w - uint32_t(x0), uint32_t(vw * sx + 0.5f)),
                                 std::min(out_h - uint32_t(y0), uint32_t(v.h * sy + 0.5f))}};
                    vkCmdSetScissor(cmd, 0, 1, &sc);
                    ViewportPush pc{};
                    memcpy(pc.proj, v.proj, 64);
                    if (full) {
                        pc.proj[0] /= widen;
                        pc.proj[8] /= widen;
                    }
                    pc.sun[0] = v.sun[0];
                    pc.sun[1] = v.sun[1];
                    pc.sun[2] = v.sun[2];
                    // The --light-gain knob: more light on the sun and on the ambient. It lands here
                    // rather than in the push block because that block is exactly 128 bytes,
                    // the guaranteed minimum, and there is no room left in it.
                    pc.sun[3] = v.sun_intensity * light_gain_;
                    pc.fog_color[0] = v.fog_color[0];
                    pc.fog_color[1] = v.fog_color[1];
                    pc.fog_color[2] = v.fog_color[2];
                    pc.fog_color[3] = v.fog_ambient;
                    pc.params[0] = v.ambient * light_gain_;
                    pc.params[1] = v.fog_density;
                    pc.params[2] = v.fog_start;
                    pc.params[3] = before;
                    pc.quality[0] = pass == 0 ? float(samples_) + alpha_seed : -float(samples_);
                    pc.quality[1] = aniso_;
                    pc.quality[2] = 1.0f / upscale_;   // DLSS upscaling: textures as sharp as the picture shown needs
                    pc.quality[3] = interp_;
                    // Translucent surfaces that are not effects (a whole model made
                    // see-through, like the Carnotaurus' camouflage): only the
                    // nearest one shows at each pixel, as on the Real3D (spec 7.2),
                    // so their depth goes first. Fire, smoke and other texture-alpha
                    // or luminous effects keep blending in list order.
                    // The world's see-through surfaces go in runs: the game's
                    // dark patch under each car (its shadow) with pipelines of
                    // its own (blob_depth_, blob_: blended once at each pixel)
                    const std::vector<R3DVertex>& vs = scene_->vertices;
                    auto is_blob = [&](uint32_t i) {
                        const R3DVertex& x = vs[i];
                        return (x.flags & (R3D_WORLD | R3D_LUMINOUS | R3D_TEXTURED)) == (R3D_WORLD | R3D_LUMINOUS) &&
                               (x.color & 0x00FFFFFFu) == 0 && (x.color >> 24) < 255;
                    };
                    auto draw_runs = [&](VkPipeline other, VkPipeline blob) {
                        if (!v.world) {
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, other);
                            vkCmdDraw(cmd, count, 1, v.trans_first, 0);
                            return;
                        }
                        uint32_t end = std::min<uint32_t>(v.trans_first + count, uint32_t(vs.size()));
                        for (uint32_t a = v.trans_first; a < end;) {
                            bool here = is_blob(a);
                            uint32_t b = a + 3;
                            while (b < end && is_blob(b) == here) b += 3;
                            b = std::min(b, end);
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, here ? blob : other);
                            vkCmdDraw(cmd, b - a, 1, a, 0);
                            a = b;
                        }
                    };
                    if (pass == 1) {
                        ViewportPush pd = pc;
                        pd.quality[0] = -1000.0f;
                        vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(pd), &pd);
                        draw_runs(trans_depth_, blob_depth_);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent_);
                    }
                    vkCmdPushConstants(cmd, r3d_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(pc), &pc);
                    if (pass == 0) {
                        vkCmdDraw(cmd, count, 1, v.opaque_first, 0);
                        continue;
                    }
                    draw_runs(translucent_, blob_);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent_);
                }
            }
            i = j;
        }
    }
    vkCmdEndRendering(cmd);
    transition(cmd, color_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cmd, glow_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// Between the opaque and the translucent surfaces of the world: its depth
// made readable (soft particles, ambient occlusion) and, with ao_on, the
// ambient occlusion computed from it and multiplied into the colour so far.
// Called with no rendering in progress; the caller resumes the 3D pass.
void R3DRenderer::record_world_depth(const Frame& f, const R3DViewport& w, bool msaa, bool ao_on) {
    VkCommandBuffer cmd = f.cmd;
    const ScreenLayout& lay = f.lay;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y, alpha_seed = f.alpha_seed;
    float sx = float(out_w) / lay.view_w, sy = float(out_h) / SCREEN_H;
    float widen = lay.view_w / SCREEN_W;
    if (!msaa) {   // a copy of the depth (with MSAA its sample 0 was resolved)
        transition(cmd, depth_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        depth_resolved_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, depth_resolved_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {out_w, out_h, 1};
        vkCmdCopyImage(cmd, depth_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_resolved_.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(cmd, depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    Image& dsrc = depth_resolved_;
    transition(cmd, dsrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (ao_on) {
    bool full = w.x <= 0.5f && w.w >= SCREEN_W - 0.5f;
    float vx = full ? 0.0f : w.x + lay.game_x0, vw = full ? lay.view_w : w.w;
    // 12 units, or less in a small world: Hoth's base (nearly everything
    // within 52 units) was all occluded, dark, and its lights' glow then
    // stood out as white blots
    // (and gentler there: between close walls everything is occluded, and
    // the stormtroopers went grey)
    // Sega Rally 2 (far ~570 units, the stands and the cars a few units
    // big): 12 units darkened whole stretches of road under the stands, and
    // its few rays left them grainy
    float ao_radius = w.scenery_far > 0 ? std::clamp(0.01f * w.scenery_far, 1.5f, 6.0f) : 6.0f;
    float ao_gain = w.scenery_far > 0 ? std::clamp(w.scenery_far / 250.0f, 0.2f, 1.0f) : 1.0f;
    static const bool rt_split = getenv("LW_RTSPLIT") != nullptr;   // debugging aid: traced left, shadow map right
    static const float ao_rays = getenv("SR2_AO_ALL_RAYS") ? 20.5f : 20.0f;   // debugging aid: no probing rays first (ao.frag)
    const GiTune gi = gi_tune();
    float ap[24] = {w.proj[0] / (full ? widen : 1.0f), w.proj[5], w.proj[8] / (full ? widen : 1.0f),
                    w.proj[9], vx * sx + jitter_x, w.y * sy + jitter_y, vw * sx, w.h * sy,
                    ao_radius, std::min(ao_strength_, 1.0f) * 0.85f * ao_gain, w.fog_density, w.fog_start,
                    alpha_seed, 0.12f * float(out_h), rt_ao_ ? ao_rays : 0.0f, rt_split ? 1.0f : 0.0f,
                    rt_gi_ ? gi.strength : 0.0f, rt_gi_ ? gi.rays : 0.0f, gi.reach, gi_turn(),
                    w.ambient * light_gain_, 0.0f, 0.0f, 0.0f};
    auto fullscreen = [&](VkPipeline pipe, Image& target, VkImageView view, uint32_t tw, uint32_t th) {
        VkRenderingAttachmentInfo at{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        at.imageView = view;
        at.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        at.loadOp = &target == &ao_ ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ar{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ar.renderArea = {{0, 0}, {tw, th}};
        ar.layerCount = 1;
        ar.colorAttachmentCount = 1;
        ar.pColorAttachments = &at;
        vkCmdBeginRendering(cmd, &ar);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0,
                                nullptr);
        if (rt_.ready())   // the traced ambient occlusion's rays (the layout always has the set)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 1, 1, &r3d_set_, 0, nullptr);
        VkViewport fv{0, 0, float(tw), float(th), 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &fv);
        VkRect2D fr{{0, 0}, {tw, th}};
        vkCmdSetScissor(cmd, 0, 1, &fr);
        vkCmdPushConstants(cmd, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ap), ap);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };
    ao_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, ao_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen(ao_pass_, ao_, ao_.view, (out_w + 1) / 2, (out_h + 1) / 2);
    transition(cmd, ao_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Multiplied into the colour as drawn so far (the opaque world)
    Image& ctarget = msaa ? color_ms_ : color_;
    fullscreen(ao_apply_, ctarget, ctarget.view, out_w, out_h);
    // ... and into the glow: what the occlusion darkens glows less (Hoth's
    // base: its pale walls, darkened, still glowed in full as white blots)
    Image& gtarget = msaa ? glow_ms_ : glow_;
    fullscreen(ao_apply_glow_, gtarget, gtarget.view, out_w, out_h);
    }
}

void R3DRenderer::record_dlaa(const Frame& f) {
    VkCommandBuffer cmd = f.cmd;
    const uint32_t out_w = f.out_w, out_h = f.out_h;
    const float jitter_x = f.jitter_x, jitter_y = f.jitter_y;
    // DLAA: colour, depth and motion vectors in, anti-aliased image out
    if (dlaa_.ready()) {
        transition(cmd, depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        transition(cmd, motion_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dlaa_out_.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_GENERAL);
        bool ok = dlaa_.evaluate(cmd, {color_.image, color_.view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                 {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                 {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                 {dlaa_out_.image, dlaa_out_.view, VK_FORMAT_R16G16B16A16_SFLOAT}, out_w, out_h, f.up_w,
                                 f.up_h, jitter_x, jitter_y, false);
        // Its alpha separately: where the 3D image is partly transparent the
        // 2D background shows through, and a jittered alpha would make every
        // edge against it shimmer (the ammo counter over the sky)
        dlaa_alpha_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_GENERAL);
        ok = ok && dlaa_.evaluate(cmd, {color_.image, color_.alpha_view, VK_FORMAT_R16G16B16A16_SFLOAT},
                                  {depth_.image, depth_.view, VK_FORMAT_D32_SFLOAT},
                                  {motion_.image, motion_.view, VK_FORMAT_R16G16_SFLOAT},
                                  {dlaa_alpha_.image, dlaa_alpha_.view, VK_FORMAT_R8G8B8A8_UNORM}, out_w, out_h,
                                  f.up_w, f.up_h, jitter_x, jitter_y, false, 1);
        transition(cmd, dlaa_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(cmd, dlaa_alpha_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!ok) bind_scene_image(color_.view, color_.alpha_view);   // stopped working: plain image from now on
    }
}

// The composition's push constants for this frame
// The race HUD goes to the corners while its rev counter is on
// screen: the needle is 3D, in a small viewport at the bottom left (16, 272,
// 96 x 96), under the 2D dial. The selects, the stage intro and the results
// have none, so they stay as they are
bool R3DRenderer::sr2_hud_on() const {
    if (!hud_edges_ || !hud_band_ || !scene_) return false;
    return std::any_of(scene_->viewports.begin(), scene_->viewports.end(), [](const R3DViewport& v) {
        return v.x < 40.0f && v.y >= 240.0f && v.w <= 128.0f && v.h <= 128.0f && v.opaque_count + v.trans_count > 0;
    });
}

// Sega Rally 2: between a stage's START!! card and the race the game shows,
// for a few frames, the race's HUD over leftovers (a black band and brown
// streaks across the whole screen) with no game world behind yet. A black
// picture instead, as the fade it comes out of: while the HUD's labels are up
// (as high-resolution text) and the scene has no game world.
// SR2_SHOW_BETWEEN: as the game draws it, to compare
bool R3DRenderer::sr2_between_screens() const {
    static const bool show = getenv("SR2_SHOW_BETWEEN") != nullptr;
    if (show || !sr2_hud_over_picture_) return false;
    return !scene_ || std::none_of(scene_->viewports.begin(), scene_->viewports.end(), [](const R3DViewport& v) { return v.world; });
}

R3DRenderer::ComposePush R3DRenderer::compose_push(const Frame& f) const {
    static_assert(sizeof(ComposePush) == 112, "compose.frag push constant block");
    const VkExtent2D extent = f.extent;
    const ScreenLayout& lay = f.lay;
    float W = float(extent.width), H = float(extent.height);
    bool extend_back = back_layer_extended();
    const bool sr2_on = sr2_hud_on();
    ComposePush pc{.scale = {lay.view_w * lay.scale / W, SCREEN_H * lay.scale / H},
                   .offset = {lay.ox / W, lay.oy / H},
                   .sharpen = {sharpen_, 0},
                   .window = {W, H},
                   .layer2d = {lay.game_x0 / lay.view_w, SCREEN_W / lay.view_w},
                   .filter2d = {smooth_2d_ ? 1.0f : 0.0f, lay.scale},
                   .hud = {sr2_on ? lay.game_x0 : 0.0f, hdr_ ? 1.0f : 0.0f,
                           overlay_brightness_, extend_back ? 1.0f : 0.0f},
                   .sky = {0, 0, 0, sr2_between_screens() ? 4.0f : 0.0f},
                   .effects = {bloom_strength_, saturation_, gamma_, 0},
                   // w: the race HUD's splits, in 2-pixel steps, one per byte (top left, top right, bottom)
                   .motion = {back_shift_, 0, 0,
                              sr2_on ? float(sr2_split_[0] / 2 | (sr2_split_[1] / 2) << 8 | (sr2_split_[2] / 2) << 16)
                                     : 0.0f}};
    return pc;
}

// The reduced passes (glow, diffusion) see the whole 3D
// view in their own target, without sky glow
R3DRenderer::ComposePush R3DRenderer::reduced(const ComposePush& pc, const Image& to) {
    ComposePush p = pc;
    p.scale[0] = p.scale[1] = 1;
    p.offset[0] = p.offset[1] = 0;
    p.filter2d[1] = float(to.h) / SCREEN_H;
    p.sky[1] = 0;
    return p;
}

// Starts rendering into a quarter-size image (the diffusion)
void R3DRenderer::begin_quarter(VkCommandBuffer cmd, Image& to, VkAttachmentLoadOp load) {
    to.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
    transition(cmd, to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ra{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ra.imageView = to.view;
    ra.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ra.loadOp = load;
    ra.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri2.renderArea = {{0, 0}, {to.w, to.h}};
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &ra;
    vkCmdBeginRendering(cmd, &ri2);
    VkViewport rv{0, 0, float(to.w), float(to.h), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &rv);
    VkRect2D rr{{0, 0}, {to.w, to.h}};
    vkCmdSetScissor(cmd, 0, 1, &rr);
}

void R3DRenderer::record_bloom(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Bloom: the glow reduced to a quarter of the size, then blurred across and down
    if (bloom_strength_ > 0) {
        struct Step {
            int source;   // bloom_sets_ index
            Image* from;
            Image* to;
            float mode, spread;
        } steps[5] = {{0, &glow_, &bloom_a_, 0, 0},
                      {1, &bloom_a_, &bloom_b_, 1, 2.0f},
                      // the tight glow, kept in bloom_c_: the halo hugging lasers and engines
                      {2, &bloom_b_, &bloom_c_, 2, 2.0f},
                      // A second, wider round: the glow spreads across the screen
                      {3, &bloom_c_, &bloom_b_, 1, 5.0f},
                      {2, &bloom_b_, &bloom_a_, 2, 5.0f}};
        bool world = scene_ && std::any_of(scene_->viewports.begin(), scene_->viewports.end(),
                                           [](const R3DViewport& v) { return v.world; });
        // The sky glows less than fire: it is large and only its brightest part
        // counts. Only behind the game world: title and menu screens are 2D pictures
        // A sky that is bright all over (Hoth's pale sky over the snow) has no
        // bright part to pick out: its glow would wash the whole picture white
        float sky_glow = world ? 0.6f * std::clamp((0.8f - back_luma_) / 0.3f, 0.0f, 1.0f) : 0.0f;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (const Step& st : steps) {
            st.to->layout = VK_IMAGE_LAYOUT_UNDEFINED;   // fully rewritten
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            VkRenderingAttachmentInfo ba{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ba.imageView = st.to->view;
            ba.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ba.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ba.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo bi{VK_STRUCTURE_TYPE_RENDERING_INFO};
            bi.renderArea = {{0, 0}, {st.to->w, st.to->h}};
            bi.layerCount = 1;
            bi.colorAttachmentCount = 1;
            bi.pColorAttachments = &ba;
            vkCmdBeginRendering(cmd, &bi);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &bloom_sets_[st.source], 0,
                                    nullptr);
            VkViewport bv{0, 0, float(st.to->w), float(st.to->h), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &bv);
            VkRect2D br{{0, 0}, {st.to->w, st.to->h}};
            vkCmdSetScissor(cmd, 0, 1, &br);
            float bp[4] = {1.0f / float(st.from->w), 1.0f / float(st.from->h), st.mode, st.spread};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            if (st.mode == 0 && sky_glow > 0) {
                // The sky as the composition shows it (the game's or its replacement,
                // with the fogs, minus what the 3D covers): its brightest part
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0,
                                        nullptr);
                ComposePush gp = reduced(pc, *st.to);
                gp.sky[1] = sky_glow;   // glow mode
                vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gp), &gp);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
            }
            vkCmdEndRendering(cmd);
            transition(cmd, *st.to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else {
        for (Image* img : {&bloom_a_, &bloom_c_})   // bound, though not read
            if (img->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) transition(cmd, *img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void R3DRenderer::record_diffusion(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // Diffusion (BLOOM): the picture behind the HUD, then blurred across and down
    if (bloom_strength_ > 0) {
        begin_quarter(cmd, diff_a_, VK_ATTACHMENT_LOAD_OP_CLEAR);   // cleared: the scene pass adds onto it
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_glow_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        ComposePush dp = reduced(pc, diff_a_);
        dp.sky[3] = 3;       // scene mode
        dp.effects[0] = 0;   // (no diffusion of the diffusion)
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dp), &dp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_);
        for (int pass = 0; pass < 2; pass++) {
            Image& to = pass == 0 ? diff_b_ : diff_a_;
            begin_quarter(cmd, to, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom_layout_, 0, 1, &diff_sets_[pass], 0,
                                    nullptr);
            float bp[4] = {1.0f / float(to.w), 1.0f / float(to.h), pass == 0 ? 1.0f : 2.0f, 3.0f};
            vkCmdPushConstants(cmd, bloom_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(bp), bp);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            transition(cmd, to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    } else if (diff_a_.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cmd, diff_a_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // bound, though not read
    }
}

void R3DRenderer::record_exposure(const Frame& f, const ComposePush& pc) {
    VkCommandBuffer cmd = f.cmd;
    // HDR lighting: the 3D image's average brightness, eased into exposure_
    if (hdr_) {
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // keeps its value
        VkRenderingAttachmentInfo ea{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ea.imageView = exposure_.view;
        ea.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ea.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ea.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo er{VK_STRUCTURE_TYPE_RENDERING_INFO};
        er.renderArea = {{0, 0}, {1, 1}};
        er.layerCount = 1;
        er.colorAttachmentCount = 1;
        er.pColorAttachments = &ea;
        vkCmdBeginRendering(cmd, &er);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_measure_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
        VkViewport ev{0, 0, 1, 1, 0, 1};
        vkCmdSetViewport(cmd, 0, 1, &ev);
        VkRect2D eb{{0, 0}, {1, 1}};
        vkCmdSetScissor(cmd, 0, 1, &eb);
        ComposePush ep = pc;
        ep.sky[3] = 2;   // measure mode
        vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ep), &ep);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        transition(cmd, exposure_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void R3DRenderer::record_compose(const Frame& f, const ComposePush& pc, VkImageView target) {
    VkCommandBuffer cmd = f.cmd;
    const VkExtent2D extent = f.extent;
    // Composition into the swapchain image
    VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ta.imageView = target;
    ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ta.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ti{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ti.renderArea = {{0, 0}, extent};
    ti.layerCount = 1;
    ti.colorAttachmentCount = 1;
    ti.pColorAttachments = &ta;
    vkCmdBeginRendering(cmd, &ti);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout_, 0, 1, &compose_set_, 0, nullptr);
    VkViewport full{0, 0, float(extent.width), float(extent.height), 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &full);
    VkRect2D all{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &all);
    vkCmdPushConstants(cmd, compose_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// A frame, pass by pass (each one in its own function above)
void R3DRenderer::record(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    // Placement in the window (widescreen or 4:3) and internal 3D resolution
    Frame f{};
    f.cmd = cmd;
    f.extent = extent;
    f.lay = compute_layout(float(extent.width), float(extent.height), max_aspect_);
    f.up_w = std::max(1u, uint32_t(f.lay.view_w * f.lay.scale * supersample_));
    f.up_h = std::max(1u, uint32_t(SCREEN_H * f.lay.scale * supersample_));
    f.out_w = upscale_ > 1.0f ? std::max(1u, uint32_t(float(f.up_w) / upscale_ + 0.5f)) : f.up_w;
    f.out_h = upscale_ > 1.0f ? std::max(1u, uint32_t(float(f.up_h) / upscale_ + 0.5f)) : f.up_h;
    ensure_targets(f.out_w, f.out_h, f.up_w, f.up_h);
    // Always timed: out/slow.log notes the slowest image of a slow second
    // (worst_gpu_frame); SR2_GPUTIME also prints the averages
    if (!gpu_times_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = GPU_MARKS;
        vkCreateQueryPool(dev_, &qi, nullptr, &gpu_times_);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk_->gpu(), &props);
        timestamp_period_ = props.limits.timestampPeriod;
    }
    gpu_time_report(f.out_w, f.out_h);
    gpu_ao_marked_ = false;
    if (gpu_times_) {
        vkCmdResetQueryPool(cmd, gpu_times_, 0, GPU_MARKS);
        gpu_time_mark(cmd, GPU_START);
    }
    f.vcount = scene_ ? scene_->vertices.size() : 0;
    record_uploads(f);
    update_atmosphere(f.lay, f.out_w, f.out_h);

    // Motion vectors point to where each surface was in the previously
    // displayed image: the same game frame part way back, or the previous
    // game frame (extrapolated backwards when it was itself interpolated)
    float before = scene_serial_ != drawn_serial_ ? drawn_interp_ - 1.0f : drawn_interp_;
    drawn_serial_ = scene_serial_;
    drawn_interp_ = interp_;
    // DLAA: the image is shifted by a different sub-pixel amount each frame
    // (Halton 2,3 over 8 phases; 18 when DLSS upscales: 8 for each pixel it
    // makes) so it can gather detail between pixels
    float jitter_x = 0, jitter_y = 0, alpha_seed = 0;
    if (dlaa_.ready()) {
        auto halton = [](uint32_t i, uint32_t base) {
            float digit = 1, r = 0;
            for (; i; i /= base) {
                digit /= float(base);
                r += digit * float(i % base);
            }
            return r;
        };
        uint32_t phase = uint32_t(jitter_index_++ % (upscale_ > 1.0f ? 18 : 8)) + 1;
        jitter_x = halton(phase, 2) - 0.5f;
        jitter_y = halton(phase, 3) - 0.5f;
        // Alpha-tested edges (foliage): noise threshold that changes every frame
        float golden = float(jitter_index_) * 0.618034f;
        alpha_seed = 0.01f + 0.98f * (golden - std::floor(golden));
    }
    f.before = before;
    f.jitter_x = jitter_x;
    f.jitter_y = jitter_y;
    f.alpha_seed = alpha_seed;

    if (gpu_times_) gpu_time_mark(cmd, GPU_UPLOADS);
    record_ray_tracing(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_RT);
    record_shadows(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_SHADOWS);
    record_world(f);
    if (gpu_times_ && !gpu_ao_marked_) {   // no occlusion pass this frame: it took no time
        gpu_time_mark(cmd, GPU_AO_FROM);
        gpu_time_mark(cmd, GPU_AO_TO);
    }
    if (gpu_times_) gpu_time_mark(cmd, GPU_WORLD);
    record_dlaa(f);
    if (gpu_times_) gpu_time_mark(cmd, GPU_DLAA);
    ComposePush pc = compose_push(f);
    record_bloom(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_BLOOM);
    record_diffusion(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_DIFFUSION);
    record_exposure(f, pc);
    if (gpu_times_) gpu_time_mark(cmd, GPU_EXPOSURE);
    record_compose(f, pc, target);
    if (gpu_times_) {
        gpu_time_mark(cmd, GPU_COMPOSE);
        gpu_pending_ = true;
    }
}

// The previous frame is done (one frame in flight): its times are added up
void R3DRenderer::gpu_time_report(uint32_t out_w, uint32_t out_h) {
    if (!gpu_times_ || !gpu_pending_) return;
    uint64_t t[GPU_MARKS];
    if (vkGetQueryPoolResults(dev_, gpu_times_, 0, GPU_MARKS, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    auto ms = [&](uint32_t from, uint32_t to) { return double(t[to] - t[from]) * timestamp_period_ * 1e-6; };
    double total = ms(GPU_START, GPU_COMPOSE);
    if (total > gpu_worst_ms_ && total < 1000.0) {
        char text[320];
        snprintf(text, sizeof(text),
                 "gpu %.1f ms: uploads %.1f rt build %.1f shadows %.1f 3D+AO %.1f DLAA %.1f bloom %.1f, %zu traced triangles",
                 total, ms(GPU_START, GPU_UPLOADS), ms(GPU_UPLOADS, GPU_RT), ms(GPU_RT, GPU_SHADOWS),
                 ms(GPU_SHADOWS, GPU_WORLD), ms(GPU_WORLD, GPU_DLAA), ms(GPU_DLAA, GPU_BLOOM),
                 rt_frame_ ? rt_.triangles() : size_t(0));
        std::lock_guard<std::mutex> lock(gpu_worst_mutex_);
        gpu_worst_ms_ = total;
        gpu_worst_ = text;
    }
    // Debugging aid: SR2_GPULOG=file, a line per image (the game frame being
    // drawn when it is read, one image later; ms: whole, traced structure, 3D,
    // occlusion, DLAA; polygons)
    static FILE* each = getenv("SR2_GPULOG") ? fopen(getenv("SR2_GPULOG"), "w") : nullptr;
    if (each)
        fprintf(each, "%llu %.2f %.2f %.2f %.2f %.2f %zu\n", (unsigned long long)(scene_ ? scene_->frame : 0), total,
                ms(GPU_UPLOADS, GPU_RT), ms(GPU_SHADOWS, GPU_AO_FROM) + ms(GPU_AO_TO, GPU_WORLD), ms(GPU_AO_FROM, GPU_AO_TO),
                ms(GPU_WORLD, GPU_DLAA), scene_ ? scene_->polygons : size_t(0));
    static const bool print = getenv("SR2_GPUTIME") || getenv("LW_GPUTIME");
    if (!print) return;
    for (uint32_t i = 1; i < GPU_MARKS; i++) gpu_sum_[i] += ms(i - 1, i);
    gpu_sum_[0] += ms(GPU_START, GPU_COMPOSE);   // the whole frame
    if (++gpu_frames_ < 300) return;
    auto avg = [&](uint32_t i) { return gpu_sum_[i] / gpu_frames_; };
    // The 3D pass runs from the shadows to its end, around the occlusion
    double world = avg(GPU_AO_FROM) + avg(GPU_WORLD);
    printf("[gpu] ms/image %ux%u: total %.2f | uploads %.2f  rt build %.2f  shadows %.2f  3D %.2f  AO %.2f  "
           "DLAA %.2f  bloom %.2f  diffusion %.2f  exposure %.2f  compose %.2f  (%zu traced triangles)\n",
           out_w, out_h, avg(0), avg(GPU_UPLOADS), avg(GPU_RT), avg(GPU_SHADOWS), world, avg(GPU_AO_TO), avg(GPU_DLAA),
           avg(GPU_BLOOM), avg(GPU_DIFFUSION), avg(GPU_EXPOSURE), avg(GPU_COMPOSE),
           rt_frame_ ? rt_.triangles() : size_t(0));
    fflush(stdout);
    std::fill(std::begin(gpu_sum_), std::end(gpu_sum_), 0.0);
    gpu_frames_ = 0;
}

std::string R3DRenderer::worst_gpu_frame() {
    std::lock_guard<std::mutex> lock(gpu_worst_mutex_);
    std::string text = gpu_worst_;
    gpu_worst_.clear();
    gpu_worst_ms_ = 0;
    return text;
}

void R3DRenderer::gpu_time_mark(VkCommandBuffer cmd, uint32_t index) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_times_, index);
}

void R3DRenderer::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (auto p : {opaque_, translucent_, blob_, blob_depth_, trans_depth_, shadow_, compose_measure_, compose_, bloom_, compose_glow_, ao_pass_, ao_apply_, ao_apply_glow_})
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    dlaa_.shutdown();
    rt_.shutdown();
    if (gpu_times_) vkDestroyQueryPool(dev_, gpu_times_, nullptr);
    for (Image* img : {&texture_ram_, &back_, &front_, &text_cells_, &sky_coords_, &color_, &color_ms_, &depth_, &motion_, &dlaa_out_, &dlaa_alpha_,
                        &glow_, &glow_ms_, &bloom_a_, &bloom_b_, &bloom_c_, &depth_resolved_, &ao_, &shadow_map_, &exposure_, &diff_a_, &diff_b_})
        destroy_image(*img);
    for (Image& img : replacements_) destroy_image(img);
    for (auto& [slot, img] : overlay_images_) destroy_image(img);
    for (Buffer* b : {&texture_staging_, &layer_staging_, &vertices_, &atmosphere_uniform_}) destroy_buffer(*b);
}
