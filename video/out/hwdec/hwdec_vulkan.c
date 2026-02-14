/*
 * Copyright (c) 2022 Philip Langdale <philipl@overt.org>
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

#include "config.h"
#include "osdep/threads.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/vulkan/context.h"
#include "video/out/placebo/ra_pl.h"

#include <string.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>

struct vulkan_hw_priv {
    struct mp_hwdec_ctx hwctx;
    pl_gpu gpu;
    struct queue_lock_ctx *lock_ctx;
};

struct vulkan_mapper_priv {
    struct mp_image layout;
    AVVkFrame *vkf;
    pl_tex tex[4];
    VkImage img[4];
    VkImageLayout img_layout[4];
    VkSemaphore sem[4];
    uint64_t sem_value[4];
    int num_images;
};

struct queue_lock_ctx {
    pl_vulkan vulkan;
    pthread_mutex_t mutex;
};

static void lock_queue(struct AVHWDeviceContext *ctx,
                       uint32_t queue_family, uint32_t index)
{
    struct queue_lock_ctx *lock_ctx = ctx->user_opaque;

    mp_mutex_lock(&lock_ctx->mutex);
    lock_ctx->vulkan->lock_queue(lock_ctx->vulkan, queue_family, index);
}

static void unlock_queue(struct AVHWDeviceContext *ctx,
                         uint32_t queue_family, uint32_t index)
{
    struct queue_lock_ctx *lock_ctx = ctx->user_opaque;

    lock_ctx->vulkan->unlock_queue(lock_ctx->vulkan, queue_family, index);
    mp_mutex_unlock(&lock_ctx->mutex);
}

static int vulkan_init(struct ra_hwdec *hw)
{
    AVBufferRef *hw_device_ctx = NULL;
    int ret = 0;
    struct vulkan_hw_priv *p = hw->priv;
    int level = hw->probing ? MSGL_V : MSGL_ERR;

    MP_VERBOSE(hw, "Vulkan: Initializing hardware decode support\n");

    struct dmpvk_ctx *vk = ra_vk_ctx_get(hw->ra_ctx);
    if (!vk) {
        MP_VERBOSE(hw, "This is not a libplacebo Vulkan GPU API context.\n");
        return 0;
    }

    p->gpu = ra_pl_get(hw->ra_ctx->ra);
    if (!p->gpu) {
        MP_MSG(hw, level, "Failed to obtain pl_gpu.\n");
        return 0;
    }

    MP_VERBOSE(hw, "Vulkan: Got pl_gpu handle, checking extensions\n");

    /*
     * Check if the required video decode extensions are enabled.
     * FFmpeg will fail with cryptic errors if they're not available.
     */
    bool has_video_decode_queue = false;
    MP_VERBOSE(hw, "Vulkan: Checking for video decode extensions (%d total extensions)\n",
               vk->vulkan->num_extensions);
    for (int i = 0; i < vk->vulkan->num_extensions; i++) {
        if (strcmp(vk->vulkan->extensions[i], "VK_KHR_video_decode_queue") == 0) {
            has_video_decode_queue = true;
            MP_VERBOSE(hw, "Vulkan: Found VK_KHR_video_decode_queue extension\n");
            break;
        }
    }

    if (!has_video_decode_queue) {
        MP_MSG(hw, level, "Vulkan device does not have the VK_KHR_video_decode_queue extension enabled.\n");
        return 0;
    }

    /*
     * libplacebo initializes all queues, but we still need to discover which
     * one is the decode queue.
     */
    uint32_t num_qf = 0;
    VkQueueFamilyProperties2 *qf = NULL;
    VkQueueFamilyVideoPropertiesKHR *qf_vid = NULL;
    vkGetPhysicalDeviceQueueFamilyProperties2(vk->vulkan->phys_device, &num_qf, NULL);
    MP_VERBOSE(hw, "Vulkan: Found %u queue families\n", num_qf);
    if (!num_qf) {
        MP_VERBOSE(hw, "Vulkan: No queue families found\n");
        goto error;
    }

    qf = talloc_array(NULL, VkQueueFamilyProperties2, num_qf);
    qf_vid = talloc_array(NULL, VkQueueFamilyVideoPropertiesKHR, num_qf);
    for (int i = 0; i < num_qf; i++) {
        qf_vid[i] = (VkQueueFamilyVideoPropertiesKHR) {
            .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR,
        };
        qf[i] = (VkQueueFamilyProperties2) {
            .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,
            .pNext = &qf_vid[i],
        };
    }

    vkGetPhysicalDeviceQueueFamilyProperties2(vk->vulkan->phys_device, &num_qf, qf);

    // Log queue family information
    for (int i = 0; i < num_qf; i++) {
        VkQueueFlags flags = qf[i].queueFamilyProperties.queueFlags;
        MP_VERBOSE(hw, "Vulkan: Queue family %d: count=%u flags=0x%x%s%s%s%s\n",
                   i, qf[i].queueFamilyProperties.queueCount, flags,
                   (flags & VK_QUEUE_GRAPHICS_BIT) ? " GRAPHICS" : "",
                   (flags & VK_QUEUE_COMPUTE_BIT) ? " COMPUTE" : "",
                   (flags & VK_QUEUE_TRANSFER_BIT) ? " TRANSFER" : "",
                   (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ? " VIDEO_DECODE" : "");
        if (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            MP_VERBOSE(hw, "Vulkan:   Video codec ops=0x%x\n", qf_vid[i].videoCodecOperations);
        }
    }

    hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!hw_device_ctx) {
        MP_VERBOSE(hw, "Vulkan: Failed to allocate AVHWDeviceContext\n");
        goto error;
    }

    MP_VERBOSE(hw, "Vulkan: Allocated AVHWDeviceContext\n");

    AVHWDeviceContext *device_ctx = (void *)hw_device_ctx->data;
    AVVulkanDeviceContext *device_hwctx = device_ctx->hwctx;

    p->lock_ctx = talloc_zero(hw, struct queue_lock_ctx);
    if (!p->lock_ctx) {
        MP_MSG(hw, level, "Failed to allocate queue lock context!\n");
        goto error;
    }
    p->lock_ctx->vulkan = vk->vulkan;
    pthread_mutex_init(&p->lock_ctx->mutex, NULL);

    device_ctx->user_opaque = (void *)p->lock_ctx;
    device_hwctx->lock_queue = lock_queue;
    device_hwctx->unlock_queue = unlock_queue;
    device_hwctx->get_proc_addr = vk->vkinst->get_proc_addr;
    device_hwctx->inst = vk->vkinst->instance;
    device_hwctx->phys_dev = vk->vulkan->phys_device;
    device_hwctx->act_dev = vk->vulkan->device;
    device_hwctx->device_features = *vk->vulkan->features;
    device_hwctx->enabled_inst_extensions = vk->vkinst->extensions;
    device_hwctx->nb_enabled_inst_extensions = vk->vkinst->num_extensions;
    device_hwctx->enabled_dev_extensions = vk->vulkan->extensions;
    device_hwctx->nb_enabled_dev_extensions = vk->vulkan->num_extensions;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    MP_VERBOSE(hw, "Vulkan: Using new queue family API (FFmpeg >= 59.34.100)\n");
    device_hwctx->nb_qf = 0;
    device_hwctx->qf[device_hwctx->nb_qf++] = (AVVulkanDeviceQueueFamily) {
        .idx = vk->vulkan->queue_graphics.index,
        .num = vk->vulkan->queue_graphics.count,
        .flags = VK_QUEUE_GRAPHICS_BIT,
    };
    MP_VERBOSE(hw, "Vulkan: Graphics queue: family=%d count=%d\n",
               vk->vulkan->queue_graphics.index, vk->vulkan->queue_graphics.count);
    device_hwctx->qf[device_hwctx->nb_qf++] = (AVVulkanDeviceQueueFamily) {
        .idx = vk->vulkan->queue_transfer.index,
        .num = vk->vulkan->queue_transfer.count,
        .flags = VK_QUEUE_TRANSFER_BIT,
    };
    MP_VERBOSE(hw, "Vulkan: Transfer queue: family=%d count=%d\n",
               vk->vulkan->queue_transfer.index, vk->vulkan->queue_transfer.count);
    device_hwctx->qf[device_hwctx->nb_qf++] = (AVVulkanDeviceQueueFamily) {
        .idx = vk->vulkan->queue_compute.index,
        .num = vk->vulkan->queue_compute.count,
        .flags = VK_QUEUE_COMPUTE_BIT,
    };
    MP_VERBOSE(hw, "Vulkan: Compute queue: family=%d count=%d\n",
               vk->vulkan->queue_compute.index, vk->vulkan->queue_compute.count);
    for (int i = 0; i < num_qf; i++) {
        if ((qf[i].queueFamilyProperties.queueFlags) & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            device_hwctx->qf[device_hwctx->nb_qf++] = (AVVulkanDeviceQueueFamily) {
                .idx = i,
                .num = qf[i].queueFamilyProperties.queueCount,
                .flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR,
                .video_caps = qf_vid[i].videoCodecOperations,
            };
            MP_VERBOSE(hw, "Vulkan: Video decode queue: family=%d count=%d caps=0x%x\n",
                       i, qf[i].queueFamilyProperties.queueCount,
                       qf_vid[i].videoCodecOperations);
        }
    }
    MP_VERBOSE(hw, "Vulkan: Configured %d queue families for FFmpeg\n", device_hwctx->nb_qf);
