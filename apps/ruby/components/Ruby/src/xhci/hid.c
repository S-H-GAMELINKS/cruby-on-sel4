#include "hid.h"

#include <stdbool.h>
#include <string.h>

/* A boot protocol report: modifiers, a reserved byte, then up to six keys. */
#define REPORT_MODIFIERS 0
#define REPORT_FIRST_KEY 2
#define REPORT_KEYS 6

#define MODIFIER_SHIFT 0x22u  /* either shift key */

#define QUEUE_SIZE 64

/* Usage identifiers, from the HID usage tables. Only the ones that become an
 * escape sequence are named; the rest are looked up in the tables below. */
#define KEY_ENTER 0x28
#define KEY_ESCAPE 0x29
#define KEY_BACKSPACE 0x2a
#define KEY_TAB 0x2b
#define KEY_SPACE 0x2c

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

void hid_keyboard_report(const volatile uint8_t *report, size_t length, uint8_t *previous)
{
    uint8_t current[REPORT_FIRST_KEY + REPORT_KEYS];
    bool shift;

    if (length < REPORT_FIRST_KEY + 1) {
        return;
    }
    for (size_t i = 0; i < sizeof(current); i++) {
        current[i] = i < length ? report[i] : 0;
    }
    shift = (current[REPORT_MODIFIERS] & MODIFIER_SHIFT) != 0;

    for (size_t i = REPORT_FIRST_KEY; i < REPORT_FIRST_KEY + REPORT_KEYS; i++) {
        uint8_t key = current[i];
        const char *sequence;

        /* Zero is an empty slot, and anything from 1 to 3 is the keyboard
         * reporting that it lost track rather than a key. */
        if (key < 4 || was_held(previous, key)) {
            continue;
        }

        sequence = escape_for(key);
        if (sequence != NULL) {
            queue_push(sequence, strlen(sequence));
            continue;
        }
        if (key >= KEY_FIRST_PRINTABLE && key <= KEY_LAST_PRINTABLE) {
            char c = (shift ? shifted : unshifted)[key - KEY_FIRST_PRINTABLE];

            if (c != '\0') {
                queue_push(&c, 1);
            }
        }
    }

    memcpy(previous, current, sizeof(current));
}

int hid_has_input(void)
{
    return queue_head != queue_tail;
}

size_t hid_take(char *buf, size_t len)
{
    size_t used = 0;

    while (used < len && queue_head != queue_tail) {
        buf[used++] = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
    }

    return used;
}
