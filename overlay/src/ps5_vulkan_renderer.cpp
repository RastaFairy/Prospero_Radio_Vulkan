// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_vulkan_renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

extern "C"
{
    int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index,
                        const void *param);
    int sceVideoOutClose(std::int32_t handle);
    int sceVideoOutGetResolutionStatus(std::int32_t handle, void *status);
}

namespace
{
constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::size_t kInitialVertexCapacity = 2 * 1024 * 1024;
constexpr std::size_t kInitialIndexCapacity = 1 * 1024 * 1024;
constexpr std::size_t kMaxRadioAtlasPixels = 16u * 1024u * 1024u;
constexpr std::uint64_t kMaxTexturePixels = 16ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxWholeFileBytes = 16ull * 1024ull * 1024ull;
constexpr VkDeviceSize kTextureUploadChunkBytes = 2 * 1024 * 1024;
constexpr std::uint32_t kDescriptorCapacity = 256;
constexpr std::uint32_t kMaxInstanceExtensions = 64;
constexpr std::uint32_t kMaxDeviceExtensions = 128;
constexpr std::uint32_t kMaxPhysicalDevices = 8;
constexpr std::uint32_t kMaxDisplays = 32;
constexpr std::uint32_t kMaxDisplayModes = 256;
constexpr std::uint32_t kMaxDisplayPlanes = 32;
constexpr std::uint32_t kMaxSupportedDisplaysPerPlane = 32;
constexpr std::uint32_t kMaxSurfaceFormats = 64;
constexpr std::uint32_t kMaxPresentModes = 64;
constexpr std::uint32_t kMaxQueueFamilies = 32;
constexpr std::uint32_t kMaxSwapchainImages = 8;

struct VideoOutResolutionStatus
{
    std::uint32_t full_width;
    std::uint32_t full_height;
    std::uint32_t pane_width;
    std::uint32_t pane_height;
    std::uint64_t refresh_rate;
    float screen_inches;
    std::uint32_t reserved[4];
};

static_assert(sizeof(VideoOutResolutionStatus) == 48);
static_assert(offsetof(VideoOutResolutionStatus, refresh_rate) == 16);

struct Ktx2MipLevel
{
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

constexpr std::uint8_t kKtx2Identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                              0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

template <typename T> T MakeVkStruct(VkStructureType type)
{
    T value{};
    value.sType = type;
    return value;
}

std::uint16_t ReadLe16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t ReadLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadLe64(const std::uint8_t *p)
{
    return static_cast<std::uint64_t>(ReadLe32(p)) |
           (static_cast<std::uint64_t>(ReadLe32(p + 4)) << 32);
}

bool IsReasonableVulkanCount(std::uint32_t count, std::uint32_t maximum, const char *what)
{
    if (count != 0 && count <= maximum)
        return true;
    std::fprintf(stderr, "[PS5-Vulkan] invalid %s count: %u (expected 1..%u)\n", what, count,
                 maximum);
    return false;
}

std::FILE *OpenAssetFile(const char *path)
{
    if (!path || !*path)
        return nullptr;
    std::FILE *file = std::fopen(path, "rb");
    if (!file && path[0] != '/')
    {
        char app_path[1024];
        std::snprintf(app_path, sizeof(app_path), "/app0/%s", path);
        file = std::fopen(app_path, "rb");
        /* Spritesheet sources arrive stylesheet-relative ("../controls/x.tga");
         * resolve them against the UI asset root. */
        if (!file && std::strstr(path, ".."))
        {
            const char *stripped = path;
            while (std::strncmp(stripped, "../", 3) == 0)
                stripped += 3;
            char ui_path[1024];
            std::snprintf(ui_path, sizeof(ui_path), "/app0/assets/ui/%s", stripped);
            file = std::fopen(ui_path, "rb");
        }
    }
    return file;
}

bool ReadActiveVideoOutResolution(VideoOutResolutionStatus &status)
{
    const int handle = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (handle < 0)
        return false;

    std::memset(&status, 0, sizeof(status));
    const int result = sceVideoOutGetResolutionStatus(handle, &status);
    sceVideoOutClose(handle);
    if (result != 0 || (status.full_width == 0 && status.pane_width == 0) ||
        (status.full_height == 0 && status.pane_height == 0))
        return false;

    std::fprintf(stderr,
                 "[PS5-Vulkan] VideoOut active signal: full=%ux%u pane=%ux%u refresh-id=%llu\n",
                 status.full_width, status.full_height, status.pane_width, status.pane_height,
                 static_cast<unsigned long long>(status.refresh_rate));
    return true;
}

bool ReadWholeFile(const char *path, std::vector<std::uint8_t> &out)
{
    std::FILE *file = OpenAssetFile(path);
    if (!file)
    {
        std::fprintf(stderr, "[PS5-Vulkan] cannot open asset: %s\n", path ? path : "(null)");
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fprintf(stderr, "[PS5-Vulkan] cannot seek to end of asset: %s\n", path);
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0)
    {
        std::fprintf(stderr, "[PS5-Vulkan] invalid empty asset: %s (size=%ld)\n", path, size);
        std::fclose(file);
        return false;
    }
    if (static_cast<std::uint64_t>(size) > kMaxWholeFileBytes)
    {
        std::fprintf(
            stderr,
            "[PS5-Vulkan] refusing oversized whole-file read: %s (%llu bytes; limit=%llu)\n", path,
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(kMaxWholeFileBytes));
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fprintf(stderr, "[PS5-Vulkan] cannot rewind asset: %s\n", path);
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t bytes_read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (bytes_read != out.size())
    {
        std::fprintf(stderr, "[PS5-Vulkan] short asset read: %s (%llu/%llu bytes)\n", path,
                     static_cast<unsigned long long>(bytes_read),
                     static_cast<unsigned long long>(out.size()));
        out.clear();
        return false;
    }
    return true;
}

} // namespace

Ps5VulkanRenderInterface::Ps5VulkanRenderInterface()
{
    scissor_ = {{0, 0}, {1920, 1080}};
}

Ps5VulkanRenderInterface::~Ps5VulkanRenderInterface()
{
    // The PS5 Vulkan title link exposes queue entry points but the public SDK
    // stub set does not provide vkDeviceWaitIdle. This application creates at
    // most the graphics and present queues below, and Vulkan defines
    // vkDeviceWaitIdle as waiting for all queues owned by the device.
    if (graphics_queue_ != VK_NULL_HANDLE)
        dispatch_.vkQueueWaitIdle(graphics_queue_);
    if (present_queue_ != VK_NULL_HANDLE && present_queue_ != graphics_queue_)
        dispatch_.vkQueueWaitIdle(present_queue_);

    if (white_texture_)
    {
        DestroyTexture(white_texture_);
        white_texture_ = nullptr;
    }

    for (Frame &frame : frames_)
    {
        if (frame.vertex_mapped)
            dispatch_.vkUnmapMemory(device_, frame.vertex_memory);
        if (frame.index_mapped)
            dispatch_.vkUnmapMemory(device_, frame.index_memory);
        DestroyBuffer(frame.vertex_buffer, frame.vertex_memory);
        DestroyBuffer(frame.index_buffer, frame.index_memory);
        if (frame.fence != VK_NULL_HANDLE)
            dispatch_.vkDestroyFence(device_, frame.fence, nullptr);
        if (frame.image_available != VK_NULL_HANDLE)
            dispatch_.vkDestroySemaphore(device_, frame.image_available, nullptr);
        if (frame.render_finished != VK_NULL_HANDLE)
            dispatch_.vkDestroySemaphore(device_, frame.render_finished, nullptr);
        if (frame.command_pool != VK_NULL_HANDLE)
            dispatch_.vkDestroyCommandPool(device_, frame.command_pool, nullptr);
    }
    frames_.clear();

    if (upload_command_pool_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyCommandPool(device_, upload_command_pool_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE)
        dispatch_.vkDestroySampler(device_, sampler_, nullptr);
    if (mipmap_sampler_ != VK_NULL_HANDLE)
        dispatch_.vkDestroySampler(device_, mipmap_sampler_, nullptr);
    if (descriptor_pool_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    for (VkFramebuffer framebuffer : framebuffers_)
        dispatch_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    if (render_pass_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyRenderPass(device_, render_pass_, nullptr);
    for (VkImageView view : swapchain_views_)
        dispatch_.vkDestroyImageView(device_, view, nullptr);
    if (swapchain_ != VK_NULL_HANDLE)
        dispatch_.vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    if (device_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
        dispatch_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_ != VK_NULL_HANDLE)
        dispatch_.vkDestroyInstance(instance_, nullptr);
}

namespace
{
template <typename T> T LoadVulkanProc(VkInstance instance, const char *name)
{
    return reinterpret_cast<T>(vkGetInstanceProcAddr(instance, name));
}
} // namespace

bool Ps5VulkanRenderInterface::VulkanDispatch::LoadGlobal()
{
    vkCreateInstance = LoadVulkanProc<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
    vkEnumerateInstanceExtensionProperties =
        LoadVulkanProc<PFN_vkEnumerateInstanceExtensionProperties>(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}

bool Ps5VulkanRenderInterface::VulkanDispatch::LoadInstance(VkInstance instance)
{
    vkAcquireNextImageKHR =
        LoadVulkanProc<PFN_vkAcquireNextImageKHR>(instance, "vkAcquireNextImageKHR");
    vkAllocateCommandBuffers =
        LoadVulkanProc<PFN_vkAllocateCommandBuffers>(instance, "vkAllocateCommandBuffers");
    vkAllocateDescriptorSets =
        LoadVulkanProc<PFN_vkAllocateDescriptorSets>(instance, "vkAllocateDescriptorSets");
    vkAllocateMemory = LoadVulkanProc<PFN_vkAllocateMemory>(instance, "vkAllocateMemory");
    vkBeginCommandBuffer =
        LoadVulkanProc<PFN_vkBeginCommandBuffer>(instance, "vkBeginCommandBuffer");
    vkBindBufferMemory = LoadVulkanProc<PFN_vkBindBufferMemory>(instance, "vkBindBufferMemory");
    vkBindImageMemory = LoadVulkanProc<PFN_vkBindImageMemory>(instance, "vkBindImageMemory");
    vkCmdBeginRenderPass =
        LoadVulkanProc<PFN_vkCmdBeginRenderPass>(instance, "vkCmdBeginRenderPass");
    vkCmdBindDescriptorSets =
        LoadVulkanProc<PFN_vkCmdBindDescriptorSets>(instance, "vkCmdBindDescriptorSets");
    vkCmdBindIndexBuffer =
        LoadVulkanProc<PFN_vkCmdBindIndexBuffer>(instance, "vkCmdBindIndexBuffer");
    vkCmdBindPipeline = LoadVulkanProc<PFN_vkCmdBindPipeline>(instance, "vkCmdBindPipeline");
    vkCmdBindVertexBuffers =
        LoadVulkanProc<PFN_vkCmdBindVertexBuffers>(instance, "vkCmdBindVertexBuffers");
    vkCmdCopyBufferToImage =
        LoadVulkanProc<PFN_vkCmdCopyBufferToImage>(instance, "vkCmdCopyBufferToImage");
    vkCmdDrawIndexed = LoadVulkanProc<PFN_vkCmdDrawIndexed>(instance, "vkCmdDrawIndexed");
    vkCmdEndRenderPass = LoadVulkanProc<PFN_vkCmdEndRenderPass>(instance, "vkCmdEndRenderPass");
    vkCmdPipelineBarrier =
        LoadVulkanProc<PFN_vkCmdPipelineBarrier>(instance, "vkCmdPipelineBarrier");
    vkCmdPushConstants = LoadVulkanProc<PFN_vkCmdPushConstants>(instance, "vkCmdPushConstants");
    vkCmdSetScissor = LoadVulkanProc<PFN_vkCmdSetScissor>(instance, "vkCmdSetScissor");
    vkCmdSetViewport = LoadVulkanProc<PFN_vkCmdSetViewport>(instance, "vkCmdSetViewport");
    vkCreateBuffer = LoadVulkanProc<PFN_vkCreateBuffer>(instance, "vkCreateBuffer");
    vkCreateCommandPool = LoadVulkanProc<PFN_vkCreateCommandPool>(instance, "vkCreateCommandPool");
    vkCreateDescriptorPool =
        LoadVulkanProc<PFN_vkCreateDescriptorPool>(instance, "vkCreateDescriptorPool");
    vkCreateDescriptorSetLayout =
        LoadVulkanProc<PFN_vkCreateDescriptorSetLayout>(instance, "vkCreateDescriptorSetLayout");
    vkCreateDevice = LoadVulkanProc<PFN_vkCreateDevice>(instance, "vkCreateDevice");
    vkCreateDisplayPlaneSurfaceKHR = LoadVulkanProc<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        instance, "vkCreateDisplayPlaneSurfaceKHR");
    vkCreateFence = LoadVulkanProc<PFN_vkCreateFence>(instance, "vkCreateFence");
    vkCreateFramebuffer = LoadVulkanProc<PFN_vkCreateFramebuffer>(instance, "vkCreateFramebuffer");
    vkCreateGraphicsPipelines =
        LoadVulkanProc<PFN_vkCreateGraphicsPipelines>(instance, "vkCreateGraphicsPipelines");
    vkCreateImage = LoadVulkanProc<PFN_vkCreateImage>(instance, "vkCreateImage");
    vkCreateImageView = LoadVulkanProc<PFN_vkCreateImageView>(instance, "vkCreateImageView");
    vkCreatePipelineLayout =
        LoadVulkanProc<PFN_vkCreatePipelineLayout>(instance, "vkCreatePipelineLayout");
    vkCreateRenderPass = LoadVulkanProc<PFN_vkCreateRenderPass>(instance, "vkCreateRenderPass");
    vkCreateSampler = LoadVulkanProc<PFN_vkCreateSampler>(instance, "vkCreateSampler");
    vkCreateSemaphore = LoadVulkanProc<PFN_vkCreateSemaphore>(instance, "vkCreateSemaphore");
    vkCreateShaderModule =
        LoadVulkanProc<PFN_vkCreateShaderModule>(instance, "vkCreateShaderModule");
    vkCreateSwapchainKHR =
        LoadVulkanProc<PFN_vkCreateSwapchainKHR>(instance, "vkCreateSwapchainKHR");
    vkDestroyBuffer = LoadVulkanProc<PFN_vkDestroyBuffer>(instance, "vkDestroyBuffer");
    vkDestroyCommandPool =
        LoadVulkanProc<PFN_vkDestroyCommandPool>(instance, "vkDestroyCommandPool");
    vkDestroyDescriptorPool =
        LoadVulkanProc<PFN_vkDestroyDescriptorPool>(instance, "vkDestroyDescriptorPool");
    vkDestroyDescriptorSetLayout =
        LoadVulkanProc<PFN_vkDestroyDescriptorSetLayout>(instance, "vkDestroyDescriptorSetLayout");
    vkDestroyDevice = LoadVulkanProc<PFN_vkDestroyDevice>(instance, "vkDestroyDevice");
    vkDestroyFence = LoadVulkanProc<PFN_vkDestroyFence>(instance, "vkDestroyFence");
    vkDestroyFramebuffer =
        LoadVulkanProc<PFN_vkDestroyFramebuffer>(instance, "vkDestroyFramebuffer");
    vkDestroyImage = LoadVulkanProc<PFN_vkDestroyImage>(instance, "vkDestroyImage");
    vkDestroyImageView = LoadVulkanProc<PFN_vkDestroyImageView>(instance, "vkDestroyImageView");
    vkDestroyInstance = LoadVulkanProc<PFN_vkDestroyInstance>(instance, "vkDestroyInstance");
    vkDestroyPipeline = LoadVulkanProc<PFN_vkDestroyPipeline>(instance, "vkDestroyPipeline");
    vkDestroyPipelineLayout =
        LoadVulkanProc<PFN_vkDestroyPipelineLayout>(instance, "vkDestroyPipelineLayout");
    vkDestroyRenderPass = LoadVulkanProc<PFN_vkDestroyRenderPass>(instance, "vkDestroyRenderPass");
    vkDestroySampler = LoadVulkanProc<PFN_vkDestroySampler>(instance, "vkDestroySampler");
    vkDestroySemaphore = LoadVulkanProc<PFN_vkDestroySemaphore>(instance, "vkDestroySemaphore");
    vkDestroyShaderModule =
        LoadVulkanProc<PFN_vkDestroyShaderModule>(instance, "vkDestroyShaderModule");
    vkDestroySurfaceKHR = LoadVulkanProc<PFN_vkDestroySurfaceKHR>(instance, "vkDestroySurfaceKHR");
    vkDestroySwapchainKHR =
        LoadVulkanProc<PFN_vkDestroySwapchainKHR>(instance, "vkDestroySwapchainKHR");
    vkEndCommandBuffer = LoadVulkanProc<PFN_vkEndCommandBuffer>(instance, "vkEndCommandBuffer");
    vkEnumerateDeviceExtensionProperties = LoadVulkanProc<PFN_vkEnumerateDeviceExtensionProperties>(
        instance, "vkEnumerateDeviceExtensionProperties");
    vkEnumeratePhysicalDevices =
        LoadVulkanProc<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    vkFreeCommandBuffers =
        LoadVulkanProc<PFN_vkFreeCommandBuffers>(instance, "vkFreeCommandBuffers");
    vkFreeMemory = LoadVulkanProc<PFN_vkFreeMemory>(instance, "vkFreeMemory");
    vkGetBufferMemoryRequirements = LoadVulkanProc<PFN_vkGetBufferMemoryRequirements>(
        instance, "vkGetBufferMemoryRequirements");
    vkGetDeviceQueue = LoadVulkanProc<PFN_vkGetDeviceQueue>(instance, "vkGetDeviceQueue");
    vkGetDisplayModePropertiesKHR = LoadVulkanProc<PFN_vkGetDisplayModePropertiesKHR>(
        instance, "vkGetDisplayModePropertiesKHR");
    vkGetDisplayPlaneCapabilitiesKHR = LoadVulkanProc<PFN_vkGetDisplayPlaneCapabilitiesKHR>(
        instance, "vkGetDisplayPlaneCapabilitiesKHR");
    vkGetDisplayPlaneSupportedDisplaysKHR =
        LoadVulkanProc<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
            instance, "vkGetDisplayPlaneSupportedDisplaysKHR");
    vkGetImageMemoryRequirements =
        LoadVulkanProc<PFN_vkGetImageMemoryRequirements>(instance, "vkGetImageMemoryRequirements");
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR =
        LoadVulkanProc<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
            instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR");
    vkGetPhysicalDeviceDisplayPropertiesKHR =
        LoadVulkanProc<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
            instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    vkGetPhysicalDeviceMemoryProperties = LoadVulkanProc<PFN_vkGetPhysicalDeviceMemoryProperties>(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    vkGetPhysicalDeviceFormatProperties = LoadVulkanProc<PFN_vkGetPhysicalDeviceFormatProperties>(
        instance, "vkGetPhysicalDeviceFormatProperties");
    vkGetPhysicalDeviceQueueFamilyProperties =
        LoadVulkanProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        LoadVulkanProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    vkGetPhysicalDeviceSurfaceFormatsKHR = LoadVulkanProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    vkGetPhysicalDeviceSurfacePresentModesKHR =
        LoadVulkanProc<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
            instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    vkGetPhysicalDeviceSurfaceSupportKHR = LoadVulkanProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    vkGetSwapchainImagesKHR =
        LoadVulkanProc<PFN_vkGetSwapchainImagesKHR>(instance, "vkGetSwapchainImagesKHR");
    vkMapMemory = LoadVulkanProc<PFN_vkMapMemory>(instance, "vkMapMemory");
    vkQueuePresentKHR = LoadVulkanProc<PFN_vkQueuePresentKHR>(instance, "vkQueuePresentKHR");
    vkQueueSubmit = LoadVulkanProc<PFN_vkQueueSubmit>(instance, "vkQueueSubmit");
    vkQueueWaitIdle = LoadVulkanProc<PFN_vkQueueWaitIdle>(instance, "vkQueueWaitIdle");
    vkResetCommandPool = LoadVulkanProc<PFN_vkResetCommandPool>(instance, "vkResetCommandPool");
    vkResetFences = LoadVulkanProc<PFN_vkResetFences>(instance, "vkResetFences");
    vkUnmapMemory = LoadVulkanProc<PFN_vkUnmapMemory>(instance, "vkUnmapMemory");
    vkUpdateDescriptorSets =
        LoadVulkanProc<PFN_vkUpdateDescriptorSets>(instance, "vkUpdateDescriptorSets");
    vkWaitForFences = LoadVulkanProc<PFN_vkWaitForFences>(instance, "vkWaitForFences");
    if (!vkAcquireNextImageKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkAcquireNextImageKHR\n");
        return false;
    }
    if (!vkAllocateCommandBuffers)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkAllocateCommandBuffers\n");
        return false;
    }
    if (!vkAllocateDescriptorSets)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkAllocateDescriptorSets\n");
        return false;
    }
    if (!vkAllocateMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkAllocateMemory\n");
        return false;
    }
    if (!vkBeginCommandBuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkBeginCommandBuffer\n");
        return false;
    }
    if (!vkBindBufferMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkBindBufferMemory\n");
        return false;
    }
    if (!vkBindImageMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkBindImageMemory\n");
        return false;
    }
    if (!vkCmdBeginRenderPass)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdBeginRenderPass\n");
        return false;
    }
    if (!vkCmdBindDescriptorSets)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdBindDescriptorSets\n");
        return false;
    }
    if (!vkCmdBindIndexBuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdBindIndexBuffer\n");
        return false;
    }
    if (!vkCmdBindPipeline)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdBindPipeline\n");
        return false;
    }
    if (!vkCmdBindVertexBuffers)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdBindVertexBuffers\n");
        return false;
    }
    if (!vkCmdCopyBufferToImage)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdCopyBufferToImage\n");
        return false;
    }
    if (!vkCmdDrawIndexed)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdDrawIndexed\n");
        return false;
    }
    if (!vkCmdEndRenderPass)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdEndRenderPass\n");
        return false;
    }
    if (!vkCmdPipelineBarrier)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdPipelineBarrier\n");
        return false;
    }
    if (!vkCmdPushConstants)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdPushConstants\n");
        return false;
    }
    if (!vkCmdSetScissor)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdSetScissor\n");
        return false;
    }
    if (!vkCmdSetViewport)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCmdSetViewport\n");
        return false;
    }
    if (!vkCreateBuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateBuffer\n");
        return false;
    }
    if (!vkCreateCommandPool)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateCommandPool\n");
        return false;
    }
    if (!vkCreateDescriptorPool)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateDescriptorPool\n");
        return false;
    }
    if (!vkCreateDescriptorSetLayout)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateDescriptorSetLayout\n");
        return false;
    }
    if (!vkCreateDevice)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateDevice\n");
        return false;
    }
    if (!vkCreateDisplayPlaneSurfaceKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateDisplayPlaneSurfaceKHR\n");
        return false;
    }
    if (!vkCreateFence)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateFence\n");
        return false;
    }
    if (!vkCreateFramebuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateFramebuffer\n");
        return false;
    }
    if (!vkCreateGraphicsPipelines)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateGraphicsPipelines\n");
        return false;
    }
    if (!vkCreateImage)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateImage\n");
        return false;
    }
    if (!vkCreateImageView)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateImageView\n");
        return false;
    }
    if (!vkCreatePipelineLayout)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreatePipelineLayout\n");
        return false;
    }
    if (!vkCreateRenderPass)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateRenderPass\n");
        return false;
    }
    if (!vkCreateSampler)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateSampler\n");
        return false;
    }
    if (!vkCreateSemaphore)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateSemaphore\n");
        return false;
    }
    if (!vkCreateShaderModule)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateShaderModule\n");
        return false;
    }
    if (!vkCreateSwapchainKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkCreateSwapchainKHR\n");
        return false;
    }
    if (!vkDestroyBuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyBuffer\n");
        return false;
    }
    if (!vkDestroyCommandPool)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyCommandPool\n");
        return false;
    }
    if (!vkDestroyDescriptorPool)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyDescriptorPool\n");
        return false;
    }
    if (!vkDestroyDescriptorSetLayout)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyDescriptorSetLayout\n");
        return false;
    }
    if (!vkDestroyDevice)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyDevice\n");
        return false;
    }
    if (!vkDestroyFence)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyFence\n");
        return false;
    }
    if (!vkDestroyFramebuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyFramebuffer\n");
        return false;
    }
    if (!vkDestroyImage)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyImage\n");
        return false;
    }
    if (!vkDestroyImageView)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyImageView\n");
        return false;
    }
    if (!vkDestroyInstance)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyInstance\n");
        return false;
    }
    if (!vkDestroyPipeline)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyPipeline\n");
        return false;
    }
    if (!vkDestroyPipelineLayout)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyPipelineLayout\n");
        return false;
    }
    if (!vkDestroyRenderPass)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyRenderPass\n");
        return false;
    }
    if (!vkDestroySampler)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroySampler\n");
        return false;
    }
    if (!vkDestroySemaphore)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroySemaphore\n");
        return false;
    }
    if (!vkDestroyShaderModule)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroyShaderModule\n");
        return false;
    }
    if (!vkDestroySurfaceKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroySurfaceKHR\n");
        return false;
    }
    if (!vkDestroySwapchainKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkDestroySwapchainKHR\n");
        return false;
    }
    if (!vkEndCommandBuffer)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkEndCommandBuffer\n");
        return false;
    }
    if (!vkEnumerateDeviceExtensionProperties)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkEnumerateDeviceExtensionProperties\n");
        return false;
    }
    if (!vkEnumeratePhysicalDevices)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkEnumeratePhysicalDevices\n");
        return false;
    }
    if (!vkFreeCommandBuffers)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkFreeCommandBuffers\n");
        return false;
    }
    if (!vkFreeMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkFreeMemory\n");
        return false;
    }
    if (!vkGetBufferMemoryRequirements)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetBufferMemoryRequirements\n");
        return false;
    }
    if (!vkGetDeviceQueue)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetDeviceQueue\n");
        return false;
    }
    if (!vkGetDisplayModePropertiesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetDisplayModePropertiesKHR\n");
        return false;
    }
    if (!vkGetDisplayPlaneCapabilitiesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetDisplayPlaneCapabilitiesKHR\n");
        return false;
    }
    if (!vkGetDisplayPlaneSupportedDisplaysKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetDisplayPlaneSupportedDisplaysKHR\n");
        return false;
    }
    if (!vkGetImageMemoryRequirements)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetImageMemoryRequirements\n");
        return false;
    }
    if (!vkGetPhysicalDeviceDisplayPlanePropertiesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceDisplayPlanePropertiesKHR\n");
        return false;
    }
    if (!vkGetPhysicalDeviceDisplayPropertiesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceDisplayPropertiesKHR\n");
        return false;
    }
    if (!vkGetPhysicalDeviceMemoryProperties)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceMemoryProperties\n");
        return false;
    }
    if (!vkGetPhysicalDeviceQueueFamilyProperties)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceQueueFamilyProperties\n");
        return false;
    }
    if (!vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceSurfaceCapabilitiesKHR\n");
        return false;
    }
    if (!vkGetPhysicalDeviceSurfaceFormatsKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceSurfaceFormatsKHR\n");
        return false;
    }
    if (!vkGetPhysicalDeviceSurfacePresentModesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceSurfacePresentModesKHR\n");
        return false;
    }
    if (!vkGetPhysicalDeviceSurfaceSupportKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetPhysicalDeviceSurfaceSupportKHR\n");
        return false;
    }
    if (!vkGetSwapchainImagesKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkGetSwapchainImagesKHR\n");
        return false;
    }
    if (!vkMapMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkMapMemory\n");
        return false;
    }
    if (!vkQueuePresentKHR)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkQueuePresentKHR\n");
        return false;
    }
    if (!vkQueueSubmit)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkQueueSubmit\n");
        return false;
    }
    if (!vkQueueWaitIdle)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkQueueWaitIdle\n");
        return false;
    }
    if (!vkResetCommandPool)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkResetCommandPool\n");
        return false;
    }
    if (!vkResetFences)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkResetFences\n");
        return false;
    }
    if (!vkUnmapMemory)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkUnmapMemory\n");
        return false;
    }
    if (!vkUpdateDescriptorSets)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkUpdateDescriptorSets\n");
        return false;
    }
    if (!vkWaitForFences)
    {
        std::fprintf(stderr, "Missing Vulkan proc: vkWaitForFences\n");
        return false;
    }
    return true;
}

