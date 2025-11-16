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
#undef HAVE_LIBPLACEBO

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libswscale/swscale.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "misc/bstr.h"
#include "options/m_config.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/hwdec.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"
#include "video/fmt-conversion.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "sub/osd.h"
#include "vo.h"
#include "win_state.h"

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
    
    // Current video frame to display
    struct mp_image *current_frame_image;
    bool is_redraw;  // Track if current frame is a redraw for OSD handling
    
    // Software frame upload resources
    VkImage upload_image;
    VkDeviceMemory upload_image_memory;
    VkBuffer upload_staging_buffer;
    VkDeviceMemory upload_staging_memory;
    struct SwsContext *sws_context;
    struct mp_image *rgb_image;  // RGB conversion buffer
    
    // Video destination rectangle (for centering and aspect ratio)
    struct mp_rect dst_rect;
    
    // OSD support (CPU-side compositing for software decoding)
    struct mp_osd_res osd_res;
    struct mp_image *osd_image;  // OSD composition buffer (BGRA)
    
    // Options cache for tracking changes
    struct m_config_cache *opts_cache;
    
    // Event handling
    Uint32 wakeup_event;
};

static void cleanup_vulkan(struct priv *p);
static int create_swapchain(struct vo *vo);
static int create_image_views(struct vo *vo);
static int create_framebuffers(struct vo *vo);
static int create_command_buffers(struct vo *vo);
static int create_sync_objects(struct vo *vo);

static int recreate_swapchain(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (!p->device)
        return -1;
    
    // Wait for device to be idle before recreating swapchain
    vkDeviceWaitIdle(p->device);
    
    // Clean up old swapchain resources
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
    
    // Reset current frame counter
    p->current_frame = 0;
    
    // Recreate swapchain and related resources
    if (create_swapchain(vo) < 0)
        return -1;
    
    if (create_image_views(vo) < 0)
        return -1;
    
    if (create_framebuffers(vo) < 0)
        return -1;
    
    if (create_command_buffers(vo) < 0)
        return -1;
    
    if (create_sync_objects(vo) < 0)
        return -1;
    
    return 0;
}

static void set_fullscreen(struct vo *vo)
{
    struct priv *p = vo->priv;
    struct mp_vo_opts *opts = p->opts_cache->opts;
    int fs = opts->fullscreen;
    
    Uint32 fs_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
    
    Uint32 old_flags = SDL_GetWindowFlags(p->window);
    int prev_fs = !!(old_flags & fs_flag);
    if (fs == prev_fs)
        return;
    
    Uint32 flags = 0;
    if (fs)
        flags |= fs_flag;
    
    if (SDL_SetWindowFullscreen(p->window, flags)) {
        MP_ERR(vo, "SDL_SetWindowFullscreen failed\n");
        return;
    }
    
    // Recreate swapchain to handle fullscreen mode change
    if (recreate_swapchain(vo) < 0) {
        MP_ERR(vo, "Failed to recreate swapchain after fullscreen toggle\n");
        return;
    }
    
    // Mark for redraw after fullscreen change
    vo->want_redraw = true;
}

static void update_screeninfo(struct vo *vo, struct mp_rect *screenrc)
{
    struct priv *p = vo->priv;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(p->window),
                                  &mode)) {
        MP_ERR(vo, "SDL_GetCurrentDisplayMode failed\n");
        return;
    }
    *screenrc = (struct mp_rect){0, 0, mode.w, mode.h};
}