#else
    MP_VERBOSE(hw, "Vulkan: Using legacy queue family API (FFmpeg < 59.34.100)\n");
    int decode_index = -1;
    for (int i = 0; i < num_qf; i++) {
        if ((qf[i].queueFamilyProperties.queueFlags) & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            decode_index = i;
            MP_VERBOSE(hw, "Vulkan: Found decode queue at family %d\n", i);
        }
    }
    device_hwctx->queue_family_index = vk->vulkan->queue_graphics.index;
    device_hwctx->nb_graphics_queues = vk->vulkan->queue_graphics.count;
    MP_VERBOSE(hw, "Vulkan: Graphics: family=%d count=%d\n",
               device_hwctx->queue_family_index, device_hwctx->nb_graphics_queues);
    device_hwctx->queue_family_tx_index = vk->vulkan->queue_transfer.index;
    device_hwctx->nb_tx_queues = vk->vulkan->queue_transfer.count;
    MP_VERBOSE(hw, "Vulkan: Transfer: family=%d count=%d\n",
               device_hwctx->queue_family_tx_index, device_hwctx->nb_tx_queues);
    device_hwctx->queue_family_comp_index = vk->vulkan->queue_compute.index;
    device_hwctx->nb_comp_queues = vk->vulkan->queue_compute.count;
    MP_VERBOSE(hw, "Vulkan: Compute: family=%d count=%d\n",
               device_hwctx->queue_family_comp_index, device_hwctx->nb_comp_queues);
    device_hwctx->queue_family_decode_index = decode_index;
    device_hwctx->nb_decode_queues = decode_index >= 0 ? qf[decode_index].queueFamilyProperties.queueCount : 0;
    MP_VERBOSE(hw, "Vulkan: Decode: family=%d count=%d\n",
               device_hwctx->queue_family_decode_index, device_hwctx->nb_decode_queues);
