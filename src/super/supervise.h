/* Per-child supervision state for dlby-super (DESIGN.md §2 E). Pure state: no
 * syscalls, time passed in, so the timing and exit-code rules are unit-tested.
 *
 *   hang:    no heartbeat within SUP_HB_FIRST_NS of start, or SUP_HB_SILENCE_NS
 *            of silence after one -> TERM; still alive SUP_KILL_GRACE_NS later -> KILL
 *   restart: backoff 100 ms doubling to 30 s, reset after 60 s healthy; never gives up
 *   exits:   0 = stopped on request, no restart; 78 EX_CONFIG = don't restart;
 *            69, 70, any other code or any signal = restart. An exit after a kill
 *            super itself started is always restarted.
 */
#ifndef DLBY_SUPERVISE_H
#define DLBY_SUPERVISE_H

#include <stdbool.h>
#include <stdint.h>

#define SUP_HB_FIRST_NS    (10LL * 1000000000LL)
#define SUP_HB_SILENCE_NS  (5LL * 1000000000LL)
#define SUP_KILL_GRACE_NS  (500LL * 1000000LL)
#define SUP_HEALTHY_NS     (60LL * 1000000000LL)
#define SUP_BACKOFF_MIN_MS 100
#define SUP_BACKOFF_MAX_MS 30000

enum child_state {
	CH_IDLE,		/* never started: start now */
	CH_RUNNING,
	CH_TERMINATING,		/* SIGTERM sent after a lost heartbeat */
	CH_KILLING,		/* SIGKILL sent */
	CH_WAIT_RESTART,
	CH_DISABLED,		/* exited 78 */
	CH_STOPPED,		/* exited 0 on its own */
};

enum sup_action {
	SUP_NONE,
	SUP_START,
	SUP_TERM,
	SUP_KILL,
};

enum sup_exit {
	SUP_EXIT_RESTART,
	SUP_EXIT_DISABLED,
	SUP_EXIT_STOPPED,
};

struct child_sm {
	enum child_state state;
	int64_t start_ns;
	int64_t last_hb_ns;
	int64_t kill_at_ns;
	int64_t restart_at_ns;
	bool    got_hb;
	bool    killed_by_us;
	int     backoff_ms;	/* delay for the next restart */
	int     delay_ms;	/* delay chosen for the pending restart */
};

void sup_init(struct child_sm *c);
void sup_on_start(struct child_sm *c, int64_t now_ns);

/* Returns true for the first heartbeat since start. */
bool sup_on_hb(struct child_sm *c, int64_t now_ns);

/* status is a waitpid() status. */
enum sup_exit sup_on_exit(struct child_sm *c, int status, int64_t now_ns);

enum sup_action sup_tick(struct child_sm *c, int64_t now_ns);

/* Earliest time sup_tick() may return something other than SUP_NONE. */
int64_t sup_deadline(const struct child_sm *c);

#endif
