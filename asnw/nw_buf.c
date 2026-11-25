/*
 * Description: buffers: fixed size buf, buf pool, buf list and object cache
 *     History: yang@haipo.me, 2016/03/16, create
 */

# include <string.h>
# include "nw_buf.h"

# define NW_BUF_POOL_INIT_SIZE 64
# define NW_CACHE_INIT_SIZE    64


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

/* the buf is always kept here, never freed on the spot: whoever gave it
 * back may still be looking at its data (a callback that closed its own
 * connection while handling a message from it). what is too much goes in
 * nw_buf_pool_trim, when nobody is looking */
void nw_buf_free(nw_buf_pool *pool, nw_buf *buf)
{
    if (pool->used > 0)
        pool->used--;
    if (pool->free == pool->free_total) {
        uint32_t new_total = pool->free_total * 2;
        void *new_arr = realloc(pool->free_arr, new_total * sizeof(nw_buf *));
        if (new_arr == NULL) {
            free(buf);
            return;
        }
        pool->free_arr = new_arr;
        pool->free_total = new_total;
    }
    pool->free_arr[pool->free++] = buf;
}

static uint32_t surplus(nw_buf_pool *pool)
{
    uint32_t wanted = pool->used + NW_BUF_POOL_KEEP;
    return pool->free > wanted ? pool->free - wanted : 0;
}

uint32_t nw_buf_pool_trim(nw_buf_pool *pool, uint32_t max)
{
    uint32_t count = surplus(pool);
    if (count > max)
        count = max;
    for (uint32_t i = 0; i < count; ++i)
        free(pool->free_arr[--pool->free]);
    /* the array of pointers comes down as well */
    while (pool->free_total > NW_BUF_POOL_INIT_SIZE && pool->free < pool->free_total / 4) {
        uint32_t new_total = pool->free_total / 2;
        void *new_arr = realloc(pool->free_arr, new_total * sizeof(nw_buf *));
        if (new_arr == NULL)
            break;
        pool->free_arr = new_arr;
        pool->free_total = new_total;
    }
    return surplus(pool);
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

/*
 * unlike a buf, an object of a cache is never given back to the allocator
 * while the cache lives, however many are waiting. that is not thrift, it
 * is a guarantee the rest of the library is built on: the memory of an
 * object that was freed stays readable and keeps its type. a pointer to a
 * closed session can still be asked whether it is closed, an iterator
 * standing on a deleted dict entry can still step off it. freeing here
 * turns those into use after free.
 */
void nw_cache_free(nw_cache *cache, void *obj)
{
    if (cache->used > 0)
        cache->used--;
    if (cache->free == cache->free_total) {
        uint32_t new_total = cache->free_total * 2;
        void *new_arr = realloc(cache->free_arr, new_total * sizeof(void *));
        if (new_arr == NULL) {
            /* the one case where the guarantee can't be kept: leak the
             * object rather than free it */
            return;
        }
        cache->free_arr = new_arr;
        cache->free_total = new_total;
    }
    cache->free_arr[cache->free++] = obj;
}

void nw_cache_release(nw_cache *cache)
{
    for (uint32_t i = 0; i < cache->free; ++i) {
        free(cache->free_arr[i]);
    }
    free(cache->free_arr);
    free(cache);
}
