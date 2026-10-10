// Vulkan device, swapchain, frames in flight, and the few allocation helpers this stage needs.
// Interim: one vkAllocateMemory per resource (a handful); the block sub-allocators of docs/02 come with streaming.

#define FRAMES_IN_FLIGHT 2
#define MAX_SWAP_IMAGES  8
#define BINDLESS_TEXTURES 256
#define UPLOAD_BYTES (4u << 20)     // per frame in flight, layout below
#define UPLOAD_VIEWS_OFFSET 1024    // FrameConstants at 0 (<= 1 KB), then ViewConstants[MAX_VIEWS]
#define UPLOAD_NODES_OFFSET 4096    // TerrainNode array, then TreeChunk array (vegetation.cpp), then OverlayData
#define GPU_TIMESTAMPS 8            // frame start, after shadows, terrain, trees, grass blades, plants, sky + TAA; end
#define GPU_PASSES (GPU_TIMESTAMPS - 2)

#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FATAL("%s:%d: %s = %d", __FILE__, __LINE__, #x, (int)r_); } while (0)

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* mapped;
    VkDeviceAddress address;
    VkDeviceSize size;
} VkBuf;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} VkTarget;

#define SCENE_FORMAT   VK_FORMAT_R16G16B16A16_SFLOAT
#define MOTION_FORMAT  VK_FORMAT_R16G16_SFLOAT
#define DEPTH_FORMAT   VK_FORMAT_D32_SFLOAT

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
    bool mesh_shaders;          // VK_EXT_mesh_shader with task + mesh shaders (vegetation); optional

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D extent;
    u32 image_count;
    VkImage images[MAX_SWAP_IMAGES];
    VkImageView views[MAX_SWAP_IMAGES];
    VkSemaphore rendered[MAX_SWAP_IMAGES];   // per swapchain image: safe reuse with presentation
    bool vsync;
    bool swap_transfer_src;

    // Frame targets, recreated with the swapchain and bound at TEX_SCENE.. (gpu_shared.h).
    VkTarget depth, scene, motion, history[2];
    bool history_valid;         // false after (re)creation: the next frame starts TAA from scratch

    VkFrame frames[FRAMES_IN_FLIGHT];
    u64 frame_index;

    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    VkSampler samplers[SAMPLER_COUNT];  // SAMPLER_* (gpu_shared.h)
    f32 anisotropy;                     // 0 = not supported
    VkSampler shadow_sampler;
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
    for (u32 i = 0; i < extension_count && n < 29; i++) exts[n++] = extensions[i];
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
    // macOS: KosmicKrisp (conformant) or MoltenVK (portability driver, only enumerated with this extension + flag).
    bool portability = false;
    {
        u32 count = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
        VkExtensionProperties props[256];
        count = MIN(count, 256u);
        vkEnumerateInstanceExtensionProperties(NULL, &count, props);
        for (u32 i = 0; i < count; i++)
            if (!strcmp(props[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) portability = true;
        if (portability) exts[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vegetation-renderer";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = layer_count;
    ci.ppEnabledLayerNames = layers;
    VK_CHECK(vkCreateInstance(&ci, NULL, &vk->instance));
    vk_load_instance(vk->instance);
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
    // Prefer a discrete GPU with Vulkan 1.3 and a graphics queue that can present; among equals prefer a conformant
    // driver over a portability one (macOS: KosmicKrisp over MoltenVK when both are installed).
    int best = -1, best_score = -1;
    u32 best_family = 0;
    for (u32 i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        printf("Vulkan device %u: %s, Vulkan %u.%u.%u\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        bool portability_subset = false;
        {
            u32 n = 0;
            vkEnumerateDeviceExtensionProperties(devs[i], NULL, &n, NULL);
            VkExtensionProperties* props = (VkExtensionProperties*)malloc(MAX(n, 1u) * sizeof(VkExtensionProperties));
            vkEnumerateDeviceExtensionProperties(devs[i], NULL, &n, props);
            for (u32 e = 0; e < n; e++) if (!strcmp(props[e].extensionName, "VK_KHR_portability_subset")) portability_subset = true;
            free(props);
        }
        u32 fam_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &fam_count, NULL);
        VkQueueFamilyProperties fams[16];
        fam_count = MIN(fam_count, 16u);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &fam_count, fams);
        for (u32 q = 0; q < fam_count; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface, &present);
            if (!(fams[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1) * 2
                      + (portability_subset ? 0 : 1);
            if (score > best_score) { best = (int)i; best_score = score; best_family = q; }
            break;
        }
    }
    if (best < 0) FATAL("no Vulkan 1.3 GPU with graphics + present (macOS: install the Vulkan SDK with KosmicKrisp, see README)");
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

    // Query, check and enable exactly the features this code uses; name what is missing instead of failing in
    // vkCreateDevice. Portability drivers (MoltenVK) are the likely place for gaps.
    bool has_portability_subset = false, has_mesh_shader = false;
    {
        u32 count = 0;
        vkEnumerateDeviceExtensionProperties(vk->phys, NULL, &count, NULL);
        VkExtensionProperties* props = (VkExtensionProperties*)malloc(MAX(count, 1u) * sizeof(VkExtensionProperties));
        vkEnumerateDeviceExtensionProperties(vk->phys, NULL, &count, props);
        for (u32 i = 0; i < count; i++) {
            if (!strcmp(props[i].extensionName, "VK_KHR_portability_subset")) has_portability_subset = true;
            if (!strcmp(props[i].extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME)) has_mesh_shader = true;
        }
        free(props);
    }
    VkPhysicalDeviceMeshShaderFeaturesEXT smesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features s13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features s12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features s11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 s2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    s2.pNext = &s11; s11.pNext = &s12; s12.pNext = &s13;
    if (has_mesh_shader) s13.pNext = &smesh;
    vkGetPhysicalDeviceFeatures2(vk->phys, &s2);
    vk->mesh_shaders = has_mesh_shader && smesh.taskShader && smesh.meshShader;
    if (!vk->mesh_shaders) printf("no task/mesh shader support (VK_EXT_mesh_shader): vegetation disabled\n");
    struct { VkBool32 ok; const char* name; } required[] = {
        {s13.dynamicRendering, "dynamicRendering"},
        {s13.synchronization2, "synchronization2"},
        {s12.bufferDeviceAddress, "bufferDeviceAddress"},
        {s12.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {s12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {s12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind"},
        {s11.shaderDrawParameters, "shaderDrawParameters"},
        {s2.features.shaderInt64, "shaderInt64"},
    };
    char missing[512] = "";
    for (u32 i = 0; i < ARRAY_COUNT(required); i++)
        if (!required[i].ok) { strncat(missing, " ", sizeof(missing) - strlen(missing) - 1); strncat(missing, required[i].name, sizeof(missing) - strlen(missing) - 1); }
    if (missing[0]) FATAL("%s lacks required Vulkan features:%s", vk->props.deviceName, missing);
    vk->wireframe_supported = s2.features.fillModeNonSolid;

    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    fmesh.taskShader = VK_TRUE;
    fmesh.meshShader = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.pNext = vk->mesh_shaders ? &fmesh : NULL;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    f11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.fillModeNonSolid = s2.features.fillModeNonSolid;
    f2.features.samplerAnisotropy = s2.features.samplerAnisotropy;
    vk->anisotropy = s2.features.samplerAnisotropy ? MIN(8.0f, vk->props.limits.maxSamplerAnisotropy) : 0.0f;

    f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = vk->queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    // VK_KHR_portability_subset must be enabled whenever the device exposes it (MoltenVK).
    const char* exts[3] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    u32 ext_count = 1;
    if (has_portability_subset) exts[ext_count++] = "VK_KHR_portability_subset";
    if (vk->mesh_shaders) exts[ext_count++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = exts;
    VK_CHECK(vkCreateDevice(vk->phys, &ci, NULL, &vk->device));
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
    vk_load_device_ext(vk->device);
    if (vk->mesh_shaders && !vkCmdDrawMeshTasksEXT) { printf("vkCmdDrawMeshTasksEXT missing: vegetation disabled\n"); vk->mesh_shaders = false; }

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
        qpi.queryCount = GPU_TIMESTAMPS;
        VK_CHECK(vkCreateQueryPool(vk->device, &qpi, NULL, &f->queries));   // reset in the command buffer before use
        f->upload = vk_buffer(vk, UPLOAD_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }

    // Bindless set: sampled images + samplers (docs/02). Push constants carry buffer device addresses.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = BINDLESS_TEXTURES;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = SAMPLER_COUNT;
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[2].binding = 2;                            // shadow comparison sampler
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[3].binding = 3;                            // shadow map (2D array; written by shadows_init)
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorBindingFlags binding_flags[4] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 4;
    bfi.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 4;
    li.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(vk->device, &li, NULL, &vk->set_layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_TEXTURES + 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLER_COUNT + 1}};
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
    VK_CHECK(vkCreateSampler(vk->device, &smp, NULL, &vk->samplers[SAMPLER_LINEAR_CLAMP]));
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.anisotropyEnable = vk->anisotropy > 0;
    smp.maxAnisotropy = MAX(vk->anisotropy, 1.0f);
    VK_CHECK(vkCreateSampler(vk->device, &smp, NULL, &vk->samplers[SAMPLER_ANISO_REPEAT]));
    VkDescriptorImageInfo sii[SAMPLER_COUNT] = {};
    for (u32 i = 0; i < SAMPLER_COUNT; i++) sii[i].sampler = vk->samplers[i];
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = vk->set;
    w.dstBinding = 1;
    w.descriptorCount = SAMPLER_COUNT;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = sii;
    vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
    VkSamplerCreateInfo cmp = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    cmp.magFilter = cmp.minFilter = VK_FILTER_LINEAR;                // bilinear PCF
    cmp.addressModeU = cmp.addressModeV = cmp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    cmp.compareEnable = VK_TRUE;
    cmp.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;                  // reversed Z: lit if not behind the closest caster
    VK_CHECK(vkCreateSampler(vk->device, &cmp, NULL, &vk->shadow_sampler));
    VkDescriptorImageInfo csi = {};
    csi.sampler = vk->shadow_sampler;
    w.dstBinding = 2;
    w.descriptorCount = 1;
    w.pImageInfo = &csi;
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
    VkTarget* targets[5] = {&vk->depth, &vk->scene, &vk->motion, &vk->history[0], &vk->history[1]};
    for (u32 i = 0; i < 5; i++) {
        vkDestroyImageView(vk->device, targets[i]->view, NULL);
        vkDestroyImage(vk->device, targets[i]->image, NULL);
        vkFreeMemory(vk->device, targets[i]->memory, NULL);
    }
}

static VkTarget vk_target(Vk* vk, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkExtent2D extent)
{
    VkTarget t = {};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, t.image, &req);
    t.memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, t.image, t.memory, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &t.view));
    return t;
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

    const VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT;
    vk->depth = vk_target(vk, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | sampled, VK_IMAGE_ASPECT_DEPTH_BIT, extent);
    vk->scene = vk_target(vk, SCENE_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, VK_IMAGE_ASPECT_COLOR_BIT, extent);
    vk->motion = vk_target(vk, MOTION_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, VK_IMAGE_ASPECT_COLOR_BIT, extent);
    for (u32 i = 0; i < 2; i++)
        vk->history[i] = vk_target(vk, SCENE_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | sampled, VK_IMAGE_ASPECT_COLOR_BIT, extent);
    // Every frame target is sampled in SHADER_READ_ONLY_OPTIMAL; frames transition them from and back to it.
    VkCommandBuffer cmd = vk_begin_once(vk);
    vk_image_barrier(cmd, vk->depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkTarget* colour[4] = {&vk->scene, &vk->motion, &vk->history[0], &vk->history[1]};
    for (u32 i = 0; i < 4; i++)
        vk_image_barrier(cmd, colour[i]->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_end_once(vk);
    vk_bind_texture(vk, TEX_SCENE, vk->scene.view);
    vk_bind_texture(vk, TEX_MOTION, vk->motion.view);
    vk_bind_texture(vk, TEX_DEPTH, vk->depth.view);
    vk_bind_texture(vk, TEX_HISTORY + 0, vk->history[0].view);
    vk_bind_texture(vk, TEX_HISTORY + 1, vk->history[1].view);
    vk->history_valid = false;
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
    const char* vs;             // vertex pipeline: vertex shader; mesh pipeline: NULL
    const char* fs;
    VkCompareOp depth_compare;
    bool depth_write;
    VkCullModeFlags cull;
    VkPolygonMode polygon;
    const char* ts;             // mesh pipeline: task shader (optional)
    const char* ms;             // mesh pipeline: mesh shader
    VkShaderModule task_module; // task shader module if different from module. Task shaders get their own module:
                                // with task and mesh shader in one module, the NVIDIA driver (2026-10) passed garbage
                                // payloads to the mesh shader although the SPIR-V validates.
    bool blend;                 // alpha blending (premultiplied: src * a + dst * (1 - a))
    u32 color_count;            // 0: scene pass (SCENE_FORMAT colour + MOTION_FORMAT motion, DEPTH_FORMAT depth)
    VkFormat color_formats[2];  // otherwise these, and no depth attachment
    bool depth_only;            // shadow pass: no colour, DEPTH_FORMAT depth (overrides the above)
} PipelineDesc;

static VkPipeline vk_create_pipeline(Vk* vk, const PipelineDesc* d)
{
    VkPipelineShaderStageCreateInfo stages[3] = {};
    u32 stage_count = 0;
    struct { const char* name; VkShaderStageFlagBits stage; } list[4] = {
        {d->ts, VK_SHADER_STAGE_TASK_BIT_EXT}, {d->ms, VK_SHADER_STAGE_MESH_BIT_EXT},
        {d->vs, VK_SHADER_STAGE_VERTEX_BIT}, {d->fs, VK_SHADER_STAGE_FRAGMENT_BIT}};
    for (u32 i = 0; i < 4; i++) {
        if (!list[i].name) continue;
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = list[i].stage;
        stages[stage_count].module = list[i].stage == VK_SHADER_STAGE_TASK_BIT_EXT && d->task_module ? d->task_module : d->module;
        stages[stage_count].pName = list[i].name;
        stage_count++;
    }
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
    bool scene = d->color_count == 0 && !d->depth_only;
    ds.depthTestEnable = scene || d->depth_only;
    ds.depthWriteEnable = d->depth_write;
    ds.depthCompareOp = d->depth_compare;
    VkPipelineColorBlendAttachmentState ba[2] = {};
    ba[0].colorWriteMask = ba[1].colorWriteMask = 0xf;
    if (d->blend) {
        ba[0].blendEnable = VK_TRUE;
        ba[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba[0].colorBlendOp = VK_BLEND_OP_ADD;
        ba[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba[0].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const VkFormat scene_formats[2] = {SCENE_FORMAT, MOTION_FORMAT};
    u32 color_count = d->depth_only ? 0 : scene ? 2 : d->color_count;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color_count;
    cb.pAttachments = ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = color_count;
    ri.pColorAttachmentFormats = scene ? scene_formats : d->color_formats;
    ri.depthAttachmentFormat = scene || d->depth_only ? DEPTH_FORMAT : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = stage_count;
    ci.pStages = stages;
    ci.pVertexInputState = d->ms ? NULL : &vi;
    ci.pInputAssemblyState = d->ms ? NULL : &ia;
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

// Device-local buffer filled from CPU memory through a host-visible staging buffer (blocking; load time only).
static VkBuf vk_buffer_static(Vk* vk, VkDeviceSize size, VkBufferUsageFlags usage, const void* data)
{
    VkBuf b = vk_buffer(vk, MAX(size, (VkDeviceSize)16), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    const VkDeviceSize piece = 64u << 20;
    VkBuf staging = vk_buffer(vk, MIN(MAX(size, (VkDeviceSize)16), piece), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (VkDeviceSize off = 0; off < size; off += piece) {
        VkDeviceSize n = MIN(piece, size - off);
        memcpy(staging.mapped, (const u8*)data + off, (size_t)n);
        VkCommandBuffer cmd = vk_begin_once(vk);
        VkBufferCopy region = {0, off, n};
        vkCmdCopyBuffer(cmd, staging.buffer, b.buffer, 1, &region);
        vk_end_once(vk);
    }
    vk_buffer_destroy(vk, &staging);
    return b;
}

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    u32 mips;
} VkTex;

// 2D texture from tightly packed texels (row 0 first), uploaded in row bands; full mip chain by linear blits when
// the format supports it. Left in SHADER_READ_ONLY_OPTIMAL.
static VkTex vk_texture_2d(Vk* vk, VkFormat format, u32 texel_bytes, u32 w, u32 h, const void* data, bool mips)
{
    VkTex t = {};
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk->phys, format, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    t.mips = 1;
    if (mips && (fp.optimalTilingFeatures & need) == need)
        while ((MAX(w, h) >> t.mips) > 0) t.mips++;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = t.mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, t.image, &req);
    t.memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, t.image, t.memory, 0));

    VkDeviceSize row_bytes = (VkDeviceSize)w * texel_bytes, staging_size = MAX(row_bytes, (VkDeviceSize)64u << 20);
    VkBuf staging = vk_buffer(vk, staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    u32 rows_per_band = (u32)(staging_size / row_bytes);
    for (u32 row = 0; row < h; row += rows_per_band) {
        u32 rows = MIN(rows_per_band, h - row);
        memcpy(staging.mapped, (const u8*)data + row * row_bytes, (size_t)(rows * row_bytes));
        VkCommandBuffer cmd = vk_begin_once(vk);
        if (row == 0)
            vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, (i32)row, 0};
        region.imageExtent = {w, rows, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vk_end_once(vk);
    }
    vk_buffer_destroy(vk, &staging);

    VkCommandBuffer cmd = vk_begin_once(vk);
    for (u32 m = 1; m < t.mips; m++) {
        vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit b = {};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        b.srcOffsets[1] = {(i32)MAX(w >> (m - 1), 1u), (i32)MAX(h >> (m - 1), 1u), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        b.dstOffsets[1] = {(i32)MAX(w >> m, 1u), (i32)MAX(h >> m, 1u), 1};
        vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (t.mips > 1)
        vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips - 1, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.mips - 1, 1, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_end_once(vk);

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.mips, 0, 1};
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &t.view));
    return t;
}

// Square 2D texture with a prebuilt mip chain (tightly packed, mip 0 first; the whole chain must fit 64 MB).
static VkTex vk_texture_2d_levels(Vk* vk, VkFormat format, u32 texel_bytes, u32 size, u32 mips, const void* data)
{
    VkTex t = {};
    t.mips = mips;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {size, size, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateImage(vk->device, &ci, NULL, &t.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, t.image, &req);
    t.memory = vk_alloc(vk, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
    VK_CHECK(vkBindImageMemory(vk->device, t.image, t.memory, 0));

    VkDeviceSize total = 0;
    for (u32 m = 0; m < mips; m++) total += (VkDeviceSize)MAX(size >> m, 1u) * MAX(size >> m, 1u) * texel_bytes;
    ASSERT(total <= (64u << 20));
    VkBuf staging = vk_buffer(vk, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    memcpy(staging.mapped, data, (size_t)total);
    VkCommandBuffer cmd = vk_begin_once(vk);
    vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy regions[16] = {};
    VkDeviceSize off = 0;
    ASSERT(mips <= 16);
    for (u32 m = 0; m < mips; m++) {
        u32 s = MAX(size >> m, 1u);
        regions[m].bufferOffset = off;
        regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        regions[m].imageExtent = {s, s, 1};
        off += (VkDeviceSize)s * s * texel_bytes;
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mips, regions);
    vk_image_barrier(cmd, t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_end_once(vk);
    vk_buffer_destroy(vk, &staging);

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VK_CHECK(vkCreateImageView(vk->device, &vi, NULL, &t.view));
    return t;
}