#include "r3d_raytrace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool R3DRayTracer::create_buffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev_, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fi;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_->find_memory(req.memoryTypeBits, host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev_, b.buffer, b.memory, 0);
    if (host) vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(dev_, &addr);
    b.size = size;
    return true;
}

void R3DRayTracer::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    b = Buffer{};
}

// A bottom level: its solid triangles, then its cut-outs right after them in
// the position buffer
void R3DRayTracer::build_info(VkAccelerationStructureBuildGeometryInfoKHR& info,
                              VkAccelerationStructureGeometryKHR geo[2], const Level& level) const {
    for (int i = 0; i < 2; i++) {
        geo[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geo[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        auto& t = geo[i].geometry.triangles;
        t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t.vertexStride = 12;
        t.indexType = VK_INDEX_TYPE_NONE_KHR;
        uint32_t count = i == 0 ? level.solid : level.cut;
        t.maxVertex = std::max(count * 3, 1u) - 1;
        t.vertexData.deviceAddress = positions_.address + VkDeviceSize(level.first + (i == 0 ? 0 : level.solid)) * 36;
    }
    geo[0].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;   // a hit is a hit
    // the shader looks at the texture first, once per triangle (the sun's rays
    // multiply what each lets through: raytrace.glsl rt_light)
    geo[1].flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // Rebuilt every image, yet built for the rays rather than for the build:
    // with some 15000 triangles it takes 0.1 ms more to build, and the tens
    // of millions of rays of an image (shadows, ambient occlusion) go through
    // it 0.2 to 0.6 ms sooner. SR2_RT_FAST_BUILD: as before, for comparing
    static const bool fast_build = getenv("SR2_RT_FAST_BUILD") != nullptr;
    info.flags = fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 2;
    info.pGeometries = geo;
}

bool R3DRayTracer::reserve(uint32_t triangles) {
    if (triangles <= capacity_ && levels_[0].blas) return true;
    uint32_t cap = std::max({triangles + triangles / 2, capacity_, 8192u});
    vkDeviceWaitIdle(dev_);
    for (Level& l : levels_) {
        if (l.blas) vkDestroyAccelerationStructureKHR(dev_, l.blas, nullptr);
        l.blas = VK_NULL_HANDLE;
        destroy_buffer(l.storage);
    }
    for (Buffer* b : {&positions_, &cutouts_, &scratch_}) destroy_buffer(*b);
    capacity_ = 0;
    if (!create_buffer(positions_, VkDeviceSize(cap) * 36,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) ||
        !create_buffer(cutouts_, HEADER + VkDeviceSize(cap) * sizeof(Cutout), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true))
        return false;
    // Each level sized for all of them (either may hold most of the world)
    Level sizing;
    sizing.solid = sizing.cut = cap;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureGeometryKHR geo[2];
    build_info(info, geo, sizing);
    uint32_t counts[2] = {cap, cap};
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, counts, &sizes);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    ci.size = sizes.accelerationStructureSize;
    auto align = [&](VkDeviceSize v) { return (v + scratch_align_ - 1) / scratch_align_ * scratch_align_; };
    VkDeviceSize scratch = 0;
    for (Level& l : levels_) {
        if (!create_buffer(l.storage, sizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
            return false;
        ci.buffer = l.storage.buffer;
        if (vkCreateAccelerationStructureKHR(dev_, &ci, nullptr, &l.blas) != VK_SUCCESS) return false;
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = l.blas;
        l.address = vkGetAccelerationStructureDeviceAddressKHR(dev_, &ai);
        // Their own parts of the scratch buffer: they are built side by side
        l.scratch_offset = scratch;
        scratch = align(scratch + sizes.buildScratchSize);
    }

    // The top level: its part of the scratch buffer after the bottom levels'
    VkAccelerationStructureGeometryKHR tgeo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    uint32_t two = 2;
    VkAccelerationStructureBuildSizesInfoKHR tsizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tinfo, &two, &tsizes);
    tlas_scratch_offset_ = scratch;
    if (!create_buffer(scratch_, scratch + tsizes.buildScratchSize + scratch_align_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       false))
        return false;
    if (!tlas_) {
        if (!create_buffer(tlas_buffer_, tsizes.accelerationStructureSize,
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false))
            return false;
        ci.buffer = tlas_buffer_.buffer;
        ci.size = tsizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(dev_, &ci, nullptr, &tlas_) != VK_SUCCESS) return false;
    }
    capacity_ = cap;
    generation_++;
    return true;
}

bool R3DRayTracer::init(VkPresenter& vk) {
    if (!vk.ray_query()) return false;
    vk_ = &vk;
    dev_ = vk.device();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &asp;
    vkGetPhysicalDeviceProperties2(vk.gpu(), &props);
    scratch_align_ = std::max<VkDeviceSize>(asp.minAccelerationStructureScratchOffsetAlignment, 1);
    if (!create_buffer(instances_, LEVELS * sizeof(VkAccelerationStructureInstanceKHR),
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true) ||
        !reserve(0)) {
        fprintf(stderr, "[raytrace] could not create the acceleration structures\n");
        shutdown();
        return false;
    }
    // Built once empty, so the shaders always see a valid structure
    VkAccelerationStructureInstanceKHR inst[LEVELS] = {};
    for (int i = 0; i < LEVELS; i++) {
        inst[i].mask = i == 0 ? RT_LOW : i == 1 ? RT_HIGH : RT_ACTOR;
        inst[i].accelerationStructureReference = levels_[i].address;
    }
    memcpy(instances_.mapped, inst, sizeof(inst));
    vk.run_once([&](VkCommandBuffer cmd) { record(cmd); });
    return true;
}

void R3DRayTracer::shutdown() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (Level& l : levels_) {
        if (l.blas) vkDestroyAccelerationStructureKHR(dev_, l.blas, nullptr);
        l.blas = VK_NULL_HANDLE;
        destroy_buffer(l.storage);
    }
    if (tlas_) vkDestroyAccelerationStructureKHR(dev_, tlas_, nullptr);
    tlas_ = VK_NULL_HANDLE;
    for (Buffer* b : {&positions_, &cutouts_, &tlas_buffer_, &scratch_, &instances_}) destroy_buffer(*b);
    capacity_ = 0;
    dev_ = VK_NULL_HANDLE;
}

