/*
 * A USB keyboard's reports, as the bytes a terminal would have sent.
 *
 * The Steam Deck's controller is one of these. It runs by default in a mode
 * where its buttons are presented as an ordinary keyboard -- which is how the
 * firmware's boot menu is navigated with the D-pad -- so the directional pad
 * arrives as the arrow keys and needs no knowledge of the controller itself.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Take one report and queue whatever was newly pressed.
 *
 * A report lists the keys held down, not the ones that changed, so it is
 * compared against the previous one: a key counts as typed when it appears
 * where it was not before. `previous` is the caller's copy of the last report
 * and is updated.
 */
void hid_keyboard_report(const volatile uint8_t *report, size_t length, uint8_t *previous);

/* Whether anything is waiting, and taking it. */
int hid_has_input(void);
size_t hid_take(char *buf, size_t len);
