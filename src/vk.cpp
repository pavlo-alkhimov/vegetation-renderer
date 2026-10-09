// Vulkan device, swapchain, frames in flight, and the few allocation helpers this stage needs.
// Interim: one vkAllocateMemory per resource (a handful); the block sub-allocators of docs/02 come with streaming.

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES  8
#define BINDLESS_TEXTURES 256
#define UPLOAD_BYTES (4u << 20)     // per frame in flight: FrameConstants + TerrainNode array

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FATAL("%s:%d: %s = %d", __FILE__, __LINE__, #x, (int)r_); } while (0)

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceAddress address;
    VkDeviceSize size;
} VkBuf;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    VkQueryPool queries;
    VkBuf upload;           // host-visible, persistently mapped
    bool submitted;
} VkFrame;

typedef struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkDevice device;
    VkQueue queue;
    u32 queue_family;
    bool timestamps;
    bool wireframe_supported;

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D extent;
    u32 image_count;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkSemaphore rendered[MAX_SWAP_IMAGES];   // per swapchain image: safe reuse with presentation
    bool vsync;
    bool swap_transfer_src;

    VkImage depth;
    VkImageView depth_view;
    VkDeviceMemory depth_memory;

    VkFrame frames[FRAMES_IN_FLIGHT];
    u64 frame_index;

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    VkSampler sampler_linear;
} Vk;

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    fprintf(stderr, "vulkan %s: %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", data->pMessage);
    return VK_FALSE;
}

