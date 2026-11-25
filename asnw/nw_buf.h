/*
 * Description: buffers: fixed size buf, buf pool, buf list and object cache
 *     History: yang@haipo.me, 2016/03/16, create
 */

# ifndef _NW_BUF_H_
# define _NW_BUF_H_

# include <stdint.h>
# include <stdlib.h>

/*
 * these are the building blocks nw_ses uses for its read buffer and pending
 * send list. you only need them directly to share one nw_buf_pool between
 * several clients (nw_clt_cfg.buf_pool) or to use nw_cache for your own
 * fixed size objects. none of it is thread safe.
 */

/* nw_buf is a fixed size buffer. the valid data is data[rpos, wpos) */
typedef struct nw_buf {
    /* capacity of data */
    uint32_t size;
    /* read position: start of the data not consumed yet */
    uint32_t rpos;
    /* write position: end of the data, new data goes here */
    uint32_t wpos;
    /* next buf when linked in a nw_buf_list */
    struct nw_buf *next;
    char data[];
} nw_buf;

/*
 * nw_buf_pool is a factory of nw_buf of one size. freed bufs are kept for
 * reuse, but not without end: no more are kept waiting than are in use at
 * the moment, plus `keep`. so the pool follows the load down as well as
 * up - after a peak the bufs of the peak are given back to the allocator
 * as the load falls - while a steady load never reaches the allocator.
 *
 * keep is 64: what an idle pool holds on to.
 */
typedef struct nw_buf_pool {
    /* capacity of every buf of this pool */
    uint32_t size;
    /* number of bufs handed out and not given back yet */
    uint32_t used;
    /* number of bufs kept beyond the ones that match `used`, see above */
    uint32_t keep;
    /* number of bufs waiting for reuse in free_arr */
    uint32_t free;
    /* capacity of free_arr, grows on demand */
    uint32_t free_total;
    nw_buf **free_arr;
} nw_buf_pool;

/* nw_buf_list is a fifo of nw_buf from one pool: data is added at the tail
 * and consumed from the head */
typedef struct nw_buf_list {
    nw_buf_pool *pool;
    /* number of bufs in the list */
    uint32_t count;
    /* max number of bufs, 0 for no limit. the list then holds at most
     * limit * pool->size bytes */
    uint32_t limit;
    nw_buf *head;
    nw_buf *tail;
} nw_buf_list;

/* nw_cache is a factory of untyped objects of one size, with the same reuse
 * behaviour as nw_buf_pool */
typedef struct nw_cache {
    /* size of every object */
    uint32_t size;
    /* number of objects handed out and not given back yet */
    uint32_t used;
    /* number of objects kept beyond the ones that match `used` */
    uint32_t keep;
    /* number of objects waiting for reuse in free_arr */
    uint32_t free;
    /* capacity of free_arr, grows on demand */
    uint32_t free_total;
    void **free_arr;
} nw_cache;

/* nw_buf operation */

/* bytes of data in the buf: wpos - rpos */
size_t nw_buf_size(nw_buf *buf);
/* bytes that can still be written: size - wpos */
size_t nw_buf_avail(nw_buf *buf);
/* copy data to the end of the buf. return the bytes copied, less than len
 * if the buf is full */
size_t nw_buf_write(nw_buf *buf, const void *data, size_t len);
/* move the data to the start of the buf so that the space of the consumed
 * part can be written again */
void nw_buf_shift(nw_buf *buf);

/* nw_buf_pool operation */

/* create a pool of bufs with `size` bytes of data each. NULL if size is 0
 * or out of memory */
nw_buf_pool *nw_buf_pool_create(uint32_t size);
/* get an empty buf, reused or newly allocated. NULL if out of memory */
nw_buf *nw_buf_alloc(nw_buf_pool *pool);
/* give a buf back to the pool it was allocated from. it is kept for reuse
 * or freed, see nw_buf_pool */
void nw_buf_free(nw_buf_pool *pool, nw_buf *buf);
/* free the pool and the bufs waiting for reuse. bufs still in use must be
 * given back first */
void nw_buf_pool_release(nw_buf_pool *pool);

/* nw_buf_list operation */

/* create an empty list taking its bufs from pool, at most `limit` of them
 * (0: no limit). NULL if out of memory */
nw_buf_list *nw_buf_list_create(nw_buf_pool *pool, uint32_t limit);
/* stream write: fill the tail buf, then add bufs as needed, data may be
 * split across bufs. return the bytes written, less than len if the limit
 * is reached or memory runs out (what was written stays in the list) */
size_t nw_buf_list_write(nw_buf_list *list, const void *data, size_t len);
/* packet write: put data in a new buf of its own, never split. return len,
 * or 0 if len is bigger than the buf size, the limit is reached or memory
 * runs out */
size_t nw_buf_list_append(nw_buf_list *list, const void *data, size_t len);
/* remove the head buf, if any, and give it back to the pool */
void nw_buf_list_shift(nw_buf_list *list);
/* free the list and give all its bufs back to the pool */
void nw_buf_list_release(nw_buf_list *list);

/* nw_cache operation */

/* create a cache of objects of `size` bytes. NULL if out of memory */
nw_cache *nw_cache_create(uint32_t size);
/* get an object, reused or newly allocated. the content is NOT zeroed.
 * NULL if out of memory */
void *nw_cache_alloc(nw_cache *cache);
/* give an object back to the cache it was allocated from */
void nw_cache_free(nw_cache *cache, void *obj);
/* free the cache and the objects waiting for reuse. objects still in use
 * must be given back first */
void nw_cache_release(nw_cache *cache);

# endif
