#include "hidden_command.h"

static const hidden_key_t sequence[] = {
    HIDDEN_KEY_UP, HIDDEN_KEY_UP, HIDDEN_KEY_DOWN, HIDDEN_KEY_DOWN,
    HIDDEN_KEY_LEFT, HIDDEN_KEY_RIGHT, HIDDEN_KEY_LEFT, HIDDEN_KEY_RIGHT,
    HIDDEN_KEY_ESCAPE, HIDDEN_KEY_ENTER,
};

#define SEQUENCE_LENGTH (sizeof(sequence) / sizeof(sequence[0]))

/* The Escape is the second to last key; it is the one held back. */
#define ESCAPE_POSITION (SEQUENCE_LENGTH - 2)

hidden_action_t hidden_command_follow(hidden_command_t *state, hidden_key_t key)
{
    hidden_action_t action = HIDDEN_PASS;

    if (state->escape_withheld) {
        state->escape_withheld = 0;
        state->progress = 0;
        if (key == sequence[SEQUENCE_LENGTH - 1]) {
            return HIDDEN_TOGGLE;
        }
        /* Not the command after all; the Escape was real. The key that broke
         * the sequence is then followed from the start like any other. */
        action = HIDDEN_RELEASE_ESCAPE;
    }

    if (key == sequence[state->progress]) {
        state->progress++;
    } else if (key == sequence[0]) {
        /* A third up still leaves the last two as a valid start. */
        state->progress = state->progress == 2 ? 2 : 1;
    } else {
        state->progress = 0;
    }

    if (state->progress == ESCAPE_POSITION + 1) {
        state->escape_withheld = 1;
        return HIDDEN_SWALLOW;
    }

    return action;
}