static void vk_create_instance(Vk* vk, const char* const* extensions, u32 extension_count, bool validation)
{
    const char* exts[32];
    u32 n = 0;
    for (u32 i = 0; i < extension_count && n < 30; i++) exts[n++] = extensions[i];
    const char* layers[1] = {"VK_LAYER_KHRONOS_validation"};
    u32 layer_count = 0;
    if (validation) {
        u32 count = 0;
        vkEnumerateInstanceLayerProperties(&count, NULL);
        VkLayerProperties props[64];
        count = MIN(count, 64u);
        vkEnumerateInstanceLayerProperties(&count, props);
        for (u32 i = 0; i < count; i++) if (!strcmp(props[i].layerName, layers[0])) layer_count = 1;
        if (layer_count) exts[n++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        else fprintf(stderr, "validation layer not found, continuing without\n");
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vegetation-renderer";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = layer_count;
    ci.ppEnabledLayerNames = layers;
    VK_CHECK(vkCreateInstance(&ci, NULL, &vk->instance));
    if (layer_count) {
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = vk_debug_callback;
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vk->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create) create(vk->instance, &mi, NULL, &vk->messenger);
    }
}

static u32 vk_memory_type(Vk* vk, u32 type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags flags = pass == 0 ? want | prefer : want;
        for (u32 i = 0; i < vk->mem_props.memoryTypeCount; i++)
            if ((type_bits & (1u << i)) && (vk->mem_props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    FATAL("no memory type for flags 0x%x", want);
}

static VkDeviceMemory vk_alloc(Vk* vk, VkMemoryRequirements req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer, bool device_address)
{
    VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = device_address ? &flags : NULL;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk_memory_type(vk, req.memoryTypeBits, want, prefer);
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(vk->device, &ai, NULL, &mem));
    return mem;
}

// Host-visible buffers prefer DEVICE_LOCAL too (resizable BAR).
static VkBuf vk_buffer(Vk* vk, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible)
{
    VkBuf b = {};
    b.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vkCreateBuffer(vk->device, &ci, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, b.buffer, &req);
    if (host_visible)
        b.memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
    else
        b.memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    VK_CHECK(vkBindBufferMemory(vk->device, b.buffer, b.memory, 0));
    if (host_visible) VK_CHECK(vkMapMemory(vk->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    VkBufferDeviceAddressInfo ai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(vk->device, &ai);
    return b;
}

static void vk_buffer_destroy(Vk* vk, VkBuf* b)
{
    vkDestroyBuffer(vk->device, b->buffer, NULL);
    vkFreeMemory(vk->device, b->memory, NULL);
    memset(b, 0, sizeof(*b));
}

// One-shot command buffer for load-time work; blocks until done.
static VkCommandBuffer vk_begin_once(Vk* vk)
{
    VkFrame* f = &vk->frames[0];
    VK_CHECK(vkResetCommandPool(vk->device, f->pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f->cmd, &bi));
    return f->cmd;
}

static void vk_end_once(Vk* vk)
{
    VkFrame* f = &vk->frames[0];
    VK_CHECK(vkEndCommandBuffer(f->cmd));
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f->cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
}

static void vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, u32 base_mip, u32 mip_count,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkImageLayout old_layout,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout new_layout)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage; b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage; b.dstAccessMask = dst_access;
    b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base_mip, mip_count, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

static void vk_init_device(Vk* vk, VkSurfaceKHR surface)
{
    vk->surface = surface;
    u32 count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(vk->instance, &count, NULL));
    VkPhysicalDevice devs[16];
    count = MIN(count, 16u);
    VK_CHECK(vkEnumeratePhysicalDevices(vk->instance, &count, devs));
    // Prefer a discrete GPU with Vulkan 1.3 and a graphics queue that can present.
    int best = -1, best_score = -1;
    u32 best_family = 0;
    for (u32 i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        u32 fam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &fam_count, NULL);
        VkQueueFamilyProperties fams[16];
        fam_count = MIN(fam_count, 16u);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &fam_count, fams);
        for (u32 q = 0; q < fam_count; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &present);
            if (!(fams[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (score > best_score) { best = (int)i; best_score = score; best_family = q; }
            break;
        }
    }
    if (best < 0) FATAL("no Vulkan 1.3 GPU with graphics + present");
    vk->phys = devs[best];
    vk->queue_family = best_family;
    vkGetPhysicalDeviceProperties(vk->phys, &vk->props);
    vkGetPhysicalDeviceMemoryProperties(vk->phys, &vk->mem_props);
    {
        u32 fam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &fam_count, NULL);
        VkQueueFamilyProperties fams[16];
        fam_count = MIN(fam_count, 16u);
        vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &fam_count, fams);
        vk->timestamps = fams[vk->queue_family].timestampValidBits > 0 && vk->props.limits.timestampPeriod > 0;
    }
    printf("GPU: %s (Vulkan %u.%u.%u)\n", vk->props.deviceName, VK_API_VERSION_MAJOR(vk->props.apiVersion),
           VK_API_VERSION_MINOR(vk->props.apiVersion), VK_API_VERSION_PATCH(vk->props.apiVersion));

    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(vk->phys, &supported);
    vk->wireframe_supported = supported.fillModeNonSolid;

    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.fillModeNonSolid = supported.fillModeNonSolid;

    f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = vk->queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = ARRAY_COUNT(exts);
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(vk->phys, &ci, NULL, &vk->device));
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkFrame* f = &vk->frames[i];
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = vk->queue_family;
        VK_CHECK(vkCreateCommandPool(vk->device, &pi, NULL, &f->pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(vk->device, &ai, &f->cmd));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(vk->device, &fi, NULL, &f->fence));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(vk->device, &si, NULL, &f->acquired));
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2;
        VK_CHECK(vkCreateQueryPool(vk->device, &qpi, NULL, &f->queries));
        vkResetQueryPool(vk->device, f->queries, 0, 2);
        f->upload = vk_buffer(vk, UPLOAD_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }

    // Bindless set: sampled images + samplers (docs/02). Push constants carry buffer device addresses.
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = BINDLESS_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorBindingFlags binding_flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 2;
    bfi.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 2;
    li.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk->device, &li, NULL, &vk->set_layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_TEXTURES}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &dpi, NULL, &vk->descriptor_pool));
    VkDescriptorSetAllocateInfo sai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = vk->descriptor_pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &vk->set_layout;
    VK_CHECK(vkAllocateDescriptorSets(vk->device, &sai, &vk->set));

    VkSamplerCreateInfo smp = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(vk->device, &smp, NULL, &vk->sampler_linear));
    VkDescriptorImageInfo sii = {};
    sii.sampler = vk->sampler_linear;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = vk->set;
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &sii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &vk->set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(vk->device, &pli, NULL, &vk->pipeline_layout));
}

