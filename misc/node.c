#include "common/common.h"
#include "misc/mp_assert.h"

#include "node.h"

// Init a node with the given format. If parent is not NULL, it is set as
// parent allocation according to m_option_type_node rules (which means
// the dmpv_node_list allocs are used for chaining the TA allocations).
// format == DMPV_FORMAT_NONE will simply initialize it with all-0.
void node_init(struct dmpv_node *dst, int format, struct dmpv_node *parent)
{
    // Other formats need to be initialized manually.
    mp_assert(format == DMPV_FORMAT_NODE_MAP || format == DMPV_FORMAT_NODE_ARRAY ||
           format == DMPV_FORMAT_FLAG || format == DMPV_FORMAT_INT64 ||
           format == DMPV_FORMAT_DOUBLE || format == DMPV_FORMAT_BYTE_ARRAY ||
           format == DMPV_FORMAT_NONE);

    void *ta_parent = NULL;
    if (parent) {
        mp_assert(parent->format == DMPV_FORMAT_NODE_MAP ||
               parent->format == DMPV_FORMAT_NODE_ARRAY);
        ta_parent = parent->u.list;
    }

    *dst = (struct dmpv_node){ .format = format };
    if (format == DMPV_FORMAT_NODE_MAP || format == DMPV_FORMAT_NODE_ARRAY)
        dst->u.list = talloc_zero(ta_parent, struct dmpv_node_list);
    if (format == DMPV_FORMAT_BYTE_ARRAY)
        dst->u.ba = talloc_zero(ta_parent, struct dmpv_byte_array);
}

// Add an entry to a DMPV_FORMAT_NODE_ARRAY.
// m_option_type_node memory management rules apply.
struct dmpv_node *node_array_add(struct dmpv_node *dst, int format)
{
    struct dmpv_node_list *list = dst->u.list;
    mp_assert(dst->format == DMPV_FORMAT_NODE_ARRAY && dst->u.list);
    MP_TARRAY_GROW(list, list->values, list->num);
    node_init(&list->values[list->num], format, dst);
    return &list->values[list->num++];
}

// Add an entry to a DMPV_FORMAT_NODE_MAP. Keep in mind that this does
// not check for already existing entries under the same key.
// m_option_type_node memory management rules apply.
struct dmpv_node *node_map_add(struct dmpv_node *dst, const char *key, int format)
{
    mp_assert(key);
    return node_map_badd(dst, bstr0(key), format);
}

struct dmpv_node *node_map_badd(struct dmpv_node *dst, struct bstr key, int format)
{
    mp_assert(key.start);

    struct dmpv_node_list *list = dst->u.list;
    mp_assert(dst->format == DMPV_FORMAT_NODE_MAP && dst->u.list);
    MP_TARRAY_GROW(list, list->values, list->num);
    MP_TARRAY_GROW(list, list->keys, list->num);
    list->keys[list->num] = bstrdup0(list, key);
    node_init(&list->values[list->num], format, dst);
    return &list->values[list->num++];
}

// Add a string entry to a DMPV_FORMAT_NODE_MAP. Keep in mind that this does
// not check for already existing entries under the same key.
// m_option_type_node memory management rules apply.
void node_map_add_string(struct dmpv_node *dst, const char *key, const char *val)
{
    mp_assert(val);

    struct dmpv_node *entry = node_map_add(dst, key, DMPV_FORMAT_NONE);
    entry->format = DMPV_FORMAT_STRING;
    entry->u.string = talloc_strdup(dst->u.list, val);
}

void node_map_add_int64(struct dmpv_node *dst, const char *key, int64_t v)
{
    node_map_add(dst, key, DMPV_FORMAT_INT64)->u.int64 = v;
}

void node_map_add_double(struct dmpv_node *dst, const char *key, double v)
{
    node_map_add(dst, key, DMPV_FORMAT_DOUBLE)->u.double_ = v;
}

void node_map_add_flag(struct dmpv_node *dst, const char *key, bool v)
{
    node_map_add(dst, key, DMPV_FORMAT_FLAG)->u.flag = v;
}

dmpv_node *node_map_get(dmpv_node *src, const char *key)
{
    return node_map_bget(src, bstr0(key));
}

dmpv_node *node_map_bget(dmpv_node *src, struct bstr key)
{
    if (src->format != DMPV_FORMAT_NODE_MAP)
        return NULL;

    for (int i = 0; i < src->u.list->num; i++) {
        if (bstr_equals0(key, src->u.list->keys[i]))
            return &src->u.list->values[i];
    }

    return NULL;
}

// Note: for DMPV_FORMAT_NODE_MAP, this (incorrectly) takes the order into
//       account, instead of treating it as set.
bool equal_dmpv_value(const void *a, const void *b, dmpv_format format)
{
    switch (format) {
    case DMPV_FORMAT_NONE:
        return true;
    case DMPV_FORMAT_STRING:
    case DMPV_FORMAT_OSD_STRING:
        return strcmp(*(char **)a, *(char **)b) == 0;
    case DMPV_FORMAT_FLAG:
        return *(int *)a == *(int *)b;
    case DMPV_FORMAT_INT64:
        return *(int64_t *)a == *(int64_t *)b;
    case DMPV_FORMAT_DOUBLE:
        return *(double *)a == *(double *)b;
    case DMPV_FORMAT_NODE:
        return equal_dmpv_node(a, b);
    case DMPV_FORMAT_BYTE_ARRAY: {
        const struct dmpv_byte_array *a_r = a, *b_r = b;
        if (a_r->size != b_r->size)
            return false;
        return memcmp(a_r->data, b_r->data, a_r->size) == 0;
    }
    case DMPV_FORMAT_NODE_ARRAY:
    case DMPV_FORMAT_NODE_MAP:
    {
        dmpv_node_list *l_a = *(dmpv_node_list **)a, *l_b = *(dmpv_node_list **)b;
        if (l_a->num != l_b->num)
            return false;
        for (int n = 0; n < l_a->num; n++) {
            if (format == DMPV_FORMAT_NODE_MAP) {
                if (strcmp(l_a->keys[n], l_b->keys[n]) != 0)
                    return false;
            }
            if (!equal_dmpv_node(&l_a->values[n], &l_b->values[n]))
                return false;
        }
        return true;
    }
    }
    MP_ASSERT_UNREACHABLE(); // supposed to be able to handle all defined types
}

// Remarks see equal_dmpv_value().
bool equal_dmpv_node(const struct dmpv_node *a, const struct dmpv_node *b)
{
    if (a->format != b->format)
        return false;
    return equal_dmpv_value(&a->u, &b->u, a->format);
}
