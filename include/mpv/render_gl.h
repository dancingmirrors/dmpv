/* Copyright (C) 2018 the mpv developers
 * SPDX-License-Identifier: ISC
 *
 * This header provides compatibility layer for applications expecting
 * standard libmpv render_gl API while using libdmpv implementation.
 */

#ifndef MPV_CLIENT_API_RENDER_GL_H_
#define MPV_CLIENT_API_RENDER_GL_H_

#include "render.h"

/* Include the actual dmpv render_gl API */
#include "../../misc/render_gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map types */
#define mpv_opengl_init_params dmpv_opengl_init_params
#define mpv_opengl_fbo dmpv_opengl_fbo
#define mpv_opengl_drm_params dmpv_opengl_drm_params
#define mpv_opengl_drm_draw_surface_size dmpv_opengl_drm_draw_surface_size
#define mpv_opengl_drm_params_v2 dmpv_opengl_drm_params_v2
#define mpv_opengl_drm_osd_size dmpv_opengl_drm_osd_size

#ifdef __cplusplus
}
#endif

#endif
