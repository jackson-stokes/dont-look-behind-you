#include "fuse.h"

#include <errno.h>
#include <string.h>

#define UP(s) (1u << (s))

/* Each side's sensors, for its hold. */
#define RIGHT_MASK (UP(SENSOR_RADAR) | UP(SENSOR_SONAR))
#define LEFT_MASK  UP(SENSOR_RADAR_LEFT)

void fuse_init(struct fuse *f, enum fuse_radar_mode mode, int64_t hold_ns)
{
	memset(f, 0, sizeof(*f));
	f->mode = mode;
	f->hold_ns = hold_ns < 0 ? 0 : hold_ns;
	f->side[FUSE_RIGHT].state = ST_BLIND;
	f->side[FUSE_LEFT].state = ST_BLIND;
}

uint32_t fuse_echo_cm(uint32_t echo_ns)
{
	/* cm = echo_ns * 1e-9 s * 34300 cm/s / 2 */
	return (uint32_t)((uint64_t)echo_ns * 343 / 20000000);
}

void fuse_radar(struct fuse *f, enum sensor s, const struct radar_msg *m, int64_t now_ns)
{
	unsigned i, n = m->ntargets < RADAR_MAX_TARGETS ? m->ntargets : RADAR_MAX_TARGETS;
	int side;
	uint16_t best = 0;
	bool found = false;

	if (f->mode == FUSE_RADAR_PRESENCE)
		return;
	if (s == SENSOR_RADAR)
		side = FUSE_RIGHT;
	else if (s == SENSOR_RADAR_LEFT)
		side = FUSE_LEFT;
	else
		return;
	for (i = 0; i < n; i++) {
		const struct radar_target *t = &m->t[i];

		if (t->speed_cms > FUSE_APPROACH_CMS || t->range_cm > FUSE_WATCH_CM)
			continue;
		if (!found || t->range_cm < best)
			best = t->range_cm;
		found = true;
	}
	if (found) {
		f->track_seen[side] = true;
		f->track_ns[side] = now_ns;
		f->track_cm[side] = best;
	}
}

void fuse_sonar(struct fuse *f, const struct sonar_msg *m, int64_t now_ns)
{
	uint32_t cm = fuse_echo_cm(m->echo_ns);
	bool hit = !(m->flags & (SONAR_NO_ECHO | SONAR_STUCK_HIGH)) &&
		   cm >= FUSE_ZONE_MIN_CM && cm <= FUSE_ZONE_MAX_CM;

	(void)now_ns;
	f->pings = (uint8_t)(((f->pings << 1) | (hit ? 1u : 0u)) & 0x7u);
}

static bool track_valid(const struct fuse *f, int side, int64_t now_ns)
{
	return f->track_seen[side] && now_ns - f->track_ns[side] <= FUSE_TRACK_TTL_NS;
}

static bool zone_occupied(const struct fuse *f)
{
	unsigned p = f->pings;

	return ((p & 1u) + ((p >> 1) & 1u) + ((p >> 2) & 1u)) >= 2;
}

/* 0 = nothing to warn about, 1 = approaching, 2 = in zone. */
static int urgency(enum fuse_state s)
{
	switch (s) {
	case ST_APPROACHING:
	case ST_DEG_APPROACHING:
		return 1;
	case ST_IN_ZONE:
	case ST_DEG_IN_ZONE:
		return 2;
	default:
		return 0;
	}
}

/* Right side: the right radar and the ultrasonic. */
static enum fuse_state raw_right(struct fuse *f, unsigned up, int64_t now_ns)
{
	bool r = up & UP(SENSOR_RADAR), s = up & UP(SENSOR_SONAR);
	bool track = r && track_valid(f, FUSE_RIGHT, now_ns);
	bool confirm = r && f->track_seen[FUSE_RIGHT] &&
		       now_ns - f->track_ns[FUSE_RIGHT] <= FUSE_CONFIRM_NS;
	bool zone = s && zone_occupied(f);
	bool unconfirmed = false;
	enum fuse_state st;

	if (r && s) {
		if (zone && (f->mode == FUSE_RADAR_PRESENCE || confirm))
			st = ST_IN_ZONE;
		else if (track)
			st = ST_APPROACHING;
		else
			st = ST_CLEAR;
		unconfirmed = zone && st != ST_IN_ZONE;
	} else if (s) {			/* radar lost: the ultrasonic alone */
		st = zone ? ST_DEG_IN_ZONE : ST_DEG_CLEAR;
	} else if (r) {			/* ultrasonic lost: the radar alone */
		st = track ? ST_DEG_APPROACHING : ST_DEG_CLEAR;
	} else {
		st = ST_BLIND;
	}

	if (unconfirmed && !f->unconfirmed)
		f->zone_unconfirmed++;
	f->unconfirmed = unconfirmed;
	return st;
}

/* Left side: the left radar only. */
static enum fuse_state raw_left(const struct fuse *f, unsigned up, int64_t now_ns)
{
	if (!(up & UP(SENSOR_RADAR_LEFT)))
		return ST_BLIND;
	return track_valid(f, FUSE_LEFT, now_ns) ? ST_APPROACHING : ST_CLEAR;
}

static enum fuse_state hold(struct fuse_hold *h, enum fuse_state raw, unsigned up,
			    int64_t hold_ns, int64_t now_ns)
{
	if (up == h->last_up && urgency(raw) < urgency(h->state)) {
		if (!h->dropping) {
			h->dropping = true;
			h->drop_ns = now_ns;
		}
		if (now_ns - h->drop_ns < hold_ns)
			return h->state;
	}
	h->dropping = false;
	h->state = raw;
	h->last_up = up;
	return raw;
}

void fuse_eval(struct fuse *f, unsigned up, int64_t now_ns, struct fuse_out *out)
{
	/* A lost sensor's old readings must not count when it comes back. */
	if (!(up & UP(SENSOR_SONAR)))
		f->pings = 0;
	if (!(up & UP(SENSOR_RADAR)))
		f->track_seen[FUSE_RIGHT] = false;
	if (!(up & UP(SENSOR_RADAR_LEFT)))
		f->track_seen[FUSE_LEFT] = false;

	out->right = hold(&f->side[FUSE_RIGHT], raw_right(f, up, now_ns), up & RIGHT_MASK,
			  f->hold_ns, now_ns);
	out->left = hold(&f->side[FUSE_LEFT], raw_left(f, up, now_ns), up & LEFT_MASK,
			 f->hold_ns, now_ns);
}

int fuse_parse_mode(const char *s, enum fuse_radar_mode *out)
{
	if (!s || !*s || strcmp(s, "tracks") == 0) {
		*out = FUSE_RADAR_TRACKS;
		return 0;
	}
	if (strcmp(s, "presence") == 0) {
		*out = FUSE_RADAR_PRESENCE;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

const char *fuse_mode_name(enum fuse_radar_mode m)
{
	return m == FUSE_RADAR_PRESENCE ? "presence" : m == FUSE_RADAR_TRACKS ? "tracks" : "unknown";
}

const char *fuse_state_name(enum fuse_state s)
{
	static const char *const names[] = {
		[ST_BLIND]           = "BLIND",
		[ST_CLEAR]           = "CLEAR",
		[ST_APPROACHING]     = "APPROACHING",
		[ST_IN_ZONE]         = "IN_ZONE",
		[ST_DEG_CLEAR]       = "DEG_CLEAR",
		[ST_DEG_APPROACHING] = "DEG_APPROACHING",
		[ST_DEG_IN_ZONE]     = "DEG_IN_ZONE",
	};

	return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "UNKNOWN";
}
