/* Copyright (C) 2018 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DMPV_CLIENT_API_RENDER_H_
#define DMPV_CLIENT_API_RENDER_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct dmpv_render_context dmpv_render_context;

typedef enum dmpv_render_param_type {
    DMPV_RENDER_PARAM_INVALID = 0,
    DMPV_RENDER_PARAM_API_TYPE = 1,
    DMPV_RENDER_PARAM_OPENGL_INIT_PARAMS = 2,
    DMPV_RENDER_PARAM_OPENGL_FBO = 3,
    DMPV_RENDER_PARAM_FLIP_Y = 4,
    DMPV_RENDER_PARAM_DEPTH = 5,
    DMPV_RENDER_PARAM_ICC_PROFILE = 6,
    DMPV_RENDER_PARAM_AMBIENT_LIGHT = 7,
    DMPV_RENDER_PARAM_WL_DISPLAY = 8,
    DMPV_RENDER_PARAM_ADVANCED_CONTROL = 9,
    DMPV_RENDER_PARAM_NEXT_FRAME_INFO = 10,
    DMPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME = 11,
    DMPV_RENDER_PARAM_SKIP_RENDERING = 12,
    DMPV_RENDER_PARAM_DRM_DISPLAY = 13,
    DMPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE = 14,
    DMPV_RENDER_PARAM_DRM_DISPLAY_V2 = 15,
    DMPV_RENDER_PARAM_SW_SIZE = 16,
    DMPV_RENDER_PARAM_SW_FORMAT = 17,
    DMPV_RENDER_PARAM_SW_STRIDE = 18,
    DMPV_RENDER_PARAM_SW_POINTER = 19,
} dmpv_render_param_type;

#define DMPV_RENDER_PARAM_DRM_OSD_SIZE DMPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE

typedef struct dmpv_render_param {
    enum dmpv_render_param_type type;
    void *data;
} dmpv_render_param;


#define DMPV_RENDER_API_TYPE_OPENGL "opengl"
#define DMPV_RENDER_API_TYPE_SW "sw"

typedef enum dmpv_render_frame_info_flag {
    DMPV_RENDER_FRAME_INFO_PRESENT         = 1 << 0,
    DMPV_RENDER_FRAME_INFO_REDRAW          = 1 << 1,
    DMPV_RENDER_FRAME_INFO_REPEAT          = 1 << 2,
    DMPV_RENDER_FRAME_INFO_BLOCK_VSYNC     = 1 << 3,
} dmpv_render_frame_info_flag;

typedef struct dmpv_render_frame_info {
    uint64_t flags;
    int64_t target_time;
} dmpv_render_frame_info;

DMPV_EXPORT int dmpv_render_context_create(dmpv_render_context **res, dmpv_handle *dmpv,
                                         dmpv_render_param *params);

DMPV_EXPORT int dmpv_render_context_set_parameter(dmpv_render_context *ctx,
                                                dmpv_render_param param);

DMPV_EXPORT int dmpv_render_context_get_info(dmpv_render_context *ctx,
                                           dmpv_render_param param);

typedef void (*dmpv_render_update_fn)(void *cb_ctx);

DMPV_EXPORT void dmpv_render_context_set_update_callback(dmpv_render_context *ctx,
                                                       dmpv_render_update_fn callback,
                                                       void *callback_ctx);

DMPV_EXPORT uint64_t dmpv_render_context_update(dmpv_render_context *ctx);

typedef enum dmpv_render_update_flag {
    DMPV_RENDER_UPDATE_FRAME         = 1 << 0,
} dmpv_render_context_flag;

DMPV_EXPORT int dmpv_render_context_render(dmpv_render_context *ctx, dmpv_render_param *params);

DMPV_EXPORT void dmpv_render_context_report_swap(dmpv_render_context *ctx);

DMPV_EXPORT void dmpv_render_context_free(dmpv_render_context *ctx);

#ifdef __cplusplus
}
#endif

#endif