#endif

    MP_VERBOSE(hw, "Vulkan: Initializing FFmpeg device context\n");
    ret = av_hwdevice_ctx_init(hw_device_ctx);
    if (ret < 0) {
        MP_MSG(hw, level, "av_hwdevice_ctx_init failed\n");
        goto error;
    }
    MP_VERBOSE(hw, "Vulkan: FFmpeg device context initialized successfully\n");

    p->hwctx = (struct mp_hwdec_ctx) {
        .driver_name = hw->driver->name,
        .av_device_ref = hw_device_ctx,
        .hw_imgfmt = IMGFMT_VULKAN,
    };
    hwdec_devices_add(hw->devs, &p->hwctx);

    talloc_free(qf);
    talloc_free(qf_vid);
    return 0;

 error:
    talloc_free(qf);
    talloc_free(qf_vid);
    av_buffer_unref(&hw_device_ctx);
    return -1;
}

static void vulkan_uninit(struct ra_hwdec *hw)
{
    struct vulkan_hw_priv *p = hw->priv;

    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);

    if (p->lock_ctx) {
        mp_mutex_destroy(&p->lock_ctx->mutex);
        talloc_free(p->lock_ctx);
        p->lock_ctx = NULL;
    }
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct vulkan_mapper_priv *p = mapper->priv;

    MP_VERBOSE(mapper, "Vulkan: Initializing mapper\n");

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    mp_image_set_params(&p->layout, &mapper->dst_params);

    MP_VERBOSE(mapper, "Vulkan: Mapper dst format=%s size=%dx%d\n",
               mp_imgfmt_to_name(mapper->dst_params.imgfmt),
               mapper->dst_params.w, mapper->dst_params.h);

    struct ra_imgfmt_desc desc = {0};
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc)) {
        MP_VERBOSE(mapper, "Vulkan: Failed to get image format descriptor\n");
        return -1;
    }

    MP_VERBOSE(mapper, "Vulkan: Mapper initialized with %d planes\n", desc.num_planes);

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{

}