bool Ps5VulkanRenderInterface::LoadGlobalVulkanDispatch()
{
    return dispatch_.LoadGlobal();
}

bool Ps5VulkanRenderInterface::LoadInstanceVulkanDispatch()
{
    return dispatch_.LoadInstance(instance_);
}

bool Ps5VulkanRenderInterface::CheckResult(VkResult result, const char *what) const
{
    if (result == VK_SUCCESS)
        return true;
    std::fprintf(stderr, "[PS5-Vulkan] %s failed: VkResult=%d\n", what, static_cast<int>(result));
    return false;
}

bool Ps5VulkanRenderInterface::Initialize(const char *vertex_shader_path,
                                          const char *fragment_shader_path)
{
    if (!LoadGlobalVulkanDispatch() || !CreateInstance() || !LoadInstanceVulkanDispatch() ||
        !PickPhysicalDevice() || !CreateSurface() || !CreateDevice() || !CreateSwapchain() ||
        !CreateRenderPass() || !CreateDescriptorResources() ||
        !CreatePipeline(vertex_shader_path, fragment_shader_path) || !CreateFrameResources() ||
        !CreateWhiteTexture())
    {
        std::fprintf(stderr, "[PS5-Vulkan] renderer initialization failed\n");
        return false;
    }
    initialized_ = true;
    return true;
}

