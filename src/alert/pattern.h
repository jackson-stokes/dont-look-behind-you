/* LED and buzzer patterns for alert (DESIGN.md §1 table). Pure: time since the
 * state was entered in, levels out, so every timing is unit-tested.
 *
 * Each mirror LED shows its own side's state; the one buzzer plays the pattern
 * of whichever side's buzzer matters most (pattern_buzz_side). Every pattern
 * starts in its on-phase, so a state change drives the output at once; that
 * first write is the edge alert latency is measured to.
 *
 *   state            LED                          buzzer
 *   CLEAR            100 ms blink every 2 s       silent
 *   APPROACHING      solid                        silent
 *   IN_ZONE          solid                        125 ms on / 250 ms (4 Hz)
 *   DEG_CLEAR        2 blinks every 2 s           one 80 ms chirp on entry
 *   DEG_APPROACHING  500 ms on / 1 s (1 Hz)       silent
 *   DEG_IN_ZONE      500 ms on / 1 s (1 Hz)       250 ms on / 1 s (1 Hz)
 *   BLIND, HUB_LOST  125 ms on / 250 ms (4 Hz)    two 80 ms chirps every 30 s
 *
 * States are enum fuse_state (msg.h) plus PAT_HUB_LOST, alert's own.
 */
#ifndef DLBY_PATTERN_H
#define DLBY_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

#define PAT_HUB_LOST 7		/* after ST_DEG_IN_ZONE */
#define PAT_NSTATES  8

/* Levels at t ns after entering state (t < 0 counts as 0). Unknown states are dark. */
bool pattern_led(unsigned state, int64_t t);
bool pattern_buzz(unsigned state, int64_t t);

/* The first time after t at which the LED or the buzzer changes; INT64_MAX if
 * neither ever will again. */
int64_t pattern_next_edge(unsigned state, int64_t t);

/* How much a state's buzzer matters: IN_ZONE 4, DEG_IN_ZONE 3, BLIND/HUB_LOST 2,
 * DEG_CLEAR 1, silent states 0. */
int pattern_buzz_rank(unsigned state);

/* Whose buzzer pattern plays: 0 = right, 1 = left. A tie goes to the right. */
int pattern_buzz_side(unsigned right, unsigned left);

const char *pattern_state_name(unsigned state);

#endif
