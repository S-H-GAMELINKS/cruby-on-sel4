/*
 * The hidden command: up up down down left right left right B A, on the Steam
 * Deck's D-pad and face buttons, which arrive as the arrow keys, Escape and
 * Enter. On a keyboard it is typed with those keys.
 *
 * Every keyboard driver feeds its key presses through here, so the command
 * works whichever one the keys come from: the USB one on the Deck, the legacy
 * controller under QEMU.
 *
 * When it completes, the final Enter is replaced by HIDDEN_COMMAND_TOGGLE_BYTE,
 * which no key produces, and the program reading input decides what that means.
 * The Escape before it would close things, so it is held back once the arrows
 * have matched, and only released if the next key turns out not to be Enter.
 */

#pragma once

#include <stddef.h>

/* The file separator control character. No key produces it, and a program that
 * does not know it ignores it, as Termfront and the slide tool both do. */
#define HIDDEN_COMMAND_TOGGLE_BYTE 0x1c

/* The keys the command is made of, in a form every driver can map to. */
typedef enum {
    HIDDEN_KEY_UP,
    HIDDEN_KEY_DOWN,
    HIDDEN_KEY_LEFT,
    HIDDEN_KEY_RIGHT,
    HIDDEN_KEY_ESCAPE,
    HIDDEN_KEY_ENTER,
    HIDDEN_KEY_OTHER,
} hidden_key_t;

/* What the driver should do with the key it just reported. */
typedef enum {
    /* Deliver the key as usual. */
    HIDDEN_PASS,
    /* Deliver nothing: an Escape held back in case the command completes. */
    HIDDEN_SWALLOW,
    /* Deliver the held back Escape, then the key as usual. */
    HIDDEN_RELEASE_ESCAPE,
    /* Deliver HIDDEN_COMMAND_TOGGLE_BYTE instead of the key. */
    HIDDEN_TOGGLE,
} hidden_action_t;

/* One per keyboard, so that two of them cannot complete each other's command.
 * Zero initialised is the starting state. */
typedef struct {
    size_t progress;
    int escape_withheld;
} hidden_command_t;

/*
 * Follow the command through one key press. Only presses count: a driver that
 * produces repeats of a held key must not report them here, so that holding an
 * arrow counts as pressing it once.
 */
hidden_action_t hidden_command_follow(hidden_command_t *state, hidden_key_t key);