bool Ps5VulkanRenderInterface::CreateInstance()
{
    uint32_t count = 0;
    if (!CheckResult(dispatch_.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
                     "vkEnumerateInstanceExtensionProperties"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxInstanceExtensions, "instance extensions"))
        return false;
    const std::uint32_t extension_capacity = count;
    std::vector<VkExtensionProperties> extensions(count);
    if (!CheckResult(
            dispatch_.vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
            "dispatch_.vkEnumerateInstanceExtensionProperties(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, extension_capacity, "enumerated instance extensions"))
        return false;
    extensions.resize(count);
    bool has_surface = false;
    bool has_display = false;
    for (const auto &extension : extensions)
    {
        has_surface |= std::strcmp(extension.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0;
        has_display |= std::strcmp(extension.extensionName, VK_KHR_DISPLAY_EXTENSION_NAME) == 0;
    }
    if (!has_surface || !has_display)
    {
        std::fprintf(
            stderr,
            "[PS5-Vulkan] required instance extensions are unavailable (surface=%d display=%d)\n",
            has_surface ? 1 : 0, has_display ? 1 : 0);
        return false;
    }

    VkApplicationInfo app_info =
        MakeVkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app_info.pApplicationName = "ProsperoRadio";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "RmlUi";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_DISPLAY_EXTENSION_NAME};
    VkInstanceCreateInfo create_info =
        MakeVkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = instance_extensions;
    return CheckResult(dispatch_.vkCreateInstance(&create_info, nullptr, &instance_),
                       "vkCreateInstance");
}

bool Ps5VulkanRenderInterface::ChooseDisplayConfiguration()
{
    uint32_t display_count = 0;
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(physical_device_,
                                                                       &display_count, nullptr),
                     "dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(display_count, kMaxDisplays, "display properties"))
        return false;
    const std::uint32_t display_capacity = display_count;
    std::vector<VkDisplayPropertiesKHR> displays(display_count);
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(
                         physical_device_, &display_count, displays.data()),
                     "dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(display_count, display_capacity, "enumerated display properties"))
        return false;
    displays.resize(display_count);
    display_ = displays[0].display;
    surface_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const VkSurfaceTransformFlagsKHR supported_transforms = displays[0].supportedTransforms;
    if ((supported_transforms & surface_transform_) == 0)
    {
        for (std::uint32_t bit = 0; bit < 32; ++bit)
        {
            const VkSurfaceTransformFlagBitsKHR candidate =
                static_cast<VkSurfaceTransformFlagBitsKHR>(1u << bit);
            if ((supported_transforms & candidate) != 0)
            {
                surface_transform_ = candidate;
                break;
            }
        }
    }

    uint32_t mode_count = 0;
    if (!CheckResult(dispatch_.vkGetDisplayModePropertiesKHR(physical_device_, display_,
                                                             &mode_count, nullptr),
                     "dispatch_.vkGetDisplayModePropertiesKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(mode_count, kMaxDisplayModes, "display modes"))
        return false;
    const std::uint32_t mode_capacity = mode_count;
    std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
    if (!CheckResult(dispatch_.vkGetDisplayModePropertiesKHR(physical_device_, display_,
                                                             &mode_count, modes.data()),
                     "dispatch_.vkGetDisplayModePropertiesKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(mode_count, mode_capacity, "enumerated display modes"))
        return false;
    modes.resize(mode_count);

    VideoOutResolutionStatus active{};
    const bool have_active_resolution = ReadActiveVideoOutResolution(active);
    const std::uint32_t target_width = active.pane_width ? active.pane_width : active.full_width;
    const std::uint32_t target_height =
        active.pane_height ? active.pane_height : active.full_height;
    const VkDisplayModePropertiesKHR *best = nullptr;

    // The pane is the active VideoOut region on PS5; use it first so a 1440p
    // signal stays 1440p even when the attached panel also advertises 4K.
    if (have_active_resolution && target_width && target_height)
    {
        for (const auto &candidate : modes)
        {
            const auto &region = candidate.parameters.visibleRegion;
            if (region.width == target_width && region.height == target_height)
            {
                best = &candidate;
                break;
            }
        }
        if (!best && active.full_width && active.full_height)
        {
            for (const auto &candidate : modes)
            {
                const auto &region = candidate.parameters.visibleRegion;
                if (region.width == active.full_width && region.height == active.full_height)
                {
                    best = &candidate;
                    break;
                }
            }
        }
        if (!best)
        {
            // If the active dimensions are between exposed display modes,
            // choose the smallest mode that does not reduce their resolution.
            std::uint64_t best_area = UINT64_MAX;
            for (const auto &candidate : modes)
            {
                const auto &region = candidate.parameters.visibleRegion;
                if (region.width < target_width || region.height < target_height)
                    continue;
                const std::uint64_t area = static_cast<std::uint64_t>(region.width) * region.height;
                if (area < best_area)
                {
                    best = &candidate;
                    best_area = area;
                }
            }
        }
    }

    if (!best)
    {
        best = &modes[0];
        for (const auto &candidate : modes)
        {
            const auto &a = candidate.parameters.visibleRegion;
            const auto &b = best->parameters.visibleRegion;
            const bool candidate_target = a.width == 3840 && a.height == 2160;
            const bool best_target = b.width == 3840 && b.height == 2160;
            const std::uint64_t candidate_area = static_cast<std::uint64_t>(a.width) * a.height;
            const std::uint64_t best_area = static_cast<std::uint64_t>(b.width) * b.height;
            if ((candidate_target && !best_target) ||
                (candidate_target == best_target && candidate_area > best_area))
                best = &candidate;
        }
    }
    display_mode_ = best->displayMode;
    display_width_ = best->parameters.visibleRegion.width;
    display_height_ = best->parameters.visibleRegion.height;
    std::fprintf(
        stderr, "[PS5-Vulkan] selected display mode: %ux%u%s\n", display_width_, display_height_,
        have_active_resolution ? " (matched to active VideoOut)" : " (VideoOut query unavailable)");
    return true;
}

bool Ps5VulkanRenderInterface::CreateSurface()
{
    if (!ChooseDisplayConfiguration())
        return false;

    uint32_t plane_count = 0;
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physical_device_,
                                                                            &plane_count, nullptr),
                     "dispatch_.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(plane_count, kMaxDisplayPlanes, "display planes"))
        return false;
    const std::uint32_t plane_capacity = plane_count;
    std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(
                         physical_device_, &plane_count, planes.data()),
                     "dispatch_.vkGetPhysicalDeviceDisplayPlanePropertiesKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(plane_count, plane_capacity, "enumerated display planes"))
        return false;
    planes.resize(plane_count);

    std::uint32_t selected_plane = UINT32_MAX;
    std::uint32_t selected_stack = 0;
    for (std::uint32_t plane = 0; plane < plane_count; ++plane)
    {
        uint32_t supported_count = 0;
        if (dispatch_.vkGetDisplayPlaneSupportedDisplaysKHR(
                physical_device_, plane, &supported_count, nullptr) != VK_SUCCESS)
            continue;
        if (supported_count > kMaxSupportedDisplaysPerPlane)
        {
            std::fprintf(stderr, "[PS5-Vulkan] invalid supported-display count: %u\n",
                         supported_count);
            return false;
        }
        const std::uint32_t supported_capacity = supported_count;
        std::vector<VkDisplayKHR> supported(supported_count);
        if (supported_count &&
            dispatch_.vkGetDisplayPlaneSupportedDisplaysKHR(
                physical_device_, plane, &supported_count, supported.data()) != VK_SUCCESS)
            continue;
        if (supported_count > supported_capacity)
        {
            std::fprintf(
                stderr, "[PS5-Vulkan] supported-display count changed beyond allocated capacity\n");
            return false;
        }
        supported.resize(supported_count);
        if (std::find(supported.begin(), supported.end(), display_) != supported.end())
        {
            selected_plane = plane;
            selected_stack = planes[plane].currentStackIndex;
            break;
        }
    }
    if (selected_plane == UINT32_MAX)
        return false;

    VkDisplayPlaneCapabilitiesKHR plane_caps{};
    if (!CheckResult(dispatch_.vkGetDisplayPlaneCapabilitiesKHR(physical_device_, display_mode_,
                                                                selected_plane, &plane_caps),
                     "vkGetDisplayPlaneCapabilitiesKHR"))
        return false;
    VkDisplayPlaneAlphaFlagBitsKHR alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    if ((plane_caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) == 0)
    {
        if (plane_caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR)
            alpha = VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
        else if (plane_caps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR)
            alpha = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;
        else
            return false;
    }

    VkDisplaySurfaceCreateInfoKHR surface_info = MakeVkStruct<VkDisplaySurfaceCreateInfoKHR>(
        VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR);
    surface_info.displayMode = display_mode_;
    surface_info.planeIndex = selected_plane;
    surface_info.planeStackIndex = selected_stack;
    surface_info.transform = surface_transform_;
    surface_info.globalAlpha = 1.0f;
    surface_info.alphaMode = alpha;
    surface_info.imageExtent = {display_width_, display_height_};
    return CheckResult(
        dispatch_.vkCreateDisplayPlaneSurfaceKHR(instance_, &surface_info, nullptr, &surface_),
        "vkCreateDisplayPlaneSurfaceKHR");
}

bool Ps5VulkanRenderInterface::PickPhysicalDevice()
{
    uint32_t count = 0;
    if (!CheckResult(dispatch_.vkEnumeratePhysicalDevices(instance_, &count, nullptr),
                     "dispatch_.vkEnumeratePhysicalDevices(count)"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxPhysicalDevices, "physical devices"))
        return false;
    const std::uint32_t device_capacity = count;
    std::vector<VkPhysicalDevice> devices(count);
    if (!CheckResult(dispatch_.vkEnumeratePhysicalDevices(instance_, &count, devices.data()),
                     "dispatch_.vkEnumeratePhysicalDevices(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, device_capacity, "enumerated physical devices"))
        return false;
    devices.resize(count);

    for (VkPhysicalDevice device : devices)
    {
        if (!ChooseDisplayForDevice(device))
            continue;
        physical_device_ = device;
        break;
    }
    return physical_device_ != VK_NULL_HANDLE;
}

bool Ps5VulkanRenderInterface::ChooseSurfaceFormat(VkSurfaceFormatKHR &format)
{
    uint32_t count = 0;
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                                    &count, nullptr),
                     "dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxSurfaceFormats, "surface formats"))
        return false;
    const std::uint32_t format_capacity = count;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_,
                                                                    &count, formats.data()),
                     "dispatch_.vkGetPhysicalDeviceSurfaceFormatsKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, format_capacity, "enumerated surface formats"))
        return false;
    formats.resize(count);
    format = formats[0];
    for (const auto &candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
            candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            format = candidate;
            break;
        }
    }
    return true;
}

bool Ps5VulkanRenderInterface::ChoosePresentMode(VkPresentModeKHR &mode)
{
    uint32_t count = 0;
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_,
                                                                         &count, nullptr),
                     "dispatch_.vkGetPhysicalDeviceSurfacePresentModesKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxPresentModes, "present modes"))
        return false;
    const std::uint32_t mode_capacity = count;
    std::vector<VkPresentModeKHR> modes(count);
    if (count && !CheckResult(dispatch_.vkGetPhysicalDeviceSurfacePresentModesKHR(
                                  physical_device_, surface_, &count, modes.data()),
                              "dispatch_.vkGetPhysicalDeviceSurfacePresentModesKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, mode_capacity, "enumerated present modes"))
        return false;
    modes.resize(count);
    mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto candidate : modes)
        if (candidate == VK_PRESENT_MODE_FIFO_KHR)
            return true;
    return false;
}

bool Ps5VulkanRenderInterface::CreateDevice()
{
    uint32_t queue_count = 0;
    dispatch_.vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count, nullptr);
    if (!IsReasonableVulkanCount(queue_count, kMaxQueueFamilies, "queue families"))
        return false;
    const std::uint32_t queue_capacity = queue_count;
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    dispatch_.vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_count,
                                                       queues.data());
    if (!IsReasonableVulkanCount(queue_count, queue_capacity, "enumerated queue families"))
        return false;
    queues.resize(queue_count);
    for (std::uint32_t i = 0; i < queue_count; ++i)
    {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
            graphics_queue_family_ == UINT32_MAX)
            graphics_queue_family_ = i;
        VkBool32 present = VK_FALSE;
        if (surface_ != VK_NULL_HANDLE)
            dispatch_.vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present);
        if (present && present_queue_family_ == UINT32_MAX)
            present_queue_family_ = i;
    }
    if (graphics_queue_family_ == UINT32_MAX || present_queue_family_ == UINT32_MAX)
        return false;

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    VkDeviceQueueCreateInfo graphics_queue =
        MakeVkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    graphics_queue.queueFamilyIndex = graphics_queue_family_;
    graphics_queue.queueCount = 1;
    graphics_queue.pQueuePriorities = &priority;
    queue_infos.push_back(graphics_queue);
    if (present_queue_family_ != graphics_queue_family_)
    {
        VkDeviceQueueCreateInfo present_queue =
            MakeVkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        present_queue.queueFamilyIndex = present_queue_family_;
        present_queue.queueCount = 1;
        present_queue.pQueuePriorities = &priority;
        queue_infos.push_back(present_queue);
    }

    if (!HasDeviceExtension(physical_device_, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        std::fprintf(stderr, "[PS5-Vulkan] VK_KHR_swapchain is unavailable\n");
        return false;
    }
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo create_info =
        MakeVkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = device_extensions;
    create_info.pEnabledFeatures = &features;
    if (!CheckResult(dispatch_.vkCreateDevice(physical_device_, &create_info, nullptr, &device_),
                     "vkCreateDevice"))
        return false;
    dispatch_.vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    dispatch_.vkGetDeviceQueue(device_, present_queue_family_, 0, &present_queue_);
    return true;
}

bool Ps5VulkanRenderInterface::CreateSwapchain()
{
    if (surface_ == VK_NULL_HANDLE)
        return false;
    if (!ChooseSurfaceFormat(surface_format_) || !ChoosePresentMode(present_mode_))
        return false;

    VkSurfaceCapabilitiesKHR caps{};
    if (!CheckResult(
            dispatch_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;
    if (caps.minImageCount == 0 ||
        (caps.maxImageCount != 0 && caps.maxImageCount < caps.minImageCount))
    {
        std::fprintf(stderr, "[PS5-Vulkan] invalid swapchain image-count limits (%u..%u)\n",
                     caps.minImageCount, caps.maxImageCount);
        return false;
    }
    extent_ = caps.currentExtent.width != 0xffffffffu ? caps.currentExtent
                                                      : VkExtent2D{display_width_, display_height_};
    if (extent_.width == 0 || extent_.height == 0 || extent_.width > 16384 ||
        extent_.height > 16384)
    {
        std::fprintf(stderr, "[PS5-Vulkan] invalid native swapchain extent: %ux%u\n", extent_.width,
                     extent_.height);
        return false;
    }

    std::uint32_t image_count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount != 0)
        image_count = std::min(image_count, caps.maxImageCount);
    if (image_count == 0 || image_count > kMaxSwapchainImages)
    {
        std::fprintf(stderr, "[PS5-Vulkan] unsupported swapchain image count requested: %u\n",
                     image_count);
        return false;
    }

    std::uint32_t queue_indices[] = {graphics_queue_family_, present_queue_family_};
    VkSwapchainCreateInfoKHR swapchain_info =
        MakeVkStruct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    swapchain_info.surface = surface_;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = surface_format_.format;
    swapchain_info.imageColorSpace = surface_format_.colorSpace;
    swapchain_info.imageExtent = extent_;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (graphics_queue_family_ != present_queue_family_)
    {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_info.queueFamilyIndexCount = 2;
        swapchain_info.pQueueFamilyIndices = queue_indices;
    }
    else
    {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    swapchain_info.preTransform = caps.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & swapchain_info.compositeAlpha) == 0)
    {
        const VkCompositeAlphaFlagBitsKHR choices[] = {
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        };
        bool found = false;
        for (const auto choice : choices)
        {
            if ((caps.supportedCompositeAlpha & choice) != 0)
            {
                swapchain_info.compositeAlpha = choice;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    swapchain_info.presentMode = present_mode_;
    swapchain_info.clipped = VK_FALSE;
    if (!CheckResult(dispatch_.vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &swapchain_),
                     "vkCreateSwapchainKHR"))
        return false;

    std::uint32_t count = 0;
    if (!CheckResult(dispatch_.vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr),
                     "dispatch_.vkGetSwapchainImagesKHR(count)"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxSwapchainImages, "swapchain images"))
        return false;
    const std::uint32_t image_capacity = count;
    swapchain_images_.resize(count);
    if (!CheckResult(dispatch_.vkGetSwapchainImagesKHR(device_, swapchain_, &count,
                                                       swapchain_images_.data()),
                     "dispatch_.vkGetSwapchainImagesKHR(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, image_capacity, "enumerated swapchain images"))
        return false;
    swapchain_images_.resize(count);
    swapchain_views_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkImageViewCreateInfo view_info =
            MakeVkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view_info.image = swapchain_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = surface_format_.format;
        view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (!CheckResult(
                dispatch_.vkCreateImageView(device_, &view_info, nullptr, &swapchain_views_[i]),
                "vkCreateImageView"))
            return false;
    }
    return true;
}

bool Ps5VulkanRenderInterface::CreateRenderPass()
{
    VkAttachmentDescription color{};
    color.format = surface_format_.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info =
        MakeVkStruct<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    return CheckResult(dispatch_.vkCreateRenderPass(device_, &info, nullptr, &render_pass_),
                       "vkCreateRenderPass");
}

bool Ps5VulkanRenderInterface::CreateDescriptorResources()
{
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = MakeVkStruct<VkDescriptorSetLayoutCreateInfo>(
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    if (!CheckResult(dispatch_.vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                                           &descriptor_set_layout_),
                     "vkCreateDescriptorSetLayout"))
        return false;

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kDescriptorCapacity;
    VkDescriptorPoolCreateInfo pool_info =
        MakeVkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = kDescriptorCapacity;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (!CheckResult(
            dispatch_.vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_),
            "vkCreateDescriptorPool"))
        return false;

    VkSamplerCreateInfo sampler_info =
        MakeVkStruct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (!CheckResult(dispatch_.vkCreateSampler(device_, &sampler_info, nullptr, &sampler_),
                     "vkCreateSampler"))
        return false;

    VkFormatProperties format_properties{};
    bool linear_filter = false;
    if (dispatch_.vkGetPhysicalDeviceFormatProperties)
    {
        dispatch_.vkGetPhysicalDeviceFormatProperties(physical_device_, VK_FORMAT_R8G8B8A8_UNORM,
                                                      &format_properties);
        linear_filter = (format_properties.optimalTilingFeatures &
                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    }
    VkSamplerCreateInfo mipmap_sampler_info = sampler_info;
    mipmap_sampler_info.magFilter = linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    mipmap_sampler_info.minFilter = linear_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    mipmap_sampler_info.mipmapMode =
        linear_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    mipmap_sampler_info.maxLod = 16.0f;
    if (!linear_filter)
        std::fprintf(stderr, "[PS5-Vulkan] linear filtering unavailable for RGBA8; mip selection "
                             "uses nearest filtering\n");
    return CheckResult(
        dispatch_.vkCreateSampler(device_, &mipmap_sampler_info, nullptr, &mipmap_sampler_),
        "vkCreateSampler(mipmap)");
}

VkShaderModule Ps5VulkanRenderInterface::CreateShaderModule(const std::vector<std::uint8_t> &spirv)
{
    if (spirv.empty() || (spirv.size() % 4) != 0)
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info =
        MakeVkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = spirv.size();
    info.pCode = reinterpret_cast<const std::uint32_t *>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (dispatch_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

bool Ps5VulkanRenderInterface::CreatePipeline(const char *vertex_shader_path,
                                              const char *fragment_shader_path)
{
    const std::vector<std::uint8_t> vertex_spirv = ReadBinaryFile(vertex_shader_path);
    const std::vector<std::uint8_t> fragment_spirv = ReadBinaryFile(fragment_shader_path);
    VkShaderModule vertex_shader = CreateShaderModule(vertex_spirv);
    VkShaderModule fragment_shader = CreateShaderModule(fragment_spirv);
    if (vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE)
        return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[3]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)};
    attributes[2] = {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, r)};

    VkPipelineVertexInputStateCreateInfo vertex_input =
        MakeVkStruct<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly =
        MakeVkStruct<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, extent_};
    VkPipelineViewportStateCreateInfo viewport_state =
        MakeVkStruct<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster =
        MakeVkStruct<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample =
        MakeVkStruct<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend_state =
        MakeVkStruct<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state = MakeVkStruct<VkPipelineDynamicStateCreateInfo>(
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(float) * 2;
    VkPipelineLayoutCreateInfo layout =
        MakeVkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptor_set_layout_;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!CheckResult(dispatch_.vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_),
                     "vkCreatePipelineLayout"))
        return false;

    VkGraphicsPipelineCreateInfo pipeline_info =
        MakeVkStruct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;
    const bool ok =
        CheckResult(dispatch_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                                        nullptr, &pipeline_),
                    "vkCreateGraphicsPipelines");
    dispatch_.vkDestroyShaderModule(device_, vertex_shader, nullptr);
    dispatch_.vkDestroyShaderModule(device_, fragment_shader, nullptr);
    return ok;
}