void R3DRayTracer::prepare(const R3DScene& scene, const R3DViewport& world, float interp, const float up[3],
                           float camera_height, float max_height, bool high) {
    for (Level& l : levels_) {
        l.solid_pos.clear();
        l.cut_pos.clear();
        l.solid_data.clear();
        l.cut_data.clear();
    }
    // The world and the viewports drawn with its camera (Star Wars Trilogy's ships beside its planets)
    for (const R3DViewport& vp : scene.viewports) {
        // and Sega Rally 2's own car, drawn in a viewport of its own (R3D_CAR):
        // it casts its shadow on the road and shows in the others' paint
        bool car = vp.opaque_count > 0 && vp.opaque_first < scene.vertices.size() &&
                   (scene.vertices[vp.opaque_first].flags & R3D_CAR);
        if (&vp != &world && !car) continue;
        auto add = [&](const R3DVertex* v, bool moves) {
            float p[9], lowest = 1e30f;
            for (int k = 0; k < 3; k++) {
                for (int c = 0; c < 3; c++)
                    p[k * 3 + c] = moves ? v[k].prev[c] + (v[k].pos[c] - v[k].prev[c]) * interp : v[k].pos[c];
                lowest = std::min(lowest, p[k * 3] * up[0] + p[k * 3 + 1] * up[1] + p[k * 3 + 2] * up[2] + camera_height);
            }
            // Canopies and roofs high above would put whole scenes in the dark:
            // they cast no sun shadows, only the ambient occlusion sees them
            uint32_t flags = v[0].flags | (car ? uint32_t(R3D_CAR) : 0u);
            bool actor = (flags & (R3D_ACTOR | R3D_CAR)) != 0;
            bool above = lowest > max_height;
            if (above && !high && !actor) return;
            Level& l = levels_[actor ? 2 : above ? 1 : 0];
            // The spectators (and the other flat figures that turn to face the
            // camera: self-lit, alpha tested pictures on an upright card) cast
            // the shadow of the card they are drawn on, a solid rectangle, and
            // the card is edge-on to the sun while it is ahead (the sun is
            // always to one side of the view): no shadow at all, then, as the
            // car passes it and the card turns with the camera, a black bar that
            // opens out across the ground in an instant: at speed, a black flash
            // by the roadside. For the rays the card is turned about its upright
            // middle to face the sun, and it is the figure's outline (its
            // texture's alpha) that stops them: a person's shadow, there from
            // afar and steady. SR2_FLAT_SHADOWS: as before, to compare
            static const bool flat_shadows = getenv("SR2_FLAT_SHADOWS") != nullptr;
            const uint32_t figure = R3D_ACTOR | R3D_LUMINOUS | R3D_ALPHA_TEST | R3D_TEXTURED;
            bool card = false;
            if (!flat_shadows && !car && (flags & figure) == figure) {
                auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                float e1[3], e2[3];
                for (int c = 0; c < 3; c++) e1[c] = p[3 + c] - p[c], e2[c] = p[6 + c] - p[c];
                float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                float len = std::sqrt(dot3(n, n));
                // The sun's direction along the ground, and across it: where the card's width goes
                float s_up = dot3(world.sun, up);
                float flat[3] = {world.sun[0] - up[0] * s_up, world.sun[1] - up[1] * s_up, world.sun[2] - up[2] * s_up};
                float flat_len = std::sqrt(dot3(flat, flat));
                if (len > 0 && std::fabs(dot3(n, up)) < 0.3f * len && flat_len > 0.1f) {
                    // the card's own width: level, in its plane
                    float h[3] = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
                    float h_len = std::sqrt(dot3(h, h));
                    float to[3] = {up[1] * flat[2] - up[2] * flat[1], up[2] * flat[0] - up[0] * flat[2], up[0] * flat[1] - up[1] * flat[0]};
                    float to_len = std::sqrt(dot3(to, to));
                    if (h_len > 0 && to_len > 0) {
                        for (int c = 0; c < 3; c++) h[c] /= h_len, to[c] /= to_len;
                        // (the two triangles of a card must turn the same way: one sense for the width)
                        if (h[0] < 0 || (h[0] == 0 && h[2] < 0))
                            for (float& x : h) x = -x;
                        if (dot3(to, h) < 0)
                            for (float& x : to) x = -x;
                        float a[3] = {dot3(p, h), dot3(p + 3, h), dot3(p + 6, h)};
                        float mid = 0.5f * (std::min({a[0], a[1], a[2]}) + std::max({a[0], a[1], a[2]}));
                        for (int k = 0; k < 3; k++)
                            for (int c = 0; c < 3; c++) p[k * 3 + c] += (to[c] - h[c]) * (a[k] - mid);
                        card = true;
                    }
                }
            }
            Cutout c;
            for (int k = 0; k < 3; k++) {
                c.uv[k * 2] = v[k].uv[0];
                c.uv[k * 2 + 1] = v[k].uv[1];
            }
            c.tex = v[0].tex;
            c.flags = flags;
            c.replace = v[0].replace;
            c.color = v[0].color;
            // The cars' decals are alpha tested over the bodywork: as cut-outs
            // their clear texels let the sun through, holes in the car's
            // shadow. The cars (and people) are solid to the rays
            // A camera flash (raytrace.glsl's glow_sprite) is a cut-out: the sun's rays pass it
            const uint32_t glow_mask = R3D_TEXTURED | R3D_ALPHA_TEST | R3D_LUMINOUS | R3D_TEX_ALPHA | R3D_SPECULAR;
            bool glow = (flags & glow_mask) == (glow_mask & ~uint32_t(R3D_SPECULAR));
            if (card || glow || ((flags & R3D_TEXTURED) && (flags & (R3D_ALPHA_TEST | R3D_TEX_ALPHA)) && !actor)) {
                l.cut_pos.insert(l.cut_pos.end(), p, p + 9);
                l.cut_data.push_back(c);
            } else {
                l.solid_pos.insert(l.solid_pos.end(), p, p + 9);
                l.solid_data.push_back(c);
            }
        };
        // The solid polygons, and the cars' see-through textured ones (their glass)
        for (int pass = 0; pass < 2; pass++) {
            size_t first = pass ? vp.trans_first : vp.opaque_first;
            size_t end = std::min<size_t>(first + (pass ? vp.trans_count : vp.opaque_count), scene.vertices.size());
            for (size_t i = first; i + 2 < end; i += 3) {
                const R3DVertex* v = &scene.vertices[i];
                if (pass && (!(v[0].flags & (R3D_ACTOR | R3D_CAR)) || !(v[0].flags & R3D_TEXTURED))) continue;
                add(v, true);
            }
        }
        // The player's car is a shell seen from behind: its sides turned away
        // from the camera were left out, and the sun's rays came out through
        // them (light patches in its shadow, behind it). They are taken as they
        // are (no motion between frames: the car keeps its place on screen)
        if (car) {
            size_t end = std::min<size_t>(size_t(vp.back_first) + vp.back_count, scene.backs.size());
            for (size_t i = vp.back_first; i + 2 < end; i += 3) add(&scene.backs[i], false);
        }
    }
    uint32_t total = 0;
    for (Level& l : levels_) total += uint32_t((l.solid_pos.size() + l.cut_pos.size()) / 9);
    if (!reserve(total)) {
        for (Level& l : levels_) l.solid = l.cut = 0;
        return;
    }
    float* pos = static_cast<float*>(positions_.mapped);
    uint32_t* header = static_cast<uint32_t*>(cutouts_.mapped);
    Cutout* cut = reinterpret_cast<Cutout*>(static_cast<uint8_t*>(cutouts_.mapped) + HEADER);
    uint32_t first = 0;
    VkAccelerationStructureInstanceKHR inst[LEVELS] = {};
    for (int i = 0; i < LEVELS; i++) {
        Level& l = levels_[i];
        l.solid = uint32_t(l.solid_pos.size() / 9);
        l.cut = uint32_t(l.cut_data.size());
        l.first = first;
        header[i] = l.solid;
        if (l.solid) {
            memcpy(pos + size_t(first) * 9, l.solid_pos.data(), l.solid_pos.size() * sizeof(float));
            memcpy(cut + first, l.solid_data.data(), l.solid * sizeof(Cutout));
        }
        if (l.cut) {
            memcpy(pos + size_t(first + l.solid) * 9, l.cut_pos.data(), l.cut_pos.size() * sizeof(float));
            memcpy(cut + first + l.solid, l.cut_data.data(), l.cut * sizeof(Cutout));
        }
        // The world as it is: no transform; the custom index finds the level's triangles
        inst[i].transform.matrix[0][0] = inst[i].transform.matrix[1][1] = inst[i].transform.matrix[2][2] = 1.0f;
        inst[i].instanceCustomIndex = first;
        inst[i].mask = i == 0 ? RT_LOW : i == 1 ? RT_HIGH : RT_ACTOR;
        inst[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst[i].accelerationStructureReference = l.address;
        first += l.solid + l.cut;
    }
    memcpy(instances_.mapped, inst, sizeof(inst));
}

void R3DRayTracer::record(VkCommandBuffer cmd) {
    VkDeviceAddress scratch = (scratch_.address + scratch_align_ - 1) / scratch_align_ * scratch_align_;
    VkAccelerationStructureBuildGeometryInfoKHR infos[LEVELS];
    VkAccelerationStructureGeometryKHR geos[LEVELS][2];
    VkAccelerationStructureBuildRangeInfoKHR ranges[LEVELS][2];
    const VkAccelerationStructureBuildRangeInfoKHR* pr[LEVELS];
    for (int i = 0; i < LEVELS; i++) {
        const Level& l = levels_[i];
        build_info(infos[i], geos[i], l);
        infos[i].dstAccelerationStructure = l.blas;
        infos[i].scratchData.deviceAddress = scratch + l.scratch_offset;
        ranges[i][0] = {l.solid, 0, 0, 0};
        ranges[i][1] = {l.cut, 0, 0, 0};
        pr[i] = ranges[i];
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, LEVELS, infos, pr);

    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);

    VkAccelerationStructureGeometryKHR tgeo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = instances_.address;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    tinfo.dstAccelerationStructure = tlas_;
    tinfo.scratchData.deviceAddress = scratch + tlas_scratch_offset_;
    VkAccelerationStructureBuildRangeInfoKHR trange{LEVELS, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &trange;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tinfo, &ptr);

    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
