/* Fusion state machine for fuse-hub (DESIGN.md §1). Pure state: no I/O, time is
 * passed in, so it is unit-tested directly and the replay harness can drive it.
 *
 * Inputs: radar and sonar messages as received, and each evaluation's up-mask
 * from health.c ((1u << enum sensor) per sensor that is up). Output: one
 * enum fuse_state per side of the car, one per mirror LED (alert_msg.state is
 * the right side, alert_msg.state_left the left).
 *
 *   track    an approaching radar target (speed <= FUSE_APPROACH_CMS, range <=
 *            FUSE_WATCH_CM), valid for FUSE_TRACK_TTL_NS after the last frame
 *            that had one. Per radar.
 *   zone     >= 2 of the last 3 pings hit (no SONAR_NO_ECHO / STUCK_HIGH, range
 *            FUSE_ZONE_MIN_CM..FUSE_ZONE_MAX_CM), so one dropout doesn't flicker.
 *   confirm  a right-radar track seen within FUSE_CONFIRM_NS.
 *
 * Right side (right radar + the ultrasonic):
 *   both up        zone && confirm -> IN_ZONE; track -> APPROACHING; else CLEAR.
 *                  zone without confirm stays CLEAR and is counted in
 *                  zone_unconfirmed (a pole, a parked car).
 *   radar lost     zone -> DEG_IN_ZONE; else DEG_CLEAR.
 *   sonar lost     track -> DEG_APPROACHING; else DEG_CLEAR.
 *   both lost      BLIND.
 * Left side (radar only, no ultrasonic):
 *   up             track -> APPROACHING; else CLEAR. Never IN_ZONE.
 *   lost           BLIND: that side is unwatched (DESIGN.md §3).
 * Neither side's sensors change the other side's state.
 *
 * Hold, per side: while that side's sensors' up-bits are unchanged, a drop to a
 * less urgent state takes effect only after hold_ns without support, measured
 * from the first evaluation that sees the drop. Health changes apply at once.
 *
 * FUSE_RADAR_PRESENCE is the fallback for radar frames without decoded targets:
 * radars count only for health, never track or confirm, and the zone alone
 * gives IN_ZONE. The left side can then show only CLEAR or BLIND.
 */
#ifndef DLBY_FUSE_H
#define DLBY_FUSE_H

#include "health.h"
#include "msg.h"

#include <stdbool.h>
#include <stdint.h>

#define FUSE_APPROACH_CMS   (-50)		/* >= 1.8 km/h closing */
#define FUSE_WATCH_CM       3000
#define FUSE_TRACK_TTL_NS   1500000000LL
#define FUSE_CONFIRM_NS     3000000000LL
#define FUSE_ZONE_MIN_CM    20
#define FUSE_ZONE_MAX_CM    430		/* DESIGN.md §1: no echo inside 4.3 m */
#define FUSE_HOLD_NS        500000000LL

#define FUSE_RADAR_ENV "DLBY_FUSE_RADAR"

enum fuse_radar_mode {
	FUSE_RADAR_TRACKS,
	FUSE_RADAR_PRESENCE,
};

enum fuse_side {
	FUSE_RIGHT = 0,
	FUSE_LEFT  = 1,
};

/* What one side shows, and its hold. */
struct fuse_hold {
	enum fuse_state state;
	unsigned last_up;		/* this side's sensors' up-bits */
	bool     dropping;		/* less urgent raw state, waiting out hold */
	int64_t  drop_ns;
};

struct fuse_out {
	enum fuse_state right;
	enum fuse_state left;
};

struct fuse {
	enum fuse_radar_mode mode;
	int64_t hold_ns;

	bool     track_seen[2];		/* enum fuse_side */
	int64_t  track_ns[2];
	uint16_t track_cm[2];		/* nearest approaching target, last frame */

	uint8_t  pings;			/* last 3 pings, bit 0 newest, 1 = hit */

	struct fuse_hold side[2];	/* enum fuse_side */

	bool     unconfirmed;		/* right zone without confirm, last evaluation */
	uint64_t zone_unconfirmed;	/* times that began */
};

void fuse_init(struct fuse *f, enum fuse_radar_mode mode, int64_t hold_ns);

/* s is SENSOR_RADAR or SENSOR_RADAR_LEFT; anything else is ignored. */
void fuse_radar(struct fuse *f, enum sensor s, const struct radar_msg *m, int64_t now_ns);
void fuse_sonar(struct fuse *f, const struct sonar_msg *m, int64_t now_ns);

/* up: (1u << enum sensor) for each sensor up. Fills the state each side shows. */
void fuse_eval(struct fuse *f, unsigned up, int64_t now_ns, struct fuse_out *out);

/* NULL or "" -> tracks; "tracks" | "presence"; else -1 with EINVAL. */
int fuse_parse_mode(const char *s, enum fuse_radar_mode *out);

const char *fuse_mode_name(enum fuse_radar_mode m);
const char *fuse_state_name(enum fuse_state s);

/* Echo width to range, at 343 m/s. */
uint32_t fuse_echo_cm(uint32_t echo_ns);

#endif
