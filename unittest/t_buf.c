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

    /* many at once, all given back: they wait, until the pool is trimmed
     * to what an idle pool keeps */
    nw_buf *many[200];
    for (int i = 0; i < 200; ++i)
        CHECK((many[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->used, 200);
    for (int i = 0; i < 200; ++i)
        nw_buf_free(pool, many[i]);
    CHECK_EQ(pool->used, 0);
    CHECK_EQ(pool->free, 200);
    CHECK_EQ(nw_buf_pool_trim(pool, 100), 36);      /* 100 freed, 36 more to go */
    CHECK_EQ(pool->free, 100);
    CHECK_EQ(nw_buf_pool_trim(pool, 100), 0);
    CHECK_EQ(pool->free, 64);
    CHECK_EQ(nw_buf_pool_trim(pool, 100), 0);       /* nothing left to trim */
    CHECK_EQ(pool->free, 64);

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
    /* a cache keeps them all, and what was freed stays readable */
    CHECK_EQ(cache->free, 200);
    CHECK_EQ(cache->used, 0);
    for (int i = 0; i < 200; ++i)
        CHECK_EQ(((unsigned char *)objs[i])[31], i);
    CHECK(nw_cache_alloc(cache) == objs[199]);
    nw_cache_free(cache, objs[199]);
    nw_cache_release(cache);

    /*
     * trimmed as the load changes, the way nw_svr does between the turns
     * of the loop, the free list follows the load down. an idle pool
     * keeps 64 bufs.
     */
    pool = nw_buf_pool_create(4096);
    CHECK(pool != NULL);
    CHECK_EQ(pool->keep, 64);
    static nw_buf *peak[3000];
    /* a steady load: nothing is given back to the allocator, the same
     * bufs go round */
    for (int i = 0; i < 20; ++i)
        CHECK((peak[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->used, 20);
    for (int round = 0; round < 50; ++round) {
        for (int i = 0; i < 10; ++i)
            nw_buf_free(pool, peak[i]);
        CHECK_EQ(nw_buf_pool_trim(pool, UINT32_MAX), 0);
        CHECK_EQ(pool->used, 10);
        CHECK_EQ(pool->free, 10);
        nw_buf *freed[10];
        memcpy(freed, peak, sizeof(freed));
        for (int i = 0; i < 10; ++i) {
            peak[i] = nw_buf_alloc(pool);
            bool reused = false;
            for (int j = 0; j < 10; ++j)
                reused = reused || peak[i] == freed[j];
            CHECK(reused);
        }
        CHECK_EQ(pool->free, 0);
    }
    /* a peak of 3000, then back down to 10 in use */
    for (int i = 20; i < 3000; ++i)
        CHECK((peak[i] = nw_buf_alloc(pool)) != NULL);
    CHECK_EQ(pool->used, 3000);
    uint32_t most_waiting = 0;
    for (int i = 2999; i >= 10; --i) {
        nw_buf_free(pool, peak[i]);
        /* a buf that was given back is not freed on the spot */
        CHECK_EQ(peak[i]->size, 4096);
        if (i % 7 == 0)
            nw_buf_pool_trim(pool, UINT32_MAX);
        /* never much more waiting than in use, plus keep */
        CHECK(pool->free <= pool->used + pool->keep + 14);
        if (pool->free > most_waiting)
            most_waiting = pool->free;
    }
    nw_buf_pool_trim(pool, UINT32_MAX);
    CHECK_EQ(pool->used, 10);
    CHECK_EQ(pool->free, 10 + 64);              /* not the 2990 of the peak */
    CHECK(most_waiting >= 1500 && most_waiting <= 1600);
    CHECK(pool->free_total <= 512);             /* the array shrank too */
    /* all the way down: what an idle pool keeps */
    for (int i = 0; i < 10; ++i)
        nw_buf_free(pool, peak[i]);
    nw_buf_pool_trim(pool, UINT32_MAX);
    CHECK_EQ(pool->used, 0);
    CHECK_EQ(pool->free, 64);
    /* and up again */
    for (int i = 0; i < 3000; ++i)
        CHECK((peak[i] = nw_buf_alloc(pool)) != NULL);
    for (int i = 0; i < 3000; ++i)
        nw_buf_free(pool, peak[i]);
    CHECK_EQ(pool->free, 3000);
    CHECK_EQ(nw_buf_pool_trim(pool, UINT32_MAX), 0);
    CHECK_EQ(pool->free, 64);
    nw_buf_pool_release(pool);


    return 0;
}
