/* Wire messages between dlby processes (DESIGN.md §1, docs/architecture.svg).
 *
 * Senders must zero-initialize every message (memset to 0) before filling it in,
 * so pad bytes are never uninitialized: messages go over IPC and into the store
 * log byte-for-byte, and valgrind flags uninitialized bytes passed to send().
 *
 * Every message starts with `u8 type` at offset 0 and `u32 seq` (per sender) at
 * offset 4, so a receiver can read the type before it knows which struct it has.
 * All but the heartbeat carry origin_ns (CLOCK_MONOTONIC) at offset 8. Sizes are
 * fixed and checked below; a receiver validates type against length with
 * ipc_check_msg(). Fields are naturally aligned with explicit padding.
 */
#ifndef DLBY_MSG_H
#define DLBY_MSG_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum msg_type {
	MSG_RADAR = 1,
	MSG_SONAR = 2,
	MSG_ALERT = 3,
	MSG_HB    = 4,
};

/* Fusion states carried to `alert` (DESIGN.md §1). HUB_LOST is alert's own. */
enum fuse_state {
	ST_BLIND = 0,
	ST_CLEAR,
	ST_APPROACHING,
	ST_IN_ZONE,
	ST_DEG_CLEAR,
	ST_DEG_APPROACHING,
	ST_DEG_IN_ZONE,
};

#define LOST_RADAR 0x01u
#define LOST_SONAR 0x02u

/* Radar target. PROVISIONAL until the LD2451 frame format is checked (Gage). */
#define RADAR_MAX_TARGETS 6
struct radar_target {
	uint16_t range_cm;
	int16_t  speed_cms;	/* negative = approaching */
	int8_t   angle_deg;
	uint8_t  dir;
	uint16_t snr;
};

struct radar_msg {		/* cap-radar -> fuse-hub, 10 Hz */
	uint8_t  type;		/* MSG_RADAR */
	uint8_t  ntargets;
	uint8_t  pad[2];
	uint32_t seq;
	uint64_t origin_ns;	/* user-space read time */
	struct radar_target t[RADAR_MAX_TARGETS];
};

#define SONAR_NO_ECHO    0x01u	/* no echo inside 4.3 m */
#define SONAR_STUCK_HIGH 0x02u	/* echo high > 60 ms */

struct sonar_msg {		/* cap-sonar -> fuse-hub, 16 Hz */
	uint8_t  type;		/* MSG_SONAR */
	uint8_t  flags;
	uint8_t  pad[2];
	uint32_t seq;
	uint64_t origin_ns;	/* kernel timestamp of the rising echo edge */
	uint32_t echo_ns;	/* rising -> falling edge */
	uint32_t trigger_seq;
};

struct alert_msg {		/* fuse-hub -> alert, on change + 2 Hz */
	uint8_t  type;		/* MSG_ALERT */
	uint8_t  state;		/* enum fuse_state */
	uint8_t  lost_mask;	/* LOST_* */
	uint8_t  pad;
	uint32_t seq;
	uint64_t origin_ns;	/* origin of the reading that caused this state */
};

/* Heartbeats only travel on dlby-super's per-child socketpairs. */
struct hb_msg {
	uint8_t  type;		/* MSG_HB */
	uint8_t  pad[3];
	uint32_t seq;
};

#define MSG_MAX 64

static_assert(sizeof(struct radar_target) == 8, "radar_target size");
static_assert(sizeof(struct radar_msg) == 64, "radar_msg size");
static_assert(sizeof(struct sonar_msg) == 24, "sonar_msg size");
static_assert(sizeof(struct alert_msg) == 16, "alert_msg size");
static_assert(sizeof(struct hb_msg) == 8, "hb_msg size");
static_assert(offsetof(struct radar_msg, seq) == 4 && offsetof(struct radar_msg, origin_ns) == 8,
	      "radar_msg header");
static_assert(offsetof(struct sonar_msg, seq) == 4 && offsetof(struct sonar_msg, origin_ns) == 8,
	      "sonar_msg header");
static_assert(offsetof(struct alert_msg, seq) == 4 && offsetof(struct alert_msg, origin_ns) == 8,
	      "alert_msg header");
static_assert(offsetof(struct hb_msg, seq) == 4, "hb_msg header");

/* Expected length for a message type, or 0 if the type is unknown. */
static inline size_t msg_size(uint8_t type)
{
	switch (type) {
	case MSG_RADAR: return sizeof(struct radar_msg);
	case MSG_SONAR: return sizeof(struct sonar_msg);
	case MSG_ALERT: return sizeof(struct alert_msg);
	case MSG_HB:    return sizeof(struct hb_msg);
	default:        return 0;
	}
}

/* CLOCK_MONOTONIC in ns, or -1 with errno set. */
static inline int64_t dlby_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#endif
