#include "vk_presenter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dlss.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace {

double seconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(freq.QuadPart);
}

bool check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vulkan] %s failed (%d)\n", what, int(r));
        return false;
    }
    return true;
}

#if defined(__APPLE__)
// Debugging aid while the macOS port is young: the first failures of the
// frame's Vulkan calls (a lost device otherwise only shows as a still picture)
VkResult mac_note(VkResult r, const char* what) {
    static int left = 20;
    if (r != VK_SUCCESS && left > 0) {
        left--;
        fprintf(stderr, "[vulkan] %s returned %d%s\n", what, int(r),
                r == VK_ERROR_DEVICE_LOST ? " (device lost)" : r == VK_TIMEOUT ? " (timeout)" : "");
    }
    return r;
}
#define MAC_NOTE(call, what) mac_note((call), what)
#else
#define MAC_NOTE(call, what) (call)
#endif

#if defined(__APPLE__)
// The Vulkan loader and MoltenVK are next to the game (CMakeLists.txt copies
// them there): the loader is told to use that MoltenVK through its ICD json,
// unless VK_DRIVER_FILES or VK_ICD_FILENAMES already say otherwise
VkResult mac_volk_initialize() {
    std::string dir = mac_exe_dir();
    std::string icd = dir + "MoltenVK_icd.json";
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES") && GetFileAttributesA(icd.c_str()) != INVALID_FILE_ATTRIBUTES)
        setenv("VK_DRIVER_FILES", icd.c_str(), 1);
    if (void* loader = dlopen((dir + "libvulkan.1.dylib").c_str(), RTLD_NOW | RTLD_LOCAL)) {
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
        if (gipa) {
            volkInitializeCustom(gipa);
            return VK_SUCCESS;
        }
    }
    return volkInitialize();   // a loader installed elsewhere (Homebrew, the Vulkan SDK)
}
#endif

}  // namespace

void vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                      VkImageAspectFlags aspect, uint32_t layers) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