static void vk_bind_texture(Vk* vk, u32 index, VkImageView view)
{
    VkDescriptorImageInfo ii = {};
    ii.imageView = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = vk->set;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
}

static void vk_destroy_swapchain_resources(Vk* vk)
{
    for (u32 i = 0; i < vk->image_count; i++) {
        vkDestroyImageView(vk->device, vk->views[i], NULL);
        vkDestroySemaphore(vk->device, vk->rendered[i], NULL);
    }
    vkDestroyImageView(vk->device, vk->depth_view, NULL);
    vkDestroyImage(vk->device, vk->depth, NULL);
    vkFreeMemory(vk->device, vk->depth_memory, NULL);
}

// (Re)creates swapchain and depth buffer for the given pixel size. Returns false if the window is minimized.
static bool vk_create_swapchain(Vk* vk, u32 width, u32 height)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys, vk->surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xffffffffu) {
        extent.width = CLAMP(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = CLAMP(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return false;

    if (!vk->swap_format) {
        u32 n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &n, NULL);
        VkSurfaceFormatKHR formats[64];
        n = MIN(n, 64u);
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &n, formats);
        vk->swap_format = formats[0].format;
        for (u32 i = 0; i < n; i++)
            if ((formats[i].format == VK_FORMAT_B8G8R8A8_SRGB || formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { vk->swap_format = formats[i].format; break; }
    }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vk->vsync) {
        u32 n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys, vk->surface, &n, NULL);
        VkPresentModeKHR modes[16];
        n = MIN(n, 16u);
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk->phys, vk->surface, &n, modes);
        for (u32 i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
        for (u32 i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
    }

    VkSwapchainKHR old = vk->swapchain;
    if (old) { vkDeviceWaitIdle(vk->device); vk_destroy_swapchain_resources(vk); }
    vk->swap_transfer_src = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = vk->surface;
    ci.minImageCount = MAX(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = MIN(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = vk->swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (vk->swap_transfer_src ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(vk->device, &ci, NULL, &vk->swapchain));
    if (old) vkDestroySwapchainKHR(vk->device, old, NULL);
    vk->extent = extent;

    vk->image_count = 0;
    VK_CHECK(vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count, NULL));
    vk->image_count = MIN(vk->image_count, (u32)MAX_SWAP_IMAGES);
    VK_CHECK(vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->image_count, vk->images));
    for (u32 i = 0; i < vk->image_count; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = vk->images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vk->swap_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &vk->views[i]));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(vk->device, &si, NULL, &vk->rendered[i]));
    }

    VkImageCreateInfo di = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    di.imageType = VK_IMAGE_TYPE_2D;
    di.format = VK_FORMAT_D32_SFLOAT;
    di.extent = {extent.width, extent.height, 1};
    di.mipLevels = di.arrayLayers = 1;
    di.samples = VK_SAMPLE_COUNT_1_BIT;
    di.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(vkCreateImage(vk->device, &di, NULL, &vk->depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, vk->depth, &req);
    vk->depth_memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, vk->depth, vk->depth_memory, 0));
    VkImageViewCreateInfo dvi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvi.image = vk->depth;
    dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = VK_FORMAT_D32_SFLOAT;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk->device, &dvi, NULL, &vk->depth_view));
    return true;
}

static VkShaderModule vk_load_shader(Vk* vk, const char* path)
{
    size_t size;
    void* code = read_file(path, &size);
    if (!code) FATAL("cannot read shader %s (run build.sh)", path);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = (const u32*)code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(vk->device, &ci, NULL, &m));
    free(code);
    return m;
}

typedef struct {
    VkShaderModule module;
    const char* vs;
    const char* fs;
    VkCompareOp depth_compare;
    bool depth_write;
    VkCullModeFlags cull;
    VkPolygonMode polygon;
} PipelineDesc;

static VkPipeline vk_create_pipeline(Vk* vk, const PipelineDesc* d)
{
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d->module;
    stages[0].pName = d->vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = d->module;
    stages[1].pName = d->fs;
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d->polygon;
    rs.cullMode = d->cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = d->depth_write;
    ds.depthCompareOp = d->depth_compare;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &vk->swap_format;
    ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = vk->pipeline_layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}
