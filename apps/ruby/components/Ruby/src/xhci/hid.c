#include "hid.h"
#include "../hidden_command.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

/* A boot protocol report: modifiers, a reserved byte, then up to six keys. */
#define REPORT_MODIFIERS 0
#define REPORT_FIRST_KEY 2
#define REPORT_KEYS 6

#define MODIFIER_SHIFT 0x22u  /* either shift key */

#define QUEUE_SIZE 64

/*
 * Typematic repeat, which a USB keyboard leaves to the host: a report says what
 * is held, and says it once. A terminal program that wants to know a key is
 * still down -- a game moving while an arrow is held -- relies on the repeats a
 * terminal would have produced, so they are produced here.
 *
 * The interval has to be shorter than any such program's patience. Termfront
 * forgets a key five frames after it last saw it, about 83 ms at its 60 Hz.
 */
#define REPEAT_DELAY_NS (250ull * 1000 * 1000)
#define REPEAT_INTERVAL_NS (33ull * 1000 * 1000)

/* Usage identifiers, from the HID usage tables. Only the ones that become an
 * escape sequence are named; the rest are looked up in the tables below. */
#define KEY_ENTER 0x28
#define KEY_ESCAPE 0x29
#define KEY_BACKSPACE 0x2a
#define KEY_TAB 0x2b
#define KEY_SPACE 0x2c

/* Letters the Steam Deck's extra buttons type. The slides ignore all of them;
 * the launcher gives them their meaning in Termfront. */
#define KEY_F 0x09   /* R2: fire */
#define KEY_J 0x0d   /* L1: turn left */
#define KEY_L 0x0f   /* R1: turn right */
#define KEY_T 0x17   /* Y: swap weapons */

#define KEY_FIRST_PRINTABLE 0x04
#define KEY_LAST_PRINTABLE 0x38

/* Unshifted, indexed from KEY_FIRST_PRINTABLE. */
static const char unshifted[] =
    "abcdefghijklmnopqrstuvwxyz"   /* 0x04 - 0x1d */
    "1234567890"                   /* 0x1e - 0x27 */
    "\r\033\177\t "                /* 0x28 - 0x2c */
    "-=[]\\"                       /* 0x2d - 0x31 */
    "\0;'`,./";                    /* 0x32 - 0x38 */

/* The same keys with shift held. */
static const char shifted[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!@#$%^&*()"
    "\r\033\177\t "
    "_+{}|"
    "\0:\"~<>?";

/* Keys that are a sequence rather than a character. */
static const char *escape_for(uint8_t key)
{
    switch (key) {
    case 0x49: return "\033[2~";  /* insert */
    case 0x4a: return "\033[H";   /* home */
    case 0x4b: return "\033[5~";  /* page up */
    case 0x4c: return "\033[3~";  /* delete */
    case 0x4d: return "\033[F";   /* end */
    case 0x4e: return "\033[6~";  /* page down */
    case 0x4f: return "\033[C";   /* right */
    case 0x50: return "\033[D";   /* left */
    case 0x51: return "\033[B";   /* down */
    case 0x52: return "\033[A";   /* up */
    default: return NULL;
    }
}

static hidden_command_t hidden_command;

/* Keys being held, and when each is next due to repeat. */
typedef struct {
    uint8_t key;
    bool shift;
    uint64_t next_ns;
} held_key_t;

static held_key_t held[REPORT_KEYS];
static size_t held_count;

/*
 * Until when what is held is known to be held. A keyboard reports only when
 * something changes, so what it last said holds until it says otherwise. The
 * Steam Deck's controller reports its state continuously instead, and the
 * report read now was taken just after the previous one was read -- which can
 * be a whole slide's redraw ago. A key let go during the redraw still reads as
 * held, and repeating it on that word would turn one press into two.
 */
static uint64_t held_known_until = UINT64_MAX;

static char queue[QUEUE_SIZE];
static size_t queue_head;
static size_t queue_tail;

static void queue_push(const char *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        size_t next = (queue_tail + 1) % QUEUE_SIZE;

        if (next == queue_head) {
            /* Nobody is reading; dropping what arrives now is better than
             * dropping what arrived first. */
            return;
        }
        queue[queue_tail] = bytes[i];
        queue_tail = next;
    }
}