uint32_t VkPresenter::find_memory(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

LRESULT CALLBACK VkPresenter::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<VkPresenter*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self && self->input_hook) {
        switch (msg) {
        case WM_KEYDOWN: case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MOUSEWHEEL:
            if (self->input_hook(msg, wp, lp)) return 0;
            break;
        }
    }
    switch (msg) {
    case WM_CLOSE:
        if (self) self->quit_ = true;
        return 0;
    case WM_SIZE:
        if (self) self->resized_ = true;
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && self) self->quit_ = true;
        if (wp == VK_F11 && self) self->toggle_fullscreen();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {   // the renderer draws its own crosshair (menus use the arrow)
            SetCursor(self && self->show_cursor_ ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
            return TRUE;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && self) { self->toggle_fullscreen(); return 0; }   // Alt+Enter
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void VkPresenter::set_cursor_visible(bool visible) {
    if (visible == show_cursor_) return;
    show_cursor_ = visible;
#if defined(__APPLE__)
    mac_window_set_cursor_visible(hwnd_, visible);
#else
    // WM_SETCURSOR only comes when the mouse moves: change it now if it is over the window
    POINT p;
    if (GetForegroundWindow() == hwnd_ && GetCursorPos(&p) && WindowFromPoint(p) == hwnd_ &&
        SendMessageA(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)) == HTCLIENT)
        SetCursor(visible ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
#endif
}

void VkPresenter::toggle_fullscreen() {
#if defined(__APPLE__)
    // macOS full screen, in a space of its own: the window sends WM_SIZE when it is there
    mac_window_toggle_fullscreen(hwnd_);
    fullscreen_ = !fullscreen_;
    return;
#else
    DWORD style = GetWindowLongA(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO mi{sizeof(mi)};
        GetWindowPlacement(hwnd_, &placement_);
        GetMonitorInfoA(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
    resized_ = true;
#endif
}

bool VkPresenter::init(const char* title, int width, int height, bool with_ngx) {
#if defined(__APPLE__)
    static const bool background = getenv("LW_BACKGROUND") != nullptr;
    hwnd_ = mac_window_create(title, width, height, background, wndproc);
    if (!hwnd_) return false;
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    if (!check(mac_volk_initialize(), "volkInitialize (are libvulkan.1.dylib and MoltenVK next to the game?)"))
        return false;
#else
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));  // res/app.rc
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = "SegaRally2Native";
    RegisterClassA(&wc);
    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    // Debugging aid: LW_BACKGROUND, the window behind all the others, never
    // taking the keyboard (test runs while somebody is using the computer)
    static const bool background = getenv("LW_BACKGROUND") != nullptr;
    hwnd_ = CreateWindowExA(background ? WS_EX_NOACTIVATE : 0, wc.lpszClassName, title,
                            WS_OVERLAPPEDWINDOW | (background ? 0 : WS_VISIBLE), CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    if (background)
        SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    if (!check(volkInitialize(), "volkInitialize (is the Vulkan driver missing?)")) return false;
#endif

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Sega Rally 2 Native";
    app.apiVersion = VK_API_VERSION_1_3;
#if defined(__APPLE__)
    // MoltenVK is a portability driver: the loader lists it only when asked to
    std::vector<std::string> inst_names = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME,
                                           VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
#else
    std::vector<std::string> inst_names = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#endif
    if (with_ngx) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());
        for (const std::string& e : dlss::instance_extensions())
            for (auto& a : avail)
                if (e == a.extensionName && std::find(inst_names.begin(), inst_names.end(), e) == inst_names.end())
                    inst_names.push_back(e);
    }
    std::vector<const char*> inst_ext;
    for (auto& e : inst_names) inst_ext.push_back(e.c_str());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
#if defined(__APPLE__)
    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    if (!check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance")) return false;
    volkLoadInstance(instance_);

#if defined(__APPLE__)
    VkMetalSurfaceCreateInfoEXT sci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    sci.pLayer = static_cast<const CAMetalLayer*>(mac_window_layer(hwnd_));
    if (!check(vkCreateMetalSurfaceEXT(instance_, &sci, nullptr, &surface_), "vkCreateMetalSurfaceEXT")) return false;
#else
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = inst;
    sci.hwnd = hwnd_;
    if (!check(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_), "vkCreateWin32SurfaceKHR")) return false;
#endif

    // GPU: prefer a discrete one with Vulkan 1.3 and a graphics queue that can present
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best_score = -1;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best_score) {
                best_score = score;
                gpu_ = g;
                queue_family_ = q;
                device_name_ = p.deviceName;
            }
            break;
        }
    }
    if (!gpu_) {
        fprintf(stderr, "[vulkan] no GPU with Vulkan 1.3 that can present to the window\n");
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    std::vector<std::string> dev_names = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#if defined(__APPLE__)
    // A device that lists VK_KHR_portability_subset must have it enabled (MoltenVK)
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, avail.data());
        for (auto& a : avail)
            if (!strcmp(a.extensionName, "VK_KHR_portability_subset")) dev_names.push_back(a.extensionName);
    }
#endif
    if (with_ngx) {   // only the ones this GPU/driver actually offers
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, avail.data());
        for (const std::string& e : dlss::device_extensions(instance_, gpu_))
            for (auto& a : avail)
                if (e == a.extensionName && std::find(dev_names.begin(), dev_names.end(), e) == dev_names.end())
                    dev_names.push_back(e);
    }
    // Present ids and waiting for them: when each image is really on screen
    VkPhysicalDevicePresentWaitFeaturesKHR fwait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR fid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    fid.pNext = &fwait;
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, avail.data());
        int found = 0;
        for (auto& a : avail)
            found += !strcmp(a.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) ||
                     !strcmp(a.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        if (found == 2) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext = &fid;
            vkGetPhysicalDeviceFeatures2(gpu_, &query);
            present_wait_ = fid.presentId && fwait.presentWait;
        }
#if defined(__APPLE__)
        // MoltenVK lists present wait, but on an AMD Mac its waits time out
        // instead of returning (50 ms every frame: 20 presents a second after
        // the first long gap). Pacing on vsync alone, as with drivers without it
        present_wait_ = false;
