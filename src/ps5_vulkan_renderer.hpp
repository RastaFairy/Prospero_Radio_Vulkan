// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <RmlUi/Core/RenderInterfaceCompatibility.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

class Ps5VulkanRenderInterface final : public Rml::RenderInterfaceCompatibility
{
  public:
    Ps5VulkanRenderInterface();
    ~Ps5VulkanRenderInterface() override;

    Ps5VulkanRenderInterface(const Ps5VulkanRenderInterface &) = delete;
    Ps5VulkanRenderInterface &operator=(const Ps5VulkanRenderInterface &) = delete;

    bool Initialize(const char *vertex_shader_path, const char *fragment_shader_path);
    bool IsInitialized() const
    {
        return initialized_;
    }

    void BeginFrame();
    bool EndFrame();

    void RenderGeometry(Rml::Vertex *vertices, int num_vertices, int *indices, int num_indices,
                        Rml::TextureHandle texture, const Rml::Vector2f &translation) override;
    bool LoadTexture(Rml::TextureHandle &texture_handle, Rml::Vector2i &texture_dimensions,
                     const Rml::String &source) override;
    bool GenerateTexture(Rml::TextureHandle &texture_handle, const Rml::byte *source,
                         const Rml::Vector2i &dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(int x, int y, int width, int height) override;

  private:
    struct VulkanDispatch
    {
        PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = nullptr;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
        PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
        PFN_vkAllocateMemory vkAllocateMemory = nullptr;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
        PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
        PFN_vkBindImageMemory vkBindImageMemory = nullptr;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
        PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
        PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
        PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
        PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
        PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
        PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
        PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
        PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
        PFN_vkCreateBuffer vkCreateBuffer = nullptr;
        PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
        PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
        PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
        PFN_vkCreateDevice vkCreateDevice = nullptr;
        PFN_vkCreateDisplayPlaneSurfaceKHR vkCreateDisplayPlaneSurfaceKHR = nullptr;
        PFN_vkCreateFence vkCreateFence = nullptr;
        PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
        PFN_vkCreateImage vkCreateImage = nullptr;
        PFN_vkCreateImageView vkCreateImageView = nullptr;
        PFN_vkCreateInstance vkCreateInstance = nullptr;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
        PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
        PFN_vkCreateSampler vkCreateSampler = nullptr;
        PFN_vkCreateSemaphore vkCreateSemaphore = nullptr;
        PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
        PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = nullptr;
        PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
        PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
        PFN_vkDestroyDevice vkDestroyDevice = nullptr;
        PFN_vkDestroyFence vkDestroyFence = nullptr;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
        PFN_vkDestroyImage vkDestroyImage = nullptr;
        PFN_vkDestroyImageView vkDestroyImageView = nullptr;
        PFN_vkDestroyInstance vkDestroyInstance = nullptr;
        PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
        PFN_vkDestroyRenderPass vkDestroyRenderPass = nullptr;
        PFN_vkDestroySampler vkDestroySampler = nullptr;
        PFN_vkDestroySemaphore vkDestroySemaphore = nullptr;
        PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
        PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = nullptr;
        PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR = nullptr;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
        PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
        PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
        PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
        PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
        PFN_vkFreeMemory vkFreeMemory = nullptr;
        PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
        PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
        PFN_vkGetDisplayModePropertiesKHR vkGetDisplayModePropertiesKHR = nullptr;
        PFN_vkGetDisplayPlaneCapabilitiesKHR vkGetDisplayPlaneCapabilitiesKHR = nullptr;
        PFN_vkGetDisplayPlaneSupportedDisplaysKHR vkGetDisplayPlaneSupportedDisplaysKHR = nullptr;
        PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
        PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR
            vkGetPhysicalDeviceDisplayPlanePropertiesKHR = nullptr;
        PFN_vkGetPhysicalDeviceDisplayPropertiesKHR vkGetPhysicalDeviceDisplayPropertiesKHR =
            nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
            nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
            nullptr;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
        PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR =
            nullptr;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
        PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = nullptr;
        PFN_vkMapMemory vkMapMemory = nullptr;
        PFN_vkQueuePresentKHR vkQueuePresentKHR = nullptr;
        PFN_vkQueueSubmit vkQueueSubmit = nullptr;
        PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
        PFN_vkResetCommandPool vkResetCommandPool = nullptr;
        PFN_vkResetFences vkResetFences = nullptr;
        PFN_vkUnmapMemory vkUnmapMemory = nullptr;
        PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
        PFN_vkWaitForFences vkWaitForFences = nullptr;
        bool LoadGlobal();
        bool LoadInstance(VkInstance instance);
    };

    struct Texture
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
        std::uint32_t mip_levels = 1;
        std::uint32_t bytes_per_pixel = 4;
    };

