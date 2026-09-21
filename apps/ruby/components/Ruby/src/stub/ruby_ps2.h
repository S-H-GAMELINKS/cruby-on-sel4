/*
 * Keys from the legacy keyboard controller, as bytes on standard input.
 *
 * See ruby_ps2.c for what this is and why a machine with no PS/2 port might
 * still answer.
 */

#pragma once

#include <stddef.h>

/* Whether a keyboard controller answered when asked. Everything below is safe to
 * call regardless; it simply reports nothing when none did. */
int ps2_available(void);

/* Take whatever the keyboard has produced since last asked, translated to the
 * bytes a terminal would have sent. Returns how many were written. */
size_t ps2_take(char *buf, size_t len);

/* Whether a key is waiting, for the input path's readiness checks. This is what
 * actually drives the controller: there is no interrupt, so the hardware is read
 * when somebody asks whether there is anything to read. */
int ps2_has_input(void);