static uint32_t find_memory_type(struct priv *p, uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(p->physical_device, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

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
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
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
    // Only CPU-side OSD buffer needs cleanup
    talloc_free(p->osd_image);
    p->osd_image = NULL;
}

static void cleanup_upload_resources(struct priv *p)
{
    if (!p->device)
        return;
    
    vkDeviceWaitIdle(p->device);
    
    if (p->sws_context) {
        sws_freeContext(p->sws_context);
        p->sws_context = NULL;
    }
    
    talloc_free(p->rgb_image);
    p->rgb_image = NULL;
    
    if (p->upload_image) {
        vkDestroyImage(p->device, p->upload_image, NULL);
        p->upload_image = VK_NULL_HANDLE;
    }
    
    if (p->upload_image_memory) {
        vkFreeMemory(p->device, p->upload_image_memory, NULL);
        p->upload_image_memory = VK_NULL_HANDLE;
    }
    
    if (p->upload_staging_buffer) {
        vkDestroyBuffer(p->device, p->upload_staging_buffer, NULL);
        p->upload_staging_buffer = VK_NULL_HANDLE;
    }
    
    if (p->upload_staging_memory) {
        vkFreeMemory(p->device, p->upload_staging_memory, NULL);
        p->upload_staging_memory = VK_NULL_HANDLE;
    }
}

static int create_upload_resources(struct vo *vo, uint32_t width, uint32_t height)
{
    struct priv *p = vo->priv;
    
    // Clean up existing resources if any
    cleanup_upload_resources(p);
    
    // Create staging buffer for uploading RGB data
    VkDeviceSize buffer_size = width * height * 4; // BGRA format
    
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    if (vkCreateBuffer(p->device, &buffer_info, NULL, &p->upload_staging_buffer) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create upload staging buffer\n");
        return -1;
    }
    
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(p->device, p->upload_staging_buffer, &mem_requirements);
    
    uint32_t memory_type = find_memory_type(p, mem_requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for upload staging buffer\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type,
    };
    
    if (vkAllocateMemory(p->device, &alloc_info, NULL, &p->upload_staging_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate upload staging memory\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    vkBindBufferMemory(p->device, p->upload_staging_buffer, p->upload_staging_memory, 0);
    
    // Create intermediate image for upload (BGRA format)
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    if (vkCreateImage(p->device, &image_info, NULL, &p->upload_image) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create upload image\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    vkGetImageMemoryRequirements(p->device, p->upload_image, &mem_requirements);
    
    memory_type = find_memory_type(p, mem_requirements.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for upload image\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    
    if (vkAllocateMemory(p->device, &alloc_info, NULL, &p->upload_image_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate upload image memory\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    vkBindImageMemory(p->device, p->upload_image, p->upload_image_memory, 0);
    
    // Allocate RGB conversion buffer
    p->rgb_image = mp_image_alloc(IMGFMT_BGRA, width, height);
    if (!p->rgb_image) {
        MP_ERR(vo, "Failed to allocate RGB conversion buffer\n");
        cleanup_upload_resources(p);
        return -1;
    }
    
    return 0;
}

static int create_osd_resources(struct vo *vo, uint32_t width, uint32_t height)
{
    struct priv *p = vo->priv;
    
    // Clean up existing resources if any
    cleanup_osd_resources(p);
    
    // Allocate OSD composition buffer (CPU-side only, no Vulkan resources needed)
    // This buffer is used for compositing OSD onto software-decoded frames
    p->osd_image = mp_image_alloc(IMGFMT_BGRA, width, height);
    if (!p->osd_image) {
        MP_ERR(vo, "Failed to allocate OSD composition buffer\n");
        return -1;
    }
    
    return 0;
}

static void cleanup_vulkan(struct priv *p)
{
    if (!p->device)
        return;
    
    vkDeviceWaitIdle(p->device);
    
    // Clean up OSD resources
    cleanup_osd_resources(p);
    
    // Clean up upload resources
    cleanup_upload_resources(p);
    
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
    
    // Initialize options cache
    p->opts_cache = m_config_cache_alloc(p, vo->global, &vo_sub_opts);
    
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        MP_ERR(vo, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    
    // Create window with undefined position - will be set in reconfig
    // Use borderless window to match vo_sdl behavior and avoid decoration issues
    p->window = SDL_CreateWindow("dmpv (Vulkan)",
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS);
    
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
    
    return 0;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    // Release current frame reference
    mp_image_unrefp(&p->current_frame_image);
    
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
    
    // Calculate window geometry using autofit and geometry options
    struct vo_win_geometry geo;
    struct mp_rect screenrc;
    
    update_screeninfo(vo, &screenrc);
    vo_calc_window_geometry(vo, &screenrc, &screenrc, false, &geo);
    vo_apply_window_geometry(vo, &geo);
    
    int win_w = vo->dwidth;
    int win_h = vo->dheight;
    
    // Resize window to calculated dimensions
    SDL_SetWindowSize(p->window, win_w, win_h);
    
    // Set window position if forced
    if (geo.flags & VO_WIN_FORCE_POS)
        SDL_SetWindowPosition(p->window, geo.win.x0, geo.win.y0);
    
    // Calculate video destination rectangle for proper centering and aspect ratio
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    p->dst_rect = dst;
    
    // Adjust OSD resolution to match video dimensions (OSD will scale with video)
    // This ensures OSD text size is proportional to video size
    p->osd_res.w = params->w;
    p->osd_res.h = params->h;
    p->osd_res.ml = 0;
    p->osd_res.mt = 0;
    p->osd_res.mr = 0;
    p->osd_res.mb = 0;
    
    // Create upload resources for software frames
    // Note: Using video dimensions, OSD will be rendered at video resolution
    if (create_upload_resources(vo, params->w, params->h) < 0) {
        MP_ERR(vo, "Failed to create upload resources\n");
        return -1;
    }
    
    // Create OSD resources at video resolution to match upload buffer
    // This means OSD will scale with the video
    if (create_osd_resources(vo, params->w, params->h) < 0) {
        MP_ERR(vo, "Failed to create OSD resources\n");
        return -1;
    }
    
    // Set fullscreen state
    set_fullscreen(vo);
    
    // Show window after all configuration is complete
    SDL_ShowWindow(p->window);
    
    return 0;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    
    // Store a reference to the current frame for rendering in flip_page
    mp_image_setrefp(&p->current_frame_image, frame->current);
    // Track if this is a redraw (for OSD rendering during seeks/pauses)
    p->is_redraw = frame->redraw;
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
    
    // Transition swapchain image to TRANSFER_DST_OPTIMAL for copying/blitting
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = p->swapchain_images[image_index],
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    
    vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, NULL, 0, NULL, 1, &barrier);
    
    // Clear the entire swapchain image to black first to avoid stale content
    VkClearColorValue clear_value = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange clear_range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(cmd, p->swapchain_images[image_index],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &clear_range);
    
    // Render the video frame
    if (p->current_frame_image) {
        struct mp_image *mpi = p->current_frame_image;
        
        if (mpi->imgfmt == IMGFMT_VULKAN) {
            // Hardware decoded Vulkan frame
            AVFrame *frame = mp_image_to_av_frame(mpi);
            if (frame && frame->data[0]) {
                AVVkFrame *vkf = (AVVkFrame *)frame->data[0];
                
                // Transition source image to TRANSFER_SRC_OPTIMAL
                VkImageMemoryBarrier src_barrier = {
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,  // Vulkan frames are typically in GENERAL layout
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = vkf->img[0],
                    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .subresourceRange.baseMipLevel = 0,
                    .subresourceRange.levelCount = 1,
                    .subresourceRange.baseArrayLayer = 0,
                    .subresourceRange.layerCount = 1,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                };
                
                vkCmdPipelineBarrier(cmd,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, 0, NULL, 0, NULL, 1, &src_barrier);
                
                // Calculate centered destination within swapchain
                // Similar to SDL VO's centering logic
                int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
                int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
                int dst_x = (p->swapchain_extent.width - dst_w) / 2;
                int dst_y = (p->swapchain_extent.height - dst_h) / 2;
                
                // Blit the Vulkan frame to swapchain
                VkImageBlit blit = {
                    .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .srcSubresource.mipLevel = 0,
                    .srcSubresource.baseArrayLayer = 0,
                    .srcSubresource.layerCount = 1,
                    .srcOffsets[0] = {0, 0, 0},
                    .srcOffsets[1] = {mpi->w, mpi->h, 1},
                    .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .dstSubresource.mipLevel = 0,
                    .dstSubresource.baseArrayLayer = 0,
                    .dstSubresource.layerCount = 1,
                    .dstOffsets[0] = {dst_x, dst_y, 0},
                    .dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1},
                };
                
                vkCmdBlitImage(cmd,
                             vkf->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             p->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, VK_FILTER_LINEAR);
                
                // Transition source image back to GENERAL
                src_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                src_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                src_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                src_barrier.dstAccessMask = 0;
                
                vkCmdPipelineBarrier(cmd,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                   0, 0, NULL, 0, NULL, 1, &src_barrier);
            }
            if (frame)
                av_frame_free(&frame);
        } else {
            // Software frame - convert to RGB and upload
            if (!p->rgb_image || !p->upload_staging_buffer || !p->upload_image) {
                MP_WARN(vo, "Upload resources not initialized\n");
            } else {
                // Convert YUV to RGB using swscale
                if (!p->sws_context) {
                    p->sws_context = sws_getContext(
                        mpi->w, mpi->h, imgfmt2pixfmt(mpi->imgfmt),
                        mpi->w, mpi->h, AV_PIX_FMT_BGRA,
                        SWS_BILINEAR, NULL, NULL, NULL);
                }
                
                if (p->sws_context) {
                    sws_scale(p->sws_context,
                             (const uint8_t *const *)mpi->planes, mpi->stride,
                             0, mpi->h,
                             p->rgb_image->planes, p->rgb_image->stride);
                    
                    // Draw OSD on top of the RGB frame (similar to vo_drm)
                    if (p->osd_image) {
                        // Use osd_image as composition buffer at video resolution
                        // Clear it first
                        mp_image_clear(p->osd_image, 0, 0, p->osd_image->w, p->osd_image->h);
                        
                        // Copy RGB frame (already at video size)
                        uint32_t copy_w = MPMIN(mpi->w, p->osd_image->w);
                        uint32_t copy_h = MPMIN(mpi->h, p->osd_image->h);
                        for (uint32_t y = 0; y < copy_h; y++) {
                            memcpy(p->osd_image->planes[0] + y * p->osd_image->stride[0],
                                   p->rgb_image->planes[0] + y * p->rgb_image->stride[0],
                                   copy_w * 4);
                        }
                        
                        // Draw OSD on top at video resolution (will scale with video)
                        // Use pts=0 for redraws to ensure fresh OSD state is used
                        double osd_pts = p->is_redraw ? 0 : mpi->pts;
                        osd_draw_on_image(vo->osd, p->osd_res, osd_pts, 0, p->osd_image);
                        
                        // Upload the composited image to staging buffer
                        void *data;
                        if (vkMapMemory(p->device, p->upload_staging_memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
                            uint32_t src_stride = p->osd_image->stride[0];
                            uint32_t dst_stride = mpi->w * 4;
                            
                            for (uint32_t y = 0; y < mpi->h; y++) {
                                memcpy((uint8_t*)data + y * dst_stride,
                                       p->osd_image->planes[0] + y * src_stride,
                                       dst_stride);
                            }
                            
                            vkUnmapMemory(p->device, p->upload_staging_memory);
                        }
                    } else {
                        // No OSD support - just upload the RGB frame
                        void *data;
                        if (vkMapMemory(p->device, p->upload_staging_memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
                            // Copy RGB image data to staging buffer
                            uint32_t src_stride = p->rgb_image->stride[0];
                            uint32_t dst_stride = mpi->w * 4;
                            
                            for (uint32_t y = 0; y < mpi->h; y++) {
                                memcpy((uint8_t*)data + y * dst_stride,
                                       p->rgb_image->planes[0] + y * src_stride,
                                       dst_stride);
                            }
                            
                            vkUnmapMemory(p->device, p->upload_staging_memory);
                        }
                    }
                    
                    // Transition upload image to TRANSFER_DST_OPTIMAL
                    VkImageMemoryBarrier upload_barrier = {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = p->upload_image,
                        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .subresourceRange.baseMipLevel = 0,
                        .subresourceRange.levelCount = 1,
                        .subresourceRange.baseArrayLayer = 0,
                        .subresourceRange.layerCount = 1,
                        .srcAccessMask = 0,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    };
                    
                    vkCmdPipelineBarrier(cmd,
                                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, 0, NULL, 0, NULL, 1, &upload_barrier);
                    
                    // Copy staging buffer to upload image
                    VkBufferImageCopy copy_region = {
                        .bufferOffset = 0,
                        .bufferRowLength = 0,
                        .bufferImageHeight = 0,
                        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .imageSubresource.mipLevel = 0,
                        .imageSubresource.baseArrayLayer = 0,
                        .imageSubresource.layerCount = 1,
                        .imageOffset = {0, 0, 0},
                        .imageExtent = {mpi->w, mpi->h, 1},
                    };
                    
                    vkCmdCopyBufferToImage(cmd, p->upload_staging_buffer, p->upload_image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
                    
                    // Transition upload image to TRANSFER_SRC_OPTIMAL
                    upload_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    upload_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    upload_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    upload_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    
                    vkCmdPipelineBarrier(cmd,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, 0, NULL, 0, NULL, 1, &upload_barrier);
                    
                    // Calculate centered destination within swapchain
                    int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
                    int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
                    int dst_x = (p->swapchain_extent.width - dst_w) / 2;
                    int dst_y = (p->swapchain_extent.height - dst_h) / 2;
                    
                    // Blit upload image (with OSD) to swapchain
                    VkImageBlit blit = {
                        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .srcSubresource.mipLevel = 0,
                        .srcSubresource.baseArrayLayer = 0,
                        .srcSubresource.layerCount = 1,
                        .srcOffsets[0] = {0, 0, 0},
                        .srcOffsets[1] = {mpi->w, mpi->h, 1},
                        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .dstSubresource.mipLevel = 0,
                        .dstSubresource.baseArrayLayer = 0,
                        .dstSubresource.layerCount = 1,
                        .dstOffsets[0] = {dst_x, dst_y, 0},
                        .dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1},
                    };
                    
                    vkCmdBlitImage(cmd,
                                 p->upload_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 p->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &blit, VK_FILTER_LINEAR);
                }
            }
        }
    }
    
    // Render OSD-only overlay when redrawing (e.g., during seeks or paused)
    // This ensures OSD elements like seek position/duration appear even when
    // we're just redrawing the last frame or have no frame at all
    if (p->is_redraw && p->osd_image && p->upload_image && p->upload_staging_buffer) {
        // During redraws, render OSD on black background and overlay it
        // Clear OSD buffer to black
        mp_image_clear(p->osd_image, 0, 0, p->osd_image->w, p->osd_image->h);
        
        // Draw OSD at current time (use pts=0 when redrawing)
        osd_draw_on_image(vo->osd, p->osd_res, 0, 0, p->osd_image);
        
        // Upload the OSD-only image
        void *data;
        if (vkMapMemory(p->device, p->upload_staging_memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
            uint32_t src_stride = p->osd_image->stride[0];
            uint32_t dst_stride = p->osd_image->w * 4;
            
            for (uint32_t y = 0; y < p->osd_image->h; y++) {
                memcpy((uint8_t*)data + y * dst_stride,
                       p->osd_image->planes[0] + y * src_stride,
                       dst_stride);
            }
            
            vkUnmapMemory(p->device, p->upload_staging_memory);
            
            // Upload to GPU and blit to swapchain
            VkImageMemoryBarrier upload_barrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = p->upload_image,
                .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .subresourceRange.baseMipLevel = 0,
                .subresourceRange.levelCount = 1,
                .subresourceRange.baseArrayLayer = 0,
                .subresourceRange.layerCount = 1,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            
            vkCmdPipelineBarrier(cmd,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, NULL, 0, NULL, 1, &upload_barrier);
            
            VkBufferImageCopy copy_region = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = 0,
                .imageSubresource.baseArrayLayer = 0,
                .imageSubresource.layerCount = 1,
                .imageOffset = {0, 0, 0},
                .imageExtent = {p->osd_image->w, p->osd_image->h, 1},
            };
            
            vkCmdCopyBufferToImage(cmd, p->upload_staging_buffer, p->upload_image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
            
            upload_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            upload_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            upload_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            upload_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            
            vkCmdPipelineBarrier(cmd,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, NULL, 0, NULL, 1, &upload_barrier);
            
            // Calculate centered destination
            int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
            int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
            int dst_x = (p->swapchain_extent.width - dst_w) / 2;
            int dst_y = (p->swapchain_extent.height - dst_h) / 2;
            
            VkImageBlit blit = {
                .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .srcSubresource.mipLevel = 0,
                .srcSubresource.baseArrayLayer = 0,
                .srcSubresource.layerCount = 1,
                .srcOffsets[0] = {0, 0, 0},
                .srcOffsets[1] = {p->osd_image->w, p->osd_image->h, 1},
                .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .dstSubresource.mipLevel = 0,
                .dstSubresource.baseArrayLayer = 0,
                .dstSubresource.layerCount = 1,
                .dstOffsets[0] = {dst_x, dst_y, 0},
                .dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1},
            };
            
            vkCmdBlitImage(cmd,
                         p->upload_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         p->swapchain_images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_LINEAR);
        }
    }
    
    // Transition swapchain image to PRESENT_SRC_KHR for presentation
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    
    vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, NULL, 0, NULL, 1, &barrier);
    
    vkEndCommandBuffer(cmd);
    
    // Submit command buffer
    VkSemaphore wait_semaphores[] = {p->image_available_semaphores[p->current_frame]};
    VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_TRANSFER_BIT};
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
    struct priv *p = vo->priv;
    
    switch (request) {
    case VOCTRL_VO_OPTS_CHANGED: {
        void *opt;
        while (m_config_cache_get_next_changed(p->opts_cache, &opt)) {
            struct mp_vo_opts *opts = p->opts_cache->opts;
            if (&opts->fullscreen == opt)
                set_fullscreen(vo);
        }
        return VO_TRUE;
    }
    case VOCTRL_RESET:
        // Handle seeking - don't immediately redraw, let playloop handle timing
        // This allows OSD state to be updated before we render
        return VO_TRUE;
    case VOCTRL_PAUSE:
        // Handle pause
        return VO_TRUE;
    case VOCTRL_RESUME:
        // Handle resume
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        // Handle panscan changes - recalculate destination rect
        if (vo->params) {
            struct mp_rect src, dst;
            struct mp_osd_res osd;
            vo_get_src_dst_rects(vo, &src, &dst, &osd);
            p->dst_rect = dst;
            // Keep OSD at video resolution (set in reconfig)
            vo->want_redraw = true;
        }
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
