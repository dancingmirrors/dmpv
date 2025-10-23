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

#ifndef DMPV_CLIENT_API_H_
#define DMPV_CLIENT_API_H_

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define DMPV_EXPORT __attribute__((visibility("default")))
#else
#define DMPV_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif


#define DMPV_MAKE_VERSION(major, minor) (((major) << 16) | (minor) | 0UL)
#define DMPV_CLIENT_API_VERSION DMPV_MAKE_VERSION(2, 1)

#ifndef DMPV_ENABLE_DEPRECATED
#define DMPV_ENABLE_DEPRECATED 1
#endif

DMPV_EXPORT unsigned long dmpv_client_api_version(void);

typedef struct dmpv_handle dmpv_handle;

typedef enum dmpv_error {
    DMPV_ERROR_SUCCESS           = 0,
    DMPV_ERROR_EVENT_QUEUE_FULL  = -1,
    DMPV_ERROR_NOMEM             = -2,
    DMPV_ERROR_UNINITIALIZED     = -3,
    DMPV_ERROR_INVALID_PARAMETER = -4,
    DMPV_ERROR_OPTION_NOT_FOUND  = -5,
    DMPV_ERROR_OPTION_FORMAT     = -6,
    DMPV_ERROR_OPTION_ERROR      = -7,
    DMPV_ERROR_PROPERTY_NOT_FOUND = -8,
    DMPV_ERROR_PROPERTY_FORMAT   = -9,
    DMPV_ERROR_PROPERTY_UNAVAILABLE = -10,
    DMPV_ERROR_PROPERTY_ERROR    = -11,
    DMPV_ERROR_COMMAND           = -12,
    DMPV_ERROR_LOADING_FAILED    = -13,
    DMPV_ERROR_AO_INIT_FAILED    = -14,
    DMPV_ERROR_VO_INIT_FAILED    = -15,
    DMPV_ERROR_NOTHING_TO_PLAY   = -16,
    DMPV_ERROR_UNKNOWN_FORMAT    = -17,
    DMPV_ERROR_UNSUPPORTED       = -18,
    DMPV_ERROR_NOT_IMPLEMENTED   = -19,
    DMPV_ERROR_GENERIC           = -20
} dmpv_error;

DMPV_EXPORT const char *dmpv_error_string(int error);

DMPV_EXPORT void dmpv_free(void *data);

DMPV_EXPORT const char *dmpv_client_name(dmpv_handle *ctx);

DMPV_EXPORT int64_t dmpv_client_id(dmpv_handle *ctx);

DMPV_EXPORT dmpv_handle *dmpv_create(void);

DMPV_EXPORT int dmpv_initialize(dmpv_handle *ctx);

DMPV_EXPORT void dmpv_destroy(dmpv_handle *ctx);

DMPV_EXPORT void dmpv_terminate_destroy(dmpv_handle *ctx);

DMPV_EXPORT dmpv_handle *dmpv_create_client(dmpv_handle *ctx, const char *name);

DMPV_EXPORT dmpv_handle *dmpv_create_weak_client(dmpv_handle *ctx, const char *name);

DMPV_EXPORT int dmpv_load_config_file(dmpv_handle *ctx, const char *filename);

DMPV_EXPORT int64_t dmpv_get_time_us(dmpv_handle *ctx);

typedef enum dmpv_format {
    DMPV_FORMAT_NONE             = 0,
    DMPV_FORMAT_STRING           = 1,
    DMPV_FORMAT_OSD_STRING       = 2,
    DMPV_FORMAT_FLAG             = 3,
    DMPV_FORMAT_INT64            = 4,
    DMPV_FORMAT_DOUBLE           = 5,
    DMPV_FORMAT_NODE             = 6,
    DMPV_FORMAT_NODE_ARRAY       = 7,
    DMPV_FORMAT_NODE_MAP         = 8,
    DMPV_FORMAT_BYTE_ARRAY       = 9
} dmpv_format;

typedef struct dmpv_node {
    union {
        char *string;
        int flag;
        int64_t int64;
        double double_;

        struct dmpv_node_list *list;
        struct dmpv_byte_array *ba;
    } u;
    dmpv_format format;
} dmpv_node;

typedef struct dmpv_node_list {
    int num;
    dmpv_node *values;
    char **keys;
} dmpv_node_list;

typedef struct dmpv_byte_array {
    void *data;
    size_t size;
} dmpv_byte_array;

DMPV_EXPORT void dmpv_free_node_contents(dmpv_node *node);

DMPV_EXPORT int dmpv_set_option(dmpv_handle *ctx, const char *name, dmpv_format format,
                              void *data);

DMPV_EXPORT int dmpv_set_option_string(dmpv_handle *ctx, const char *name, const char *data);

DMPV_EXPORT int dmpv_command(dmpv_handle *ctx, const char **args);

DMPV_EXPORT int dmpv_command_node(dmpv_handle *ctx, dmpv_node *args, dmpv_node *result);

DMPV_EXPORT int dmpv_command_ret(dmpv_handle *ctx, const char **args, dmpv_node *result);

DMPV_EXPORT int dmpv_command_string(dmpv_handle *ctx, const char *args);

DMPV_EXPORT int dmpv_command_async(dmpv_handle *ctx, uint64_t reply_userdata,
                                 const char **args);

DMPV_EXPORT int dmpv_command_node_async(dmpv_handle *ctx, uint64_t reply_userdata,
                                      dmpv_node *args);

DMPV_EXPORT void dmpv_abort_async_command(dmpv_handle *ctx, uint64_t reply_userdata);

DMPV_EXPORT int dmpv_set_property(dmpv_handle *ctx, const char *name, dmpv_format format,
                                void *data);

DMPV_EXPORT int dmpv_set_property_string(dmpv_handle *ctx, const char *name, const char *data);

