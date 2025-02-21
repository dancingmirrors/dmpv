#pragma once

#include "video/out/gpu/context.h"
#include "common.h"

// Helpers for ra_ctx based on ra_vk. These initialize ctx->ra and ctx->swchain.
void ra_vk_ctx_uninit(struct ra_ctx *ctx);
bool ra_vk_ctx_init(struct ra_ctx *ctx, struct dmpvk_ctx *vk,
                    struct ra_ctx_params params,
                    VkPresentModeKHR preferred_mode);

// Handles a resize request, and updates ctx->vo->dwidth/dheight
bool ra_vk_ctx_resize(struct ra_ctx *ctx, int width, int height);

// May be called on a ra_ctx of any type.
struct dmpvk_ctx *ra_vk_ctx_get(struct ra_ctx *ctx);

// Get the user requested Vulkan device name.
char *ra_vk_ctx_get_device_name(struct ra_ctx *ctx);