bool Ps5VulkanRenderInterface::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                            VkMemoryPropertyFlags properties, VkBuffer &buffer,
                                            VkDeviceMemory &memory, void **mapped)
{
    VkBufferCreateInfo buffer_info =
        MakeVkStruct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!CheckResult(dispatch_.vkCreateBuffer(device_, &buffer_info, nullptr, &buffer),
                     "vkCreateBuffer"))
        return false;
    VkMemoryRequirements requirements{};
    dispatch_.vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    VkMemoryAllocateInfo allocation =
        MakeVkStruct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, properties);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        return false;
    if (!CheckResult(dispatch_.vkAllocateMemory(device_, &allocation, nullptr, &memory),
                     "dispatch_.vkAllocateMemory(buffer)"))
        return false;
    if (!CheckResult(dispatch_.vkBindBufferMemory(device_, buffer, memory, 0),
                     "vkBindBufferMemory"))
        return false;
    if (mapped && (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        if (!CheckResult(dispatch_.vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, mapped),
                         "vkMapMemory"))
            return false;
    }
    return true;
}

bool Ps5VulkanRenderInterface::CreateImage(std::uint32_t width, std::uint32_t height,
                                           VkFormat format, VkImageUsageFlags usage, VkImage &image,
                                           VkDeviceMemory &memory, std::uint32_t mip_levels)
{
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        pixel_count > kMaxTexturePixels || mip_levels == 0 || mip_levels > 32)
    {
        std::fprintf(
            stderr, "[PS5-Vulkan] refusing unreasonable texture allocation: %ux%u, mip-levels=%u\n",
            width, height, mip_levels);
        return false;
    }
    VkImageCreateInfo image_info =
        MakeVkStruct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!CheckResult(dispatch_.vkCreateImage(device_, &image_info, nullptr, &image),
                     "vkCreateImage"))
        return false;
    VkMemoryRequirements requirements{};
    dispatch_.vkGetImageMemoryRequirements(device_, image, &requirements);
    VkMemoryAllocateInfo allocation =
        MakeVkStruct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        allocation.memoryTypeIndex =
            FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX)
        return false;
    if (!CheckResult(dispatch_.vkAllocateMemory(device_, &allocation, nullptr, &memory),
                     "dispatch_.vkAllocateMemory(image)"))
        return false;
    return CheckResult(dispatch_.vkBindImageMemory(device_, image, memory, 0), "vkBindImageMemory");
}

