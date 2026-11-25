/*
 * Description: buffers: fixed size buf, buf pool, buf list and object cache
 *     History: yang@haipo.me, 2016/03/16, create
 */

# include <string.h>
# include "nw_buf.h"

# define NW_BUF_POOL_INIT_SIZE 64
# define NW_CACHE_INIT_SIZE    64

/* how many objects an idle pool or cache holds on to */
# define NW_KEEP_COUNT         64

/*
 * an object comes back to a free list (the same code serves nw_buf_pool
 * and nw_cache). keep it for reuse unless as many are waiting already as
 * are in use, plus keep: then free it, and one of the waiting ones too if
 * there are more than that, so that after a peak the list comes down with
 * the load instead of staying at the size of the peak. `used` has been
 * decremented by the caller.
 */
static void free_list_put(void ***free_arr, uint32_t *free_count, uint32_t *free_total, uint32_t used, uint32_t keep, void *obj)
{
    if (*free_count >= used + keep) {
        free(obj);
        if (*free_count > used + keep)
            free((*free_arr)[--*free_count]);
        /* the array of pointers shrinks as well */
        if (*free_total > NW_BUF_POOL_INIT_SIZE && *free_count < *free_total / 4) {
            uint32_t new_total = *free_total / 2;
            void *new_arr = realloc(*free_arr, new_total * sizeof(void *));
            if (new_arr) {
                *free_arr = new_arr;
                *free_total = new_total;
            }
        }
        return;
    }
    if (*free_count == *free_total) {
        uint32_t new_total = *free_total * 2;
        void *new_arr = realloc(*free_arr, new_total * sizeof(void *));
        if (new_arr == NULL) {
            free(obj);
            return;
        }
        *free_arr = new_arr;
        *free_total = new_total;
    }
    (*free_arr)[(*free_count)++] = obj;
}

size_t nw_buf_size(nw_buf *buf)
{
    return buf->wpos - buf->rpos;
}

size_t nw_buf_avail(nw_buf *buf)
{
    return buf->size - buf->wpos;
}

size_t nw_buf_write(nw_buf *buf, const void *data, size_t len)
{
    size_t available = buf->size - buf->wpos;
    size_t wlen = len > available ? available : len;
    memcpy(buf->data + buf->wpos, data, wlen);
    buf->wpos += wlen;
    return wlen;
}

void nw_buf_shift(nw_buf *buf)
{
    if (buf->rpos == buf->wpos) {
        buf->rpos = buf->wpos = 0;
    } else if (buf->rpos != 0) {
        memmove(buf->data, buf->data + buf->rpos, buf->wpos - buf->rpos);
        buf->wpos -= buf->rpos;
        buf->rpos = 0;
    }
}

nw_buf_pool *nw_buf_pool_create(uint32_t size)
{
    if (size == 0)
        return NULL;
    nw_buf_pool *pool = malloc(sizeof(nw_buf_pool));
    if (pool == NULL)
        return NULL;

    pool->size = size;
    pool->used = 0;
    pool->keep = NW_KEEP_COUNT;
    pool->free = 0;
    pool->free_total = NW_BUF_POOL_INIT_SIZE;
    pool->free_arr = malloc(pool->free_total * sizeof(nw_buf *));
    if (pool->free_arr == NULL) {
        free(pool);
        return NULL;
    }

    return pool;
}

nw_buf *nw_buf_alloc(nw_buf_pool *pool)
{
    nw_buf *buf;
    if (pool->free) {
        buf = pool->free_arr[--pool->free];
    } else {
        buf = malloc(sizeof(nw_buf) + pool->size);
        if (buf == NULL)
            return NULL;
    }
    pool->used++;
    buf->size = pool->size;
    buf->rpos = 0;
    buf->wpos = 0;
    buf->next = NULL;

    return buf;
}

void nw_buf_free(nw_buf_pool *pool, nw_buf *buf)
{
    if (pool->used > 0)
        pool->used--;
    free_list_put((void ***)&pool->free_arr, &pool->free, &pool->free_total, pool->used, pool->keep, buf);
}

