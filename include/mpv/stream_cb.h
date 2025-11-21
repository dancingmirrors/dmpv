/* Copyright (C) 2017 the mpv developers
 * SPDX-License-Identifier: ISC
 *
 * This header provides compatibility layer for applications expecting
 * standard libmpv stream_cb API while using libdmpv implementation.
 */

#ifndef MPV_STREAM_CB_H_
#define MPV_STREAM_CB_H_

#include "client.h"

/* Include the actual dmpv stream_cb API */
#include "../../misc/stream_cb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map types */
#define mpv_stream_cb_read_fn dmpv_stream_cb_read_fn
#define mpv_stream_cb_seek_fn dmpv_stream_cb_seek_fn
#define mpv_stream_cb_size_fn dmpv_stream_cb_size_fn
#define mpv_stream_cb_close_fn dmpv_stream_cb_close_fn
#define mpv_stream_cb_cancel_fn dmpv_stream_cb_cancel_fn
#define mpv_stream_cb_info dmpv_stream_cb_info
#define mpv_stream_cb_open_ro_fn dmpv_stream_cb_open_ro_fn

/* Map functions */
#define mpv_stream_cb_add_ro dmpv_stream_cb_add_ro

#ifdef __cplusplus
}
#endif

#endif