    struct Vertex
    {
        float x;
        float y;
        float u;
        float v;
        std::uint8_t r;
        std::uint8_t g;
        std::uint8_t b;
        std::uint8_t a;
    };

    struct Frame
    {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore render_finished = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        void *vertex_mapped = nullptr;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceMemory index_memory = VK_NULL_HANDLE;
        void *index_mapped = nullptr;
    };

    bool LoadGlobalVulkanDispatch();
    bool LoadInstanceVulkanDispatch();
    bool CreateInstance();
    bool CreateSurface();
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool CreateRenderPass();
    bool CreateDescriptorResources();
    bool CreatePipeline(const char *vertex_shader_path, const char *fragment_shader_path);
    bool CreateFrameResources();
    bool CreateWhiteTexture();
    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer &buffer, VkDeviceMemory &memory, void **mapped = nullptr);
    bool CreateImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkImage &image, VkDeviceMemory &memory,
                     std::uint32_t mip_levels = 1);
    bool CreateTextureFromPixels(const std::uint8_t *pixels, std::uint32_t width,
                                 std::uint32_t height, VkFormat format, Texture **out_texture);
    bool UploadTexture(Texture &texture, const std::uint8_t *pixels, std::uint32_t width,
                       std::uint32_t height);
    bool CreateImageView(Texture &texture, VkFormat format);
    bool AllocateTextureDescriptor(Texture &texture);
    bool LoadKtx2Texture(const Rml::String &source, Rml::TextureHandle &texture_handle,
                         Rml::Vector2i &texture_dimensions);
    bool ReadTextureFile(const Rml::String &source, std::vector<std::uint8_t> &pixels, int &width,
                         int &height, bool &alpha_only);
    bool DecodeRadioAtlas(const std::uint8_t *payload, std::size_t payload_size,
                          std::vector<std::uint8_t> &pixels, std::uint32_t width,
                          std::uint32_t height);
    std::vector<std::uint8_t> ReadBinaryFile(const char *path);
    bool ReadTextTextureHeader(const std::vector<std::uint8_t> &bytes, int &width,
                               int &height) const;

    std::uint32_t FindMemoryType(std::uint32_t type_bits, VkMemoryPropertyFlags properties) const;
    bool HasDeviceExtension(VkPhysicalDevice device, const char *name) const;
    void DestroyTexture(Texture *texture);
    void DestroyBuffer(VkBuffer buffer, VkDeviceMemory memory);
    bool BeginSingleUseCommands(VkCommandBuffer &buffer, VkCommandPool &pool);
    bool EndSingleUseCommands(VkCommandBuffer buffer, VkCommandPool pool);
    bool TransitionImage(VkCommandBuffer command_buffer, VkImage image, VkImageLayout old_layout,
                         VkImageLayout new_layout, std::uint32_t mip_levels = 1);
    bool CopyBufferToImage(VkCommandBuffer command_buffer, VkBuffer buffer, VkImage image,
                           std::uint32_t width, std::uint32_t height);
    VkShaderModule CreateShaderModule(const std::vector<std::uint8_t> &spirv);
    bool ChooseDisplayConfiguration();
    bool ChooseDisplayForDevice(VkPhysicalDevice device);
    bool ChooseSurfaceFormat(VkSurfaceFormatKHR &format);
    bool ChoosePresentMode(VkPresentModeKHR &mode);
    bool CheckResult(VkResult result, const char *what) const;

    VulkanDispatch dispatch_{};

    bool initialized_ = false;
    bool in_frame_ = false;
    bool scissor_enabled_ = false;
    VkRect2D scissor_{};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    std::uint32_t graphics_queue_family_ = UINT32_MAX;
    std::uint32_t present_queue_family_ = UINT32_MAX;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkDisplayKHR display_ = VK_NULL_HANDLE;
    VkDisplayModeKHR display_mode_ = VK_NULL_HANDLE;
    VkSurfaceTransformFlagBitsKHR surface_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::uint32_t display_width_ = 1920;
    std::uint32_t display_height_ = 1080;
    VkSurfaceFormatKHR surface_format_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSampler mipmap_sampler_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkCommandPool upload_command_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet white_descriptor_ = VK_NULL_HANDLE;
    Texture *white_texture_ = nullptr;

    std::vector<Frame> frames_;
    std::uint32_t frame_index_ = 0;
    std::uint32_t swapchain_image_index_ = 0;
    VkExtent2D extent_{};

    std::uint32_t viewport_width_ = 1920;
    std::uint32_t viewport_height_ = 1080;
    std::size_t max_vertex_bytes_ = 8 * 1024 * 1024;
    std::size_t max_index_bytes_ = 4 * 1024 * 1024;
    std::size_t frame_vertex_cursor_ = 0;
    std::size_t frame_index_cursor_ = 0;
};