bool Ps5VulkanRenderInterface::BeginSingleUseCommands(VkCommandBuffer &buffer, VkCommandPool &pool)
{
    if (pool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo pool_info =
            MakeVkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!CheckResult(dispatch_.vkCreateCommandPool(device_, &pool_info, nullptr, &pool),
                         "dispatch_.vkCreateCommandPool(upload)"))
            return false;
    }
    VkCommandBufferAllocateInfo allocation =
        MakeVkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocation.commandPool = pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    if (!CheckResult(dispatch_.vkAllocateCommandBuffers(device_, &allocation, &buffer),
                     "dispatch_.vkAllocateCommandBuffers(upload)"))
        return false;
    VkCommandBufferBeginInfo begin =
        MakeVkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return CheckResult(dispatch_.vkBeginCommandBuffer(buffer, &begin),
                       "dispatch_.vkBeginCommandBuffer(upload)");
}

bool Ps5VulkanRenderInterface::EndSingleUseCommands(VkCommandBuffer buffer, VkCommandPool pool)
{
    if (!CheckResult(dispatch_.vkEndCommandBuffer(buffer), "dispatch_.vkEndCommandBuffer(upload)"))
        return false;
    VkSubmitInfo submit = MakeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &buffer;
    if (!CheckResult(dispatch_.vkQueueSubmit(graphics_queue_, 1, &submit, VK_NULL_HANDLE),
                     "dispatch_.vkQueueSubmit(upload)"))
        return false;
    if (!CheckResult(dispatch_.vkQueueWaitIdle(graphics_queue_),
                     "dispatch_.vkQueueWaitIdle(upload)"))
        return false;
    dispatch_.vkFreeCommandBuffers(device_, pool, 1, &buffer);
    return true;
}

bool Ps5VulkanRenderInterface::TransitionImage(VkCommandBuffer command_buffer, VkImage image,
                                               VkImageLayout old_layout, VkImageLayout new_layout,
                                               std::uint32_t mip_levels)
{
    VkImageMemoryBarrier barrier =
        MakeVkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.layerCount = 1;
    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
             new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    else
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    }
    dispatch_.vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, nullptr,
                                   0, nullptr, 1, &barrier);
    return true;
}

