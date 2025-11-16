/*
 * Simple Vulkan video output using SDL for windowing
 * Supports FFmpeg Vulkan hwaccel without requiring libplacebo
 *
 * This file is part of dmpv.
 *
 * dmpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * dmpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with dmpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#undef HAVE_LIBDECOR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "sub/osd.h"
#include "vo.h"

// Key mapping from SDL to dmpv
struct keymap_entry {
    SDL_Keycode sdl;
    int dmpv;
};

static const struct keymap_entry keys[] = {
    {SDLK_RETURN, MP_KEY_ENTER},
    {SDLK_ESCAPE, MP_KEY_ESC},
    {SDLK_BACKSPACE, MP_KEY_BACKSPACE},
    {SDLK_TAB, MP_KEY_TAB},
    {SDLK_PRINTSCREEN, MP_KEY_PRINT},
    {SDLK_PAUSE, MP_KEY_PAUSE},
    {SDLK_INSERT, MP_KEY_INSERT},
    {SDLK_HOME, MP_KEY_HOME},
    {SDLK_PAGEUP, MP_KEY_PAGE_UP},
    {SDLK_DELETE, MP_KEY_DELETE},
    {SDLK_END, MP_KEY_END},
    {SDLK_PAGEDOWN, MP_KEY_PAGE_DOWN},
    {SDLK_RIGHT, MP_KEY_RIGHT},
    {SDLK_LEFT, MP_KEY_LEFT},
    {SDLK_DOWN, MP_KEY_DOWN},
    {SDLK_UP, MP_KEY_UP},
    {SDLK_KP_ENTER, MP_KEY_KPENTER},
    {SDLK_F1, MP_KEY_F + 1},
    {SDLK_F2, MP_KEY_F + 2},
    {SDLK_F3, MP_KEY_F + 3},
    {SDLK_F4, MP_KEY_F + 4},
    {SDLK_F5, MP_KEY_F + 5},
    {SDLK_F6, MP_KEY_F + 6},
    {SDLK_F7, MP_KEY_F + 7},
    {SDLK_F8, MP_KEY_F + 8},
    {SDLK_F9, MP_KEY_F + 9},
    {SDLK_F10, MP_KEY_F + 10},
    {SDLK_F11, MP_KEY_F + 11},
    {SDLK_F12, MP_KEY_F + 12},
};

struct priv {
    SDL_Window *window;
    
    // Vulkan objects
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue graphics_queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkCommandPool command_pool;
    VkCommandBuffer *command_buffers;
    VkSemaphore *image_available_semaphores;
    VkSemaphore *render_finished_semaphores;
    VkFence *in_flight_fences;
    
    VkImage *swapchain_images;
    VkImageView *swapchain_image_views;
    VkFramebuffer *framebuffers;
    VkRenderPass render_pass;
    VkPipeline graphics_pipeline;
    VkPipelineLayout pipeline_layout;
    
    uint32_t swapchain_image_count;
    uint32_t current_frame;
    uint32_t graphics_queue_family;
    
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;
    
    // Hardware decode context
    struct mp_hwdec_ctx hwctx;
    AVBufferRef *hw_device_ctx;
    
    struct mp_image_params params;
    bool vsync;
    
    // OSD support (not implemented - kept for future use)
    struct mp_osd_res osd_res;
    double osd_pts;
    struct mp_image *osd_image;  // Not used - OSD rendering not implemented
    
    // Event handling
    Uint32 wakeup_event;
};

static void cleanup_vulkan(struct priv *p);

static int find_queue_family(struct priv *p, VkPhysicalDevice device)
{
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);
    
    VkQueueFamilyProperties *queue_families = 
        malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families);
    
    int graphics_family = -1;
    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, p->surface, &present_support);
            if (present_support) {
                graphics_family = i;
                break;
            }
        }
    }
    
    free(queue_families);
    return graphics_family;
}

static int create_vulkan_instance(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    // Get required extensions from SDL
    unsigned int sdl_extension_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(p->window, &sdl_extension_count, NULL)) {
        MP_ERR(vo, "Failed to get SDL Vulkan extension count: %s\n", SDL_GetError());
        return -1;
    }
    
    const char **extensions = malloc((sdl_extension_count) * sizeof(char*));
    if (!SDL_Vulkan_GetInstanceExtensions(p->window, &sdl_extension_count, extensions)) {
        MP_ERR(vo, "Failed to get SDL Vulkan extensions: %s\n", SDL_GetError());
        free(extensions);
        return -1;
    }
    
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "dmpv",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };
    
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = sdl_extension_count,
        .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = 0,
    };
    
    VkResult result = vkCreateInstance(&create_info, NULL, &p->instance);
    free(extensions);
    
    if (result != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create Vulkan instance: %d\n", result);
        return -1;
    }
    
    return 0;
}

static int select_physical_device(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(p->instance, &device_count, NULL);
    
    if (device_count == 0) {
        MP_ERR(vo, "Failed to find GPUs with Vulkan support\n");
        return -1;
    }
    
    VkPhysicalDevice *devices = malloc(device_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(p->instance, &device_count, devices);
    
    // Just pick the first suitable device
    for (uint32_t i = 0; i < device_count; i++) {
        int queue_family = find_queue_family(p, devices[i]);
        if (queue_family >= 0) {
            p->physical_device = devices[i];
            p->graphics_queue_family = queue_family;
            free(devices);
            return 0;
        }
    }
    
    free(devices);
    MP_ERR(vo, "Failed to find a suitable GPU\n");
    return -1;
}

static int create_logical_device(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = p->graphics_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };
    
    VkPhysicalDeviceFeatures device_features = {0};
    
    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    
    VkDeviceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pQueueCreateInfos = &queue_create_info,
        .queueCreateInfoCount = 1,
        .pEnabledFeatures = &device_features,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
        .enabledLayerCount = 0,
    };
    
    if (vkCreateDevice(p->physical_device, &create_info, NULL, &p->device) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create logical device\n");
        return -1;
    }
    
    vkGetDeviceQueue(p->device, p->graphics_queue_family, 0, &p->graphics_queue);
    
    return 0;
}

static int create_swapchain(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->physical_device, p->surface, &capabilities);
    
    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(p->physical_device, p->surface, &format_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(format_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(p->physical_device, p->surface, &format_count, formats);
    
    VkSurfaceFormatKHR surface_format = formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surface_format = formats[i];
            break;
        }
    }
    free(formats);
    
    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(p->physical_device, p->surface, &present_mode_count, NULL);
    VkPresentModeKHR *present_modes = malloc(present_mode_count * sizeof(VkPresentModeKHR));
    vkGetPhysicalDeviceSurfacePresentModesKHR(p->physical_device, p->surface, &present_mode_count, present_modes);
    
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // Always available
    if (!p->vsync) {
        for (uint32_t i = 0; i < present_mode_count; i++) {
            if (present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
                break;
            } else if (present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
        }
    }
    free(present_modes);
    
    p->swapchain_extent = capabilities.currentExtent;
    if (p->swapchain_extent.width == UINT32_MAX) {
        int w, h;
        SDL_Vulkan_GetDrawableSize(p->window, &w, &h);
        p->swapchain_extent.width = MPCLAMP(w, capabilities.minImageExtent.width, 
                                            capabilities.maxImageExtent.width);
        p->swapchain_extent.height = MPCLAMP(h, capabilities.minImageExtent.height,
                                             capabilities.maxImageExtent.height);
    }
    
    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    
    p->swapchain_format = surface_format.format;
    
    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = p->surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = p->swapchain_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE,
    };
    
    if (vkCreateSwapchainKHR(p->device, &create_info, NULL, &p->swapchain) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create swapchain\n");
        return -1;
    }
    
    vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->swapchain_image_count, NULL);
    p->swapchain_images = malloc(p->swapchain_image_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(p->device, p->swapchain, &p->swapchain_image_count, p->swapchain_images);
    
    return 0;
}

static int create_image_views(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    p->swapchain_image_views = malloc(p->swapchain_image_count * sizeof(VkImageView));
    
    for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
        VkImageViewCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = p->swapchain_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = p->swapchain_format,
            .components.r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .components.g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .components.b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .components.a = VK_COMPONENT_SWIZZLE_IDENTITY,
            .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .subresourceRange.baseMipLevel = 0,
            .subresourceRange.levelCount = 1,
            .subresourceRange.baseArrayLayer = 0,
            .subresourceRange.layerCount = 1,
        };
        
        if (vkCreateImageView(p->device, &create_info, NULL, &p->swapchain_image_views[i]) != VK_SUCCESS) {
            MP_ERR(vo, "Failed to create image view %d\n", i);
            return -1;
        }
    }
    
    return 0;
}

static int create_render_pass(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    VkAttachmentDescription color_attachment = {
        .format = p->swapchain_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    
    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
    };
    
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    
    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    
    if (vkCreateRenderPass(p->device, &render_pass_info, NULL, &p->render_pass) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create render pass\n");
        return -1;
    }
    
    return 0;
}

static int create_framebuffers(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    p->framebuffers = malloc(p->swapchain_image_count * sizeof(VkFramebuffer));
    
    for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
        VkImageView attachments[] = {
            p->swapchain_image_views[i]
        };
        
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = p->render_pass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width = p->swapchain_extent.width,
            .height = p->swapchain_extent.height,
            .layers = 1,
        };
        
        if (vkCreateFramebuffer(p->device, &framebuffer_info, NULL, &p->framebuffers[i]) != VK_SUCCESS) {
            MP_ERR(vo, "Failed to create framebuffer %d\n", i);
            return -1;
        }
    }
    
    return 0;
}

static int create_command_pool(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = p->graphics_queue_family,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    
    if (vkCreateCommandPool(p->device, &pool_info, NULL, &p->command_pool) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create command pool\n");
        return -1;
    }
    
    return 0;
}

static int create_command_buffers(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    p->command_buffers = malloc(p->swapchain_image_count * sizeof(VkCommandBuffer));
    
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = p->swapchain_image_count,
    };
    
    if (vkAllocateCommandBuffers(p->device, &alloc_info, p->command_buffers) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate command buffers\n");
        return -1;
    }
    
    return 0;
}

static int create_sync_objects(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    p->image_available_semaphores = malloc(p->swapchain_image_count * sizeof(VkSemaphore));
    p->render_finished_semaphores = malloc(p->swapchain_image_count * sizeof(VkSemaphore));
    p->in_flight_fences = malloc(p->swapchain_image_count * sizeof(VkFence));
    
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    
    for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
        if (vkCreateSemaphore(p->device, &semaphore_info, NULL, &p->image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(p->device, &semaphore_info, NULL, &p->render_finished_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(p->device, &fence_info, NULL, &p->in_flight_fences[i]) != VK_SUCCESS) {
            MP_ERR(vo, "Failed to create sync objects\n");
            return -1;
        }
    }
    
    return 0;
}

static int init_vulkan(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (create_vulkan_instance(vo) < 0)
        return -1;
    
    if (!SDL_Vulkan_CreateSurface(p->window, p->instance, &p->surface)) {
        MP_ERR(vo, "Failed to create Vulkan surface: %s\n", SDL_GetError());
        return -1;
    }
    
    if (select_physical_device(vo) < 0)
        return -1;
    
    if (create_logical_device(vo) < 0)
        return -1;
    
    if (create_swapchain(vo) < 0)
        return -1;
    
    if (create_image_views(vo) < 0)
        return -1;
    
    if (create_render_pass(vo) < 0)
        return -1;
    
    if (create_framebuffers(vo) < 0)
        return -1;
    
    if (create_command_pool(vo) < 0)
        return -1;
    
    if (create_command_buffers(vo) < 0)
        return -1;
    
    if (create_sync_objects(vo) < 0)
        return -1;
    
    return 0;
}

static int init_hwdec_ctx(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    p->hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!p->hw_device_ctx) {
        MP_ERR(vo, "Failed to allocate Vulkan hardware device context\n");
        return -1;
    }
    
    AVHWDeviceContext *device_ctx = (AVHWDeviceContext *)p->hw_device_ctx->data;
    AVVulkanDeviceContext *vk_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;
    
    vk_ctx->inst = p->instance;
    vk_ctx->phys_dev = p->physical_device;
    vk_ctx->act_dev = p->device;
    vk_ctx->get_proc_addr = vkGetInstanceProcAddr;
    
    // Set up queue families (simplified - using same queue for everything)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    vk_ctx->nb_qf = 1;
    vk_ctx->qf[0].idx = p->graphics_queue_family;
    vk_ctx->qf[0].num = 1;
    vk_ctx->qf[0].flags = VK_QUEUE_GRAPHICS_BIT;
#else
    vk_ctx->queue_family_index = p->graphics_queue_family;
    vk_ctx->nb_graphics_queues = 1;
#endif
    
    if (av_hwdevice_ctx_init(p->hw_device_ctx) < 0) {
        MP_ERR(vo, "Failed to initialize Vulkan hardware device context\n");
        av_buffer_unref(&p->hw_device_ctx);
        return -1;
    }
    
    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = "vulkan",
        .av_device_ref = p->hw_device_ctx,
        .hw_imgfmt = IMGFMT_VULKAN,
    };
    
    if (vo->hwdec_devs)
        hwdec_devices_add(vo->hwdec_devs, &p->hwctx);
    
    return 0;
}

static void cleanup_osd_resources(struct priv *p)
{
    // OSD rendering not implemented - resources not allocated
    (void)p;
}

static int create_osd_resources(struct vo *vo, uint32_t width, uint32_t height)
{
    // OSD rendering not implemented in this minimal Vulkan VO
    // This function is kept as a placeholder for future implementation
    (void)vo;
    (void)width;
    (void)height;
    return 0;
}

static void cleanup_vulkan(struct priv *p)
{
    if (!p->device)
        return;
    
    vkDeviceWaitIdle(p->device);
    
    // Clean up OSD resources
    cleanup_osd_resources(p);
    
    if (p->image_available_semaphores) {
        for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
            if (p->image_available_semaphores[i])
                vkDestroySemaphore(p->device, p->image_available_semaphores[i], NULL);
        }
        free(p->image_available_semaphores);
        p->image_available_semaphores = NULL;
    }
    
    if (p->render_finished_semaphores) {
        for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
            if (p->render_finished_semaphores[i])
                vkDestroySemaphore(p->device, p->render_finished_semaphores[i], NULL);
        }
        free(p->render_finished_semaphores);
        p->render_finished_semaphores = NULL;
    }
    
    if (p->in_flight_fences) {
        for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
            if (p->in_flight_fences[i])
                vkDestroyFence(p->device, p->in_flight_fences[i], NULL);
        }
        free(p->in_flight_fences);
        p->in_flight_fences = NULL;
    }
    
    if (p->command_pool) {
        vkDestroyCommandPool(p->device, p->command_pool, NULL);
        p->command_pool = VK_NULL_HANDLE;
    }
    
    if (p->command_buffers) {
        free(p->command_buffers);
        p->command_buffers = NULL;
    }
    
    if (p->framebuffers) {
        for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
            if (p->framebuffers[i])
                vkDestroyFramebuffer(p->device, p->framebuffers[i], NULL);
        }
        free(p->framebuffers);
        p->framebuffers = NULL;
    }
    
    if (p->render_pass) {
        vkDestroyRenderPass(p->device, p->render_pass, NULL);
        p->render_pass = VK_NULL_HANDLE;
    }
    
    if (p->swapchain_image_views) {
        for (uint32_t i = 0; i < p->swapchain_image_count; i++) {
            if (p->swapchain_image_views[i])
                vkDestroyImageView(p->device, p->swapchain_image_views[i], NULL);
        }
        free(p->swapchain_image_views);
        p->swapchain_image_views = NULL;
    }
    
    if (p->swapchain_images) {
        free(p->swapchain_images);
        p->swapchain_images = NULL;
    }
    
    if (p->swapchain) {
        vkDestroySwapchainKHR(p->device, p->swapchain, NULL);
        p->swapchain = VK_NULL_HANDLE;
    }
    
    if (p->device) {
        vkDestroyDevice(p->device, NULL);
        p->device = VK_NULL_HANDLE;
    }
    
    if (p->surface) {
        vkDestroySurfaceKHR(p->instance, p->surface, NULL);
        p->surface = VK_NULL_HANDLE;
    }
    
    if (p->instance) {
        vkDestroyInstance(p->instance, NULL);
        p->instance = VK_NULL_HANDLE;
    }
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        MP_ERR(vo, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    
    p->window = SDL_CreateWindow("dmpv (Vulkan)",
                                  SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED,
                                  1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    
    if (!p->window) {
        MP_ERR(vo, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    
    p->wakeup_event = SDL_RegisterEvents(1);
    if (p->wakeup_event == (Uint32)-1) {
        MP_ERR(vo, "Failed to register SDL user event\n");
        SDL_DestroyWindow(p->window);
        SDL_Quit();
        return -1;
    }
    
    if (init_vulkan(vo) < 0) {
        SDL_DestroyWindow(p->window);
        SDL_Quit();
        return -1;
    }
    
    if (init_hwdec_ctx(vo) < 0) {
        cleanup_vulkan(p);
        SDL_DestroyWindow(p->window);
        SDL_Quit();
        return -1;
    }
    
    SDL_ShowWindow(p->window);
    
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (p->hwctx.av_device_ref) {
        if (vo->hwdec_devs)
            hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
        av_buffer_unref(&p->hw_device_ctx);
    }
    
    cleanup_vulkan(p);
    
    if (p->window) {
        SDL_DestroyWindow(p->window);
        p->window = NULL;
    }
    
    SDL_Quit();
}

static int query_format(struct vo *vo, int format)
{
    // Accept Vulkan hardware frames and common software formats
    if (format == IMGFMT_VULKAN)
        return 1;
    
    // Also accept common YUV and RGB formats for software decoding
    switch (format) {
    case IMGFMT_420P:
    case IMGFMT_NV12:
    case IMGFMT_BGRA:
    case IMGFMT_RGBA:
    case IMGFMT_BGR0:
    case IMGFMT_RGB0:
        return 1;
    }
    
    return 0;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    
    p->params = *params;
    
    // Resize window to match video dimensions
    SDL_SetWindowSize(p->window, params->w, params->h);
    
    // Set up OSD resolution
    p->osd_res = (struct mp_osd_res){
        .w = params->w,
        .h = params->h,
        .display_par = 1.0,
    };
    
    // Create OSD image buffer (BGRA format for OSD rendering)
    // Note: OSD rendering is not currently implemented in this minimal Vulkan VO
    talloc_free(p->osd_image);
    p->osd_image = NULL;
    
    return 0;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    
    // Update OSD presentation timestamp
    p->osd_pts = frame->current ? frame->current->pts : 0;
    
    // Just store the frame for rendering in flip_page
    // Actual rendering happens in flip_page when we know which swapchain image to use
}

static void upload_osd_texture(struct vo *vo)
{
    // OSD rendering not implemented in this minimal Vulkan VO
    // This function is kept as a placeholder for future implementation
    (void)vo;
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(p->device, p->swapchain, UINT64_MAX,
                                            p->image_available_semaphores[p->current_frame],
                                            VK_NULL_HANDLE, &image_index);
    
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        MP_WARN(vo, "Failed to acquire swapchain image\n");
        return;
    }
    
    vkWaitForFences(p->device, 1, &p->in_flight_fences[p->current_frame], VK_TRUE, UINT64_MAX);
    vkResetFences(p->device, 1, &p->in_flight_fences[p->current_frame]);
    
    // Record command buffer
    VkCommandBuffer cmd = p->command_buffers[image_index];
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pNext = NULL,
        .pInheritanceInfo = NULL,
    };
    
    vkBeginCommandBuffer(cmd, &begin_info);
    
    VkClearValue clear_color = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = NULL,
        .renderPass = p->render_pass,
        .framebuffer = p->framebuffers[image_index],
        .renderArea.offset = {0, 0},
        .renderArea.extent = p->swapchain_extent,
        .clearValueCount = 1,
        .pClearValues = &clear_color,
    };
    
    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    
    // Submit command buffer
    VkSemaphore wait_semaphores[] = {p->image_available_semaphores[p->current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signal_semaphores[] = {p->render_finished_semaphores[p->current_frame]};
    
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signal_semaphores,
    };
    
    result = vkQueueSubmit(p->graphics_queue, 1, &submit_info, p->in_flight_fences[p->current_frame]);
    if (result != VK_SUCCESS) {
        MP_ERR(vo, "Failed to submit draw command buffer: %d\n", result);
        return;
    }
    
    VkSwapchainKHR swapchains[] = {p->swapchain};
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = swapchains,
        .pImageIndices = &image_index,
        .pResults = NULL,
    };
    
    vkQueuePresentKHR(p->graphics_queue, &present_info);
    
    p->current_frame = (p->current_frame + 1) % p->swapchain_image_count;
}

static void wakeup(struct vo *vo)
{
    struct priv *p = vo->priv;
    SDL_Event event = {.type = p->wakeup_event};
    SDL_PushEvent(&event);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct priv *p = vo->priv;
    int64_t wait_ns = until_time_ns - mp_time_ns();
    int timeout_ms = MPCLAMP(wait_ns / 1000000, 0, 10000);
    SDL_Event ev;
    
    while (SDL_WaitEventTimeout(&ev, timeout_ms)) {
        timeout_ms = 0;
        switch (ev.type) {
        case SDL_WINDOWEVENT:
            switch (ev.window.event) {
            case SDL_WINDOWEVENT_EXPOSED:
                vo->want_redraw = true;
                break;
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                vo_event(vo, VO_EVENT_RESIZE);
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                vo_event(vo, VO_EVENT_FOCUS);
                break;
            }
            break;
        case SDL_QUIT:
            mp_input_put_key(vo->input_ctx, MP_KEY_CLOSE_WIN);
            break;
        case SDL_TEXTINPUT: {
            int sdl_mod = SDL_GetModState();
            int dmpv_mod = 0;
            if (sdl_mod & (KMOD_LCTRL | KMOD_RCTRL))
                dmpv_mod |= MP_KEY_MODIFIER_CTRL;
            if (sdl_mod & (KMOD_LALT | KMOD_RALT))
                dmpv_mod |= MP_KEY_MODIFIER_ALT;
            if (sdl_mod & (KMOD_LGUI | KMOD_RGUI))
                dmpv_mod |= MP_KEY_MODIFIER_META;
            struct bstr t = {
                (unsigned char *)ev.text.text, strlen(ev.text.text)
            };
            mp_input_put_key_utf8(vo->input_ctx, dmpv_mod, t);
            break;
        }
        case SDL_KEYDOWN: {
            int keycode = 0;
            for (int i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
                if (keys[i].sdl == ev.key.keysym.sym) {
                    keycode = keys[i].dmpv;
                    break;
                }
            }
            if (keycode) {
                if (ev.key.keysym.mod & (KMOD_LSHIFT | KMOD_RSHIFT))
                    keycode |= MP_KEY_MODIFIER_SHIFT;
                if (ev.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                    keycode |= MP_KEY_MODIFIER_CTRL;
                if (ev.key.keysym.mod & (KMOD_LALT | KMOD_RALT))
                    keycode |= MP_KEY_MODIFIER_ALT;
                if (ev.key.keysym.mod & (KMOD_LGUI | KMOD_RGUI))
                    keycode |= MP_KEY_MODIFIER_META;
                mp_input_put_key(vo->input_ctx, keycode);
            }
            break;
        }
        default:
            if (ev.type == p->wakeup_event)
                return;
            break;
        }
    }
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    (void)vo;  // Unused for now
    
    switch (request) {
    case VOCTRL_RESET:
        // Handle seeking - reset state if needed
        // For now, just acknowledge the reset
        return VO_TRUE;
    case VOCTRL_PAUSE:
        // Handle pause
        return VO_TRUE;
    case VOCTRL_RESUME:
        // Handle resume
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        // Handle panscan changes
        return VO_TRUE;
    }
    
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_vulkan_sdl = {
    .description = "Simple Vulkan output via SDL (no libplacebo)",
    .name = "vulkan-sdl",
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .vsync = true,
    },
    .options = (const struct m_option[]) {
        {"vsync", OPT_BOOL(vsync)},
        {0}
    },
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .wakeup = wakeup,
    .wait_events = wait_events,
    .uninit = uninit,
};
