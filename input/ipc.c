/*
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

#include "common/msg.h"
#include "input/input.h"
#include "misc/json.h"
#include "misc/node.h"
#include "options/m_option.h"
#include "options/options.h"
#include "options/path.h"
#include "player/client.h"

static dmpv_node *dmpv_node_array_get(dmpv_node *src, int index)
{
    if (src->format != DMPV_FORMAT_NODE_ARRAY)
        return NULL;

    if (src->u.list->num < (index + 1))
        return NULL;

    return &src->u.list->values[index];
}

static void dmpv_node_map_add(void *ta_parent, dmpv_node *src, const char *key, dmpv_node *val)
{
    if (src->format != DMPV_FORMAT_NODE_MAP)
        return;

    if (!src->u.list)
        src->u.list = talloc_zero(ta_parent, dmpv_node_list);

    MP_TARRAY_GROW(src->u.list, src->u.list->keys, src->u.list->num);
    MP_TARRAY_GROW(src->u.list, src->u.list->values, src->u.list->num);

    src->u.list->keys[src->u.list->num] = talloc_strdup(ta_parent, key);

    static const struct m_option type = { .type = CONF_TYPE_NODE };
    m_option_get_node(&type, ta_parent, &src->u.list->values[src->u.list->num], val);

    src->u.list->num++;
}

static void dmpv_node_map_add_null(void *ta_parent, dmpv_node *src, const char *key)
{
    dmpv_node val_node = {.format = DMPV_FORMAT_NONE};
    dmpv_node_map_add(ta_parent, src, key, &val_node);
}

static void dmpv_node_map_add_int64(void *ta_parent, dmpv_node *src, const char *key, int64_t val)
{
    dmpv_node val_node = {.format = DMPV_FORMAT_INT64, .u.int64 = val};
    dmpv_node_map_add(ta_parent, src, key, &val_node);
}

static void dmpv_node_map_add_string(void *ta_parent, dmpv_node *src, const char *key, const char *val)
{
    dmpv_node val_node = {.format = DMPV_FORMAT_STRING, .u.string = (char*)val};
    dmpv_node_map_add(ta_parent, src, key, &val_node);
}

// This is supposed to write a reply that looks like "normal" command execution.
static void dmpv_format_command_reply(void *ta_parent, dmpv_event *event,
                                     dmpv_node *dst)
{
    mp_assert(event->event_id == DMPV_EVENT_COMMAND_REPLY);
    dmpv_event_command *cmd = event->data;

    dmpv_node_map_add_int64(ta_parent, dst, "request_id", event->reply_userdata);

    dmpv_node_map_add_string(ta_parent, dst, "error",
                            dmpv_error_string(event->error));

    dmpv_node_map_add(ta_parent, dst, "data", &cmd->result);
}

char *mp_json_encode_event(dmpv_event *event)
{
    void *ta_parent = talloc_new(NULL);

    struct dmpv_node event_node;
    if (event->event_id == DMPV_EVENT_COMMAND_REPLY) {
        event_node = (dmpv_node){.format = DMPV_FORMAT_NODE_MAP, .u.list = NULL};
        dmpv_format_command_reply(ta_parent, event, &event_node);
    } else {
        dmpv_event_to_node(&event_node, event);
        // Abuse dmpv_event_to_node() internals.
        talloc_steal(ta_parent, node_get_alloc(&event_node));
    }

    char *output = talloc_strdup(NULL, "");
    json_write(&output, &event_node);
    output = ta_talloc_strdup_append(output, "\n");

    talloc_free(ta_parent);

    return output;
}

// Function is allowed to modify src[n].
static char *json_execute_command(struct dmpv_handle *client, void *ta_parent,
                                  char *src)
{
    int rc;
    const char *cmd = NULL;
    struct mp_log *log = mp_client_get_log(client);

    dmpv_node msg_node;
    dmpv_node reply_node = {.format = DMPV_FORMAT_NODE_MAP, .u.list = NULL};
    dmpv_node *reqid_node = NULL;
    int64_t reqid = 0;
    dmpv_node *async_node = NULL;
    bool async = false;
    bool send_reply = true;

    rc = json_parse(ta_parent, &msg_node, &src, MAX_JSON_DEPTH);
    if (rc < 0) {
        mp_err(log, "malformed JSON received: '%s'\n", src);
        rc = DMPV_ERROR_INVALID_PARAMETER;
        goto error;
    }

    if (msg_node.format != DMPV_FORMAT_NODE_MAP) {
        rc = DMPV_ERROR_INVALID_PARAMETER;
        goto error;
    }

    async_node = node_map_get(&msg_node, "async");
    if (async_node) {
        if (async_node->format != DMPV_FORMAT_FLAG) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }
        async = async_node->u.flag;
    }

    reqid_node = node_map_get(&msg_node, "request_id");
    if (reqid_node) {
        if (reqid_node->format == DMPV_FORMAT_INT64) {
            reqid = reqid_node->u.int64;
        } else if (async) {
            mp_err(log, "'request_id' must be an integer for async commands.\n");
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        } else {
            mp_err(log, "'request_id' must be an integer.\n");
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }
    }

    dmpv_node *cmd_node = node_map_get(&msg_node, "command");
    if (!cmd_node) {
        rc = DMPV_ERROR_INVALID_PARAMETER;
        goto error;
    }

    if (cmd_node->format == DMPV_FORMAT_NODE_ARRAY) {
        dmpv_node *cmd_str_node = dmpv_node_array_get(cmd_node, 0);
        if (!cmd_str_node || (cmd_str_node->format != DMPV_FORMAT_STRING)) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        cmd = cmd_str_node->u.string;
    }

    if (cmd && !strcmp("client_name", cmd)) {
        const char *client_name = dmpv_client_name(client);
        dmpv_node_map_add_string(ta_parent, &reply_node, "data", client_name);
        rc = DMPV_ERROR_SUCCESS;
    } else if (cmd && !strcmp("get_time_us", cmd)) {
        int64_t time_us = dmpv_get_time_us(client);
        dmpv_node_map_add_int64(ta_parent, &reply_node, "data", time_us);
        rc = DMPV_ERROR_SUCCESS;
    } else if (cmd && !strcmp("get_version", cmd)) {
        int64_t ver = dmpv_client_api_version();
        dmpv_node_map_add_int64(ta_parent, &reply_node, "data", ver);
        rc = DMPV_ERROR_SUCCESS;
    } else if (cmd && !strcmp("get_property", cmd)) {
        dmpv_node result_node;

        if (cmd_node->u.list->num != 2) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_get_property(client, cmd_node->u.list->values[1].u.string,
                              DMPV_FORMAT_NODE, &result_node);
        if (rc >= 0) {
            dmpv_node_map_add(ta_parent, &reply_node, "data", &result_node);
            dmpv_free_node_contents(&result_node);
        }
    } else if (cmd && !strcmp("get_property_string", cmd)) {
        if (cmd_node->u.list->num != 2) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        char *result = dmpv_get_property_string(client,
                                        cmd_node->u.list->values[1].u.string);
        if (result) {
            dmpv_node_map_add_string(ta_parent, &reply_node, "data", result);
            dmpv_free(result);
        } else {
            dmpv_node_map_add_null(ta_parent, &reply_node, "data");
        }
    } else if (cmd && (!strcmp("set_property", cmd) ||
                       !strcmp("set_property_string", cmd)))
    {
        if (cmd_node->u.list->num != 3) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_set_property(client, cmd_node->u.list->values[1].u.string,
                              DMPV_FORMAT_NODE, &cmd_node->u.list->values[2]);
    } else if (cmd && !strcmp("observe_property", cmd)) {
        if (cmd_node->u.list->num != 3) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_INT64) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[2].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_observe_property(client,
                                  cmd_node->u.list->values[1].u.int64,
                                  cmd_node->u.list->values[2].u.string,
                                  DMPV_FORMAT_NODE);
    } else if (cmd && !strcmp("observe_property_string", cmd)) {
        if (cmd_node->u.list->num != 3) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_INT64) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[2].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_observe_property(client,
                                  cmd_node->u.list->values[1].u.int64,
                                  cmd_node->u.list->values[2].u.string,
                                  DMPV_FORMAT_STRING);
    } else if (cmd && !strcmp("unobserve_property", cmd)) {
        if (cmd_node->u.list->num != 2) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_INT64) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_unobserve_property(client,
                                    cmd_node->u.list->values[1].u.int64);
    } else if (cmd && !strcmp("request_log_messages", cmd)) {
        if (cmd_node->u.list->num != 2) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        rc = dmpv_request_log_messages(client,
                                      cmd_node->u.list->values[1].u.string);
    } else if (cmd && (!strcmp("enable_event", cmd) ||
                       !strcmp("disable_event", cmd)))
    {
        bool enable = !strcmp("enable_event", cmd);

        if (cmd_node->u.list->num != 2) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        if (cmd_node->u.list->values[1].format != DMPV_FORMAT_STRING) {
            rc = DMPV_ERROR_INVALID_PARAMETER;
            goto error;
        }

        char *name = cmd_node->u.list->values[1].u.string;
        if (strcmp(name, "all") == 0) {
            for (int n = 0; n < 64; n++)
                dmpv_request_event(client, n, enable);
            rc = DMPV_ERROR_SUCCESS;
        } else {
            int event = -1;
            for (int n = 0; n < 64; n++) {
                const char *evname = dmpv_event_name(n);
                if (evname && strcmp(evname, name) == 0)
                    event = n;
            }
            if (event < 0) {
                rc = DMPV_ERROR_INVALID_PARAMETER;
                goto error;
            }
            rc = dmpv_request_event(client, event, enable);
        }
    } else {
        dmpv_node result_node = {0};

        if (async) {
            rc = dmpv_command_node_async(client, reqid, cmd_node);
            if (rc >= 0)
                send_reply = false;
        } else {
            rc = dmpv_command_node(client, cmd_node, &result_node);
            if (rc >= 0)
                dmpv_node_map_add(ta_parent, &reply_node, "data", &result_node);
        }

        dmpv_free_node_contents(&result_node);
    }

error:
    /* If the request contains a "request_id", copy it back into the response.
     * This makes it easier on the requester to match up the IPC results with
     * the original requests.
     */
    if (reqid_node) {
        dmpv_node_map_add(ta_parent, &reply_node, "request_id", reqid_node);
    } else {
        dmpv_node_map_add_int64(ta_parent, &reply_node, "request_id", 0);
    }

    dmpv_node_map_add_string(ta_parent, &reply_node, "error", dmpv_error_string(rc));

    char *output = talloc_strdup(ta_parent, "");

    if (send_reply) {
        json_write(&output, &reply_node);
        output = ta_talloc_strdup_append(output, "\n");
    }

    return output;
}

static char *text_execute_command(struct dmpv_handle *client, void *tmp, char *src)
{
    dmpv_command_string(client, src);

    return NULL;
}

char *mp_ipc_consume_next_command(struct dmpv_handle *client, void *ctx, bstr *buf)
{
    void *tmp = talloc_new(NULL);

    bstr rest;
    bstr line = bstr_getline(*buf, &rest);
    char *line0 = bstrto0(tmp, line);
    talloc_steal(tmp, buf->start);
    *buf = bstrdup(NULL, rest);

    json_skip_whitespace(&line0);

    char *reply_msg = NULL;
    if (line0[0] == '\0' || line0[0] == '#') {
        // skip
    } else if (line0[0] == '{') {
        reply_msg = json_execute_command(client, tmp, line0);
    } else {
        reply_msg = text_execute_command(client, tmp, line0);
    }

    talloc_steal(ctx, reply_msg);
    talloc_free(tmp);
    return reply_msg;
}