static bool was_held(const uint8_t *previous, uint8_t key)
{
    for (size_t i = REPORT_FIRST_KEY; i < REPORT_FIRST_KEY + REPORT_KEYS; i++) {
        if (previous[i] == key) {
            return true;
        }
    }

    return false;
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Queue what a key types, as a terminal would send it. */
static void emit_key(uint8_t key, bool shift)
{
    const char *sequence = escape_for(key);

    if (sequence != NULL) {
        queue_push(sequence, strlen(sequence));
        return;
    }
    if (key >= KEY_FIRST_PRINTABLE && key <= KEY_LAST_PRINTABLE) {
        char c = (shift ? shifted : unshifted)[key - KEY_FIRST_PRINTABLE];

        if (c != '\0') {
            queue_push(&c, 1);
        }
    }
}

/* The hidden command's name for a key, from its usage identifier. */
static hidden_key_t hidden_key_for(uint8_t key)
{
    switch (key) {
    case 0x52: return HIDDEN_KEY_UP;
    case 0x51: return HIDDEN_KEY_DOWN;
    case 0x50: return HIDDEN_KEY_LEFT;
    case 0x4f: return HIDDEN_KEY_RIGHT;
    case KEY_ESCAPE: return HIDDEN_KEY_ESCAPE;
    case KEY_ENTER: return HIDDEN_KEY_ENTER;
    default: return HIDDEN_KEY_OTHER;
    }
}

static void hold(uint8_t key, bool shift, uint64_t now)
{
    /* Escape closes things; repeating it would close several. A weapon swap
     * held down would likewise cycle through every weapon. */
    if (key == KEY_ESCAPE || key == KEY_T || held_count >= REPORT_KEYS) {
        return;
    }
    held[held_count].key = key;
    held[held_count].shift = shift;
    /* The delay before repeating is what lets a tap on an arrow move a slide by
     * one page. Firing and turning mean nothing to the slides and are held, not
     * tapped, so they repeat at once: otherwise turning would start, pause for a
     * quarter of a second, and only then carry on. */
    held[held_count].next_ns = now + (key == KEY_F || key == KEY_J || key == KEY_L
                                          ? REPEAT_INTERVAL_NS : REPEAT_DELAY_NS);
    held_count++;
}

/* Drop the keys a report no longer lists. */
static void release_missing(const uint8_t *current)
{
    size_t kept = 0;

    for (size_t i = 0; i < held_count; i++) {
        if (was_held(current, held[i].key)) {
            held[kept++] = held[i];
        }
    }
    held_count = kept;
}

/*
 * Produce the repeats that are due. Only into an empty queue: if the reader has
 * fallen behind, more of the same key would only make it act on stale input for
 * longer after the key is let go.
 */
static void repeat_due(void)
{
    uint64_t now;

    if (held_count == 0 || queue_head != queue_tail) {
        return;
    }

    now = now_ns();
    for (size_t i = 0; i < held_count; i++) {
        if (now >= held[i].next_ns && held[i].next_ns <= held_known_until) {
            emit_key(held[i].key, held[i].shift);
            held[i].next_ns = now + REPEAT_INTERVAL_NS;
        }
    }
}

/*
 * Act on a boot protocol report: queue what was newly pressed, and keep track
 * of what is held.
 */
static void keys_report(const uint8_t *current, uint8_t *previous)
{
    hidden_action_t action;
    bool shift;
    uint64_t now;

    shift = (current[REPORT_MODIFIERS] & MODIFIER_SHIFT) != 0;
    now = now_ns();

    /* Held keys are tracked across every keyboard at once. With one attached,
     * which is the case this serves, that is exact. */
    release_missing(current);

    for (size_t i = REPORT_FIRST_KEY; i < REPORT_FIRST_KEY + REPORT_KEYS; i++) {
        uint8_t key = current[i];

        /* Zero is an empty slot, and anything from 1 to 3 is the keyboard
         * reporting that it lost track rather than a key. */
        if (key < 4 || was_held(previous, key)) {
            continue;
        }

        /* Repeats never come through here, so holding an arrow counts as one
         * press of it, as the hidden command needs. */
        action = hidden_command_follow(&hidden_command, hidden_key_for(key));
        switch (action) {
        case HIDDEN_SWALLOW:
            continue;
        case HIDDEN_TOGGLE: {
            char toggle = HIDDEN_COMMAND_TOGGLE_BYTE;

            queue_push(&toggle, 1);
            continue;
        }
        case HIDDEN_RELEASE_ESCAPE:
            queue_push("\033", 1);
            break;
        case HIDDEN_PASS:
            break;
        }
        emit_key(key, shift);
        hold(key, shift, now);
    }

    memcpy(previous, current, REPORT_FIRST_KEY + REPORT_KEYS);
}

void hid_keyboard_report(const volatile uint8_t *report, size_t length, uint8_t *previous)
{
    uint8_t current[REPORT_FIRST_KEY + REPORT_KEYS];

    if (length < REPORT_FIRST_KEY + 1) {
        return;
    }
    for (size_t i = 0; i < sizeof(current); i++) {
        current[i] = i < length ? report[i] : 0;
    }

    held_known_until = UINT64_MAX;
    keys_report(current, previous);
}

/*
 * The Steam Deck's controller, read through its own interface.
 *
 * Presented as a keyboard, the controller types for the D-pad, A and B and
 * nothing else; the shoulder buttons, the triggers, X and Y send no keys at
 * all. Its own interface reports every control, as a state report the Linux
 * driver documents (hid-steam.c, steam_do_deck_input_event): a header of 01 00,
 * the report type, a length, and from byte 8 the buttons as bits.
 *
 * The buttons are turned into a keyboard report and handled as one, so the
 * hidden command, the repeats and the bytes typed are exactly the keyboard's.
 */
#define DECK_REPORT_STATE 0x09
#define DECK_REPORT_MIN_LENGTH 16

typedef struct {
    uint8_t byte;
    uint8_t bit;
    uint8_t usage;
} deck_button_t;

static const deck_button_t deck_buttons[] = {
    { 9, 0, 0x52 },         /* D-pad up: up arrow */
    { 9, 3, 0x51 },         /* D-pad down: down arrow */
    { 9, 2, 0x50 },         /* D-pad left: left arrow */
    { 9, 1, 0x4f },         /* D-pad right: right arrow */
    { 8, 7, KEY_ENTER },    /* A */
    { 8, 5, KEY_ESCAPE },   /* B */
    { 8, 0, KEY_F },        /* R2 */
    { 8, 3, KEY_J },        /* L1 */
    { 8, 2, KEY_L },        /* R1 */
    { 8, 4, KEY_T },        /* Y */
};

#define DECK_BUTTON_COUNT (sizeof(deck_buttons) / sizeof(deck_buttons[0]))

/* Holding L2 switches between the slides and Termfront, as the hidden command
 * does in ten presses. It types the same byte, so nothing above can tell the
 * two apart. It has to be held, so that brushing it in the middle of a talk
 * does not throw the audience into a game; and it switches once per hold. */
#define DECK_TOGGLE_BYTE 8
#define DECK_TOGGLE_BIT 1
#define DECK_TOGGLE_HOLD_NS (500ull * 1000 * 1000)

void hid_steam_report(const volatile uint8_t *report, size_t length, uint8_t *previous)
{
    uint8_t current[REPORT_FIRST_KEY + REPORT_KEYS] = { 0 };
    size_t slot = REPORT_FIRST_KEY;

    if (length < DECK_REPORT_MIN_LENGTH) {
        return;
    }

    /* Other report types carry battery and connection state, not buttons. */
    if (report[0] == 0x01 && report[1] == 0x00 && report[2] == DECK_REPORT_STATE) {
        static bool toggle_held;
        static bool toggle_done;
        static uint64_t toggle_since;
        bool toggle = (report[DECK_TOGGLE_BYTE] & (1u << DECK_TOGGLE_BIT)) != 0;

        if (!toggle) {
            toggle_held = false;
        } else if (!toggle_held) {
            toggle_held = true;
            toggle_done = false;
            toggle_since = now_ns();
        } else if (!toggle_done && now_ns() - toggle_since >= DECK_TOGGLE_HOLD_NS) {
            char byte = HIDDEN_COMMAND_TOGGLE_BYTE;

            toggle_done = true;
            queue_push(&byte, 1);
        }

        for (size_t i = 0; i < DECK_BUTTON_COUNT && slot < sizeof(current); i++) {
            if ((report[deck_buttons[i].byte] & (1u << deck_buttons[i].bit)) != 0) {
                current[slot++] = deck_buttons[i].usage;
            }
        }
        /* The controller filled this report as soon as the previous one was
         * taken away, so that is when it says the buttons were as they are. */
        static uint64_t previous_read_ns;
        uint64_t now = now_ns();

        held_known_until = previous_read_ns;
        previous_read_ns = now;
        keys_report(current, previous);
    }
}

int hid_has_input(void)
{
    repeat_due();
    return queue_head != queue_tail;
}

size_t hid_take(char *buf, size_t len)
{
    size_t used = 0;

    repeat_due();
    while (used < len && queue_head != queue_tail) {
        buf[used++] = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
    }

    return used;
}