void nw_buf_pool_release(nw_buf_pool *pool)
{
    for (uint32_t i = 0; i < pool->free; ++i) {
        free(pool->free_arr[i]);
    }
    free(pool->free_arr);
    free(pool);
}

nw_buf_list *nw_buf_list_create(nw_buf_pool *pool, uint32_t limit)
{
    nw_buf_list *list = malloc(sizeof(nw_buf_list));
    if (list == NULL)
        return NULL;
    list->pool = pool;
    list->count = 0;
    list->limit = limit;
    list->head = NULL;
    list->tail = NULL;

    return list;
}

size_t nw_buf_list_write(nw_buf_list *list, const void *data, size_t len)
{
    const void *pos = data;
    size_t left = len;

    if (list->tail && nw_buf_avail(list->tail)) {
        size_t ret = nw_buf_write(list->tail, pos, left);
        left -= ret;
        pos += ret;
    }

    while (left) {
        if (list->limit && list->count >= list->limit)
            return len - left;
        nw_buf *buf = nw_buf_alloc(list->pool);
        if (buf == NULL)
            return len - left;
        if (list->head == NULL)
            list->head = buf;
        if (list->tail != NULL)
            list->tail->next = buf;
        list->tail = buf;
        list->count++;
        size_t ret = nw_buf_write(list->tail, pos, left);
        left -= ret;
        pos += ret;
    }

    return len;
}

size_t nw_buf_list_append(nw_buf_list *list, const void *data, size_t len)
{
    if (list->limit && list->count >= list->limit)
        return 0;
    nw_buf *buf = nw_buf_alloc(list->pool);
    if (buf == NULL)
        return 0;
    if (len > buf->size) {
        nw_buf_free(list->pool, buf);
        return 0;
    }
    nw_buf_write(buf, data, len);
    if (list->head == NULL)
        list->head = buf;
    if (list->tail != NULL)
        list->tail->next = buf;
    list->tail = buf;
    list->count++;

    return len;
}

void nw_buf_list_shift(nw_buf_list *list)
{
    if (list->head) {
        nw_buf *tmp = list->head;
        list->head = tmp->next;
        if (list->head == NULL) {
            list->tail = NULL;
        }
        list->count--;
        nw_buf_free(list->pool, tmp);
    }
}

void nw_buf_list_release(nw_buf_list *list)
{
    nw_buf *curr = list->head;
    nw_buf *next = NULL;
    while (curr) {
        next = curr->next;
        nw_buf_free(list->pool, curr);
        curr = next;
    }
    free(list);
}


nw_cache *nw_cache_create(uint32_t size)
{
    nw_cache *cache = malloc(sizeof(nw_cache));
    if (cache == NULL)
        return NULL;

    cache->size = size;
    cache->used = 0;
    cache->keep = NW_KEEP_COUNT;
    cache->free = 0;
    cache->free_total = NW_CACHE_INIT_SIZE;
    cache->free_arr = malloc(cache->free_total * sizeof(void *));
    if (cache->free_arr == NULL) {
        free(cache);
        return NULL;
    }

    return cache;
}

void *nw_cache_alloc(nw_cache *cache)
{
    void *obj;
    if (cache->free) {
        obj = cache->free_arr[--cache->free];
    } else {
        obj = malloc(cache->size);
        if (obj == NULL)
            return NULL;
    }
    cache->used++;
    return obj;
}

void nw_cache_free(nw_cache *cache, void *obj)
{
    if (cache->used > 0)
        cache->used--;
    free_list_put(&cache->free_arr, &cache->free, &cache->free_total, cache->used, cache->keep, obj);
}

void nw_cache_release(nw_cache *cache)
{
    for (uint32_t i = 0; i < cache->free; ++i) {
        free(cache->free_arr[i]);
    }
    free(cache->free_arr);
    free(cache);
}
