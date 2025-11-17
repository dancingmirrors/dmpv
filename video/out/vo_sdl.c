/*
 * SDL video output with automatic Vulkan/GL backend selection
 * 
 * This file consolidates vo_vulkan_sdl and vo_sdl into one driver.
 * Use --sdl-backend=vulkan or --sdl-backend=gl to force a specific backend.
 * By default, tries Vulkan first, falls back to GL.
 *
 * This file is part of dmpv.
 */

#include "config.h"

// Ensure at least one backend is available
#if !defined(HAVE_SDL_VULKAN_BACKEND) || !HAVE_SDL_VULKAN_BACKEND
#if !defined(HAVE_GL) || !HAVE_GL
#error "vo_sdl requires either Vulkan (HAVE_SDL_VULKAN_BACKEND) or OpenGL (HAVE_GL) support"
#endif
#endif


#if defined(HAVE_SDL_VULKAN_BACKEND) && HAVE_SDL_VULKAN_BACKEND

// ============================================================================
// VULKAN BACKEND - Complete implementation from vo_vulkan_sdl.c
// ============================================================================

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

struct vk_priv {
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
    
    // Video source and destination rectangles (for centering and aspect ratio)
    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    
    // OSD support (CPU-side compositing for software decoding)
    struct mp_osd_res osd_res;
    struct mp_image *osd_image;  // OSD composition buffer (BGRA)
    
    // Options cache for tracking changes
    struct m_config_cache *opts_cache;
    
    // Event handling
    Uint32 wakeup_event;
    
    // Options
    bool borderless;
    bool switch_mode;  // Use SDL_WINDOW_FULLSCREEN instead of FULLSCREEN_DESKTOP
};

static void vk_cleanup_vulkan(struct vk_priv *p);
static int create_swapchain(struct vo *vo);
static int create_image_views(struct vo *vo);
static int create_framebuffers(struct vo *vo);
static int create_command_buffers(struct vo *vo);
static int create_sync_objects(struct vo *vo);

static int recreate_swapchain(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    
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

static void vk_set_fullscreen(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    struct mp_vo_opts *opts = p->opts_cache->opts;
    int fs = opts->fullscreen;
    
    Uint32 fs_flag;
    if (p->switch_mode)
        fs_flag = SDL_WINDOW_FULLSCREEN;
    else
        fs_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
    
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
    
    // Update window dimensions and recalculate video rectangles for new size
    // This is necessary because fullscreen changes the actual window/drawable size
    int w, h;
    SDL_GetWindowSize(p->window, &w, &h);
    vo->dwidth = w;
    vo->dheight = h;
    
    // Recalculate src/dst rectangles with updated dimensions
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    p->src_rect = src;
    p->dst_rect = dst;
    
    // Mark for redraw after fullscreen change
    vo->want_redraw = true;
}

static void vk_update_screeninfo(struct vo *vo, struct mp_rect *screenrc)
{
    struct vk_priv *p = vo->priv;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(p->window),
                                  &mode)) {
        MP_ERR(vo, "SDL_GetCurrentDisplayMode failed\n");
        return;
    }
    *screenrc = (struct mp_rect){0, 0, mode.w, mode.h};
}

static uint32_t vk_find_memory_type(struct vk_priv *p, uint32_t type_filter, VkMemoryPropertyFlags properties)
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

static int vk_find_queue_family(struct vk_priv *p, VkPhysicalDevice device)
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
        int queue_family = vk_find_queue_family(p, devices[i]);
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    
    // Always query the actual drawable size from SDL instead of relying on
    // capabilities.currentExtent, which may return stale values after window resize
    int w, h;
    SDL_Vulkan_GetDrawableSize(p->window, &w, &h);
    p->swapchain_extent.width = MPCLAMP(w, capabilities.minImageExtent.width, 
                                        capabilities.maxImageExtent.width);
    p->swapchain_extent.height = MPCLAMP(h, capabilities.minImageExtent.height,
                                         capabilities.maxImageExtent.height);
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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
    struct vk_priv *p = vo->priv;
    
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

static void vk_cleanup_osd_resources(struct vk_priv *p)
{
    // Only CPU-side OSD buffer needs cleanup
    talloc_free(p->osd_image);
    p->osd_image = NULL;
}

