#ifndef MP_MISC_NODE_H_
#define MP_MISC_NODE_H_

#include "misc/client.h"
#include "misc/bstr.h"

void node_init(struct dmpv_node *dst, int format, struct dmpv_node *parent);
struct dmpv_node *node_array_add(struct dmpv_node *dst, int format);
struct dmpv_node *node_map_add(struct dmpv_node *dst, const char *key, int format);
struct dmpv_node *node_map_badd(struct dmpv_node *dst, struct bstr key, int format);
void node_map_add_string(struct dmpv_node *dst, const char *key, const char *val);
void node_map_add_int64(struct dmpv_node *dst, const char *key, int64_t v);
void node_map_add_double(struct dmpv_node *dst, const char *key, double v);
void node_map_add_flag(struct dmpv_node *dst, const char *key, bool v);
dmpv_node *node_map_get(dmpv_node *src, const char *key);
dmpv_node *node_map_bget(dmpv_node *src, struct bstr key);
bool equal_dmpv_value(const void *a, const void *b, dmpv_format format);
bool equal_dmpv_node(const struct dmpv_node *a, const struct dmpv_node *b);

#endif
