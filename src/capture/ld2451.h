/* LD2451 target payload -> radar_msg targets. Pure, so it is unit-tested
 * directly. Layout verified on the Pi (docs/evidence/radar-targets-pi.txt):
 *
 *   payload[0]  target count
 *   payload[1]  alarm flag (always 1 in the capture; not used)
 *   then 5 bytes per target:
 *     angle + 0x80 (deg), distance (m), direction (1 = approaching),
 *     speed (km/h), SNR
 *
 * The radar only reported approaching targets in the capture. Direction 0 and
 * frames with more than one target were never seen: 0 is taken as receding,
 * and the 5-byte stride for several targets is assumed.
 */
#ifndef DLBY_LD2451_H
#define DLBY_LD2451_H

#include "msg.h"

#include <stddef.h>
#include <stdint.h>

#define LD2451_HDR_LEN      2
#define LD2451_TARGET_LEN   5
#define LD2451_DIR_APPROACH 1

/* Fills m->ntargets and m->t[] from a non-empty payload; other fields of m are
 * untouched. Returns the number of targets, or -1 with EINVAL (and
 * m->ntargets = 0) if the length doesn't match the count or the count is over
 * RADAR_MAX_TARGETS. */
int ld2451_decode(const uint8_t *payload, size_t len, struct radar_msg *m);

#endif
