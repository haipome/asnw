/*
 * Description: dictionary, a hash table from 64 bit integer keys to pointers
 *     History: 2026/11/25, create
 */

# include <stdlib.h>

# include "nw_dict.h"

# define NW_DICT_MIN_BITS 6
# define NW_DICT_MAX_BITS 31

/* fibonacci hashing: multiply by 2^64 / golden ratio and keep the top bits.
 * spreads sequential keys as well as keys that share their low bits */
static inline uint32_t bucket_of(uint64_t key, uint32_t bits)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ULL) >> (64 - bits));
}

nw_dict *nw_dict_create(uint32_t init_size)
{
    uint32_t bits = NW_DICT_MIN_BITS;
    while (bits < NW_DICT_MAX_BITS && ((uint32_t)1 << bits) < init_size)
        bits++;

    nw_dict *dict = malloc(sizeof(nw_dict));
    if (dict == NULL)
        return NULL;
    dict->bits = bits;
    dict->min_bits = bits;
    dict->size = (uint32_t)1 << bits;
    dict->used = 0;
    dict->cache = nw_cache_create(sizeof(nw_dict_entry));
    if (dict->cache == NULL) {
        free(dict);
        return NULL;
    }
    dict->table = calloc(dict->size, sizeof(nw_dict_entry *));
    if (dict->table == NULL) {
        nw_cache_release(dict->cache);
        free(dict);
        return NULL;
    }

    return dict;
}

/*
 * called before an insert. double the table once there is one entry per
 * bucket; shrink it when less than one bucket in eight is used, to four
 * buckets per entry so that it does not grow again right away. if there is
 * no memory for the new table the dict keeps working with the old one.
 */
static void resize_if_needed(nw_dict *dict)
{
    uint32_t new_bits = dict->bits;
    if (dict->used >= dict->size) {
        if (dict->bits >= NW_DICT_MAX_BITS)
            return;
        new_bits = dict->bits + 1;
    } else if (dict->bits > dict->min_bits && dict->used < dict->size / 8) {
        while (new_bits > dict->min_bits && dict->used < ((uint32_t)1 << new_bits) / 4)
            new_bits--;
    }
    if (new_bits == dict->bits)
        return;

    uint32_t new_size = (uint32_t)1 << new_bits;
    nw_dict_entry **new_table = calloc(new_size, sizeof(nw_dict_entry *));
    if (new_table == NULL)
        return;

    for (uint32_t i = 0; i < dict->size; ++i) {
        nw_dict_entry *entry = dict->table[i];
        while (entry) {
            nw_dict_entry *next = entry->next;
            uint32_t index = bucket_of(entry->key, new_bits);
            entry->next = new_table[index];
            new_table[index] = entry;
            entry = next;
        }
    }

    free(dict->table);
    dict->table = new_table;
    dict->size = new_size;
    dict->bits = new_bits;
}

nw_dict_entry *nw_dict_find(nw_dict *dict, uint64_t key)
{
    nw_dict_entry *entry = dict->table[bucket_of(key, dict->bits)];
    while (entry) {
        if (entry->key == key)
            return entry;
        entry = entry->next;
    }

    return NULL;
}

void *nw_dict_get(nw_dict *dict, uint64_t key)
{
    nw_dict_entry *entry = nw_dict_find(dict, key);
    return entry ? entry->val : NULL;
}

static int dict_insert(nw_dict *dict, uint64_t key, void *val)
{
    if (dict->used == UINT32_MAX)
        return -1;
    nw_dict_entry *entry = nw_cache_alloc(dict->cache);
    if (entry == NULL)
        return -1;
    resize_if_needed(dict);
    uint32_t index = bucket_of(key, dict->bits);
    entry->key = key;
    entry->val = val;
    entry->next = dict->table[index];
    dict->table[index] = entry;
    dict->used++;

    return 0;
}

int nw_dict_add(nw_dict *dict, uint64_t key, void *val)
{
    if (nw_dict_find(dict, key) != NULL)
        return -1;
    return dict_insert(dict, key, val);
}

int nw_dict_set(nw_dict *dict, uint64_t key, void *val)
{
    nw_dict_entry *entry = nw_dict_find(dict, key);
    if (entry != NULL) {
        entry->val = val;
        return 0;
    }
    return dict_insert(dict, key, val);
}

int nw_dict_del(nw_dict *dict, uint64_t key)
{
    nw_dict_entry **link = &dict->table[bucket_of(key, dict->bits)];
    while (*link) {
        nw_dict_entry *entry = *link;
        if (entry->key == key) {
            *link = entry->next;
            nw_cache_free(dict->cache, entry);
            dict->used--;
            return 0;
        }
        link = &entry->next;
    }

    return -1;
}

size_t nw_dict_count(nw_dict *dict)
{
    return dict->used;
}

void nw_dict_clear(nw_dict *dict)
{
    for (uint32_t i = 0; i < dict->size; ++i) {
        nw_dict_entry *entry = dict->table[i];
        while (entry) {
            nw_dict_entry *next = entry->next;
            nw_cache_free(dict->cache, entry);
            entry = next;
        }
        dict->table[i] = NULL;
    }
    dict->used = 0;
}

void nw_dict_release(nw_dict *dict)
{
    nw_dict_clear(dict);
    nw_cache_release(dict->cache);
    free(dict->table);
    free(dict);
}

/* next_entry always points at the entry to return next, so the one just
 * returned can be deleted by the caller */
static void iter_advance(nw_dict_iterator *iter)
{
    while (iter->next_entry == NULL && iter->index < iter->dict->size) {
        iter->next_entry = iter->dict->table[iter->index++];
    }
}

void nw_dict_iter_init(nw_dict_iterator *iter, nw_dict *dict)
{
    iter->dict = dict;
    iter->index = 0;
    iter->next_entry = NULL;
    iter_advance(iter);
}

nw_dict_entry *nw_dict_next(nw_dict_iterator *iter)
{
    nw_dict_entry *entry = iter->next_entry;
    if (entry == NULL)
        return NULL;
    iter->next_entry = entry->next;
    iter_advance(iter);
    return entry;
}
