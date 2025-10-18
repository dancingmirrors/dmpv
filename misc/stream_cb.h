/* Copyright (C) 2017 the mpv developers
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

#ifndef MPV_CLIENT_API_STREAM_CB_H_
#define MPV_CLIENT_API_STREAM_CB_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef int64_t (*mpv_stream_cb_read_fn)(void *cookie, char *buf, uint64_t nbytes);

typedef int64_t (*mpv_stream_cb_seek_fn)(void *cookie, int64_t offset);

typedef int64_t (*mpv_stream_cb_size_fn)(void *cookie);

typedef void (*mpv_stream_cb_close_fn)(void *cookie);

typedef void (*mpv_stream_cb_cancel_fn)(void *cookie);

typedef struct mpv_stream_cb_info {
    void *cookie;

    mpv_stream_cb_read_fn read_fn;
    mpv_stream_cb_seek_fn seek_fn;
    mpv_stream_cb_size_fn size_fn;
    mpv_stream_cb_close_fn close_fn;
    mpv_stream_cb_cancel_fn cancel_fn;
} mpv_stream_cb_info;

typedef int (*mpv_stream_cb_open_ro_fn)(void *user_data, char *uri,
                                        mpv_stream_cb_info *info);

MPV_EXPORT int mpv_stream_cb_add_ro(mpv_handle *ctx, const char *protocol, void *user_data,
                                    mpv_stream_cb_open_ro_fn open_fn);

#ifdef __cplusplus
}
#endif

#endif