static void mapper_unmap(struct ra_hwdec_mapper *mapper)
{
    struct vulkan_hw_priv *p_owner = mapper->owner->priv;
    struct vulkan_mapper_priv *p = mapper->priv;
    if (!mapper->src)
        goto end;

    AVHWFramesContext *hwfc = (AVHWFramesContext *) mapper->src->hwctx->data;;
    const AVVulkanFramesContext *vkfc = hwfc->hwctx;
    AVVkFrame *vkf = p->vkf;

    int num_images = p->num_images;

    VkImageLayout new_layout[4] = {0};
    uint64_t reserved_sem_value[4] = {0};
    bool ok[4] = {false};
    bool will_process[4] = {false};

    for (int i = 0; (p->tex[i] != NULL); i++) {
        if (!p->tex[i])
            continue;

        int index = p->layout.num_planes > 1 && num_images == 1 ? 0 : i;
        will_process[index] = true;
    }

    vkfc->lock_frame(hwfc, vkf);
    for (int i = 0; i < num_images && i < 4; i++) {
        if (will_process[i]) {
            reserved_sem_value[i] = ++vkf->sem_value[i];
        }
    }
    vkfc->unlock_frame(hwfc, vkf);

    bool processed[4] = {false};
    for (int i = 0; (p->tex[i] != NULL); i++) {
        pl_tex *tex = &p->tex[i];
        if (!*tex)
            continue;

        // If we have multiple planes and one image, then that is a multiplane
        // frame. Anything else is treated as one-image-per-plane.
        int index = p->layout.num_planes > 1 && num_images == 1 ? 0 : i;

        if (!processed[index]) {
            ok[index] = pl_vulkan_hold_ex(p_owner->gpu, pl_vulkan_hold_params(
                .tex = *tex,
                .out_layout = &new_layout[index],
                .qf = VK_QUEUE_FAMILY_IGNORED,
                .semaphore = (pl_vulkan_sem) {
                    .sem = p->sem[index],
                    .value = reserved_sem_value[index],
                },
            ));

            processed[index] = true;
        }
        *tex = NULL;
    }

    vkfc->lock_frame(hwfc, vkf);
    for (int i = 0; i < num_images && i < 4; i++) {
        if (will_process[i]) {
            if (ok[i]) {
                vkf->layout[i] = new_layout[i];
            } else {
                vkf->sem_value[i] = reserved_sem_value[i] - 1;
            }
        }
        vkf->access[i] = 0;
    }
    vkfc->unlock_frame(hwfc, vkf);

 end:
    for (int i = 0; i < p->layout.num_planes; i++)
        ra_tex_free(mapper->ra, &mapper->tex[i]);

    p->vkf = NULL;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    bool result = false;
    struct vulkan_hw_priv *p_owner = mapper->owner->priv;
    struct vulkan_mapper_priv *p = mapper->priv;
    pl_vulkan vk = pl_vulkan_get(p_owner->gpu);
    if (!vk)
        return -1;

    AVHWFramesContext *hwfc = (AVHWFramesContext *) mapper->src->hwctx->data;
    const AVVulkanFramesContext *vkfc = hwfc->hwctx;
    AVVkFrame *vkf = (AVVkFrame *) mapper->src->planes[0];

    /*
     * We need to use the dimensions from the HW Frames Context for the
     * textures, as the underlying images may be larger than the logical frame
     * size. This most often happens with 1080p content where the actual frame
     * height is 1088.
     */
    struct mp_image raw_layout;
    mp_image_setfmt(&raw_layout, p->layout.params.imgfmt);
    mp_image_set_size(&raw_layout, hwfc->width, hwfc->height);

    int num_images;
    const VkFormat *vk_fmt = av_vkfmt_from_pixfmt(hwfc->sw_format);

    vkfc->lock_frame(hwfc, vkf);
    for (num_images = 0; num_images < 4 && (vkf->img[num_images] != VK_NULL_HANDLE); num_images++);
    for (int i = 0; i < num_images && i < 4; i++) {
        p->img[i] = vkf->img[i];
        p->img_layout[i] = vkf->layout[i];
        p->sem[i] = vkf->sem[i];
        p->sem_value[i] = vkf->sem_value[i];
    }
    p->num_images = num_images;
    vkfc->unlock_frame(hwfc, vkf);

    for (int i = 0; i < p->layout.num_planes; i++) {
        pl_tex *tex = &p->tex[i];
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        int index = i;

        // If we have multiple planes and one image, then that is a multiplane
        // frame. Anything else is treated as one-image-per-plane.
        if (p->layout.num_planes > 1 && num_images == 1) {
            index = 0;

            switch (i) {
            case 0:
                aspect = VK_IMAGE_ASPECT_PLANE_0_BIT_KHR;
                break;
            case 1:
                aspect = VK_IMAGE_ASPECT_PLANE_1_BIT_KHR;
                break;
            case 2:
                aspect = VK_IMAGE_ASPECT_PLANE_2_BIT_KHR;
                break;
            default:
                goto error;
            }
        }

        *tex = pl_vulkan_wrap(p_owner->gpu, pl_vulkan_wrap_params(
            .image = p->img[index],
            .width = mp_image_plane_w(&raw_layout, i),
            .height = mp_image_plane_h(&raw_layout, i),
            .format = vk_fmt[i],
            .usage = vkfc->usage,
            .aspect = aspect,
        ));
        if (!*tex)
            goto error;

        pl_vulkan_release_ex(p_owner->gpu, pl_vulkan_release_params(
            .tex = p->tex[i],
            .layout = p->img_layout[index],
            .qf = VK_QUEUE_FAMILY_IGNORED,
            .semaphore = (pl_vulkan_sem) {
                .sem = p->sem[index],
                .value = p->sem_value[index],
            },
        ));

        struct ra_tex *ratex = talloc_ptrtype(NULL, ratex);
        result = mppl_wrap_tex(mapper->ra, *tex, ratex);
        if (!result) {
            pl_tex_destroy(p_owner->gpu, tex);
            talloc_free(ratex);
            goto error;
        }
        mapper->tex[i] = ratex;
    }

    p->vkf = vkf;
    return 0;

 error:
    mapper_unmap(mapper);
    return -1;
}

const struct ra_hwdec_driver ra_hwdec_vulkan = {
    .name = "vulkan",
    .imgfmts = {IMGFMT_VULKAN, 0},
    .priv_size = sizeof(struct vulkan_hw_priv),
    .init = vulkan_init,
    .uninit = vulkan_uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct vulkan_mapper_priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
        .unmap = mapper_unmap,
    },
};
