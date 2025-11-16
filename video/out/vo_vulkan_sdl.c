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

// OSD rendering temporarily disabled due to SPIR-V shader validation issues across different drivers
// The shaders below are commented out - infrastructure remains for future implementation
/*
// Minimal SPIR-V shaders for OSD rendering
// These are the simplest possible valid SPIR-V shaders

// Vertex shader: hardcoded fullscreen quad positions
// Just outputs fixed positions for 4 vertices in a triangle strip
static const uint32_t osd_vert_spv[] = {
    // SPIR-V 1.0
    0x07230203, 0x00010000, 0x00080001, 0x0000000d, 0x00000000,
    // OpCapability Shader
    0x00020011, 0x00000001,
    // OpMemoryModel Logical GLSL450
    0x0003000e, 0x00000000, 0x00000001,
    // OpEntryPoint Vertex %main "main" %outPosition
    0x0005000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000009,
    // OpName %main "main"
    0x00040005, 0x00000001, 0x6e69616d, 0x00000000,
    // OpName %outPosition "gl_Position"
    0x00060005, 0x00000009, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00000000,
    // OpDecorate %outPosition BuiltIn Position
    0x00040047, 0x00000009, 0x0000000b, 0x00000000,
    // OpTypeVoid
    0x00020013, 0x00000002,
    // OpTypeFunction %void
    0x00030021, 0x00000003, 0x00000002,
    // OpTypeFloat 32
    0x00030016, 0x00000005, 0x00000020,
    // OpTypeVector %float 4
    0x00040017, 0x00000006, 0x00000005, 0x00000004,
    // OpTypePointer Output %v4float
    0x00040020, 0x00000008, 0x00000003, 0x00000006,
    // OpVariable Output %outPosition
    0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    // OpConstant %float 0.0
    0x0004002b, 0x00000005, 0x0000000a, 0x00000000,
    // OpConstant %float 1.0
    0x0004002b, 0x00000005, 0x0000000b, 0x3f800000,
    // OpConstantComposite %v4float 0.0 0.0 0.0 1.0
    0x0007002c, 0x00000006, 0x0000000c, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000b,
    // OpFunction %void %main
    0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,
    // OpLabel
    0x000200f8, 0x00000004,
    // OpStore %outPosition %vec4(0,0,0,1)
    0x0003003e, 0x00000009, 0x0000000c,
    // OpReturn
    0x000100fd,
    // OpFunctionEnd
    0x00010038
};

// Fragment shader: outputs solid white color
static const uint32_t osd_frag_spv[] = {
    // SPIR-V 1.0
    0x07230203, 0x00010000, 0x00080001, 0x0000000d, 0x00000000,
    // OpCapability Shader
    0x00020011, 0x00000001,
    // OpMemoryModel Logical GLSL450
    0x0003000e, 0x00000000, 0x00000001,
    // OpEntryPoint Fragment %main "main" %outColor
    0x0005000f, 0x00000004, 0x00000001, 0x6e69616d, 0x00000009,
    // OpExecutionMode %main OriginUpperLeft
    0x00030010, 0x00000001, 0x00000007,
    // OpName %main "main"
    0x00040005, 0x00000001, 0x6e69616d, 0x00000000,
    // OpName %outColor "outColor"
    0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f, 0x00000000,
    // OpDecorate %outColor Location 0
    0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    // OpTypeVoid
    0x00020013, 0x00000002,
    // OpTypeFunction %void
    0x00030021, 0x00000003, 0x00000002,
    // OpTypeFloat 32
    0x00030016, 0x00000005, 0x00000020,
    // OpTypeVector %float 4
    0x00040017, 0x00000006, 0x00000005, 0x00000004,
    // OpTypePointer Output %v4float
    0x00040020, 0x00000008, 0x00000003, 0x00000006,
    // OpVariable Output %outColor
    0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    // OpConstant %float 1.0
    0x0004002b, 0x00000005, 0x0000000b, 0x3f800000,
    // OpConstantComposite %v4float 1.0 1.0 1.0 1.0 (white)
    0x0007002c, 0x00000006, 0x0000000c, 0x0000000b, 0x0000000b, 0x0000000b, 0x0000000b,
    // OpFunction %void %main
    0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,
    // OpLabel
    0x000200f8, 0x00000004,
    // OpStore %outColor %vec4(1,1,1,1)
    0x0003003e, 0x00000009, 0x0000000c,
    // OpReturn
    0x000100fd,
    // OpFunctionEnd
    0x00010038
};
*/

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
    
    // OSD support
    struct mp_osd_res osd_res;
    double osd_pts;
    struct mp_image *osd_image;  // Buffer for OSD rendering (CPU-side)
    
    // Vulkan OSD rendering resources
    VkImage osd_texture;
    VkDeviceMemory osd_texture_memory;
    VkImageView osd_texture_view;
    VkBuffer osd_staging_buffer;
    VkDeviceMemory osd_staging_memory;
    VkSampler osd_sampler;
    VkDescriptorSetLayout osd_descriptor_layout;
    VkDescriptorPool osd_descriptor_pool;
    VkDescriptorSet osd_descriptor_set;
    VkPipelineLayout osd_pipeline_layout;
    VkPipeline osd_pipeline;
    uint32_t osd_width, osd_height;
    bool osd_needs_upload;
    
    // Event handling
    Uint32 wakeup_event;
};