bool Ps5VulkanRenderInterface::CopyBufferToImage(VkCommandBuffer command_buffer, VkBuffer buffer,
                                                 VkImage image, std::uint32_t width,
                                                 std::uint32_t height)
{
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    dispatch_.vkCmdCopyBufferToImage(command_buffer, buffer, image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return true;
}

bool Ps5VulkanRenderInterface::CreateImageView(Texture &texture, VkFormat format)
{
    VkImageViewCreateInfo view_info =
        MakeVkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    if (format == VK_FORMAT_R8_UNORM)
    {
        // RTA1 bitmap-font atlases store only coverage. Swizzle that coverage
        // into alpha while keeping RGB white so the existing UI shader can
        // multiply it by the per-vertex font color without a shader variant.
        view_info.components.r = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.g = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.b = VK_COMPONENT_SWIZZLE_ONE;
        view_info.components.a = VK_COMPONENT_SWIZZLE_R;
    }
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = texture.mip_levels;
    view_info.subresourceRange.layerCount = 1;
    return CheckResult(dispatch_.vkCreateImageView(device_, &view_info, nullptr, &texture.view),
                       "dispatch_.vkCreateImageView(texture)");
}

bool Ps5VulkanRenderInterface::AllocateTextureDescriptor(Texture &texture)
{
    VkDescriptorSetAllocateInfo allocate =
        MakeVkStruct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = descriptor_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_set_layout_;
    if (!CheckResult(dispatch_.vkAllocateDescriptorSets(device_, &allocate, &texture.descriptor),
                     "vkAllocateDescriptorSets"))
        return false;
    VkDescriptorImageInfo image_info{};
    image_info.sampler = texture.mip_levels > 1 ? mipmap_sampler_ : sampler_;
    image_info.imageView = texture.view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write =
        MakeVkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = texture.descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    dispatch_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

bool Ps5VulkanRenderInterface::UploadTexture(Texture &texture, const std::uint8_t *pixels,
                                             std::uint32_t width, std::uint32_t height)
{
    if (!pixels || width == 0 || height == 0 || texture.bytes_per_pixel == 0)
        return false;
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) * height;
    if (pixel_count > kMaxTexturePixels ||
        pixel_count > std::numeric_limits<VkDeviceSize>::max() / texture.bytes_per_pixel)
    {
        std::fprintf(stderr,
                     "[PS5-Vulkan] refusing unreasonable texture upload: %ux%u, %u bytes/pixel\n",
                     width, height, texture.bytes_per_pixel);
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pixel_count * texture.bytes_per_pixel);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    if (!CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_memory, &mapped))
        return false;
    std::memcpy(mapped, pixels, static_cast<std::size_t>(bytes));
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!BeginSingleUseCommands(cmd, upload_command_pool_))
        return false;
    TransitionImage(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(cmd, staging, texture.image, width, height);
    TransitionImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const bool ok = EndSingleUseCommands(cmd, upload_command_pool_);
    DestroyBuffer(staging, staging_memory);
    return ok;
}

bool Ps5VulkanRenderInterface::CreateTextureFromPixels(const std::uint8_t *pixels,
                                                       std::uint32_t width, std::uint32_t height,
                                                       VkFormat format, Texture **out_texture)
{
    if (!pixels || !out_texture || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > kMaxTexturePixels)
    {
        std::fprintf(stderr, "[PS5-Vulkan] invalid texture pixels/dimensions: %ux%u\n", width,
                     height);
        return false;
    }
    *out_texture = nullptr;
    auto *texture = new (std::nothrow) Texture;
    if (!texture)
        return false;
    texture->width = static_cast<int>(width);
    texture->height = static_cast<int>(height);
    texture->bytes_per_pixel = format == VK_FORMAT_R8_UNORM ? 1u : 4u;
    if (!CreateImage(width, height, format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, texture->image,
                     texture->memory) ||
        !UploadTexture(*texture, pixels, width, height) || !CreateImageView(*texture, format) ||
        !AllocateTextureDescriptor(*texture))
    {
        DestroyTexture(texture);
        return false;
    }
    *out_texture = texture;
    return true;
}

bool Ps5VulkanRenderInterface::CreateWhiteTexture()
{
    const std::uint8_t pixels[] = {255, 255, 255, 255};
    const bool ok =
        CreateTextureFromPixels(pixels, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, &white_texture_);
    if (ok)
        white_descriptor_ = white_texture_->descriptor;
    return ok;
}

bool Ps5VulkanRenderInterface::CreateFrameResources()
{
    frames_.resize(kFramesInFlight);
    max_vertex_bytes_ = kInitialVertexCapacity;
    max_index_bytes_ = kInitialIndexCapacity;
    for (Frame &frame : frames_)
    {
        VkCommandPoolCreateInfo pool_info =
            MakeVkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!CheckResult(
                dispatch_.vkCreateCommandPool(device_, &pool_info, nullptr, &frame.command_pool),
                "dispatch_.vkCreateCommandPool(frame)"))
            return false;
        VkCommandBufferAllocateInfo allocation = MakeVkStruct<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocation.commandPool = frame.command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        if (!CheckResult(
                dispatch_.vkAllocateCommandBuffers(device_, &allocation, &frame.command_buffer),
                "dispatch_.vkAllocateCommandBuffers(frame)"))
            return false;
        VkSemaphoreCreateInfo semaphore =
            MakeVkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        if (!CheckResult(
                dispatch_.vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_available),
                "dispatch_.vkCreateSemaphore(image)"))
            return false;
        if (!CheckResult(
                dispatch_.vkCreateSemaphore(device_, &semaphore, nullptr, &frame.render_finished),
                "dispatch_.vkCreateSemaphore(render)"))
            return false;
        VkFenceCreateInfo fence =
            MakeVkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!CheckResult(dispatch_.vkCreateFence(device_, &fence, nullptr, &frame.fence),
                         "vkCreateFence"))
            return false;
        if (!CreateBuffer(max_vertex_bytes_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frame.vertex_buffer, frame.vertex_memory, &frame.vertex_mapped))
            return false;
        if (!CreateBuffer(max_index_bytes_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          frame.index_buffer, frame.index_memory, &frame.index_mapped))
            return false;
    }

    framebuffers_.resize(swapchain_views_.size());
    for (std::size_t i = 0; i < swapchain_views_.size(); ++i)
    {
        VkFramebufferCreateInfo info =
            MakeVkStruct<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        info.renderPass = render_pass_;
        info.attachmentCount = 1;
        info.pAttachments = &swapchain_views_[i];
        info.width = extent_.width;
        info.height = extent_.height;
        info.layers = 1;
        if (!CheckResult(dispatch_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]),
                         "vkCreateFramebuffer"))
            return false;
    }
    return true;
}

