/*
 * KNOWN BUG: nw_job_release() leaks.
 * nw_job_free() never calls nw_cache_release(job->cache), so the entry
 * cache and every cached nw_job_entry are leaked. Requests still queued
 * and replies not yet delivered at release time are dropped as well,
 * without on_cleanup being called for them.
 * Detected by LeakSanitizer (asan build) or valgrind.
 */
# include "common.h"

static int done;

static void on_job(nw_job_entry *entry, void *privdata)
{
}

static void on_finish(nw_job_entry *entry)
{
    if (++done == 10)
        nw_loop_break();
}

int main(void)
{
    nw_job_type type;
    memset(&type, 0, sizeof(type));
    type.on_job = on_job;
    type.on_finish = on_finish;
    nw_job *job = nw_job_create(&type, 2);
    CHECK(job != NULL);
    for (int i = 0; i < 10; ++i)
        CHECK_EQ(nw_job_add(job, i, NULL), 0);
    ut_run_for(5.0);
    CHECK_EQ(done, 10);
    nw_job_release(job);
    return 0;
}
