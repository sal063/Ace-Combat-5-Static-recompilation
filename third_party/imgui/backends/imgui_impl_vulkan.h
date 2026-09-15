#pragma once
#ifndef IMGUI_DISABLE
#include "imgui.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif

#if defined(IMGUI_IMPL_VULKAN_NO_PROTOTYPES) && !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#ifdef IMGUI_IMPL_VULKAN_VOLK_FILENAME
#include IMGUI_IMPL_VULKAN_VOLK_FILENAME
#else
#include <volk.h>
#endif
#else
#include <vulkan/vulkan.h>
#endif
#if defined(VK_VERSION_1_3) || defined(VK_KHR_dynamic_rendering)
#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
#endif

#define IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE   (8)
#define IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE         (2)

struct ImGui_ImplVulkan_PipelineInfo
{
    VkRenderPass                    RenderPass;
    uint32_t                        Subpass;
    VkSampleCountFlagBits           MSAASamples = {};
    ImVector<VkDynamicState>        ExtraDynamicStates;
#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
    VkPipelineRenderingCreateInfoKHR PipelineRenderingCreateInfo;
#endif
};

struct ImGui_ImplVulkan_InitInfo
{
    uint32_t                        ApiVersion;
    VkInstance                      Instance;
    VkPhysicalDevice                PhysicalDevice;
    VkDevice                        Device;
    uint32_t                        QueueFamily;
    VkQueue                         Queue;
    VkDescriptorPool                DescriptorPool;
    uint32_t                        DescriptorPoolSize;
    uint32_t                        MinImageCount;
    uint32_t                        ImageCount;
    VkPipelineCache                 PipelineCache;

    ImGui_ImplVulkan_PipelineInfo   PipelineInfoMain;

    bool                            UseDynamicRendering;

    const VkAllocationCallbacks*    Allocator;
    void                            (*CheckVkResultFn)(VkResult err);
    VkDeviceSize                    MinAllocationSize;

    VkShaderModuleCreateInfo        CustomShaderVertCreateInfo;
    VkShaderModuleCreateInfo        CustomShaderFragCreateInfo;
};

IMGUI_IMPL_API bool             ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo* info);
IMGUI_IMPL_API void             ImGui_ImplVulkan_Shutdown();
IMGUI_IMPL_API void             ImGui_ImplVulkan_NewFrame();
IMGUI_IMPL_API void             ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline = VK_NULL_HANDLE);
IMGUI_IMPL_API void             ImGui_ImplVulkan_SetMinImageCount(uint32_t min_image_count);

IMGUI_IMPL_API void             ImGui_ImplVulkan_CreateMainPipeline(const ImGui_ImplVulkan_PipelineInfo* info);

IMGUI_IMPL_API void             ImGui_ImplVulkan_UpdateTexture(ImTextureData* tex);

IMGUI_IMPL_API VkDescriptorSet  ImGui_ImplVulkan_AddTexture(VkImageView image_view, VkImageLayout image_layout);
IMGUI_IMPL_API void             ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet descriptor_set);

#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
IMGUI_IMPL_API VkDescriptorSet  ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout);
#endif

IMGUI_IMPL_API bool             ImGui_ImplVulkan_LoadFunctions(uint32_t api_version, PFN_vkVoidFunction(*loader_func)(const char* function_name, void* user_data), void* user_data = nullptr);

struct ImGui_ImplVulkan_RenderState
{
    VkCommandBuffer     CommandBuffer;
    VkPipeline          Pipeline;
    VkPipelineLayout    PipelineLayout;
};

struct ImGui_ImplVulkanH_Frame;
struct ImGui_ImplVulkanH_Window;

IMGUI_IMPL_API void                 ImGui_ImplVulkanH_CreateOrResizeWindow(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, ImGui_ImplVulkanH_Window* wd, uint32_t queue_family, const VkAllocationCallbacks* allocator, int w, int h, uint32_t min_image_count, VkImageUsageFlags image_usage);
IMGUI_IMPL_API void                 ImGui_ImplVulkanH_DestroyWindow(VkInstance instance, VkDevice device, ImGui_ImplVulkanH_Window* wd, const VkAllocationCallbacks* allocator);
IMGUI_IMPL_API VkSurfaceFormatKHR   ImGui_ImplVulkanH_SelectSurfaceFormat(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkFormat* request_formats, int request_formats_count, VkColorSpaceKHR request_color_space);
IMGUI_IMPL_API VkPresentModeKHR     ImGui_ImplVulkanH_SelectPresentMode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkPresentModeKHR* request_modes, int request_modes_count);
IMGUI_IMPL_API VkPhysicalDevice     ImGui_ImplVulkanH_SelectPhysicalDevice(VkInstance instance);
IMGUI_IMPL_API uint32_t             ImGui_ImplVulkanH_SelectQueueFamilyIndex(VkPhysicalDevice physical_device);
IMGUI_IMPL_API int                  ImGui_ImplVulkanH_GetMinImageCountFromPresentMode(VkPresentModeKHR present_mode);

struct ImGui_ImplVulkanH_Frame
{
    VkCommandPool       CommandPool;
    VkCommandBuffer     CommandBuffer;
    VkFence             Fence;
    VkImage             Backbuffer;
    VkImageView         BackbufferView;
    VkFramebuffer       Framebuffer;
};

struct ImGui_ImplVulkanH_FrameSemaphores
{
    VkSemaphore         ImageAcquiredSemaphore;
    VkSemaphore         RenderCompleteSemaphore;
};

struct ImGui_ImplVulkanH_Window
{
    bool                    UseDynamicRendering;
    VkSurfaceKHR            Surface;
    VkSurfaceFormatKHR      SurfaceFormat;
    VkPresentModeKHR        PresentMode;
    VkAttachmentDescription AttachmentDesc;
    VkClearValue            ClearValue;

    int                     Width;
    int                     Height;
    VkSwapchainKHR          Swapchain;
    VkRenderPass            RenderPass;
    VkPipeline              Pipeline;
    uint32_t                FrameIndex;
    uint32_t                ImageCount;
    uint32_t                SemaphoreCount;
    uint32_t                SemaphoreIndex;
    ImVector<ImGui_ImplVulkanH_Frame>           Frames;
    ImVector<ImGui_ImplVulkanH_FrameSemaphores> FrameSemaphores;

    ImGui_ImplVulkanH_Window()
    {
        memset((void*)this, 0, sizeof(*this));

        PresentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;

        AttachmentDesc.format = VK_FORMAT_UNDEFINED;
        AttachmentDesc.samples = VK_SAMPLE_COUNT_1_BIT;
        AttachmentDesc.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        AttachmentDesc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        AttachmentDesc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        AttachmentDesc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        AttachmentDesc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        AttachmentDesc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif
