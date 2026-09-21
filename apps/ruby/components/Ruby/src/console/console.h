/*
 * Framebuffer console for running CRuby inside a CAmkES component.
 *
 * On the Steam Deck there is no serial port, so the debug console the rest of
 * this component prints to reaches nothing at all. The framebuffer GRUB leaves
 * behind is the only output device that survives ExitBootServices, and this
 * turns it into a terminal.
 *
 * It has to be a terminal rather than a way to draw text, because Reline drives
 * the display entirely through escape sequences: it moves the cursor, erases to
 * end of line, and asks where the cursor is. A renderer that only appended
 * characters would leave the line editor drawing over itself.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define CONSOLE_FONT_WIDTH 8
#define CONSOLE_FONT_HEIGHT 16
#define CONSOLE_FONT_FIRST 0x20
#define CONSOLE_FONT_LAST 0x7e
#define CONSOLE_FONT_GLYPHS (CONSOLE_FONT_LAST - CONSOLE_FONT_FIRST + 1)

extern const uint8_t console_font[CONSOLE_FONT_GLYPHS][CONSOLE_FONT_HEIGHT];

/*
 * Everything above ASCII comes from Misaki, an 8x8 Japanese font whose half
 * width characters occupy 4x8 and full width ones 8x8. Both are exactly half a
 * console cell in each direction, so a glyph drawn at twice the console's scale
 * fills one cell or two precisely -- which is what lets a 4x8 face share a
 * terminal with an 8x16 one without either being stretched.
 *
 * `wide` is the font's own DWIDTH, and decides how many cells the character
 * advances. Reline computes the same distinction from East Asian Width when it
 * lays out a line, so the two have to agree or the cursor drifts.
 */
typedef struct {
    uint16_t codepoint;
    uint8_t wide;
    uint8_t rows[8];
} console_glyph_t;

/* Sorted by code point, so lookup bisects. */
extern const console_glyph_t console_misaki[];
extern const size_t console_misaki_count;

/*
 * Whether a framebuffer was configured and could be mapped. Everything below is
 * safe to call regardless; it simply does nothing when this is false, so that a
 * build without a framebuffer behaves exactly as it did before.
 */
int console_fb_available(void);

/* Feed output to the terminal. Escape sequences are interpreted. */
void console_fb_write(const char *buf, size_t len);

/*
 * Collect anything the terminal owes the program in reply to a query, such as
 * the cursor position report a DSR request asks for.
 *
 * The answer to a terminal query arrives on standard input on a real terminal,
 * because the terminal is a separate machine typing back. Here both ends are the
 * same component, so the reply is queued on the way out and the input path drains
 * it before it considers blocking. Reline's cursor_pos would otherwise wait for a
 * keystroke that only the reply could have produced.
 */
size_t console_fb_take_reply(char *buf, size_t len);

/* Whether a reply is waiting, for the input path's readiness checks. */
int console_fb_reply_pending(void);

/*
 * Put a line of text on the row below the terminal, and leave it there.
 *
 * The bottom row is not part of the terminal: nothing written to the console can
 * reach it, and clearing the screen does not touch it. That is the point. On a
 * machine with no serial port a driver's report otherwise scrolls past in the
 * moment before the program that owns the screen redraws it, and the one message
 * worth reading is the one that cannot be read.
 */
void console_fb_set_status(const char *text);

/* The terminal's size in character cells, or zero when unavailable. */
int console_fb_rows(void);
int console_fb_cols(void);
