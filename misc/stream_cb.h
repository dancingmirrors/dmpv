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

#ifndef DMPV_CLIENT_API_STREAM_CB_H_
#define DMPV_CLIENT_API_STREAM_CB_H_

#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef int64_t (*dmpv_stream_cb_read_fn)(void *cookie, char *buf, uint64_t nbytes);

typedef int64_t (*dmpv_stream_cb_seek_fn)(void *cookie, int64_t offset);

typedef int64_t (*dmpv_stream_cb_size_fn)(void *cookie);

typedef void (*dmpv_stream_cb_close_fn)(void *cookie);

typedef void (*dmpv_stream_cb_cancel_fn)(void *cookie);

typedef struct dmpv_stream_cb_info {
    void *cookie;

    dmpv_stream_cb_read_fn read_fn;
    dmpv_stream_cb_seek_fn seek_fn;
    dmpv_stream_cb_size_fn size_fn;
    dmpv_stream_cb_close_fn close_fn;
    dmpv_stream_cb_cancel_fn cancel_fn;
} dmpv_stream_cb_info;

typedef int (*dmpv_stream_cb_open_ro_fn)(void *user_data, char *uri,
                                        dmpv_stream_cb_info *info);

DMPV_EXPORT int dmpv_stream_cb_add_ro(dmpv_handle *ctx, const char *protocol, void *user_data,
                                    dmpv_stream_cb_open_ro_fn open_fn);

#ifdef __cplusplus
}
#endif

#endif
