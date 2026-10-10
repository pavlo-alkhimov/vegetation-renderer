// Vulkan entry points loaded at runtime through the vkGetInstanceProcAddr of the loader SDL opened, so the process
// holds exactly one Vulkan loader (macOS: SDK, Homebrew and system copies can coexist) and nothing links libvulkan.
// Add a function here when the code starts using it. Device-level functions go through the instance (trampolines).

#define VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCTIONS(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginRendering) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBlitImage) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdEndRendering) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdPushConstants) \
    X(vkCmdResetQueryPool) \
    X(vkCmdSetScissor) \
    X(vkCmdSetViewport) \
    X(vkCmdWriteTimestamp2) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDevice) \
    X(vkCreateFence) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreatePipelineLayout) \
    X(vkCreateQueryPool) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroyPipeline) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFreeMemory) \
    X(vkGetBufferDeviceAddress) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetQueryPoolResults) \
    X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandPool) \
    X(vkResetFences) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)

// Optional device-extension functions, loaded with vkGetDeviceProcAddr when the extension is enabled; NULL otherwise.
#define VK_DEVICE_EXT_FUNCTIONS(X) \
    X(vkCmdDrawMeshTasksEXT)

#define X(name) static PFN_##name name;
X(vkGetInstanceProcAddr)
VK_GLOBAL_FUNCTIONS(X)
VK_INSTANCE_FUNCTIONS(X)
VK_DEVICE_EXT_FUNCTIONS(X)
#undef X

static void vk_load_global(PFN_vkGetInstanceProcAddr get_proc)
{
    vkGetInstanceProcAddr = get_proc;
#define X(name) if (!(name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) FATAL("Vulkan loader lacks %s", #name);
    VK_GLOBAL_FUNCTIONS(X)
#undef X
}

static void vk_load_instance(VkInstance instance)
{
#define X(name) if (!(name = (PFN_##name)vkGetInstanceProcAddr(instance, #name))) FATAL("Vulkan instance lacks %s", #name);
    VK_INSTANCE_FUNCTIONS(X)
#undef X
}

static void vk_load_device_ext(VkDevice device)
{
#define X(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
    VK_DEVICE_EXT_FUNCTIONS(X)
#undef X
}