DMPV_EXPORT int dmpv_del_property(dmpv_handle *ctx, const char *name);

DMPV_EXPORT int dmpv_set_property_async(dmpv_handle *ctx, uint64_t reply_userdata,
                                      const char *name, dmpv_format format, void *data);

DMPV_EXPORT int dmpv_get_property(dmpv_handle *ctx, const char *name, dmpv_format format,
                                void *data);

DMPV_EXPORT char *dmpv_get_property_string(dmpv_handle *ctx, const char *name);

DMPV_EXPORT char *dmpv_get_property_osd_string(dmpv_handle *ctx, const char *name);

DMPV_EXPORT int dmpv_get_property_async(dmpv_handle *ctx, uint64_t reply_userdata,
                                      const char *name, dmpv_format format);

DMPV_EXPORT int dmpv_observe_property(dmpv_handle *dmpv, uint64_t reply_userdata,
                                    const char *name, dmpv_format format);

DMPV_EXPORT int dmpv_unobserve_property(dmpv_handle *dmpv, uint64_t registered_reply_userdata);

typedef enum dmpv_event_id {
    DMPV_EVENT_NONE              = 0,
    DMPV_EVENT_SHUTDOWN          = 1,
    DMPV_EVENT_LOG_MESSAGE       = 2,
    DMPV_EVENT_GET_PROPERTY_REPLY = 3,
    DMPV_EVENT_SET_PROPERTY_REPLY = 4,
    DMPV_EVENT_COMMAND_REPLY     = 5,
    DMPV_EVENT_START_FILE        = 6,
    DMPV_EVENT_END_FILE          = 7,
    DMPV_EVENT_FILE_LOADED       = 8,
#if DMPV_ENABLE_DEPRECATED
    DMPV_EVENT_IDLE              = 11,
    DMPV_EVENT_TICK              = 14,
#endif
    DMPV_EVENT_CLIENT_MESSAGE    = 16,
    DMPV_EVENT_VIDEO_RECONFIG    = 17,
    DMPV_EVENT_AUDIO_RECONFIG    = 18,
    DMPV_EVENT_SEEK              = 20,
    DMPV_EVENT_PLAYBACK_RESTART  = 21,
    DMPV_EVENT_PROPERTY_CHANGE   = 22,
    DMPV_EVENT_QUEUE_OVERFLOW    = 24,
    DMPV_EVENT_HOOK              = 25,
} dmpv_event_id;

DMPV_EXPORT const char *dmpv_event_name(dmpv_event_id event);

typedef struct dmpv_event_property {
    const char *name;
    dmpv_format format;
    void *data;
} dmpv_event_property;

typedef enum dmpv_log_level {
    DMPV_LOG_LEVEL_NONE  = 0,
    DMPV_LOG_LEVEL_FATAL = 10,
    DMPV_LOG_LEVEL_ERROR = 20,
    DMPV_LOG_LEVEL_WARN  = 30,
    DMPV_LOG_LEVEL_INFO  = 40,
    DMPV_LOG_LEVEL_V     = 50,
    DMPV_LOG_LEVEL_DEBUG = 60,
    DMPV_LOG_LEVEL_TRACE = 70,
} dmpv_log_level;

typedef struct dmpv_event_log_message {
    const char *prefix;
    const char *level;
    const char *text;
    dmpv_log_level log_level;
} dmpv_event_log_message;

typedef enum dmpv_end_file_reason {
    DMPV_END_FILE_REASON_EOF = 0,
    DMPV_END_FILE_REASON_STOP = 2,
    DMPV_END_FILE_REASON_QUIT = 3,
    DMPV_END_FILE_REASON_ERROR = 4,
    DMPV_END_FILE_REASON_REDIRECT = 5,
} dmpv_end_file_reason;

typedef struct dmpv_event_start_file {
    int64_t playlist_entry_id;
} dmpv_event_start_file;

typedef struct dmpv_event_end_file {
    dmpv_end_file_reason reason;
    int error;
    int64_t playlist_entry_id;
    int64_t playlist_insert_id;
    int playlist_insert_num_entries;
} dmpv_event_end_file;

typedef struct dmpv_event_client_message {
    int num_args;
    const char **args;
} dmpv_event_client_message;

typedef struct dmpv_event_hook {
    const char *name;
    uint64_t id;
} dmpv_event_hook;

typedef struct dmpv_event_command {
    dmpv_node result;
} dmpv_event_command;

typedef struct dmpv_event {
    dmpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void *data;
} dmpv_event;

DMPV_EXPORT int dmpv_event_to_node(dmpv_node *dst, dmpv_event *src);

DMPV_EXPORT int dmpv_request_event(dmpv_handle *ctx, dmpv_event_id event, int enable);

DMPV_EXPORT int dmpv_request_log_messages(dmpv_handle *ctx, const char *min_level);

DMPV_EXPORT dmpv_event *dmpv_wait_event(dmpv_handle *ctx, double timeout);

DMPV_EXPORT void dmpv_wakeup(dmpv_handle *ctx);

DMPV_EXPORT void dmpv_set_wakeup_callback(dmpv_handle *ctx, void (*cb)(void *d), void *d);

DMPV_EXPORT void dmpv_wait_async_requests(dmpv_handle *ctx);

DMPV_EXPORT int dmpv_hook_add(dmpv_handle *ctx, uint64_t reply_userdata,
                            const char *name, int priority);

DMPV_EXPORT int dmpv_hook_continue(dmpv_handle *ctx, uint64_t id);

#if DMPV_ENABLE_DEPRECATED

DMPV_EXPORT int dmpv_get_wakeup_pipe(dmpv_handle *ctx);

#endif

#ifdef __cplusplus
}
#endif

#endif
