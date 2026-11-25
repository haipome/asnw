/*
 * Description: the event loop, a thin layer over libev
 *     History: yang@haipo.me, 2016/03/19, create
 */

# ifndef _NW_EVT_H_
# define _NW_EVT_H_

# include "ev.h"

/*
 * asnw runs on one global libev loop. everything in the library - servers,
 * clients, timers, states, job results - is driven by it and runs on the
 * thread that calls nw_loop_run. the library is not thread safe.
 */

/* the loop instance, valid after nw_loop_init. use it to add your own
 * libev watchers to the same loop */
extern struct ev_loop *nw_default_loop;

/* create the loop. calling it again is a no-op. nw_svr_create, nw_clt_create,
 * nw_timer_set, nw_state_create and nw_job_create call it for you */
void nw_loop_init(void);

/* run the loop. returns after nw_loop_break, or when nothing is left to
 * wait for (no listening server, connection or active timer) */
void nw_loop_run(void);

/* make nw_loop_run return once the current callback is done */
void nw_loop_break(void);

/* make nw_loop_run return after waittime seconds, e.g. to let pending data
 * go out before exit. only has an effect while the loop is running, i.e.
 * when called from a callback */
void nw_loop_break_later(double waittime);

# endif
