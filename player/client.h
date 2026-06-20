#ifndef MP_CLIENT_H_
#define MP_CLIENT_H_

#include <stdint.h>
#include <stdbool.h>

#include "misc/client.h"
#include "misc/stream_cb.h"
#include "misc/bstr.h"

struct MPContext;
struct dmpv_handle;
struct mp_client_api;
struct mp_log;
struct dmpv_global;

// Includes space for \0
#define MAX_CLIENT_NAME 64

void mp_clients_init(struct MPContext *mpctx);
void mp_clients_destroy(struct MPContext *mpctx);
void mp_shutdown_clients(struct MPContext *mpctx);
bool mp_is_shutting_down(struct MPContext *mpctx);
bool mp_clients_all_initialized(struct MPContext *mpctx);

bool mp_client_id_exists(struct MPContext *mpctx, int64_t id);
void mp_client_broadcast_event(struct MPContext *mpctx, int event, void *data);
int mp_client_send_event(struct MPContext *mpctx, const char *client_name,
                         uint64_t reply_userdata, int event, void *data);
int mp_client_send_event_dup(struct MPContext *mpctx, const char *client_name,
                             int event, void *data);
void mp_client_property_change(struct MPContext *mpctx, const char *name);
void mp_client_send_property_changes(struct MPContext *mpctx);

struct dmpv_handle *mp_new_client(struct mp_client_api *clients, const char *name);
void mp_client_set_weak(struct dmpv_handle *ctx);
struct mp_log *mp_client_get_log(struct dmpv_handle *ctx);
struct dmpv_global *mp_client_get_global(struct dmpv_handle *ctx);

void mp_client_broadcast_event_external(struct mp_client_api *api, int event,
                                        void *data);

// m_option.c
void *node_get_alloc(struct dmpv_node *node);

struct osd_state;
struct dmpv_render_context;
bool mp_set_main_render_context(struct mp_client_api *client_api,
                                struct dmpv_render_context *ctx, bool active);
struct dmpv_render_context *
mp_client_api_acquire_render_context(struct mp_client_api *ca);
void kill_video_async(struct mp_client_api *client_api);

bool mp_streamcb_lookup(struct dmpv_global *g, const char *protocol,
                        void **out_user_data, dmpv_stream_cb_open_ro_fn *out_fn);

#endif
