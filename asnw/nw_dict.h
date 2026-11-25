/*
 * Description: dictionary, a hash table from 64 bit integer keys to pointers
 *     History: 2026/11/25, create
 */

# ifndef _NW_DICT_H_
# define _NW_DICT_H_

# include <stdint.h>
# include <stddef.h>

# include "nw_buf.h"

/*
 * nw_dict maps a uint64_t key to a void pointer. it is what nw_svr uses to
 * find a connection by its id and what nw_state uses to find an entry, and
 * it can be used on its own for any id -> object table.
 *
 * the dict stores the pointers, it does not own what they point to: free
 * your values yourself, before or after removing them.
 *
 * lookup, insert and delete are O(1) on average. the table doubles when it
 * holds as many entries as it has buckets, moving every entry at once, and
 * never shrinks. any key distribution is fine (sequential ids, pointers,
 * multiples of a power of two): keys are mixed before they pick a bucket.
 *
 * not thread safe.
 */

typedef struct nw_dict_entry {
    uint64_t key;
    void *val;
    struct nw_dict_entry *next;
} nw_dict_entry;

/* the fields are for reading only */
typedef struct nw_dict {
    nw_dict_entry **table;
    /* number of buckets, a power of two */
    uint32_t size;
    /* log2 of size */
    uint32_t bits;
    /* number of entries */
    uint32_t used;
    nw_cache *cache;
} nw_dict;

/* lives on the stack of the caller, see nw_dict_iter_init */
typedef struct nw_dict_iterator {
    nw_dict *dict;
    uint32_t index;
    nw_dict_entry *next_entry;
} nw_dict_iterator;

/* create a dict with room for about init_size entries before it first
 * grows. 0 picks a small default. return NULL if out of memory */
nw_dict *nw_dict_create(uint32_t init_size);

/* add key -> val. return 0, or -1 if the key is already there (the dict is
 * left unchanged) or memory runs out */
int nw_dict_add(nw_dict *dict, uint64_t key, void *val);

/* add key -> val, or change the value of a key that is already there.
 * return 0, or -1 if memory runs out */
int nw_dict_set(nw_dict *dict, uint64_t key, void *val);

/* the value of a key, NULL if the key is not there. use nw_dict_find to
 * tell a missing key from a key whose value is NULL */
void *nw_dict_get(nw_dict *dict, uint64_t key);

/* the entry of a key, NULL if the key is not there. entry->val may be read
 * and written; the entry is valid until the key is deleted */
nw_dict_entry *nw_dict_find(nw_dict *dict, uint64_t key);

/* remove a key. return 0, or -1 if the key is not there */
int nw_dict_del(nw_dict *dict, uint64_t key);

/* number of entries */
size_t nw_dict_count(nw_dict *dict);

/* remove every entry, the dict stays usable */
void nw_dict_clear(nw_dict *dict);

/* free the dict. the values are not touched */
void nw_dict_release(nw_dict *dict);

/*
 * walk over all entries, in no particular order:
 *
 *   nw_dict_iterator iter;
 *   nw_dict_entry *entry;
 *   nw_dict_iter_init(&iter, dict);
 *   while ((entry = nw_dict_next(&iter)) != NULL) {
 *       ... entry->key, entry->val ...
 *   }
 *
 * deleting the entry just returned is allowed; adding entries or deleting
 * other ones invalidates the iterator
 */
void nw_dict_iter_init(nw_dict_iterator *iter, nw_dict *dict);
/* the next entry, NULL when all were visited */
nw_dict_entry *nw_dict_next(nw_dict_iterator *iter);

# endif
