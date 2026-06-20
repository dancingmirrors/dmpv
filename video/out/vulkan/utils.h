#pragma once
#include "common.h"
#include "video/out/gpu/context.h"

bool dmpvk_init(struct dmpvk_ctx *vk, struct ra_ctx *ctx, const char *surface_ext);
void dmpvk_uninit(struct dmpvk_ctx *vk);