std::uint32_t Ps5VulkanRenderInterface::FindMemoryType(std::uint32_t type_bits,
                                                       VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memory_properties{};
    dispatch_.vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((type_bits & (1u << i)) != 0 &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

bool Ps5VulkanRenderInterface::HasDeviceExtension(VkPhysicalDevice device, const char *name) const
{
    uint32_t count = 0;
    if (!CheckResult(
            dispatch_.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
            "vkEnumerateDeviceExtensionProperties(count)"))
        return false;
    if (!IsReasonableVulkanCount(count, kMaxDeviceExtensions, "device extensions"))
        return false;
    const std::uint32_t extension_capacity = count;
    std::vector<VkExtensionProperties> extensions(count);
    if (!CheckResult(dispatch_.vkEnumerateDeviceExtensionProperties(device, nullptr, &count,
                                                                    extensions.data()),
                     "vkEnumerateDeviceExtensionProperties(data)"))
        return false;
    if (!IsReasonableVulkanCount(count, extension_capacity, "enumerated device extensions"))
        return false;
    extensions.resize(count);
    for (const auto &extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0)
            return true;
    return false;
}

bool Ps5VulkanRenderInterface::ChooseDisplayForDevice(VkPhysicalDevice device)
{
    uint32_t count = 0;
    if (!CheckResult(dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(device, &count, nullptr),
                     "vkGetPhysicalDeviceDisplayPropertiesKHR(device count)") ||
        !IsReasonableVulkanCount(count, kMaxDisplays, "device display properties"))
        return false;
    const std::uint32_t display_capacity = count;
    std::vector<VkDisplayPropertiesKHR> displays(count);
    if (!CheckResult(
            dispatch_.vkGetPhysicalDeviceDisplayPropertiesKHR(device, &count, displays.data()),
            "vkGetPhysicalDeviceDisplayPropertiesKHR(device data)") ||
        !IsReasonableVulkanCount(count, display_capacity, "enumerated device display properties"))
        return false;
    displays.resize(count);
    if (!HasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return false;
    return true;
}

void Ps5VulkanRenderInterface::DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
    if (buffer != VK_NULL_HANDLE)
        dispatch_.vkDestroyBuffer(device_, buffer, nullptr);
    if (memory != VK_NULL_HANDLE)
        dispatch_.vkFreeMemory(device_, memory, nullptr);
}

void Ps5VulkanRenderInterface::DestroyTexture(Texture *texture)
{
    if (!texture)
        return;
    if (texture->view != VK_NULL_HANDLE)
        dispatch_.vkDestroyImageView(device_, texture->view, nullptr);
    if (texture->image != VK_NULL_HANDLE)
        dispatch_.vkDestroyImage(device_, texture->image, nullptr);
    if (texture->memory != VK_NULL_HANDLE)
        dispatch_.vkFreeMemory(device_, texture->memory, nullptr);
    delete texture;
}

std::vector<std::uint8_t> Ps5VulkanRenderInterface::ReadBinaryFile(const char *path)
{
    std::vector<std::uint8_t> bytes;
    ReadWholeFile(path, bytes);
    return bytes;
}

bool Ps5VulkanRenderInterface::ReadTextTextureHeader(const std::vector<std::uint8_t> &bytes,
                                                     int &width, int &height) const
{
    if (bytes.size() < 18)
        return false;
    width = bytes[12] | (bytes[13] << 8);
    height = bytes[14] | (bytes[15] << 8);
    return width > 0 && height > 0;
}

bool Ps5VulkanRenderInterface::LoadKtx2Texture(const Rml::String &source,
                                               Rml::TextureHandle &texture_handle,
                                               Rml::Vector2i &texture_dimensions)
{
    std::FILE *file = OpenAssetFile(source.c_str());
    if (!file)
    {
        std::fprintf(stderr, "[PS5-Vulkan] cannot open KTX2 texture: %s\n", source.c_str());
        return false;
    }
    const auto close_and_fail = [file]()
    {
        std::fclose(file);
        return false;
    };

    if (std::fseek(file, 0, SEEK_END) != 0)
        return close_and_fail();
    const long file_end = std::ftell(file);
    if (file_end < 80 || std::fseek(file, 0, SEEK_SET) != 0)
        return close_and_fail();
    const std::uint64_t file_size = static_cast<std::uint64_t>(file_end);

    std::uint8_t header[80]{};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, kKtx2Identifier, sizeof(kKtx2Identifier)) != 0)
    {
        std::fprintf(stderr, "[PS5-Vulkan] invalid KTX2 header: %s\n", source.c_str());
        return close_and_fail();
    }

    const VkFormat format = static_cast<VkFormat>(ReadLe32(header + 12));
    const std::uint32_t type_size = ReadLe32(header + 16);
    const std::uint32_t width = ReadLe32(header + 20);
    const std::uint32_t height = ReadLe32(header + 24);
    const std::uint32_t depth = ReadLe32(header + 28);
    const std::uint32_t layers = ReadLe32(header + 32);
    const std::uint32_t faces = ReadLe32(header + 36);
    const std::uint32_t mip_levels = ReadLe32(header + 40);
    const std::uint32_t supercompression = ReadLe32(header + 44);
    const std::uint32_t dfd_offset = ReadLe32(header + 48);
    const std::uint32_t dfd_length = ReadLe32(header + 52);
    const std::uint32_t kvd_offset = ReadLe32(header + 56);
    const std::uint32_t kvd_length = ReadLe32(header + 60);
    const std::uint64_t sgd_length = ReadLe64(header + 72);

    const bool supported_format =
        format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
    const std::uint32_t level_count = mip_levels == 0 ? 1 : mip_levels;
    std::uint32_t max_levels = 1;
    for (std::uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1)
        ++max_levels;
    const std::uint64_t index_end = 80ull + static_cast<std::uint64_t>(level_count) * 24ull;
    const std::uint64_t dfd_end = static_cast<std::uint64_t>(dfd_offset) + dfd_length;
    const std::uint64_t kvd_end = static_cast<std::uint64_t>(kvd_offset) + kvd_length;
    if (!supported_format || type_size != 1 || width == 0 || height == 0 || width > 16384 ||
        height > 16384 || depth != 0 || layers != 0 || faces != 1 || level_count == 0 ||
        level_count > max_levels || level_count > 32 || supercompression != 0 || sgd_length != 0 ||
        dfd_length == 0 || index_end > file_size || dfd_end > file_size ||
        (kvd_length != 0 && kvd_end > file_size))
    {
        std::fprintf(stderr,
                     "[PS5-Vulkan] unsupported KTX2 layout: %s format=%u size=%ux%u levels=%u "
                     "supercompression=%u\n",
                     source.c_str(), static_cast<unsigned>(format), width, height, level_count,
                     supercompression);
        return close_and_fail();
    }

    std::vector<Ktx2MipLevel> levels;
    levels.reserve(level_count);
    std::uint64_t minimum_data_offset = std::max(index_end, dfd_end);
    if (kvd_length != 0)
        minimum_data_offset = std::max(minimum_data_offset, kvd_end);
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    for (std::uint32_t level_index = 0; level_index < level_count; ++level_index)
    {
        std::uint8_t level_index_bytes[24]{};
        const long index_offset =
            static_cast<long>(80ull + static_cast<std::uint64_t>(level_index) * 24ull);
        if (std::fseek(file, index_offset, SEEK_SET) != 0 ||
            std::fread(level_index_bytes, 1, sizeof(level_index_bytes), file) !=
                sizeof(level_index_bytes))
            return close_and_fail();

        const std::uint64_t offset = ReadLe64(level_index_bytes);
        const std::uint64_t byte_length = ReadLe64(level_index_bytes + 8);
        const std::uint64_t uncompressed_length = ReadLe64(level_index_bytes + 16);
        const std::uint64_t expected_length =
            static_cast<std::uint64_t>(level_width) * level_height * 4ull;
        if (offset < minimum_data_offset || (offset & 3u) != 0 || byte_length != expected_length ||
            uncompressed_length != expected_length || offset > file_size ||
            byte_length > file_size - offset)
        {
            std::fprintf(stderr, "[PS5-Vulkan] invalid KTX2 mip %u in %s\n", level_index,
                         source.c_str());
            return close_and_fail();
        }
        Ktx2MipLevel level{};
        level.offset = offset;
        level.size = byte_length;
        level.width = level_width;
        level.height = level_height;
        levels.push_back(level);
        level_width = std::max(1u, level_width / 2u);
        level_height = std::max(1u, level_height / 2u);
    }

    if (!dispatch_.vkGetPhysicalDeviceFormatProperties)
    {
        std::fprintf(stderr, "[PS5-Vulkan] vkGetPhysicalDeviceFormatProperties is unavailable\n");
        return close_and_fail();
    }
    VkFormatProperties format_properties{};
    dispatch_.vkGetPhysicalDeviceFormatProperties(physical_device_, format, &format_properties);
    if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
    {
        std::fprintf(stderr, "[PS5-Vulkan] KTX2 format %u cannot be sampled by this device\n",
                     static_cast<unsigned>(format));
        return close_and_fail();
    }

    auto *texture = new (std::nothrow) Texture;
    if (!texture)
        return close_and_fail();
    texture->width = static_cast<int>(width);
    texture->height = static_cast<int>(height);
    texture->mip_levels = level_count;

    if (!CreateImage(width, height, format,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, texture->image,
                     texture->memory, level_count))
    {
        DestroyTexture(texture);
        return close_and_fail();
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    if (!CreateBuffer(kTextureUploadChunkBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_memory, &mapped))
    {
        DestroyBuffer(staging, staging_memory);
        DestroyTexture(texture);
        return close_and_fail();
    }

    bool uploaded = false;
    bool transitioned_to_transfer = false;
    for (std::uint32_t level_index = 0; level_index < level_count && !uploaded; ++level_index)
    {
        const Ktx2MipLevel &level = levels[level_index];
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(level.width) * 4ull;
        const std::uint32_t rows_per_chunk =
            std::max(1u, static_cast<std::uint32_t>(kTextureUploadChunkBytes / row_bytes));

        for (std::uint32_t first_row = 0; first_row < level.height; first_row += rows_per_chunk)
        {
            const std::uint32_t row_count = std::min(rows_per_chunk, level.height - first_row);
            const std::uint64_t chunk_bytes = row_bytes * row_count;
            const std::uint64_t read_offset = level.offset + row_bytes * first_row;
            if (read_offset > static_cast<std::uint64_t>(std::numeric_limits<long>::max()) ||
                std::fseek(file, static_cast<long>(read_offset), SEEK_SET) != 0 ||
                std::fread(mapped, 1, static_cast<std::size_t>(chunk_bytes), file) != chunk_bytes)
            {
                std::fprintf(stderr, "[PS5-Vulkan] failed reading KTX2 mip %u rows %u-%u\n",
                             level_index, first_row, first_row + row_count);
                uploaded = true;
                break;
            }

            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            if (!BeginSingleUseCommands(command_buffer, upload_command_pool_))
            {
                uploaded = true;
                break;
            }
            if (!transitioned_to_transfer)
            {
                TransitionImage(command_buffer, texture->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level_count);
                transitioned_to_transfer = true;
            }

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level_index;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, static_cast<std::int32_t>(first_row), 0};
            region.imageExtent = {level.width, row_count, 1};
            dispatch_.vkCmdCopyBufferToImage(command_buffer, staging, texture->image,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            if (!EndSingleUseCommands(command_buffer, upload_command_pool_))
            {
                uploaded = true;
                break;
            }
        }
    }

    if (!uploaded && transitioned_to_transfer)
    {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        if (BeginSingleUseCommands(command_buffer, upload_command_pool_))
        {
            TransitionImage(command_buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level_count);
            uploaded = EndSingleUseCommands(command_buffer, upload_command_pool_);
        }
    }

    if (mapped)
        dispatch_.vkUnmapMemory(device_, staging_memory);
    DestroyBuffer(staging, staging_memory);
    std::fclose(file);
    if (!uploaded)
    {
        DestroyTexture(texture);
        return false;
    }

    texture->mip_levels = level_count;
    if (!CreateImageView(*texture, format) || !AllocateTextureDescriptor(*texture))
    {
        DestroyTexture(texture);
        return false;
    }

    texture_dimensions = {static_cast<int>(width), static_cast<int>(height)};
    texture_handle = reinterpret_cast<Rml::TextureHandle>(texture);
    std::fprintf(stderr, "[PS5-Vulkan] loaded KTX2 %s: %ux%u, %u mip levels, staging=%llu bytes\n",
                 source.c_str(), width, height, level_count,
                 static_cast<unsigned long long>(kTextureUploadChunkBytes));
    return true;
}

bool Ps5VulkanRenderInterface::DecodeRadioAtlas(const std::uint8_t *payload,
                                                std::size_t payload_size,
                                                std::vector<std::uint8_t> &pixels,
                                                std::uint32_t width, std::uint32_t height)
{
    if (!payload || width == 0 || height == 0 ||
        static_cast<std::size_t>(width) > kMaxRadioAtlasPixels / height)
        return false;

    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (count > kMaxRadioAtlasPixels)
        return false;

    // The source is an alpha-only bitmap-font atlas. Keep it as R8 instead of
    // expanding each coverage byte to white RGBA; this cuts the working set
    // and the resident Vulkan image from 16 MiB to 4 MiB for a 2048x2048 page.
    pixels.assign(count, 255);
    std::size_t input = 0;
    std::size_t output = 0;
    while (input < payload_size && output < count)
    {
        const std::uint8_t token = payload[input++];
        const std::size_t length = static_cast<std::size_t>(token & 0x7f) + 1;
        if (length > count - output)
            return false;
        if ((token & 0x80) == 0)
        {
            for (std::size_t i = 0; i < length; ++i)
                pixels[output + i] = 0;
        }
        else
        {
            const std::size_t bytes = (length + 1) / 2;
            if (bytes > payload_size - input)
                return false;
            for (std::size_t i = 0; i < length; ++i)
            {
                const std::uint8_t packed = payload[input + i / 2];
                const std::uint8_t alpha =
                    (i & 1u) ? ((packed & 0x0f) * 17u) : ((packed >> 4) * 17u);
                pixels[output + i] = alpha;
            }
            input += bytes;
        }
        output += length;
    }
    return output == count;
}

bool Ps5VulkanRenderInterface::ReadTextureFile(const Rml::String &source,
                                               std::vector<std::uint8_t> &pixels, int &width,
                                               int &height, bool &alpha_only)
{
    alpha_only = false;
    std::vector<std::uint8_t> bytes;
    if (!ReadWholeFile(source.c_str(), bytes))
        return false;
    if (bytes.size() >= 16 && std::memcmp(bytes.data(), "RTA1", 4) == 0)
    {
        width = ReadLe16(bytes.data() + 4);
        height = ReadLe16(bytes.data() + 6);
        const std::uint32_t pixel_count = ReadLe32(bytes.data() + 8);
        const std::uint32_t payload_length = ReadLe32(bytes.data() + 12);
        const std::uint64_t expected_pixel_count = static_cast<std::uint64_t>(width) * height;
        if (width <= 0 || height <= 0 || expected_pixel_count != pixel_count ||
            expected_pixel_count > kMaxRadioAtlasPixels || bytes.size() != 16ull + payload_length)
            return false;
        std::fprintf(
            stderr, "[PS5-Vulkan] decode RTA1 alpha atlas %s: %dx%d, payload=%u, output=%u bytes\n",
            source.c_str(), width, height, payload_length, pixel_count);
        alpha_only =
            DecodeRadioAtlas(bytes.data() + 16, payload_length, pixels,
                             static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        return alpha_only;
    }
    if (bytes.size() < 18 || bytes[0] != 0 || bytes[1] != 0 || bytes[2] != 2 ||
        (bytes[16] != 24 && bytes[16] != 32) || (bytes[17] & 0x0f) != 8)
        return false;
    if (!ReadTextTextureHeader(bytes, width, height))
        return false;
    const bool top_down = (bytes[17] & 0x30) == 0x20;
    const std::size_t source_stride = bytes[16] == 32 ? 4u : 3u;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    const std::size_t source_bytes = pixel_count * source_stride;
    if (bytes.size() != 18 + source_bytes)
        return false;
    // TGA is BGRA; the runtime intentionally keeps this ordering for the backplate
    // path. 24-bit sources (the cabinet finish themes) expand to 32-bit with
    // opaque alpha, honouring the TGA row order.
    const std::uint8_t *source_data = bytes.data() + 18;
    pixels.assign(pixel_count * 4, 255);
    for (std::size_t row = 0; row < static_cast<std::size_t>(height); ++row)
    {
        const std::size_t destination_row_index =
            top_down ? row : (static_cast<std::size_t>(height) - 1U - row);
        const std::uint8_t *source_row =
            source_data + row * static_cast<std::size_t>(width) * source_stride;
        std::uint8_t *destination_row =
            pixels.data() + destination_row_index * static_cast<std::size_t>(width) * 4;
        for (std::size_t column = 0; column < static_cast<std::size_t>(width); ++column)
        {
            destination_row[column * 4 + 0] = source_row[column * source_stride + 0];
            destination_row[column * 4 + 1] = source_row[column * source_stride + 1];
            destination_row[column * 4 + 2] = source_row[column * source_stride + 2];
            if (source_stride == 4)
                destination_row[column * 4 + 3] = source_row[column * 4 + 3];
        }
    }
    return true;
}

bool Ps5VulkanRenderInterface::LoadTexture(Rml::TextureHandle &texture_handle,
                                           Rml::Vector2i &texture_dimensions,
                                           const Rml::String &source)
{
    texture_handle = {};
    texture_dimensions = {};
    if (source.size() >= 5 && source.compare(source.size() - 5, 5, ".ktx2") == 0)
        return LoadKtx2Texture(source, texture_handle, texture_dimensions);

    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    bool alpha_only = false;
    std::fprintf(stderr, "[PS5-Vulkan] LoadTexture: %s\n", source.c_str());
    if (!ReadTextureFile(source, pixels, width, height, alpha_only))
        return false;

    VkFormat format = alpha_only ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    if (!alpha_only && source.find("lvgl-bitmap") != Rml::String::npos)
    {
        for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);
        format = VK_FORMAT_R8G8B8A8_UNORM;
    }
    Texture *texture = nullptr;
    if (!CreateTextureFromPixels(pixels.data(), static_cast<std::uint32_t>(width),
                                 static_cast<std::uint32_t>(height), format, &texture))
        return false;
    texture_dimensions = {width, height};
    texture_handle = reinterpret_cast<Rml::TextureHandle>(texture);
    return true;
}

bool Ps5VulkanRenderInterface::GenerateTexture(Rml::TextureHandle &texture_handle,
                                               const Rml::byte *source,
                                               const Rml::Vector2i &dimensions)
{
    texture_handle = {};
    if (!source || dimensions.x <= 0 || dimensions.y <= 0 ||
        static_cast<std::uint64_t>(dimensions.x) * static_cast<std::uint64_t>(dimensions.y) >
            kMaxTexturePixels)
    {
        std::fprintf(stderr, "[PS5-Vulkan] rejected generated texture dimensions: %dx%d\n",
                     dimensions.x, dimensions.y);
        return false;
    }
    Texture *texture = nullptr;
    if (!CreateTextureFromPixels(reinterpret_cast<const std::uint8_t *>(source),
                                 static_cast<std::uint32_t>(dimensions.x),
                                 static_cast<std::uint32_t>(dimensions.y), VK_FORMAT_R8G8B8A8_UNORM,
                                 &texture))
        return false;
    texture_handle = reinterpret_cast<Rml::TextureHandle>(texture);
    return true;
}

void Ps5VulkanRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    DestroyTexture(reinterpret_cast<Texture *>(texture));
}