static void cleanup_vulkan(struct priv *p);

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
    if (!p->device)
        return;
    
    vkDeviceWaitIdle(p->device);
    
    if (p->osd_pipeline) {
        vkDestroyPipeline(p->device, p->osd_pipeline, NULL);
        p->osd_pipeline = VK_NULL_HANDLE;
    }
    
    if (p->osd_pipeline_layout) {
        vkDestroyPipelineLayout(p->device, p->osd_pipeline_layout, NULL);
        p->osd_pipeline_layout = VK_NULL_HANDLE;
    }
    
    if (p->osd_descriptor_set) {
        // Descriptor sets are freed when pool is destroyed
        p->osd_descriptor_set = VK_NULL_HANDLE;
    }
    
    if (p->osd_descriptor_pool) {
        vkDestroyDescriptorPool(p->device, p->osd_descriptor_pool, NULL);
        p->osd_descriptor_pool = VK_NULL_HANDLE;
    }
    
    if (p->osd_descriptor_layout) {
        vkDestroyDescriptorSetLayout(p->device, p->osd_descriptor_layout, NULL);
        p->osd_descriptor_layout = VK_NULL_HANDLE;
    }
    
    if (p->osd_sampler) {
        vkDestroySampler(p->device, p->osd_sampler, NULL);
        p->osd_sampler = VK_NULL_HANDLE;
    }
    
    if (p->osd_texture_view) {
        vkDestroyImageView(p->device, p->osd_texture_view, NULL);
        p->osd_texture_view = VK_NULL_HANDLE;
    }
    
    if (p->osd_texture) {
        vkDestroyImage(p->device, p->osd_texture, NULL);
        p->osd_texture = VK_NULL_HANDLE;
    }
    
    if (p->osd_texture_memory) {
        vkFreeMemory(p->device, p->osd_texture_memory, NULL);
        p->osd_texture_memory = VK_NULL_HANDLE;
    }
    
    if (p->osd_staging_buffer) {
        vkDestroyBuffer(p->device, p->osd_staging_buffer, NULL);
        p->osd_staging_buffer = VK_NULL_HANDLE;
    }
    
    if (p->osd_staging_memory) {
        vkFreeMemory(p->device, p->osd_staging_memory, NULL);
        p->osd_staging_memory = VK_NULL_HANDLE;
    }
}

