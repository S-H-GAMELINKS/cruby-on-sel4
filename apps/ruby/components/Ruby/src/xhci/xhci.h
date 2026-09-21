/*
 * A USB host controller driver, far enough to read a keyboard.
 *
 * This exists because the Steam Deck has no other way in. Its buttons are a USB
 * device on an xHCI controller; its firmware offers no legacy keyboard
 * emulation, the status port reads 0xff; and the firmware's own input protocols
 * are gone the moment the loader leaves boot services, which happens before seL4
 * starts. Everything the machine can be told has to come through this.
 */

#pragma once

#include <stdbool.h>

/*
 * Bring the controller up. Safe to call repeatedly; only the first call does
 * anything, and it reports what it found. Returns whether the controller is
 * running.
 */
bool xhci_init(void);

/*
 * Collect whatever the devices have reported since last asked.
 *
 * There is no interrupt; the event ring is read when somebody wants to know.
 * What arrives is shown as it is, because what the Steam Deck's controller puts
 * in a report is not something a specification says -- it has to be watched.
 */
void xhci_poll(void);