void Ps5VulkanRenderInterface::EnableScissorRegion(bool enable)
{
    scissor_enabled_ = enable;
}

void Ps5VulkanRenderInterface::SetScissorRegion(int x, int y, int width, int height)
{
    const float sx = extent_.width / static_cast<float>(viewport_width_);
    const float sy = extent_.height / static_cast<float>(viewport_height_);
    scissor_.offset.x = std::max(0, static_cast<int>(x * sx));
    scissor_.offset.y = std::max(0, static_cast<int>(y * sy));
    scissor_.extent.width = static_cast<std::uint32_t>(std::max(0, static_cast<int>(width * sx)));
    scissor_.extent.height = static_cast<std::uint32_t>(std::max(0, static_cast<int>(height * sy)));
    if (scissor_.offset.x + static_cast<int>(scissor_.extent.width) >
        static_cast<int>(extent_.width))
        scissor_.extent.width =
            extent_.width - std::min(extent_.width, static_cast<std::uint32_t>(scissor_.offset.x));
    if (scissor_.offset.y + static_cast<int>(scissor_.extent.height) >
        static_cast<int>(extent_.height))
        scissor_.extent.height =
            extent_.height -
            std::min(extent_.height, static_cast<std::uint32_t>(scissor_.offset.y));
}

void Ps5VulkanRenderInterface::BeginFrame()
{
    if (!initialized_ || in_frame_)
        return;
    Frame &frame = frames_[frame_index_];
    dispatch_.vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    dispatch_.vkResetFences(device_, 1, &frame.fence);
    VkResult acquire =
        dispatch_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.image_available,
                                        VK_NULL_HANDLE, &swapchain_image_index_);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
    {
        std::fprintf(stderr, "[PS5-Vulkan] vkAcquireNextImageKHR failed: %d\n",
                     static_cast<int>(acquire));
        return;
    }
    dispatch_.vkResetCommandPool(device_, frame.command_pool, 0);
    VkCommandBufferBeginInfo begin =
        MakeVkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dispatch_.vkBeginCommandBuffer(frame.command_buffer, &begin) != VK_SUCCESS)
        return;
    VkClearValue clear{};
    clear.color.float32[0] = 0.025f;
    clear.color.float32[1] = 0.05f;
    clear.color.float32[2] = 0.07f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass =
        MakeVkStruct<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffers_[swapchain_image_index_];
    pass.renderArea.extent = extent_;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    dispatch_.vkCmdBeginRenderPass(frame.command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dispatch_.vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const float viewport[2] = {static_cast<float>(extent_.width),
                               static_cast<float>(extent_.height)};
    dispatch_.vkCmdPushConstants(frame.command_buffer, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                                 0, sizeof(viewport), viewport);
    const VkViewport dynamic_viewport{
        0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height),
        0.0f, 1.0f};
    const VkRect2D dynamic_scissor{{0, 0}, extent_};
    dispatch_.vkCmdSetViewport(frame.command_buffer, 0, 1, &dynamic_viewport);
    dispatch_.vkCmdSetScissor(frame.command_buffer, 0, 1, &dynamic_scissor);
    frame_vertex_cursor_ = 0;
    frame_index_cursor_ = 0;
    in_frame_ = true;
}

void Ps5VulkanRenderInterface::RenderGeometry(Rml::Vertex *rml_vertices, int num_vertices,
                                              int *indices, int num_indices,
                                              Rml::TextureHandle texture,
                                              const Rml::Vector2f &translation)
{
    if (!in_frame_ || num_vertices <= 0 || num_indices <= 0)
        return;
    Frame &frame = frames_[frame_index_];
    const float sx = extent_.width / static_cast<float>(viewport_width_);
    const float sy = extent_.height / static_cast<float>(viewport_height_);
    const std::size_t vertex_count = static_cast<std::size_t>(num_vertices);
    const std::size_t index_count = static_cast<std::size_t>(num_indices);
    if (vertex_count > max_vertex_bytes_ / sizeof(Vertex) ||
        index_count > max_index_bytes_ / sizeof(std::uint32_t) ||
        frame_vertex_cursor_ > max_vertex_bytes_ || frame_index_cursor_ > max_index_bytes_)
    {
        std::fprintf(stderr, "[PS5-Vulkan] UI geometry exceeds frame buffer capacity\n");
        return;
    }
    const std::size_t vertex_bytes = vertex_count * sizeof(Vertex);
    const std::size_t index_bytes = index_count * sizeof(std::uint32_t);
    if (vertex_bytes > max_vertex_bytes_ - frame_vertex_cursor_ ||
        index_bytes > max_index_bytes_ - frame_index_cursor_ || !frame.vertex_mapped ||
        !frame.index_mapped)
    {
        std::fprintf(stderr,
                     "[PS5-Vulkan] UI frame buffer capacity exceeded (vertices=%llu/%llu "
                     "indices=%llu/%llu)\n",
                     static_cast<unsigned long long>(vertex_bytes),
                     static_cast<unsigned long long>(max_vertex_bytes_ - frame_vertex_cursor_),
                     static_cast<unsigned long long>(index_bytes),
                     static_cast<unsigned long long>(max_index_bytes_ - frame_index_cursor_));
        return;
    }
    const std::size_t vertex_base = frame_vertex_cursor_;
    const std::size_t index_base = frame_index_cursor_;
    auto *destination_vertices =
        reinterpret_cast<Vertex *>(static_cast<std::uint8_t *>(frame.vertex_mapped) + vertex_base);
    auto *destination_indices = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(frame.index_mapped) + index_base);
    for (std::size_t i = 0; i < vertex_count; ++i)
    {
        const Rml::Vertex &source = rml_vertices[i];
        Vertex &destination = destination_vertices[i];
        destination.x = (source.position.x + translation.x) * sx;
        destination.y = (source.position.y + translation.y) * sy;
        destination.u = source.tex_coord.x;
        destination.v = source.tex_coord.y;
        destination.r = source.colour.red;
        destination.g = source.colour.green;
        destination.b = source.colour.blue;
        destination.a = source.colour.alpha;
    }
    for (std::size_t i = 0; i < index_count; ++i)
    {
        const int index = indices[i];
        if (index < 0 || index >= num_vertices)
        {
            std::fprintf(stderr, "[PS5-Vulkan] UI geometry contains an invalid index\n");
            return;
        }
        destination_indices[i] = static_cast<std::uint32_t>(index);
    }
    frame_vertex_cursor_ += vertex_bytes;
    frame_index_cursor_ += index_bytes;

    VkDeviceSize vertex_offset = static_cast<VkDeviceSize>(vertex_base);
    dispatch_.vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, &frame.vertex_buffer,
                                     &vertex_offset);
    dispatch_.vkCmdBindIndexBuffer(frame.command_buffer, frame.index_buffer,
                                   static_cast<VkDeviceSize>(index_base), VK_INDEX_TYPE_UINT32);
    Texture *app_texture = reinterpret_cast<Texture *>(texture);
    VkDescriptorSet descriptor = app_texture ? app_texture->descriptor : white_descriptor_;
    dispatch_.vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      pipeline_layout_, 0, 1, &descriptor, 0, nullptr);
    if (scissor_enabled_)
    {
        dispatch_.vkCmdSetScissor(frame.command_buffer, 0, 1, &scissor_);
    }
    else
    {
        const VkRect2D full{{0, 0}, extent_};
        dispatch_.vkCmdSetScissor(frame.command_buffer, 0, 1, &full);
    }
    dispatch_.vkCmdDrawIndexed(frame.command_buffer, static_cast<std::uint32_t>(num_indices), 1, 0,
                               0, 0);
}

bool Ps5VulkanRenderInterface::EndFrame()
{
    if (!in_frame_)
        return false;
    Frame &frame = frames_[frame_index_];
    dispatch_.vkCmdEndRenderPass(frame.command_buffer);
    if (!CheckResult(dispatch_.vkEndCommandBuffer(frame.command_buffer), "vkEndCommandBuffer"))
        return false;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = MakeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.render_finished;
    if (!CheckResult(dispatch_.vkQueueSubmit(graphics_queue_, 1, &submit, frame.fence),
                     "dispatch_.vkQueueSubmit(frame)"))
        return false;
    VkPresentInfoKHR present = MakeVkStruct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.render_finished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &swapchain_image_index_;
    const VkResult result = dispatch_.vkQueuePresentKHR(present_queue_, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::fprintf(stderr, "[PS5-Vulkan] vkQueuePresentKHR failed: %d\n",
                     static_cast<int>(result));
        return false;
    }
    in_frame_ = false;
    frame_index_ = (frame_index_ + 1) % static_cast<std::uint32_t>(frames_.size());
    return true;
}