static void vk_cleanup_upload_resources(struct vk_priv *p)
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
    struct vk_priv *p = vo->priv;
    
    // Clean up existing resources if any
    vk_cleanup_upload_resources(p);
    
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
    
    uint32_t memory_type = vk_find_memory_type(p, mem_requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for upload staging buffer\n");
        vk_cleanup_upload_resources(p);
        return -1;
    }
    
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type,
    };
    
    if (vkAllocateMemory(p->device, &alloc_info, NULL, &p->upload_staging_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate upload staging memory\n");
        vk_cleanup_upload_resources(p);
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
        vk_cleanup_upload_resources(p);
        return -1;
    }
    
    vkGetImageMemoryRequirements(p->device, p->upload_image, &mem_requirements);
    
    memory_type = vk_find_memory_type(p, mem_requirements.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for upload image\n");
        vk_cleanup_upload_resources(p);
        return -1;
    }
    
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    
    if (vkAllocateMemory(p->device, &alloc_info, NULL, &p->upload_image_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate upload image memory\n");
        vk_cleanup_upload_resources(p);
        return -1;
    }
    
    vkBindImageMemory(p->device, p->upload_image, p->upload_image_memory, 0);
    
    // Allocate RGB conversion buffer
    p->rgb_image = mp_image_alloc(IMGFMT_BGRA, width, height);
    if (!p->rgb_image) {
        MP_ERR(vo, "Failed to allocate RGB conversion buffer\n");
        vk_cleanup_upload_resources(p);
        return -1;
    }
    
    return 0;
}

static int create_osd_resources(struct vo *vo, uint32_t width, uint32_t height)
{
    struct vk_priv *p = vo->priv;
    
    // Clean up existing resources if any
    vk_cleanup_osd_resources(p);
    
    // Allocate OSD composition buffer (CPU-side only, no Vulkan resources needed)
    // This buffer is used for compositing OSD onto software-decoded frames
    p->osd_image = mp_image_alloc(IMGFMT_BGRA, width, height);
    if (!p->osd_image) {
        MP_ERR(vo, "Failed to allocate OSD composition buffer\n");
        return -1;
    }
    
    return 0;
}

static void vk_cleanup_vulkan(struct vk_priv *p)
{
    if (!p->device)
        return;
    
    vkDeviceWaitIdle(p->device);
    
    // Clean up OSD resources
    vk_cleanup_osd_resources(p);
    
    // Clean up upload resources
    vk_cleanup_upload_resources(p);
    
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

static int vk_preinit(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    
    // Initialize options cache
    p->opts_cache = m_config_cache_alloc(p, vo->global, &vo_sub_opts);
    
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        MP_ERR(vo, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    
    // Create window with undefined position - will be set in reconfig
    p->window = SDL_CreateWindow("dmpv (Vulkan)",
                                  SDL_WINDOWPOS_UNDEFINED,
                                  SDL_WINDOWPOS_UNDEFINED,
                                  1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    
    if (!p->window) {
        MP_ERR(vo, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    
    // Set window border based on option
    if (p->borderless) {
        SDL_SetWindowBordered(p->window, SDL_FALSE);
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
        vk_cleanup_vulkan(p);
        SDL_DestroyWindow(p->window);
        SDL_Quit();
        return -1;
    }
    
    return 0;
}

static void vk_uninit(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    
    // Release current frame reference
    mp_image_unrefp(&p->current_frame_image);
    
    if (p->hwctx.av_device_ref) {
        if (vo->hwdec_devs)
            hwdec_devices_remove(vo->hwdec_devs, &p->hwctx);
        av_buffer_unref(&p->hw_device_ctx);
    }
    
    vk_cleanup_vulkan(p);
    
    if (p->window) {
        SDL_DestroyWindow(p->window);
        p->window = NULL;
    }
    
    SDL_Quit();
}

static int vk_query_format(struct vo *vo, int format)
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

static int vk_reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct vk_priv *p = vo->priv;
    
    p->params = *params;
    
    // Calculate window geometry using autofit and geometry options
    struct vo_win_geometry geo;
    struct mp_rect screenrc;
    
    vk_update_screeninfo(vo, &screenrc);
    vo_calc_window_geometry(vo, &screenrc, &screenrc, false, &geo);
    vo_apply_window_geometry(vo, &geo);
    
    int win_w = vo->dwidth;
    int win_h = vo->dheight;
    
    // Resize window to calculated dimensions
    SDL_SetWindowSize(p->window, win_w, win_h);
    
    // Set window position if forced
    if (geo.flags & VO_WIN_FORCE_POS)
        SDL_SetWindowPosition(p->window, geo.win.x0, geo.win.y0);
    
    // Recreate swapchain to match new window size
    if (recreate_swapchain(vo) < 0) {
        MP_ERR(vo, "Failed to recreate swapchain after window resize\n");
        return -1;
    }
    
    // Calculate video destination rectangle for proper centering and aspect ratio
    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);
    p->src_rect = src;
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
    vk_set_fullscreen(vo);
    
    // Force update of window dimensions and video rectangles
    // This ensures correct centering even when starting with --fs=yes
    // where set_fullscreen() might not have updated dimensions
    int actual_w, actual_h;
    SDL_GetWindowSize(p->window, &actual_w, &actual_h);
    if (vo->dwidth != actual_w || vo->dheight != actual_h) {
        vo->dwidth = actual_w;
        vo->dheight = actual_h;
        
        // Recreate swapchain to match actual window size
        if (recreate_swapchain(vo) < 0) {
            MP_ERR(vo, "Failed to recreate swapchain for actual window size\n");
            return -1;
        }
        
        // Recalculate video rectangles for the actual window size
        struct mp_rect src2, dst2;
        struct mp_osd_res osd2;
        vo_get_src_dst_rects(vo, &src2, &dst2, &osd2);
        p->src_rect = src2;
        p->dst_rect = dst2;
    }
    
    // Show window after it's been properly configured
    SDL_ShowWindow(p->window);
    
    return 0;
}

static void vk_draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct vk_priv *p = vo->priv;
    
    // Store a reference to the current frame for rendering in flip_page
    mp_image_setrefp(&p->current_frame_image, frame->current);
    // Track if this is a redraw (for OSD rendering during seeks/pauses)
    p->is_redraw = frame->redraw;
}

static void vk_flip_page(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    
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
                // Query actual swapchain size to ensure we use current dimensions
                int swap_w = p->swapchain_extent.width;
                int swap_h = p->swapchain_extent.height;
                int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
                int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
                int dst_x = (swap_w - dst_w) / 2;
                int dst_y = (swap_h - dst_h) / 2;
                
                MP_VERBOSE(vo, "Centering: swap=%dx%d dst=%dx%d pos=%d,%d\n",
                          swap_w, swap_h, dst_w, dst_h, dst_x, dst_y);
                
                // Blit the Vulkan frame to swapchain
                VkImageBlit blit = {
                    .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .srcSubresource.mipLevel = 0,
                    .srcSubresource.baseArrayLayer = 0,
                    .srcSubresource.layerCount = 1,
                    .srcOffsets[0] = {p->src_rect.x0, p->src_rect.y0, 0},
                    .srcOffsets[1] = {p->src_rect.x1, p->src_rect.y1, 1},
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
                    // Query actual swapchain size to ensure we use current dimensions
                    int swap_w = p->swapchain_extent.width;
                    int swap_h = p->swapchain_extent.height;
                    int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
                    int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
                    int dst_x = (swap_w - dst_w) / 2;
                    int dst_y = (swap_h - dst_h) / 2;
                    
                    // Blit upload image (with OSD) to swapchain
                    VkImageBlit blit = {
                        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .srcSubresource.mipLevel = 0,
                        .srcSubresource.baseArrayLayer = 0,
                        .srcSubresource.layerCount = 1,
                        .srcOffsets[0] = {p->src_rect.x0, p->src_rect.y0, 0},
                        .srcOffsets[1] = {p->src_rect.x1, p->src_rect.y1, 1},
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
            
            // Calculate centered destination within swapchain
            // Query actual swapchain size to ensure we use current dimensions
            int swap_w = p->swapchain_extent.width;
            int swap_h = p->swapchain_extent.height;
            int dst_w = p->dst_rect.x1 - p->dst_rect.x0;
            int dst_h = p->dst_rect.y1 - p->dst_rect.y0;
            int dst_x = (swap_w - dst_w) / 2;
            int dst_y = (swap_h - dst_h) / 2;
            
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

static void vk_wakeup(struct vo *vo)
{
    struct vk_priv *p = vo->priv;
    SDL_Event event = {.type = p->wakeup_event};
    SDL_PushEvent(&event);
}

static void vk_wait_events(struct vo *vo, int64_t until_time_ns)
{
    struct vk_priv *p = vo->priv;
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

static int vk_control(struct vo *vo, uint32_t request, void *data)
{
    struct vk_priv *p = vo->priv;
    
    switch (request) {
    case VOCTRL_VO_OPTS_CHANGED: {
        void *opt;
        while (m_config_cache_get_next_changed(p->opts_cache, &opt)) {
            struct mp_vo_opts *opts = p->opts_cache->opts;
            if (&opts->fullscreen == opt)
                vk_set_fullscreen(vo);
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
            p->src_rect = src;
            p->dst_rect = dst;
            // Keep OSD at video resolution (set in reconfig)
            vo->want_redraw = true;
        }
        return VO_TRUE;
    
    case VOCTRL_CHECK_EVENTS:
        // Handle window events including resize
        {
            int w, h;
            SDL_GetWindowSize(p->window, &w, &h);
            if (vo->dwidth != w || vo->dheight != h) {
                vo->dwidth = w;
                vo->dheight = h;
                
                // Recreate swapchain for new window size
                if (recreate_swapchain(vo) < 0) {
                    MP_ERR(vo, "Failed to recreate swapchain after resize\n");
                    return VO_ERROR;
                }
                
                // Recalculate video rectangles
                if (vo->params) {
                    struct mp_rect src, dst;
                    struct mp_osd_res osd;
                    vo_get_src_dst_rects(vo, &src, &dst, &osd);
                    p->src_rect = src;
                    p->dst_rect = dst;
                }
                
                vo->want_redraw = true;
            }
        }
        return VO_TRUE;
    }
    
    return VO_NOTIMPL;
}

#define OPT_BASE_STRUCT struct vk_priv

static const struct vo_driver vk_driver = {
    .description = "Simple Vulkan output via SDL (no libplacebo)",
    .name = "vulkan-sdl",
    .priv_size = sizeof(struct vk_priv),
    .priv_defaults = &(const struct vk_priv) {
        .vsync = true,
        .borderless = true,
        .switch_mode = false,
    },
    .options = (const struct m_option[]) {
        {"vsync", OPT_BOOL(vsync)},
        {"borderless", OPT_BOOL(borderless)},
        {"switch-mode", OPT_BOOL(switch_mode)},
        {0}
    },
    .preinit = vk_preinit,
    .query_format = vk_query_format,
    .reconfig = vk_reconfig,
    .control = vk_control,
    .draw_frame = vk_draw_frame,
    .flip_page = vk_flip_page,
    .wakeup = vk_wakeup,
    .wait_events = vk_wait_events,
    .uninit = vk_uninit,
    .options_prefix = "vulkan-sdl",
};


#endif // HAVE_SDL_VULKAN_BACKEND


#if defined(HAVE_GL) && HAVE_GL

// ============================================================================  
// GL BACKEND - Complete implementation from vo_sdl.c
// ============================================================================

/*
 * video output driver for SDL 2.0+
 *
 * Copyright (C) 2012 Rudolf Polzer <divVerent@xonotic.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#include <SDL2/SDL.h>

#include "input/input.h"
#include "input/keycodes.h"
#include "input/input.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"

#include "osdep/timer.h"

#include "sub/osd.h"

#include "video/mp_image.h"

#include "win_state.h"
#include "vo.h"

#undef HAVE_LIBDECOR

struct formatmap_entry {
    Uint32 sdl;
    unsigned int dmpv;
    int is_rgba;
};
const struct formatmap_entry formats[] = {
    {SDL_PIXELFORMAT_YV12, IMGFMT_420P, 0},
    {SDL_PIXELFORMAT_IYUV, IMGFMT_420P, 0},
    {SDL_PIXELFORMAT_UYVY, IMGFMT_UYVY, 0},
    //{SDL_PIXELFORMAT_YVYU, IMGFMT_YVYU, 0},
#if BYTE_ORDER == BIG_ENDIAN
    {SDL_PIXELFORMAT_RGB888, IMGFMT_0RGB, 0}, // RGB888 means XRGB8888
    {SDL_PIXELFORMAT_RGBX8888, IMGFMT_RGB0, 0}, // has no alpha -> bad for OSD
    {SDL_PIXELFORMAT_BGR888, IMGFMT_0BGR, 0}, // BGR888 means XBGR8888
    {SDL_PIXELFORMAT_BGRX8888, IMGFMT_BGR0, 0}, // has no alpha -> bad for OSD
    {SDL_PIXELFORMAT_ARGB8888, IMGFMT_ARGB, 1}, // matches SUBBITMAP_BGRA
    {SDL_PIXELFORMAT_RGBA8888, IMGFMT_RGBA, 1},
    {SDL_PIXELFORMAT_ABGR8888, IMGFMT_ABGR, 1},
    {SDL_PIXELFORMAT_BGRA8888, IMGFMT_BGRA, 1},
#else
    {SDL_PIXELFORMAT_RGB888, IMGFMT_BGR0, 0}, // RGB888 means XRGB8888
    {SDL_PIXELFORMAT_RGBX8888, IMGFMT_0BGR, 0}, // has no alpha -> bad for OSD
    {SDL_PIXELFORMAT_BGR888, IMGFMT_RGB0, 0}, // BGR888 means XBGR8888
    {SDL_PIXELFORMAT_BGRX8888, IMGFMT_0RGB, 0}, // has no alpha -> bad for OSD
    {SDL_PIXELFORMAT_ARGB8888, IMGFMT_BGRA, 1}, // matches SUBBITMAP_BGRA
    {SDL_PIXELFORMAT_RGBA8888, IMGFMT_ABGR, 1},
    {SDL_PIXELFORMAT_ABGR8888, IMGFMT_RGBA, 1},
    {SDL_PIXELFORMAT_BGRA8888, IMGFMT_ARGB, 1},
#endif
    {SDL_PIXELFORMAT_RGB24, IMGFMT_RGB24, 0},
    {SDL_PIXELFORMAT_BGR24, IMGFMT_BGR24, 0},
    {SDL_PIXELFORMAT_RGB565, IMGFMT_RGB565, 0},
};

struct gl_keymap_entry {
    SDL_Keycode sdl;
    int dmpv;
};
static const struct gl_keymap_entry gl_keys[] = {
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
    {SDLK_KP_1, MP_KEY_KP1},
    {SDLK_KP_2, MP_KEY_KP2},
    {SDLK_KP_3, MP_KEY_KP3},
    {SDLK_KP_4, MP_KEY_KP4},
    {SDLK_KP_5, MP_KEY_KP5},
    {SDLK_KP_6, MP_KEY_KP6},
    {SDLK_KP_7, MP_KEY_KP7},
    {SDLK_KP_8, MP_KEY_KP8},
    {SDLK_KP_9, MP_KEY_KP9},
    {SDLK_KP_0, MP_KEY_KP0},
    {SDLK_KP_PERIOD, MP_KEY_KPDEC},
    {SDLK_POWER, MP_KEY_POWER},
    {SDLK_MENU, MP_KEY_MENU},
    {SDLK_STOP, MP_KEY_STOP},
    {SDLK_MUTE, MP_KEY_MUTE},
    {SDLK_VOLUMEUP, MP_KEY_VOLUME_UP},
    {SDLK_VOLUMEDOWN, MP_KEY_VOLUME_DOWN},
    {SDLK_KP_COMMA, MP_KEY_KPDEC},
    {SDLK_AUDIONEXT, MP_KEY_NEXT},
    {SDLK_AUDIOPREV, MP_KEY_PREV},
    {SDLK_AUDIOSTOP, MP_KEY_STOP},
    {SDLK_AUDIOPLAY, MP_KEY_PLAY},
    {SDLK_AUDIOMUTE, MP_KEY_MUTE},
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
    {SDLK_F13, MP_KEY_F + 13},
    {SDLK_F14, MP_KEY_F + 14},
    {SDLK_F15, MP_KEY_F + 15},
    {SDLK_F16, MP_KEY_F + 16},
    {SDLK_F17, MP_KEY_F + 17},
    {SDLK_F18, MP_KEY_F + 18},
    {SDLK_F19, MP_KEY_F + 19},
    {SDLK_F20, MP_KEY_F + 20},
    {SDLK_F21, MP_KEY_F + 21},
    {SDLK_F22, MP_KEY_F + 22},
    {SDLK_F23, MP_KEY_F + 23},
    {SDLK_F24, MP_KEY_F + 24}
};

struct mousemap_entry {
    Uint8 sdl;
    int dmpv;
};
const struct mousemap_entry mousebtns[] = {
    {SDL_BUTTON_LEFT, MP_MBTN_LEFT},
    {SDL_BUTTON_MIDDLE, MP_MBTN_MID},
    {SDL_BUTTON_RIGHT, MP_MBTN_RIGHT},
    {SDL_BUTTON_X1, MP_MBTN_BACK},
    {SDL_BUTTON_X2, MP_MBTN_FORWARD},
};

struct gl_priv {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int renderer_index;
    SDL_RendererInfo renderer_info;
    SDL_Texture *tex;
    int tex_swapped;
    struct mp_image_params params;
    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    struct mp_osd_res osd_res;
    struct formatmap_entry osd_format;
    struct osd_bitmap_surface {
        int change_id;
        struct osd_target {
            SDL_Rect source;
            SDL_Rect dest;
            SDL_Texture *tex;
            SDL_Texture *tex2;
        } *targets;
        int num_targets;
        int targets_size;
    } osd_surfaces[MAX_OSD_PARTS];
    double osd_pts;
    Uint32 wakeup_event;
    bool screensaver_enabled;
    struct m_config_cache *opts_cache;

    // options
    bool allow_sw;
    bool switch_mode;
    bool vsync;
    bool borderless;
};

static bool lock_texture(struct vo *vo, struct mp_image *texmpi)
{
    struct gl_priv *vc = vo->priv;
    *texmpi = (struct mp_image){0};
    mp_image_set_size(texmpi, vc->params.w, vc->params.h);
    mp_image_setfmt(texmpi, vc->params.imgfmt);
    switch (texmpi->num_planes) {
    case 1:
    case 3:
        break;
    default:
        MP_ERR(vo, "Invalid plane count\n");
        return false;
    }
    void *pixels;
    int pitch;
    if (SDL_LockTexture(vc->tex, NULL, &pixels, &pitch)) {
        MP_ERR(vo, "SDL_LockTexture failed\n");
        return false;
    }
    texmpi->planes[0] = pixels;
    texmpi->stride[0] = pitch;
    if (texmpi->num_planes == 3) {
        if (vc->tex_swapped) {
            texmpi->planes[2] =
                ((Uint8 *) texmpi->planes[0] + texmpi->h * pitch);
            texmpi->stride[2] = pitch / 2;
            texmpi->planes[1] =
                ((Uint8 *) texmpi->planes[2] + (texmpi->h * pitch) / 4);
            texmpi->stride[1] = pitch / 2;
        } else {
            texmpi->planes[1] =
                ((Uint8 *) texmpi->planes[0] + texmpi->h * pitch);
            texmpi->stride[1] = pitch / 2;
            texmpi->planes[2] =
                ((Uint8 *) texmpi->planes[1] + (texmpi->h * pitch) / 4);
            texmpi->stride[2] = pitch / 2;
        }
    }
    return true;
}

static bool is_good_renderer(SDL_RendererInfo *ri,
                             const char *driver_name_wanted, bool allow_sw,
                             struct formatmap_entry *osd_format)
{
    if (driver_name_wanted && driver_name_wanted[0])
        if (strcmp(driver_name_wanted, ri->name))
            return false;

    if (!allow_sw &&
        !(ri->flags & SDL_RENDERER_ACCELERATED))
        return false;

    int i, j;
    for (i = 0; i < ri->num_texture_formats; ++i)
        for (j = 0; j < sizeof(formats) / sizeof(formats[0]); ++j)
            if (ri->texture_formats[i] == formats[j].sdl)
                if (formats[j].is_rgba) {
                    if (osd_format)
                        *osd_format = formats[j];
                    return true;
                }

    return false;
}

static void destroy_renderer(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;

    // free ALL the textures
    if (vc->tex) {
        SDL_DestroyTexture(vc->tex);
        vc->tex = NULL;
    }

    int i, j;
    for (i = 0; i < MAX_OSD_PARTS; ++i) {
        for (j = 0; j < vc->osd_surfaces[i].targets_size; ++j) {
            if (vc->osd_surfaces[i].targets[j].tex) {
                SDL_DestroyTexture(vc->osd_surfaces[i].targets[j].tex);
                vc->osd_surfaces[i].targets[j].tex = NULL;
            }
            if (vc->osd_surfaces[i].targets[j].tex2) {
                SDL_DestroyTexture(vc->osd_surfaces[i].targets[j].tex2);
                vc->osd_surfaces[i].targets[j].tex2 = NULL;
            }
        }
    }

    if (vc->renderer) {
        SDL_DestroyRenderer(vc->renderer);
        vc->renderer = NULL;
    }
}

static bool try_create_renderer(struct vo *vo, int i, const char *driver)
{
    struct gl_priv *vc = vo->priv;

    // first probe
    SDL_RendererInfo ri;
    if (SDL_GetRenderDriverInfo(i, &ri))
        return false;
    if (!is_good_renderer(&ri, driver, vc->allow_sw, NULL))
        return false;

    vc->renderer = SDL_CreateRenderer(vc->window, i, 0);
    if (!vc->renderer) {
        MP_ERR(vo, "SDL_CreateRenderer failed\n");
        return false;
    }

    if (SDL_GetRendererInfo(vc->renderer, &vc->renderer_info)) {
        MP_ERR(vo, "SDL_GetRendererInfo failed\n");
        destroy_renderer(vo);
        return false;
    }

    if (!is_good_renderer(&vc->renderer_info, NULL, vc->allow_sw,
                          &vc->osd_format)) {
        MP_ERR(vo, "Renderer '%s' does not fulfill "
                                  "requirements on this system\n",
                                  vc->renderer_info.name);
        destroy_renderer(vo);
        return false;
    }

    if (vc->renderer_index != i) {
        MP_INFO(vo, "Using %s\n", vc->renderer_info.name);
        vc->renderer_index = i;
    }

    return true;
}

static int init_renderer(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;

    int n = SDL_GetNumRenderDrivers();
    int i;

    if (vc->renderer_index >= 0)
        if (try_create_renderer(vo, vc->renderer_index, NULL))
            return 0;

    for (i = 0; i < n; ++i)
        if (try_create_renderer(vo, i, SDL_GetHint(SDL_HINT_RENDER_DRIVER)))
            return 0;

    for (i = 0; i < n; ++i)
        if (try_create_renderer(vo, i, NULL))
            return 0;

    MP_ERR(vo, "No supported renderer\n");
    return -1;
}

static void resize(struct vo *vo, int w, int h)
{
    struct gl_priv *vc = vo->priv;
    vo->dwidth = w;
    vo->dheight = h;
    vo_get_src_dst_rects(vo, &vc->src_rect, &vc->dst_rect,
                         &vc->osd_res);
    SDL_RenderSetLogicalSize(vc->renderer, w, h);
    vo->want_redraw = true;
    vo_wakeup(vo);
}

static void force_resize(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    int w, h;
    SDL_GetWindowSize(vc->window, &w, &h);
    resize(vo, w, h);
}

static void check_resize(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    int w, h;
    SDL_GetWindowSize(vc->window, &w, &h);
    if (vo->dwidth != w || vo->dheight != h)
        resize(vo, w, h);
}

static inline void set_screensaver(bool enabled)
{
    if (!!enabled == !!SDL_IsScreenSaverEnabled())
        return;

    if (enabled)
        SDL_EnableScreenSaver();
    else
        SDL_DisableScreenSaver();
}

static void set_fullscreen(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    struct mp_vo_opts *opts = vc->opts_cache->opts;
    int fs = opts->fullscreen;
    SDL_bool prev_screensaver_state = SDL_IsScreenSaverEnabled();

    Uint32 fs_flag;
    if (vc->switch_mode)
        fs_flag = SDL_WINDOW_FULLSCREEN;
    else
        fs_flag = SDL_WINDOW_FULLSCREEN_DESKTOP;

    Uint32 old_flags = SDL_GetWindowFlags(vc->window);
    int prev_fs = !!(old_flags & fs_flag);
    if (fs == prev_fs)
        return;

    Uint32 flags = 0;
    if (fs)
        flags |= fs_flag;

    if (SDL_SetWindowFullscreen(vc->window, flags)) {
        MP_ERR(vo, "SDL_SetWindowFullscreen failed\n");
        return;
    }

    // toggling fullscreen might recreate the window, so better guard for this
    set_screensaver(prev_screensaver_state);

    force_resize(vo);
}

static void update_screeninfo(struct vo *vo, struct mp_rect *screenrc)
{
    struct gl_priv *vc = vo->priv;
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(vc->window),
                                  &mode)) {
        MP_ERR(vo, "SDL_GetCurrentDisplayMode failed\n");
        return;
    }
    *screenrc = (struct mp_rect){0, 0, mode.w, mode.h};
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct gl_priv *vc = vo->priv;

    struct vo_win_geometry geo;
    struct mp_rect screenrc;

    update_screeninfo(vo, &screenrc);
    vo_calc_window_geometry(vo, &screenrc, &screenrc, false, &geo);
    vo_apply_window_geometry(vo, &geo);

    int win_w = vo->dwidth;
    int win_h = vo->dheight;

    SDL_SetWindowSize(vc->window, win_w, win_h);
    if (geo.flags & VO_WIN_FORCE_POS)
        SDL_SetWindowPosition(vc->window, geo.win.x0, geo.win.y0);

    if (vc->tex)
        SDL_DestroyTexture(vc->tex);
    Uint32 texfmt = SDL_PIXELFORMAT_UNKNOWN;
    int i, j;
    for (i = 0; i < vc->renderer_info.num_texture_formats; ++i)
        for (j = 0; j < sizeof(formats) / sizeof(formats[0]); ++j)
            if (vc->renderer_info.texture_formats[i] == formats[j].sdl)
                if (params->imgfmt == formats[j].dmpv)
                    texfmt = formats[j].sdl;
    if (texfmt == SDL_PIXELFORMAT_UNKNOWN) {
        MP_ERR(vo, "Invalid pixel format\n");
        return -1;
    }

    vc->tex_swapped = texfmt == SDL_PIXELFORMAT_YV12;
    vc->tex = SDL_CreateTexture(vc->renderer, texfmt,
                                SDL_TEXTUREACCESS_STREAMING,
                                params->w, params->h);
    if (!vc->tex) {
        MP_ERR(vo, "Could not create a texture\n");
        return -1;
    }

    vc->params = *params;

    struct mp_image tmp;
    if (!lock_texture(vo, &tmp)) {
        SDL_DestroyTexture(vc->tex);
        vc->tex = NULL;
        return -1;
    }
    mp_image_clear(&tmp, 0, 0, tmp.w, tmp.h);
    SDL_UnlockTexture(vc->tex);

    resize(vo, win_w, win_h);

    set_screensaver(vc->screensaver_enabled);
    set_fullscreen(vo);

    SDL_ShowWindow(vc->window);

    check_resize(vo);

    return 0;
}

static void flip_page(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    SDL_RenderPresent(vc->renderer);
}

static void wakeup(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    SDL_Event event = {.type = vc->wakeup_event};
    // Note that there is no context - SDL is a singleton.
    SDL_PushEvent(&event);
}

static void wait_events(struct vo *vo, int64_t until_time_ns)
{
    int64_t wait_ns = until_time_ns - mp_time_ns();
    // Round-up to 1ms for short timeouts (100us, 1000us]
    if (wait_ns > MP_TIME_US_TO_NS(100))
        wait_ns = MPMAX(wait_ns, MP_TIME_MS_TO_NS(1));
    int timeout_ms = MPCLAMP(wait_ns / MP_TIME_MS_TO_NS(1), 0, 10000);
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
                check_resize(vo);
                vo_event(vo, VO_EVENT_RESIZE);
                break;
            case SDL_WINDOWEVENT_ENTER:
                mp_input_put_key(vo->input_ctx, MP_KEY_MOUSE_ENTER);
                break;
            case SDL_WINDOWEVENT_LEAVE:
                mp_input_put_key(vo->input_ctx, MP_KEY_MOUSE_LEAVE);
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
            // we ignore KMOD_LSHIFT, KMOD_RSHIFT and KMOD_RALT (if
            // mp_input_use_alt_gr() is true) because these are already
            // factored into ev.text.text
            if (sdl_mod & (KMOD_LCTRL | KMOD_RCTRL))
                dmpv_mod |= MP_KEY_MODIFIER_CTRL;
            if ((sdl_mod & KMOD_LALT) ||
                ((sdl_mod & KMOD_RALT) && !mp_input_use_alt_gr(vo->input_ctx)))
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
            // Issue: we don't know in advance whether this keydown event
            // will ALSO cause a SDL_TEXTINPUT event
            // So we're conservative, and only map non printable keycodes
            // (e.g. function keys, arrow keys, etc.)
            // However, this does lose some keypresses at least on X11
            // (e.g. Ctrl-A generates SDL_KEYDOWN only, but the key is
            // 'a'... and 'a' is normally also handled by SDL_TEXTINPUT).
            // The default config does not use Ctrl, so this is fine...
            int keycode = 0;
            int i;
            for (i = 0; i < sizeof(gl_keys) / sizeof(gl_keys[0]); ++i)
                if (gl_keys[i].sdl == ev.key.keysym.sym) {
                    keycode = gl_keys[i].dmpv;
                    break;
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
        case SDL_MOUSEMOTION:
            mp_input_set_mouse_pos(vo->input_ctx, ev.motion.x, ev.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN: {
            int i;
            for (i = 0; i < sizeof(mousebtns) / sizeof(mousebtns[0]); ++i)
                if (mousebtns[i].sdl == ev.button.button) {
                    mp_input_put_key(vo->input_ctx, mousebtns[i].dmpv | MP_KEY_STATE_DOWN);
                    break;
                }
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            int i;
            for (i = 0; i < sizeof(mousebtns) / sizeof(mousebtns[0]); ++i)
                if (mousebtns[i].sdl == ev.button.button) {
                    mp_input_put_key(vo->input_ctx, mousebtns[i].dmpv | MP_KEY_STATE_UP);
                    break;
                }
            break;
        }
        case SDL_MOUSEWHEEL: {
#if SDL_VERSION_ATLEAST(2, 0, 4)
            double multiplier = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
#else
            double multiplier = 1;
#endif
            int y_code = ev.wheel.y > 0 ? MP_WHEEL_UP : MP_WHEEL_DOWN;
            mp_input_put_wheel(vo->input_ctx, y_code, abs(ev.wheel.y) * multiplier);
            int x_code = ev.wheel.x > 0 ? MP_WHEEL_RIGHT : MP_WHEEL_LEFT;
            mp_input_put_wheel(vo->input_ctx, x_code, abs(ev.wheel.x) * multiplier);
            break;
        }
        }
    }
}

static void uninit(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    destroy_renderer(vo);
    SDL_DestroyWindow(vc->window);
    vc->window = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    talloc_free(vc);
}

static inline void upload_to_texture(struct vo *vo, SDL_Texture *tex,
                                     int w, int h, void *bitmap, int stride)
{
    struct gl_priv *vc = vo->priv;

    if (vc->osd_format.sdl == SDL_PIXELFORMAT_ARGB8888) {
        // NOTE: this optimization is questionable, because SDL docs say
        // that this way is slow.
        // It did measure up faster, though...
        SDL_UpdateTexture(tex, NULL, bitmap, stride);
        return;
    }

    void *pixels;
    int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
        MP_ERR(vo, "Could not lock texture\n");
    } else {
        SDL_ConvertPixels(w, h, SDL_PIXELFORMAT_ARGB8888,
                          bitmap, stride,
                          vc->osd_format.sdl,
                          pixels, pitch);
        SDL_UnlockTexture(tex);
    }
}

static inline void subbitmap_to_texture(struct vo *vo, SDL_Texture *tex,
                                        struct sub_bitmap *bmp,
                                        uint32_t ormask)
{
    if (ormask == 0) {
        upload_to_texture(vo, tex, bmp->w, bmp->h,
                          bmp->bitmap, bmp->stride);
    } else {
        uint32_t *temppixels;
        temppixels = talloc_array(vo, uint32_t, bmp->w * bmp->h);

        int x, y;
        for (y = 0; y < bmp->h; ++y) {
            const uint32_t *src =
                (const uint32_t *) ((const char *) bmp->bitmap + y * bmp->stride);
            uint32_t *dst = temppixels + y * bmp->w;
            for (x = 0; x < bmp->w; ++x)
                dst[x] = src[x] | ormask;
        }

        upload_to_texture(vo, tex, bmp->w, bmp->h,
                          temppixels, sizeof(uint32_t) * bmp->w);

        talloc_free(temppixels);
    }
}

static void generate_osd_part(struct vo *vo, struct sub_bitmaps *imgs)
{
    struct gl_priv *vc = vo->priv;
    struct osd_bitmap_surface *sfc = &vc->osd_surfaces[imgs->render_index];

    if (imgs->format == SUBBITMAP_EMPTY || imgs->num_parts == 0)
        return;

    if (imgs->change_id == sfc->change_id)
        return;

    if (imgs->num_parts > sfc->targets_size) {
        sfc->targets = talloc_realloc(vc, sfc->targets,
                                      struct osd_target, imgs->num_parts);
        memset(&sfc->targets[sfc->targets_size], 0, sizeof(struct osd_target) *
               (imgs->num_parts - sfc->targets_size));
        sfc->targets_size = imgs->num_parts;
    }
    sfc->num_targets = imgs->num_parts;

    for (int i = 0; i < imgs->num_parts; i++) {
        struct osd_target *target = sfc->targets + i;
        struct sub_bitmap *bmp = imgs->parts + i;

        target->source = (SDL_Rect){
            0, 0, bmp->w, bmp->h
        };
        target->dest = (SDL_Rect){
            bmp->x, bmp->y, bmp->dw, bmp->dh
        };

        // tex: alpha blended texture
        if (target->tex) {
            SDL_DestroyTexture(target->tex);
            target->tex = NULL;
        }
        if (!target->tex)
            target->tex = SDL_CreateTexture(vc->renderer,
                    vc->osd_format.sdl, SDL_TEXTUREACCESS_STREAMING,
                    bmp->w, bmp->h);
        if (!target->tex) {
            MP_ERR(vo, "Could not create texture\n");
        }
        if (target->tex) {
            SDL_SetTextureBlendMode(target->tex,
                                    SDL_BLENDMODE_BLEND);
            SDL_SetTextureColorMod(target->tex, 0, 0, 0);
            subbitmap_to_texture(vo, target->tex, bmp, 0); // RGBA -> 000A
        }

        // tex2: added texture
        if (target->tex2) {
            SDL_DestroyTexture(target->tex2);
            target->tex2 = NULL;
        }
        if (!target->tex2)
            target->tex2 = SDL_CreateTexture(vc->renderer,
                    vc->osd_format.sdl, SDL_TEXTUREACCESS_STREAMING,
                    bmp->w, bmp->h);
        if (!target->tex2) {
            MP_ERR(vo, "Could not create texture\n");
        }
        if (target->tex2) {
            SDL_SetTextureBlendMode(target->tex2,
                                    SDL_BLENDMODE_ADD);
            subbitmap_to_texture(vo, target->tex2, bmp,
                                    0xFF000000); // RGBA -> RGB1
        }
    }

    sfc->change_id = imgs->change_id;
}

static void draw_osd_part(struct vo *vo, int index)
{
    struct gl_priv *vc = vo->priv;
    struct osd_bitmap_surface *sfc = &vc->osd_surfaces[index];
    int i;

    for (i = 0; i < sfc->num_targets; i++) {
        struct osd_target *target = sfc->targets + i;
        if (target->tex)
            SDL_RenderCopy(vc->renderer, target->tex,
                           &target->source, &target->dest);
        if (target->tex2)
            SDL_RenderCopy(vc->renderer, target->tex2,
                           &target->source, &target->dest);
    }
}

static void draw_osd_cb(void *ctx, struct sub_bitmaps *imgs)
{
    struct vo *vo = ctx;
    generate_osd_part(vo, imgs);
    draw_osd_part(vo, imgs->render_index);
}

static void draw_osd(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;

    static const bool osdformats[SUBBITMAP_COUNT] = {
        [SUBBITMAP_BGRA] = true,
    };

    osd_draw(vo->osd, vc->osd_res, vc->osd_pts, 0, osdformats, draw_osd_cb, vo);
}

static int preinit(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;

    if (SDL_WasInit(SDL_INIT_EVENTS)) {
        MP_ERR(vo, "Another component is using SDL already.\n");
        return -1;
    }

    vc->opts_cache = m_config_cache_alloc(vc, vo->global, &vo_sub_opts);

    // predefine SDL defaults (SDL env vars shall override)
    SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "1",
                            SDL_HINT_DEFAULT);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0",
                            SDL_HINT_DEFAULT);

    // predefine DMPV options (SDL env vars shall be overridden)
    SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, vc->vsync ? "1" : "0",
                            SDL_HINT_OVERRIDE);

    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        MP_ERR(vo, "SDL_Init failed\n");
        return -1;
    }

    // then actually try
    vc->window = SDL_CreateWindow("DMPV",
                                  SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  640, 480,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!vc->window) {
        MP_ERR(vo, "SDL_CreateWindow failed\n");
        return -1;
    }

    if (vc->borderless) {
        SDL_SetWindowBordered(vc->window, SDL_FALSE);
    }

    // try creating a renderer (this also gets the renderer_info data
    // for query_format to use!)
    if (init_renderer(vo) != 0) {
        SDL_DestroyWindow(vc->window);
        vc->window = NULL;
        return -1;
    }

    vc->wakeup_event = SDL_RegisterEvents(1);
    if (vc->wakeup_event == (Uint32)-1)
        MP_ERR(vo, "SDL_RegisterEvents() failed.\n");

    return 0;
}

static int query_format(struct vo *vo, int format)
{
    struct gl_priv *vc = vo->priv;
    int i, j;
    for (i = 0; i < vc->renderer_info.num_texture_formats; ++i)
        for (j = 0; j < sizeof(formats) / sizeof(formats[0]); ++j)
            if (vc->renderer_info.texture_formats[i] == formats[j].sdl)
                if (format == formats[j].dmpv)
                    return 1;
    return 0;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct gl_priv *vc = vo->priv;

    // typically this runs in parallel with the following mp_image_copy call
    SDL_SetRenderDrawColor(vc->renderer, 0, 0, 0, 255);
    SDL_RenderClear(vc->renderer);

    SDL_SetTextureBlendMode(vc->tex, SDL_BLENDMODE_NONE);

    if (frame->current) {
        vc->osd_pts = frame->current->pts;

        mp_image_t texmpi;
    if (!lock_texture(vo, &texmpi))
            return;

        mp_image_copy(&texmpi, frame->current);

        SDL_UnlockTexture(vc->tex);
    }

    SDL_Rect src, dst;
    src.x = vc->src_rect.x0;
    src.y = vc->src_rect.y0;
    src.w = vc->src_rect.x1 - vc->src_rect.x0;
    src.h = vc->src_rect.y1 - vc->src_rect.y0;
    dst.x = vc->dst_rect.x0;
    dst.y = vc->dst_rect.y0;
    dst.w = vc->dst_rect.x1 - vc->dst_rect.x0;
    dst.h = vc->dst_rect.y1 - vc->dst_rect.y0;

    {
        int out_w = 0, out_h = 0;
        if (vc->renderer) {
            if (SDL_GetRendererOutputSize(vc->renderer, &out_w, &out_h) != 0) {
                SDL_GetWindowSize(vc->window, &out_w, &out_h);
            }
        } else {
            SDL_GL_GetDrawableSize(vc->window, &out_w, &out_h);
            if (out_w == 0 || out_h == 0)
                SDL_GetWindowSize(vc->window, &out_w, &out_h);
        }

        if (out_w > 0 && out_h > 0) {
            dst.x = (out_w - dst.w) / 2;
            dst.y = (out_h - dst.h) / 2;
        }
    }

    SDL_RenderCopy(vc->renderer, vc->tex, &src, &dst);

    draw_osd(vo);
}

static struct mp_image *get_window_screenshot(struct vo *vo)
{
    struct gl_priv *vc = vo->priv;
    struct mp_image *image = mp_image_alloc(vc->osd_format.dmpv, vo->dwidth,
                                                                vo->dheight);
    if (!image)
        return NULL;
    if (SDL_RenderReadPixels(vc->renderer, NULL, vc->osd_format.sdl,
                             image->planes[0], image->stride[0])) {
        MP_ERR(vo, "SDL_RenderReadPixels failed\n");
        talloc_free(image);
        return NULL;
    }
    return image;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct gl_priv *vc = vo->priv;

    switch (request) {
    case VOCTRL_VO_OPTS_CHANGED: {
        void *opt;
        while (m_config_cache_get_next_changed(vc->opts_cache, &opt)) {
            struct mp_vo_opts *opts = vc->opts_cache->opts;
            if (&opts->fullscreen == opt)
                set_fullscreen(vo);
        }
        return 1;
    }
    case VOCTRL_SET_PANSCAN:
        force_resize(vo);
        return VO_TRUE;
    case VOCTRL_SCREENSHOT_WIN:
        *(struct mp_image **)data = get_window_screenshot(vo);
        return true;
    case VOCTRL_SET_CURSOR_VISIBILITY:
        SDL_ShowCursor(*(bool *)data);
        return true;
    case VOCTRL_KILL_SCREENSAVER:
        vc->screensaver_enabled = false;
        set_screensaver(vc->screensaver_enabled);
        return VO_TRUE;
    case VOCTRL_RESTORE_SCREENSAVER:
        vc->screensaver_enabled = true;
        set_screensaver(vc->screensaver_enabled);
        return VO_TRUE;
    case VOCTRL_UPDATE_WINDOW_TITLE:
        SDL_SetWindowTitle(vc->window, (char *)data);
        return true;
    case VOCTRL_GET_FOCUSED:
        *(bool *)data = SDL_GetWindowFlags(vc->window) & SDL_WINDOW_INPUT_FOCUS;
        return VO_TRUE;
    }
    return VO_NOTIMPL;
}

#undef OPT_BASE_STRUCT
#define OPT_BASE_STRUCT struct gl_priv

static const struct vo_driver gl_driver = {
    .description = "SDL 2.0 Renderer",
    .name = "sdl",
    .priv_size = sizeof(struct gl_priv),
    .priv_defaults = &(const struct gl_priv) {
        .renderer_index = -1,
        .vsync = true,
        .allow_sw = true,
        .borderless = true,
    },
    .options = (const struct m_option []){
        {"sw", OPT_BOOL(allow_sw)},
        {"switch-mode", OPT_BOOL(switch_mode)},
        {"vsync", OPT_BOOL(vsync)},
        {"borderless", OPT_BOOL(borderless)},
        {NULL}
    },
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .uninit = uninit,
    .flip_page = flip_page,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .options_prefix = "sdl",
};


#endif // HAVE_GL


// ============================================================================
// UNIFIED DRIVER - Backend selection with goto
// ============================================================================

enum sdl_backend_type {
    BACKEND_AUTO = 0,
    BACKEND_VULKAN,
    BACKEND_GL,
};

// Simple priv just to hold the backend option during parsing
struct backend_choice {
    int backend;  // Using int instead of enum for OPT_CHOICE compatibility
};

static int unified_preinit(struct vo *vo)
{
    // Parse backend option from vo->priv (set by option system)
    struct backend_choice *choice = vo->priv;
    enum sdl_backend_type requested = choice->backend;
    
    // Jump to appropriate backend based on choice
    if (requested == BACKEND_AUTO || requested == BACKEND_VULKAN) {
        goto try_vulkan;
    } else if (requested == BACKEND_GL) {
        goto try_gl;
    }
    
try_vulkan:
#if defined(HAVE_SDL_VULKAN_BACKEND) && HAVE_SDL_VULKAN_BACKEND
    {
        MP_VERBOSE(vo, "Trying Vulkan backend...\n");
        
        // Replace vo->priv with backend-specific priv
        vo->priv = talloc_zero_size(vo, vk_driver.priv_size);
        if (vk_driver.priv_defaults) {
            memcpy(vo->priv, vk_driver.priv_defaults, vk_driver.priv_size);
        }
        
        // Replace vo->driver with backend driver
        vo->driver = &vk_driver;
        
        if (vk_driver.preinit(vo) >= 0) {
            MP_VERBOSE(vo, "Vulkan backend initialized successfully\n");
            return 0;
        }
        
        MP_WARN(vo, "Vulkan backend failed to initialize\n");
        
        // Fall back to GL in auto mode
        if (requested == BACKEND_AUTO) {
            goto try_gl;
        }
    }
#else
    if (requested == BACKEND_VULKAN) {
        MP_ERR(vo, "Vulkan backend not compiled in\n");
        return -1;
    }
    if (requested == BACKEND_AUTO) {
        goto try_gl;
    }
#endif
    goto fail;
    
try_gl:
#if defined(HAVE_GL) && HAVE_GL
    {
        MP_VERBOSE(vo, "Trying OpenGL backend...\n");
        
        // Replace vo->priv with backend-specific priv
        vo->priv = talloc_zero_size(vo, gl_driver.priv_size);
        if (gl_driver.priv_defaults) {
            memcpy(vo->priv, gl_driver.priv_defaults, gl_driver.priv_size);
        }
        
        // Replace vo->driver with backend driver
        vo->driver = &gl_driver;
        
        if (gl_driver.preinit(vo) >= 0) {
            MP_VERBOSE(vo, "OpenGL backend initialized successfully\n");
            return 0;
        }
        
        MP_WARN(vo, "OpenGL backend failed to initialize\n");
    }
#else
    if (requested == BACKEND_GL) {
        MP_ERR(vo, "OpenGL backend not compiled in\n");
    }
#endif
    
fail:
    MP_ERR(vo, "All SDL backends failed to initialize\n");
    return -1;
}

#undef OPT_BASE_STRUCT
#define OPT_BASE_STRUCT struct backend_choice

const struct vo_driver video_out_sdl = {
    .description = "SDL video output (Vulkan or OpenGL backend)",
    .name = "sdl",
    .priv_size = sizeof(struct backend_choice),
    .priv_defaults = &(const struct backend_choice) {
        .backend = BACKEND_AUTO,
    },
    .options = (const struct m_option[]) {
        {"backend", OPT_CHOICE(backend,
            {"auto", BACKEND_AUTO},
            {"vulkan", BACKEND_VULKAN},
            {"gl", BACKEND_GL})},
        {0}
    },
    .preinit = unified_preinit,
    .options_prefix = "sdl",
};