#endif
        if (present_wait_) {
            dev_names.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            dev_names.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
    }
    // Ray queries (sun shadows traced through the game world): only when the
    // driver offers all three extensions and the features they need
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR frq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateDeviceExtensionProperties(gpu_, nullptr, &count, avail.data());
        int found = 0;
        for (auto& a : avail)
            found += !strcmp(a.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
                     !strcmp(a.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
                     !strcmp(a.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        if (found == 3) {
            VkPhysicalDeviceVulkan12Features have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            frq.pNext = &have;
            fas.pNext = &frq;
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext = &fas;
            vkGetPhysicalDeviceFeatures2(gpu_, &query);
            ray_query_ = fas.accelerationStructure && frq.rayQuery && have.bufferDeviceAddress;
        }
        if (ray_query_) {
            for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                  VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
                if (std::find(dev_names.begin(), dev_names.end(), e) == dev_names.end()) dev_names.push_back(e);
            fas = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            fas.accelerationStructure = VK_TRUE;
            frq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            frq.rayQuery = VK_TRUE;
            fas.pNext = &frq;
        }
    }
    std::vector<const char*> dev_ext;
    for (auto& e : dev_names) dev_ext.push_back(e.c_str());
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    // Texture pack: an array of replacement images indexed per polygon
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    {
        // Its size is set at run time (the GPU's limits, r3d_renderer.cpp), and
        // with update-after-bind the much higher limits for that kind of binding
        // apply (Intel drivers allow few textures per shader without it)
        VkPhysicalDeviceVulkan12Features have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &have;
        vkGetPhysicalDeviceFeatures2(gpu_, &query);
        f12.runtimeDescriptorArray = have.runtimeDescriptorArray;
        if (!have.runtimeDescriptorArray) fprintf(stderr, "[vulkan] the driver has no runtimeDescriptorArray\n");
        update_after_bind_ = have.descriptorBindingSampledImageUpdateAfterBind;
        f12.descriptorBindingSampledImageUpdateAfterBind = have.descriptorBindingSampledImageUpdateAfterBind;
    }
    if (with_ngx) {   // NGX reads its own buffers through device addresses
        VkPhysicalDeviceVulkan12Features have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &have;
        vkGetPhysicalDeviceFeatures2(gpu_, &query);
        f12.bufferDeviceAddress = have.bufferDeviceAddress;
    }
    if (ray_query_) f12.bufferDeviceAddress = VK_TRUE;   // acceleration structures are built from buffer addresses
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = &f12;
    f13.dynamicRendering = VK_TRUE;
    if (present_wait_) {
        fwait.pNext = f13.pNext;
        f13.pNext = &fid;
    }
    if (ray_query_) {
        frq.pNext = f13.pNext;
        f13.pNext = &fas;
    }
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f13;
    features.features.samplerAnisotropy = VK_TRUE;
    // DLAA: the translucent pass blends colour but leaves the motion vectors alone
    features.features.independentBlend = with_ngx ? VK_TRUE : VK_FALSE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    if (!check(vkCreateDevice(gpu_, &dci, nullptr, &device_), "vkCreateDevice")) return false;
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cai, &cmd_);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &semi, nullptr, &acquired_);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fi, nullptr, &fence_);

    return create_swapchain();
}

bool VkPresenter::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;   // minimized
    extent_ = caps.currentExtent;

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR format = formats[0];
    for (auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) format = f;
    format_ = format.format;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = std::min(std::max(caps.minImageCount, 3u), caps.maxImageCount ? caps.maxImageCount : 8u);
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync at the monitor's refresh rate
    ci.clipped = VK_TRUE;
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    swapchain_first_id_ = present_id_ + 1;
    VkSwapchainKHR old = swapchain_;
    ci.oldSwapchain = old;
    if (!check(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (old) vkDestroySwapchainKHR(device_, old, nullptr);

    // The driver decides how many images the swapchain holds: ask, then fetch
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, nullptr), "vkGetSwapchainImagesKHR")) return false;
    images_.assign(image_count_, VK_NULL_HANDLE);
    views_.assign(image_count_, VK_NULL_HANDLE);
    if (!check(vkGetSwapchainImagesKHR(device_, swapchain_, &image_count_, images_.data()), "vkGetSwapchainImagesKHR")) return false;
    if (rendered_.size() < image_count_) rendered_.resize(image_count_, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < image_count_; i++) {
        if (!rendered_[i]) vkCreateSemaphore(device_, &semi, nullptr, &rendered_[i]);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &vi, nullptr, &views_[i]);
    }
    return true;
}

