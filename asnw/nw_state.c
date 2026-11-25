/*
 * Description: table of pending states with timeout
 *     History: yang@haipo.me, 2016/03/22, create
 */

# include <stdlib.h>
# include <string.h>
# include <stdbool.h>

# include "nw_state.h"

nw_state *nw_state_create(nw_state_type *type, uint32_t data_size)
{
    if (type->on_timeout == NULL)
        return NULL;

    nw_loop_init();
    nw_state *context = malloc(sizeof(nw_state));
    if (context == NULL) {
        return NULL;
    }
    memset(context, 0, sizeof(nw_state));
    context->loop = nw_default_loop;
    context->type = *type;
    context->data_size = data_size;
    context->cache = nw_cache_create(sizeof(nw_state_entry) + data_size);
    if (context->cache == NULL) {
        free(context);
        return NULL;
    }
    context->dict = nw_dict_create(0);
    if (context->dict == NULL) {
        nw_cache_release(context->cache);
        free(context);
        return NULL;
    }

    return context;
}

static void state_release(nw_state *context, nw_state_entry *entry)
{
    if (context->type.on_release)
        context->type.on_release(entry);
    nw_cache_free(context->cache, entry);
}

static void on_timeout(struct ev_loop *loop, ev_timer *ev, int events)
{
    nw_state_entry *entry = (nw_state_entry *)ev;
    nw_state *context = entry->context;
    /* take it out of the dict first: whatever the callback does with this
     * id (del, mod, add it again) can no longer touch the entry being
     * timed out */
    nw_dict_del(context->dict, entry->id);
    context->type.on_timeout(entry);
    state_release(context, entry);
}

static uint32_t get_available_id(nw_state *context)
{
    while (true) {
        context->id_start++;
        if (context->id_start == 0)
            context->id_start++;
        if (nw_state_get(context, context->id_start) == NULL)
            break;
    }
    return context->id_start;
}

nw_state_entry *nw_state_add(nw_state *context, double timeout, uint32_t id)
{
    if (id != 0 && nw_state_get(context, id) != NULL)
        return NULL;
    if (nw_dict_count(context->dict) == UINT32_MAX)
        return NULL;
    nw_state_entry *entry = nw_cache_alloc(context->cache);
    if (entry == NULL) {
        return NULL;
    }

    if (id != 0) {
        entry->id = id;
    } else {
        entry->id = get_available_id(context);
    }
    if (nw_dict_add(context->dict, entry->id, entry) < 0) {
        nw_cache_free(context->cache, entry);
        return NULL;
    }
    ev_timer_init(&entry->ev, on_timeout, timeout, 0);
    ev_timer_start(context->loop, &entry->ev);
    entry->context = context;
    entry->data = ((void *)entry + sizeof(nw_state_entry));
    memset(entry->data, 0, context->data_size);

    return entry;
}

nw_state_entry *nw_state_get(nw_state *context, uint32_t id)
{
    return nw_dict_get(context->dict, id);
}

int nw_state_mod(nw_state *context, uint32_t id, double timeout)
{
    nw_state_entry *entry = nw_state_get(context, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(context->loop, &entry->ev);
    ev_timer_set(&entry->ev, timeout, 0);
    ev_timer_start(context->loop, &entry->ev);

    return 0;
}

int nw_state_del(nw_state *context, uint32_t id)
{
    nw_state_entry *entry = nw_state_get(context, id);
    if (entry == NULL)
        return -1;
    ev_timer_stop(context->loop, &entry->ev);
    nw_dict_del(context->dict, id);
    state_release(context, entry);

    return 0;
}

size_t nw_state_count(nw_state *context)
{
    return nw_dict_count(context->dict);
}

void nw_state_release(nw_state *context)
{
    nw_dict_iterator iter;
    nw_dict_entry *item;
    nw_dict_iter_init(&iter, context->dict);
    while ((item = nw_dict_next(&iter)) != NULL) {
        nw_state_entry *entry = item->val;
        ev_timer_stop(context->loop, &entry->ev);
        state_release(context, entry);
    }
    nw_dict_release(context->dict);
    nw_cache_release(context->cache);
    free(context);
}

void nw_state_iter_init(nw_state_iterator *iter, nw_state *context)
{
    nw_dict_iter_init(&iter->iter, context->dict);
}

nw_state_entry *nw_state_next(nw_state_iterator *iter)
{
    nw_dict_entry *item = nw_dict_next(&iter->iter);
    return item ? item->val : NULL;
}
