/*
 * Keys from the legacy keyboard controller at ports 0x60 and 0x64.
 *
 * This component runs on a machine with no keyboard controller. The Steam Deck
 * has no PS/2 port, and neither does any other machine built this decade. What
 * some firmware still has is USB legacy emulation: access to these ports is
 * trapped and answered on behalf of a USB keyboard, so that software which only
 * knows about a keyboard controller keeps working.
 *
 * The Steam Deck's firmware does not. Its status port reads 0xff, and the same
 * code drives a keyboard under QEMU, so this is inert there and kept for
 * machines that do -- which includes the development loop, where it is the only
 * keyboard the slide tool has.
 *
 * It cannot reach the Deck's own buttons. Those are a USB HID device on the
 * internal xHCI controller, and no amount of legacy emulation reaches a gamepad.
 * Reading them needs an xHCI driver, which the seL4 tree does not have -- its
 * libusbdrivers has device drivers for HID and keyboards but only an EHCI host
 * controller underneath them.
 *
 * There is no interrupt. One could be wired up -- the controller raises IRQ 1 --
 * but it would mean an interrupt connection, an acknowledgement path and a third
 * badge to distinguish in the wait loop, for latency that does not matter here.
 * The caller polls the port when it asks whether input is available, which the
 * slide tool does every 200 milliseconds while it waits for a key.
 *
 * Scancodes are set 1, which is what a controller in translated mode produces.
 * They are turned into the bytes a terminal would have sent, because that is
 * what everything above expects: Reline parses escape sequences for the cursor
 * keys, and so does the slide tool.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <camkes/io.h>
#include <platsupport/io.h>

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

/* Set when the controller has a byte for us. */
#define PS2_STATUS_OUTPUT_FULL 0x01
/* Set when the byte came from the auxiliary port -- a mouse, not a keyboard. */
#define PS2_STATUS_AUX 0x20

/* An absent device leaves the bus pulled high, so every bit reads as one. */
#define PS2_NO_DEVICE 0xff

/* A scancode with the top bit set is a key being released. */
#define PS2_BREAK 0x80
/* Introduces the two byte codes: the cursor keys and their neighbours. */
#define PS2_EXTENDED 0xe0

#define QUEUE_SIZE 64

/* Unshifted ASCII for the single byte codes, indexed by scancode. Zero means
 * nothing this produces a byte for. The layout is the one every PC has had since
 * the AT: positional, so this is a US keyboard by construction. */
static const char scancode_ascii[PS2_BREAK] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
    [0x0c] = '-', [0x0d] = '=', [0x0e] = '\177',                /* backspace */
    [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1a] = '[', [0x1b] = ']', [0x1c] = '\r',                  /* enter */
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

/* What the extended codes send, as a terminal would. The cursor keys matter to
 * Reline; page up and down are what a presentation clicker produces. */
static const char *extended_sequence(uint8_t code)
{
    switch (code) {
    case 0x48: return "\033[A";     /* up */
    case 0x50: return "\033[B";     /* down */
    case 0x4d: return "\033[C";     /* right */
    case 0x4b: return "\033[D";     /* left */
    case 0x47: return "\033[H";     /* home */
    case 0x4f: return "\033[F";     /* end */
    case 0x49: return "\033[5~";    /* page up */
    case 0x51: return "\033[6~";    /* page down */
    case 0x53: return "\033[3~";    /* delete */
    case 0x1c: return "\r";         /* keypad enter */
    default: return NULL;
    }
}

static ps_io_port_ops_t port_ops;
static int ops_ready;
static int present;
static int probed;

static char queue[QUEUE_SIZE];
static size_t queue_head;
static size_t queue_tail;

static int port_read(uint16_t port)
{
    uint32_t value = PS2_NO_DEVICE;

    if (!ops_ready) {
        if (camkes_io_port_ops(&port_ops) != 0) {
            return PS2_NO_DEVICE;
        }
        ops_ready = 1;
    }
    if (ps_io_port_in(&port_ops, port, 1, &value) != 0) {
        return PS2_NO_DEVICE;
    }

    return (int)(value & 0xff);
}

int ps2_available(void)
{
    if (!probed) {
        probed = 1;
        /* Nothing is asked of the controller, only whether one answers. A status
         * of all ones is an unclaimed bus, which is what an absent controller
         * and an absent emulation both look like. */
        int status = port_read(PS2_STATUS);

        present = status != PS2_NO_DEVICE;
        /* Said once. On a machine with a serial line this is how an absent
         * controller is told apart from a bug; the Steam Deck answers 0xff,
         * which is how it was established that its firmware does not emulate
         * one. */
        printf("PS/2: status=0x%02x %s\n", status,
               present ? "controller present" : "nothing there");
    }

    return present;
}

static void queue_push(const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
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

/*
 * Drain the controller into the queue.
 *
 * Key releases are ignored: there is no modifier state here, so a release means
 * nothing. That also means no shift, so the letters are the ones in the table
 * and nothing else.
 */
static void ps2_drain(void)
{
    static int extended;

    if (!ps2_available()) {
        return;
    }

    for (;;) {
        int status = port_read(PS2_STATUS);
        uint8_t code;

        if (status == PS2_NO_DEVICE || (status & PS2_STATUS_OUTPUT_FULL) == 0) {
            return;
        }
        if ((status & PS2_STATUS_AUX) != 0) {
            /* A mouse. Read it away so it does not block the keyboard. */
            (void)port_read(PS2_DATA);
            continue;
        }

        code = (uint8_t)port_read(PS2_DATA);

        if (code == PS2_EXTENDED) {
            extended = 1;
            continue;
        }
        if ((code & PS2_BREAK) != 0) {
            extended = 0;
            continue;
        }

        if (extended) {
            const char *sequence = extended_sequence(code);

            extended = 0;
            if (sequence != NULL) {
                queue_push(sequence, strlen(sequence));
            }
            continue;
        }

        if (scancode_ascii[code] != '\0') {
            queue_push(&scancode_ascii[code], 1);
        }
    }
}

int ps2_has_input(void)
{
    ps2_drain();

    return queue_head != queue_tail;
}

size_t ps2_take(char *buf, size_t len)
{
    size_t used = 0;

    ps2_drain();
    while (used < len && queue_head != queue_tail) {
        buf[used++] = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
    }

    return used;
}
