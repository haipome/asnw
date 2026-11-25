/* nw_timer: one-shot, repeat, stop, remaining */
# include "common.h"

static int once_count;
static int repeat_count;

static void on_once(nw_timer *timer, void *privdata)
{
    once_count++;
    CHECK(privdata == &once_count);
}

static void on_repeat(nw_timer *timer, void *privdata)
{
    repeat_count++;
    if (repeat_count == 5)
        nw_timer_stop(timer);
}

int main(void)
{
    nw_timer once, repeat, never;
    nw_timer_set(&once, 0.02, false, on_once, &once_count);
    nw_timer_set(&repeat, 0.01, true, on_repeat, NULL);
    nw_timer_set(&never, 0.03, false, on_once, &once_count);

    CHECK(!nw_timer_active(&once));
    nw_timer_start(&once);
    nw_timer_start(&once);              /* double start is a no-op */
    nw_timer_start(&repeat);
    nw_timer_start(&never);
    CHECK(nw_timer_active(&once));
    CHECK(nw_timer_remaining(&once) > 0.0 && nw_timer_remaining(&once) < 0.021);
    nw_timer_stop(&never);
    nw_timer_stop(&never);              /* double stop is a no-op */

    nw_loop_run();                      /* returns once no watcher is left */

    CHECK_EQ(once_count, 1);
    CHECK_EQ(repeat_count, 5);
    CHECK(!nw_timer_active(&once));
    CHECK(!nw_timer_active(&repeat));

    return 0;
}