void VkPresenter::destroy_swapchain() {
    std::unique_lock<std::shared_mutex> lock(swapchain_mutex_);
    for (uint32_t i = 0; i < image_count_; i++)
        if (views_[i]) vkDestroyImageView(device_, views_[i], nullptr);
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkPresenter::pump() {
#if defined(__APPLE__)
    mac_window_pump();
#else
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
#endif
    return !quit_;
}

void VkPresenter::run_once(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd);
}

void VkPresenter::frame(const RecordFn& record) {
    if (!device_) return;
    if (resized_) {
        vkDeviceWaitIdle(device_);
        if (!create_swapchain()) { Sleep(16); return; }
        resized_ = false;
    }
    if (!swapchain_) return;

    double t0 = seconds();
    MAC_NOTE(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    double t_fence = seconds();
    uint32_t index = 0;
    VkResult r = MAC_NOTE(vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index),
                           "vkAcquireNextImageKHR");
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { resized_ = true; return; }
    vkResetFences(device_, 1, &fence_);
    double t1 = seconds();

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    record(cmd_, views_[index], extent_);
    double t2 = seconds();
    bool capture = !capture_path_.empty();
    if (capture) {
        // Copy the finished image to a host-visible buffer
        if (capture_buffer_) {
            vkDestroyBuffer(device_, capture_buffer_, nullptr);
            vkFreeMemory(device_, capture_memory_, nullptr);
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(extent_.width) * extent_.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkCreateBuffer(device_, &bci, nullptr, &capture_buffer_);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, capture_buffer_, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device_, &mai, nullptr, &capture_memory_);
        vkBindBufferMemory(device_, capture_buffer_, capture_memory_, 0);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer_, 1, &c);
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        vk_image_barrier(cmd_, images_[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
    MAC_NOTE(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    VkPresentIdKHR pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
    uint64_t id = 0;
    if (present_wait_) {
        id = ++present_id_;
        pid.swapchainCount = 1;
        pid.pPresentIds = &id;
        pi.pNext = &pid;
    }
    r = MAC_NOTE(vkQueuePresentKHR(queue_, &pi), "vkQueuePresentKHR");
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) resized_ = true;
    double t3 = seconds();
    timing_.gpu_wait += t1 - t0;
    timing_.record += t2 - t1;
    timing_.present += t3 - t2;
    timing_.frames++;
    last_ = {t_fence - t0, t1 - t_fence, t2 - t1, t3 - t2, id};
    if (capture) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        void* data = nullptr;
        vkMapMemory(device_, capture_memory_, 0, VK_WHOLE_SIZE, 0, &data);
        if (FILE* f = fopen(capture_path_.c_str(), "wb")) {
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + extent_.width * extent_.height * 4;
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(extent_.width);
            ih.biHeight = -LONG(extent_.height);   // top-down
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&ih, sizeof(ih), 1, f);
            fwrite(data, 4, size_t(extent_.width) * extent_.height, f);   // BGRA swapchain = BMP order
            fclose(f);
        }
        vkUnmapMemory(device_, capture_memory_);
        capture_path_.clear();
    }
}

bool VkPresenter::wait_present(uint64_t id, uint64_t timeout_ns) {
    std::shared_lock<std::shared_mutex> lock(swapchain_mutex_);
    if (!present_wait_ || !swapchain_ || id < swapchain_first_id_) return false;
    VkResult r = vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns);
    return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

double VkPresenter::limit_queue(int depth, uint64_t timeout_ns) {
    if (!present_wait_ || !swapchain_ || depth <= 0 || present_id_ < uint64_t(depth)) return 0;
    uint64_t id = present_id_ + 1 - uint64_t(depth);
    if (id < swapchain_first_id_ || vkWaitForPresentKHR(device_, swapchain_, id, timeout_ns) != VK_SUCCESS) return 0;
    return seconds();
}

void VkPresenter::shutdown() {
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroy_swapchain();
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
#if defined(__APPLE__)
    if (hwnd_) mac_window_destroy(hwnd_);
#else
    if (hwnd_) DestroyWindow(hwnd_);
#endif
}