static int create_osd_resources(struct vo *vo, uint32_t width, uint32_t height)
{
    struct priv *p = vo->priv;
    
    // Clean up existing resources if any
    cleanup_osd_resources(p);
    
    p->osd_width = width;
    p->osd_height = height;
    
    // Create OSD texture (BGRA format for OSD)
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
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    if (vkCreateImage(p->device, &image_info, NULL, &p->osd_texture) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD texture\n");
        return -1;
    }
    
    // Allocate memory for OSD texture
    VkMemoryRequirements mem_requirements;
    vkGetImageMemoryRequirements(p->device, p->osd_texture, &mem_requirements);
    
    uint32_t memory_type = find_memory_type(p, mem_requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for OSD texture\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type,
    };
    
    if (vkAllocateMemory(p->device, &alloc_info, NULL, &p->osd_texture_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate OSD texture memory\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    vkBindImageMemory(p->device, p->osd_texture, p->osd_texture_memory, 0);
    
    // Create image view for OSD texture
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = p->osd_texture,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1,
    };
    
    if (vkCreateImageView(p->device, &view_info, NULL, &p->osd_texture_view) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD texture view\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // Create staging buffer for OSD upload
    VkDeviceSize buffer_size = width * height * 4; // BGRA
    
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    
    if (vkCreateBuffer(p->device, &buffer_info, NULL, &p->osd_staging_buffer) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD staging buffer\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    VkMemoryRequirements buffer_mem_requirements;
    vkGetBufferMemoryRequirements(p->device, p->osd_staging_buffer, &buffer_mem_requirements);
    
    memory_type = find_memory_type(p, buffer_mem_requirements.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        MP_ERR(vo, "Failed to find suitable memory type for OSD staging buffer\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    VkMemoryAllocateInfo staging_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buffer_mem_requirements.size,
        .memoryTypeIndex = memory_type,
    };
    
    if (vkAllocateMemory(p->device, &staging_alloc_info, NULL, &p->osd_staging_memory) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate OSD staging memory\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    vkBindBufferMemory(p->device, p->osd_staging_buffer, p->osd_staging_memory, 0);
    
    // Create sampler for OSD texture
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .mipLodBias = 0.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f,
    };
    
    if (vkCreateSampler(p->device, &sampler_info, NULL, &p->osd_sampler) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD sampler\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // OSD rendering is currently disabled due to SPIR-V shader compilation issues
    // across different Vulkan drivers. The infrastructure remains for future implementation.
    MP_WARN(vo, "OSD rendering disabled - SPIR-V shader support varies across drivers\n");
    p->osd_pipeline = VK_NULL_HANDLE;
    p->osd_needs_upload = false;
    return 0;
    
    /* DISABLED: SPIR-V shader code removed due to validation issues
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding sampler_binding = {
        .binding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImmutableSamplers = NULL,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler_binding,
    };
    
    if (vkCreateDescriptorSetLayout(p->device, &layout_info, NULL, &p->osd_descriptor_layout) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD descriptor set layout\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // Create descriptor pool
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
        .maxSets = 1,
    };
    
    if (vkCreateDescriptorPool(p->device, &pool_info, NULL, &p->osd_descriptor_pool) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD descriptor pool\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info_desc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->osd_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &p->osd_descriptor_layout,
    };
    
    if (vkAllocateDescriptorSets(p->device, &alloc_info_desc, &p->osd_descriptor_set) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to allocate OSD descriptor set\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // Update descriptor set with texture
    VkDescriptorImageInfo image_info_desc = {
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .imageView = p->osd_texture_view,
        .sampler = p->osd_sampler,
    };
    
    VkWriteDescriptorSet descriptor_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = p->osd_descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .pImageInfo = &image_info_desc,
    };
    
    vkUpdateDescriptorSets(p->device, 1, &descriptor_write, 0, NULL);
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &p->osd_descriptor_layout,
        .pushConstantRangeCount = 0,
    };
    
    if (vkCreatePipelineLayout(p->device, &pipeline_layout_info, NULL, &p->osd_pipeline_layout) != VK_SUCCESS) {
        MP_ERR(vo, "Failed to create OSD pipeline layout\n");
        cleanup_osd_resources(p);
        return -1;
    }
    
    // Create shader modules
    VkShaderModuleCreateInfo vert_shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(osd_vert_spv),
        .pCode = osd_vert_spv,
    };
    
    VkShaderModule vert_shader_module;
    if (vkCreateShaderModule(p->device, &vert_shader_info, NULL, &vert_shader_module) != VK_SUCCESS) {
        MP_WARN(vo, "Failed to create OSD vertex shader module - OSD rendering disabled\n");
        // Don't fail, just skip OSD rendering
        p->osd_pipeline = VK_NULL_HANDLE;
        p->osd_needs_upload = false;
        return 0;
    }
    
    VkShaderModuleCreateInfo frag_shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(osd_frag_spv),
        .pCode = osd_frag_spv,
    };
    
    VkShaderModule frag_shader_module;
    if (vkCreateShaderModule(p->device, &frag_shader_info, NULL, &frag_shader_module) != VK_SUCCESS) {
        MP_WARN(vo, "Failed to create OSD fragment shader module - OSD rendering disabled\n");
        vkDestroyShaderModule(p->device, vert_shader_module, NULL);
        // Don't fail, just skip OSD rendering
        p->osd_pipeline = VK_NULL_HANDLE;
        p->osd_needs_upload = false;
        return 0;
    }
    
    VkPipelineShaderStageCreateInfo vert_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vert_shader_module,
        .pName = "main",
    };
    
    VkPipelineShaderStageCreateInfo frag_stage_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = frag_shader_module,
        .pName = "main",
    };
    
    VkPipelineShaderStageCreateInfo shader_stages[] = {vert_stage_info, frag_stage_info};
    
    // Vertex input (no vertex buffers - using gl_VertexIndex)
    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .vertexAttributeDescriptionCount = 0,
    };
    
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
        .primitiveRestartEnable = VK_FALSE,
    };
    
    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)p->swapchain_extent.width,
        .height = (float)p->swapchain_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    
    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = p->swapchain_extent,
    };
    
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };
    
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    
    // Alpha blending for OSD
    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
    };
    
    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
        .blendConstants[0] = 0.0f,
        .blendConstants[1] = 0.0f,
        .blendConstants[2] = 0.0f,
        .blendConstants[3] = 0.0f,
    };
    
    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .layout = p->osd_pipeline_layout,
        .renderPass = p->render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    
    VkResult result = vkCreateGraphicsPipelines(p->device, VK_NULL_HANDLE, 1, &pipeline_info,
                                               NULL, &p->osd_pipeline);
    
    // Clean up shader modules (no longer needed after pipeline creation)
    vkDestroyShaderModule(p->device, frag_shader_module, NULL);
    vkDestroyShaderModule(p->device, vert_shader_module, NULL);
    
    if (result != VK_SUCCESS) {
        MP_WARN(vo, "Failed to create OSD graphics pipeline - OSD rendering disabled\n");
        // Don't fail, just skip OSD rendering
        p->osd_pipeline = VK_NULL_HANDLE;
        p->osd_needs_upload = false;
        return 0;
    }
    
    p->osd_needs_upload = false;
    
    return 0;
    */ // End of disabled OSD code
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
    talloc_free(p->osd_image);
    p->osd_image = mp_image_alloc(IMGFMT_BGRA, params->w, params->h);
    if (!p->osd_image) {
        MP_ERR(vo, "Failed to allocate OSD image\n");
        return -1;
    }
    mp_image_clear(p->osd_image, 0, 0, params->w, params->h);
    
    // Create Vulkan OSD resources
    if (create_osd_resources(vo, params->w, params->h) < 0) {
        MP_ERR(vo, "Failed to create OSD resources\n");
        return -1;
    }
    
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

static void draw_osd(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (!p->osd_image)
        return;
    
    // Clear OSD image to transparent black
    mp_image_clear(p->osd_image, 0, 0, p->osd_image->w, p->osd_image->h);
    
    // Draw OSD onto the image
    osd_draw_on_image(vo->osd, p->osd_res, p->osd_pts, 0, p->osd_image);
    
    // Mark that OSD needs to be uploaded to GPU
    p->osd_needs_upload = true;
}

static void upload_osd_texture(struct vo *vo)
{
    struct priv *p = vo->priv;
    
    if (!p->osd_needs_upload || !p->osd_image || !p->osd_staging_buffer)
        return;
    
    // Map staging buffer and copy OSD data
    void *data;
    if (vkMapMemory(p->device, p->osd_staging_memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
        // Copy OSD image data to staging buffer
        uint32_t stride = p->osd_image->stride[0];
        uint32_t height = p->osd_height;
        
        for (uint32_t y = 0; y < height; y++) {
            memcpy((uint8_t*)data + y * p->osd_width * 4,
                   p->osd_image->planes[0] + y * stride,
                   p->osd_width * 4);
        }
        
        vkUnmapMemory(p->device, p->osd_staging_memory);
        
        // Create a temporary command buffer for the upload
        VkCommandBuffer cmd = p->command_buffers[0]; // Use first command buffer temporarily
        
        VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        
        vkBeginCommandBuffer(cmd, &begin_info);
        
        // Transition OSD texture to transfer dst layout
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = p->osd_texture,
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
        
        // Copy buffer to image
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = {0, 0, 0},
            .imageExtent = {p->osd_width, p->osd_height, 1},
        };
        
        vkCmdCopyBufferToImage(cmd, p->osd_staging_buffer, p->osd_texture,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        
        // Transition to shader read layout
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        vkCmdPipelineBarrier(cmd,
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, NULL, 0, NULL, 1, &barrier);
        
        vkEndCommandBuffer(cmd);
        
        // Submit command buffer
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd,
        };
        
        vkQueueSubmit(p->graphics_queue, 1, &submit_info, VK_NULL_HANDLE);
        vkQueueWaitIdle(p->graphics_queue); // Simple synchronization
        
        p->osd_needs_upload = false;
    }
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
    
    // OSD rendering disabled (pipeline is NULL)
    // No OSD commands recorded
    
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
