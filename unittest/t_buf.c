/* nw_buf / nw_buf_pool / nw_buf_list / nw_cache */
# include "common.h"

int main(void)
{
    nw_buf_pool *pool = nw_buf_pool_create(8);
    CHECK(pool != NULL);

    nw_buf *buf = nw_buf_alloc(pool);
    CHECK(buf != NULL);
    CHECK_EQ(nw_buf_size(buf), 0);
    CHECK_EQ(nw_buf_avail(buf), 8);
    CHECK_EQ(nw_buf_write(buf, "abcdef", 6), 6);
    CHECK_EQ(nw_buf_write(buf, "ghij", 4), 2);      /* truncated to capacity */
    CHECK_EQ(nw_buf_avail(buf), 0);
    buf->rpos = 3;
    nw_buf_shift(buf);
    CHECK_EQ(buf->rpos, 0);
    CHECK_EQ(buf->wpos, 5);
    CHECK(memcmp(buf->data, "defgh", 5) == 0);
    buf->rpos = buf->wpos;
    nw_buf_shift(buf);
    CHECK_EQ(buf->wpos, 0);

    /* a freed buf is reused, and comes back clean */
    buf->wpos = 4;
    nw_buf_free(pool, buf);
    nw_buf *again = nw_buf_alloc(pool);
    CHECK(again == buf);
    CHECK_EQ(again->wpos, 0);
    nw_buf_free(pool, again);

    /* free list grows past its initial 64 slots */
    nw_buf *many[200];
    for (int i = 0; i < 200; ++i)
        CHECK((many[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->used, 200);
    for (int i = 0; i < 200; ++i)
        nw_buf_free(pool, many[i]);
    CHECK_EQ(pool->free, 200);
    CHECK_EQ(pool->used, 0);

    /* nw_buf_pool_shrink: the caller says how many free bufs stay */
    CHECK_EQ(nw_buf_pool_shrink(pool, 150), 50);
    CHECK_EQ(pool->free, 150);
    CHECK_EQ(nw_buf_pool_shrink(pool, 150), 0);
    CHECK_EQ(nw_buf_pool_shrink(pool, 1000), 0);        /* more than there are */
    CHECK_EQ(nw_buf_pool_shrink(pool, 100), 50);
    CHECK_EQ(pool->free, 100);
    /* but never less than the minimum */
    CHECK_EQ(nw_buf_pool_shrink(pool, 0), 100 - NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(pool->free, NW_BUF_POOL_MIN_FREE);
    CHECK_EQ(nw_buf_pool_shrink(pool, 0), 0);
    CHECK_EQ(nw_buf_pool_shrink(pool, 10), 0);
    CHECK_EQ(pool->used, 0);
    /* bufs in use are not its business */
    nw_buf *more[100];
    for (int i = 0; i < 100; ++i)
        CHECK((more[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->free, 0);
    CHECK_EQ(nw_buf_pool_shrink(pool, 0), 0);
    CHECK_EQ(pool->used, 100);
    for (int i = 0; i < 100; ++i)
        nw_buf_free(pool, more[i]);
    CHECK_EQ(nw_buf_pool_shrink(pool, 80), 20);
    CHECK_EQ(pool->free, 80);
    /* and the pool goes on as before: the bufs kept are used first */
    again = nw_buf_alloc(pool);
    CHECK(again != NULL);
    CHECK_EQ(pool->free, 79);
    CHECK_EQ(again->size, 8);
    nw_buf_free(pool, again);

    /*
     * giving back never needs memory: the place a buf or an object goes
     * to when it comes back is made when it is handed out. (it used to be
     * made when it came back, and when that failed the object was freed
     * on the spot, while the library may still read it.)
     */
    {
        nw_buf_pool *p = nw_buf_pool_create(8);
        nw_cache *c = nw_cache_create(32);
        CHECK(p != NULL && c != NULL);
        enum { N = 1000 };
        static nw_buf *bufs[N];
        static void *things[N];
        for (int i = 0; i < N; ++i) {
            CHECK((bufs[i] = nw_buf_alloc(p)) != NULL);
            CHECK((things[i] = nw_cache_alloc(c)) != NULL);
            /* room for all that are out, at every moment */
            CHECK(p->free_total >= p->used + p->free);
            CHECK(c->free_total >= c->used + c->free);
        }
        uint32_t pool_room = p->free_total, cache_room = c->free_total;
        CHECK(pool_room >= N && cache_room >= N);
        for (int i = 0; i < N; ++i) {
            nw_buf_free(p, bufs[i]);
            nw_cache_free(c, things[i]);
        }
        /* all of them are kept, and nothing had to grow for it */
        CHECK_EQ(p->free, N);
        CHECK_EQ(c->free, N);
        CHECK_EQ(p->free_total, pool_room);
        CHECK_EQ(c->free_total, cache_room);
        /* taking them again and giving them back changes nothing either */
        for (int i = 0; i < N; ++i)
            CHECK((bufs[i] = nw_buf_alloc(p)) != NULL);
        CHECK_EQ(p->free_total, pool_room);
        for (int i = 0; i < N; ++i)
            nw_buf_free(p, bufs[i]);
        CHECK_EQ(p->free, N);
        nw_buf_pool_release(p);
        nw_cache_release(c);
    }

    /* stream write spans bufs and honours the limit */
    nw_buf_list *list = nw_buf_list_create(pool, 3);
    CHECK(list != NULL);
    CHECK_EQ(nw_buf_list_write(list, "0123456789", 10), 10);
    CHECK_EQ(list->count, 2);
    CHECK_EQ(nw_buf_list_write(list, "abcdefghijklmnopqrstuvwxyz", 26), 14); /* 24 bytes max */
    CHECK_EQ(list->count, 3);
    CHECK(memcmp(list->head->data, "01234567", 8) == 0);
    CHECK(memcmp(list->head->next->data, "89abcdef", 8) == 0);
    CHECK(memcmp(list->tail->data, "ghijklmn", 8) == 0);
    nw_buf_list_shift(list);
    CHECK_EQ(list->count, 2);
    nw_buf_list_shift(list);
    nw_buf_list_shift(list);
    CHECK_EQ(list->count, 0);
    CHECK(list->head == NULL && list->tail == NULL);
    nw_buf_list_shift(list);                        /* no-op on empty list */

    /* packet append: one packet per buf, oversize rejected, limit honoured */
    CHECK_EQ(nw_buf_list_append(list, "123456789", 9), 0);
    CHECK_EQ(list->count, 0);
    CHECK_EQ(nw_buf_list_append(list, "abc", 3), 3);
    CHECK_EQ(nw_buf_list_append(list, "de", 2), 2);
    CHECK_EQ(nw_buf_list_append(list, "f", 1), 1);
    CHECK_EQ(nw_buf_list_append(list, "g", 1), 0);
    CHECK_EQ(nw_buf_size(list->head), 3);
    CHECK_EQ(nw_buf_size(list->tail), 1);
    nw_buf_list_release(list);
    nw_buf_pool_release(pool);

    nw_cache *cache = nw_cache_create(32);
    CHECK(cache != NULL);
    void *objs[200];
    for (int i = 0; i < 200; ++i) {
        CHECK((objs[i] = nw_cache_alloc(cache)) != NULL);
        memset(objs[i], i, 32);
    }
    for (int i = 0; i < 200; ++i)
        nw_cache_free(cache, objs[i]);
    CHECK_EQ(cache->free, 200);
    CHECK(nw_cache_alloc(cache) == objs[199]);
    nw_cache_free(cache, objs[199]);

    /* nw_cache_shrink, the same as nw_buf_pool_shrink: the caller says how
     * many free objects stay, and the minimum stays whatever it says */
    CHECK_EQ(cache->free, 200);
    CHECK_EQ(nw_cache_shrink(cache, 150), 50);
    CHECK_EQ(cache->free, 150);
    CHECK_EQ(nw_cache_shrink(cache, 150), 0);
    CHECK_EQ(nw_cache_shrink(cache, 1000), 0);
    CHECK_EQ(nw_cache_shrink(cache, 0), 150 - NW_CACHE_MIN_FREE);
    CHECK_EQ(cache->free, NW_CACHE_MIN_FREE);
    CHECK_EQ(nw_cache_shrink(cache, 10), 0);
    CHECK_EQ(cache->used, 0);
    /* objects in use are not its business */
    for (int i = 0; i < 100; ++i)
        CHECK((objs[i] = nw_cache_alloc(cache)) != NULL);
    CHECK_EQ(cache->free, 0);
    CHECK_EQ(nw_cache_shrink(cache, 0), 0);
    CHECK_EQ(cache->used, 100);
    for (int i = 0; i < 100; ++i) {
        memset(objs[i], 0x5a, 32);              /* still ours to write to */
        nw_cache_free(cache, objs[i]);
    }
    CHECK_EQ(nw_cache_shrink(cache, 80), 20);
    CHECK_EQ(cache->free, 80);
    /* and the cache goes on as before */
    void *obj = nw_cache_alloc(cache);
    CHECK(obj != NULL);
    CHECK_EQ(cache->free, 79);
    nw_cache_free(cache, obj);
    nw_cache_release(cache);

    return 0;
}
