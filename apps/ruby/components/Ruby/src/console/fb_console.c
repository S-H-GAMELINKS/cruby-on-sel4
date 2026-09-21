/*
 * A terminal on the framebuffer GRUB hands over through multiboot2.
 *
 * The geometry is not discovered here. seL4 reports what GRUB set in an extended
 * bootinfo header, but a CAmkES component never sees bootinfo, and the mapping of
 * the framebuffer has to exist in the capDL specification before the system
 * starts -- there is no way to map a physical address that was only learned at
 * run time. So the address and the mode come from the assembly, as the paddr of a
 * hardware dataport and as attributes, and the kernel's own boot message is where
 * the values are read off:
 *
 *   Got framebuffer info in multiboot2. Current video mode is at physical
 *   address=80000000 pitch=5120 resolution=1280x800@32 type=1
 *
 * Changing display or resolution therefore means changing the assembly. That is
 * the cost of a static system; in exchange nothing can fail at run time.
 *
 * Pixel format. The multiboot2 framebuffer tag carries the colour field
 * positions, but seL4's copy of the tag stops at the bit depth
 * (multiboot2.h:36-43), so they are not available even indirectly. Type 1 at 32
 * bits is assumed to be the UEFI GOP ordering that every x86 firmware in practice
 * reports, blue in the low byte, which is 0x00RRGGBB read as a little endian
 * word. If a machine ever disagrees the symptom is unmistakable -- red and blue
 * swap -- and only the palette below has to change.
 *
 * Drawing. The framebuffer is mapped uncached, because that is all a CAmkES
 * hardware dataport offers: the template picks between write-back and uncached
 * from a boolean (seL4HardwareMMIO.template.c:39) and write-combining, which is
 * what a framebuffer actually wants, is not expressible. Uncached stores are
 * expensive enough that redrawing the screen for every change is not an option,
 * so the terminal keeps what it has drawn and repaints only the cells that differ.
 * A scroll dirties every cell that holds text, but not the blank ones, which is
 * most of the screen in practice.
 */

#include "console.h"

#include <string.h>

#include <camkes.h>

/* Bounds for the statically allocated shadow buffers. Large enough for 1920x1080
 * unscaled; a mode past this renders the part that fits and ignores the rest,
 * which is preferable to failing to boot. */
#define MAX_COLS 256
#define MAX_ROWS 128

#define TAB_WIDTH 8
#define MAX_PARAMS 8

/* Marks a cell in the shadow as never drawn, so the first flush paints
 * everything. U+FFFF is a permanent noncharacter, so no text can hold it. */
#define CELL_UNDRAWN 0xffffu

/* A code point outside the range the glyph tables cover. Drawn as a hollow box
 * so that a character the font lacks is visible as an omission rather than as a
 * gap that reads like a space. */
#define CELL_MISSING 0xfffdu

#define DEFAULT_FG 7
#define DEFAULT_BG 0

/* A cell that a character to its left extends over. The character is drawn once,
 * from its leftmost cell, and the two move together. */
#define CELL_TAIL 0x01u

/* Largest magnification ESC [ n z can select, and so the most rows a single line
 * of text can cover. */
#define MAX_MAGNIFY 4

typedef struct {
    uint16_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t flags;
    /* How many cells this character covers, counting its own: the glyph's own
     * width in cells multiplied by the magnification of the line it is on. Zero
     * on a cell that is covered rather than covering. */
    uint8_t span;
} cell_t;

/* The usual sixteen ANSI colours. Index 0-7 are the normal ones, 8-15 the bright
 * ones that bold and the 90-97 range select. */
static const uint32_t palette[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
    0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

enum parse_state {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CSI,
    STATE_STRING,       /* OSC and friends, discarded up to their terminator */
    STATE_STRING_ESC,
};

static struct {
    volatile uint8_t *pixels;
    int pitch;
    int scale;
    int cols;
    int rows;

    /*
     * Where logical pixel (0, 0) lands, and what one step right and one step
     * down come to, as offsets in 32 bit pixels.
     *
     * Rotation is expressed this way rather than as a transform per pixel
     * because every rotation is an affine map of the same shape: a corner to
     * start from and two steps. Drawing then never asks which way round the
     * screen is, and the unrotated case costs nothing extra.
     */
    long origin;
    long step_x;
    long step_y;

    int col;
    int row;
    int saved_col;
    int saved_row;

    uint8_t fg;
    uint8_t bg;
    int bold;
    int reverse;
    int cursor_visible;

    /*
     * A character written in the last column leaves the cursor on that column
     * rather than past it, so that a line exactly as wide as the screen does not
     * scroll until something more is written. Every terminal behaves this way and
     * Reline's column arithmetic assumes it.
     */
    int wrap_pending;

    /*
     * Magnification, as ESC [ n z selects it: 1 through MAX_MAGNIFY.
     *
     * This is an attribute of what is written next, not of where the cursor is,
     * because that is the order it arrives in -- the cursor is positioned, then
     * the size is chosen, then the text is written. It takes effect on the row
     * that actually receives a character.
     */
    int magnify;

    /* Partially decoded UTF-8. A character may be split across two writes, so
     * this has to survive between calls. */
    uint32_t utf8;
    int utf8_need;

    enum parse_state state;
    int params[MAX_PARAMS];
    int nparams;
    int have_param;
    char private;

    int ready;
    int usable;
} con;

static cell_t cells[MAX_ROWS][MAX_COLS];
static cell_t shadow[MAX_ROWS][MAX_COLS];

/*
 * The magnification each row was written at.
 *
 * It is per row rather than per cell because a magnified character is taller
 * than a cell, so it reaches into the rows beneath and they cannot hold anything
 * of their own. Mixing sizes on one line would mean cells of several heights in
 * the same band, and the last size written to a row wins instead.
 */
static uint8_t row_magnify[MAX_ROWS];
static uint8_t shadow_magnify[MAX_ROWS];

static int shadow_row = -1;
static int shadow_col = -1;
static int shadow_cursor;

/* Replies owed to the program, as a ring so that a query issued while an earlier
 * reply is still unread does not lose either. */
static char reply_buf[64];
static size_t reply_head;
static size_t reply_tail;

static void reply_push(const char *s)
{
    for (; *s != '\0'; s++) {
        size_t next = (reply_tail + 1) % sizeof(reply_buf);

        if (next == reply_head) {
            /* Nothing is reading; dropping the tail of a stale reply is better
             * than overwriting one that is still wanted. */
            return;
        }
        reply_buf[reply_tail] = *s;
        reply_tail = next;
    }
}

int console_fb_reply_pending(void)
{
    return reply_head != reply_tail;
}

size_t console_fb_take_reply(char *buf, size_t len)
{
    size_t used = 0;

    while (used < len && reply_head != reply_tail) {
        buf[used++] = reply_buf[reply_head];
        reply_head = (reply_head + 1) % sizeof(reply_buf);
    }

    return used;
}

/*
 * The framebuffer is a dataport, so the mapping is already in place by the time
 * anything runs; this only works out the geometry and clears the screen.
 *
 * It runs on first use rather than from a constructor because the very first
 * thing printed may come from a constructor itself, and constructor order between
 * translation units is not something to depend on.
 */
static void console_init(void)
{
    int cols;
    int rows;
    int logical_w;
    int logical_h;

    con.ready = 1;

    if (fb_width <= 0 || fb_height <= 0 || fb_pitch <= 0 || fb_scale <= 0) {
        return;
    }

    con.pixels = (volatile uint8_t *)fb;
    if (con.pixels == NULL) {
        return;
    }

    con.pitch = fb_pitch;
    con.scale = fb_scale;

    /*
     * The panel may be mounted turned. The Steam Deck's is: an 800x1280 portrait
     * panel in a landscape body, with its top edge along the left side, which is
     * why anything drawn without allowing for it appears lying on its side.
     *
     * The mode is still whatever the firmware set, so the framebuffer stays
     * 800 wide; only what the console calls width and height swap over.
     */
    {
        long row_pixels = fb_pitch / (int)sizeof(uint32_t);

        switch (fb_rotate) {
        case 90:
            logical_w = fb_height;
            logical_h = fb_width;
            con.origin = fb_width - 1;
            con.step_x = row_pixels;
            con.step_y = -1;
            break;
        case 180:
            logical_w = fb_width;
            logical_h = fb_height;
            con.origin = ((long)fb_height - 1) * row_pixels + fb_width - 1;
            con.step_x = -1;
            con.step_y = -row_pixels;
            break;
        case 270:
            logical_w = fb_height;
            logical_h = fb_width;
            con.origin = ((long)fb_height - 1) * row_pixels;
            con.step_x = -row_pixels;
            con.step_y = 1;
            break;
        default:
            logical_w = fb_width;
            logical_h = fb_height;
            con.origin = 0;
            con.step_x = 1;
            con.step_y = row_pixels;
            break;
        }
    }

    cols = logical_w / (CONSOLE_FONT_WIDTH * fb_scale);
    rows = logical_h / (CONSOLE_FONT_HEIGHT * fb_scale);
    /* The last row belongs to the status line rather than to the terminal. */
    rows -= 1;
    if (cols <= 0 || rows <= 0) {
        return;
    }
    con.cols = cols > MAX_COLS ? MAX_COLS : cols;
    con.rows = rows > MAX_ROWS ? MAX_ROWS : rows;

    con.fg = DEFAULT_FG;
    con.bg = DEFAULT_BG;
    con.cursor_visible = 1;
    con.magnify = 1;

    for (int r = 0; r < con.rows; r++) {
        for (int c = 0; c < con.cols; c++) {
            cells[r][c].ch = ' ';
            cells[r][c].fg = DEFAULT_FG;
            cells[r][c].bg = DEFAULT_BG;
            cells[r][c].flags = 0;
            cells[r][c].span = 1;
            shadow[r][c].ch = CELL_UNDRAWN;
            shadow[r][c].flags = 0;
            shadow[r][c].span = 1;
        }
        row_magnify[r] = 1;
        shadow_magnify[r] = 1;
    }

    con.usable = 1;
}

int console_fb_available(void)
{
    if (!con.ready) {
        console_init();
    }

    return con.usable;
}

int console_fb_rows(void)
{
    return console_fb_available() ? con.rows : 0;
}

int console_fb_cols(void)
{
    return console_fb_available() ? con.cols : 0;
}

/*
 * A glyph as the renderer needs it: the rows, how wide the source bitmap is, how
 * tall, and how many cells it covers.
 *
 * The three sources have different dimensions but all magnify to the same cell.
 * A cell is 8*scale by 16*scale pixels, so Terminus at 8x16 magnifies by scale,
 * and Misaki at 4x8 or 8x8 by twice that -- filling one cell or two exactly. The
 * magnification is therefore derived rather than chosen, and is uniform in both
 * directions for every source, so nothing is ever stretched.
 */
typedef struct {
    const uint8_t *rows;
    int src_w;
    int src_h;
    int cells;
} glyph_t;

/* Shown where the font has no glyph for a character. */
static const uint8_t missing_rows[8] = {
    0x00, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x7e, 0x00,
};

static const console_glyph_t *misaki_find(uint16_t codepoint)
{
    size_t low = 0;
    size_t high = console_misaki_count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;

        if (console_misaki[mid].codepoint < codepoint) {
            low = mid + 1;
        } else if (console_misaki[mid].codepoint > codepoint) {
            high = mid;
        } else {
            return &console_misaki[mid];
        }
    }

    return NULL;
}

/*
 * How many cells a character occupies when the font has no opinion.
 *
 * Misaki states the width of every character it draws, so this only decides the
 * ones it does not. The ranges are the blocks East Asian Width calls wide or
 * full width, which is what Reline uses for the same purpose: getting this wrong
 * does not just misplace a glyph, it makes the line editor's idea of the cursor
 * column disagree with the screen.
 */
static int codepoint_is_wide(uint32_t codepoint)
{
    return (codepoint >= 0x1100 && codepoint <= 0x115f) ||
           (codepoint >= 0x2e80 && codepoint <= 0x303e) ||
           (codepoint >= 0x3041 && codepoint <= 0x33ff) ||
           (codepoint >= 0x3400 && codepoint <= 0x4dbf) ||
           (codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
           (codepoint >= 0xa000 && codepoint <= 0xa4cf) ||
           (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
           (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
           (codepoint >= 0xfe30 && codepoint <= 0xfe6f) ||
           (codepoint >= 0xff00 && codepoint <= 0xff60) ||
           (codepoint >= 0xffe0 && codepoint <= 0xffe6) ||
           codepoint > 0xffff;
}

static int cells_for(uint32_t codepoint)
{
    const console_glyph_t *glyph;

    if (codepoint >= CONSOLE_FONT_FIRST && codepoint <= CONSOLE_FONT_LAST) {
        return 1;
    }

    glyph = codepoint <= 0xffff ? misaki_find((uint16_t)codepoint) : NULL;
    if (glyph != NULL) {
        return glyph->wide ? 2 : 1;
    }

    return codepoint_is_wide(codepoint) ? 2 : 1;
}

static glyph_t glyph_for(uint16_t codepoint)
{
    glyph_t glyph;
    const console_glyph_t *found;

    if (codepoint >= CONSOLE_FONT_FIRST && codepoint <= CONSOLE_FONT_LAST) {
        glyph.rows = console_font[codepoint - CONSOLE_FONT_FIRST];
        glyph.src_w = CONSOLE_FONT_WIDTH;
        glyph.src_h = CONSOLE_FONT_HEIGHT;
        glyph.cells = 1;
        return glyph;
    }

    found = misaki_find(codepoint);
    if (found != NULL) {
        glyph.rows = found->rows;
        glyph.src_w = found->wide ? 8 : 4;
        glyph.src_h = 8;
        glyph.cells = found->wide ? 2 : 1;
        return glyph;
    }

    glyph.rows = missing_rows;
    glyph.src_w = 8;
    glyph.src_h = 8;
    glyph.cells = codepoint_is_wide(codepoint) ? 2 : 1;
    return glyph;
}

/*
 * Draw the contents of one cell over the area it claims.
 *
 * The area is given rather than derived: `span` cells across and `rows_tall`
 * rows down. Both are what the terminal state says, not what the glyph would
 * like, so a cell whose character no longer fits the space -- the tail of a
 * character whose head was overwritten, or a blank sitting in a magnified row --
 * still has its whole area repainted instead of leaving part of the previous
 * contents behind.
 *
 * Magnification needs no special case. Every glyph source is exactly half its
 * cell in each direction, so scaling one to fill n cells across also fills n
 * rows down, and the factor is simply the ratio. Where the ratios disagree the
 * glyph does not belong here and the area is painted as background.
 */
/* The framebuffer word for a logical pixel, wherever the panel happens to face. */
static inline volatile uint32_t *pixel_at(int x, int y)
{
    return (volatile uint32_t *)con.pixels + con.origin + (long)x * con.step_x +
           (long)y * con.step_y;
}

static void draw_glyph(int row, int col, int span, int rows_tall, int cursor)
{
    const cell_t *cell = &cells[row][col];
    uint16_t ch = (cell->flags & CELL_TAIL) != 0 ? (uint16_t)' ' : cell->ch;
    glyph_t glyph = glyph_for(ch == CELL_UNDRAWN ? (uint16_t)' ' : ch);
    int scale = con.scale;
    int cell_w = CONSOLE_FONT_WIDTH * scale;
    int cell_h = CONSOLE_FONT_HEIGHT * scale;
    int width_px = cell_w * span;
    int height_px = cell_h * rows_tall;
    uint32_t fg = palette[cell->fg & 0x0f];
    uint32_t bg = palette[cell->bg & 0x0f];
    int pixels_x;
    int pixels_y;

    if (cursor) {
        uint32_t swap = fg;

        fg = bg;
        bg = swap;
    }

    pixels_x = width_px / glyph.src_w;
    pixels_y = height_px / glyph.src_h;
    if (span % glyph.cells != 0 || pixels_x != pixels_y || pixels_x < 1 ||
        width_px % glyph.src_w != 0 || height_px % glyph.src_h != 0) {
        glyph.rows = NULL;
    }

    if (glyph.rows == NULL) {
        for (int y = row * cell_h; y < row * cell_h + height_px; y++) {
            volatile uint32_t *out = pixel_at(col * cell_w, y);

            for (int x = 0; x < width_px; x++) {
                *out = bg;
                out += con.step_x;
            }
        }
        return;
    }

    for (int sy = 0; sy < glyph.src_h; sy++) {
        uint8_t bits = glyph.rows[sy];

        for (int ry = 0; ry < pixels_y; ry++) {
            int y = row * cell_h + sy * pixels_y + ry;
            volatile uint32_t *out = pixel_at(col * cell_w, y);

            for (int sx = 0; sx < glyph.src_w; sx++) {
                uint32_t colour = (bits & (0x80u >> sx)) != 0 ? fg : bg;

                for (int rx = 0; rx < pixels_x; rx++) {
                    *out = colour;
                    out += con.step_x;
                }
            }
        }
    }
}

/*
 * The status row, drawn straight rather than through the terminal.
 *
 * It sits at row index con.rows, one past the last the terminal knows about, so
 * nothing the terminal does can disturb it and it needs no place in the shadow.
 * Inverted colours mark it as not being part of what the program is showing.
 */
void console_fb_set_status(const char *text)
{
    int row;

    if (!console_fb_available() || text == NULL) {
        return;
    }
    row = con.rows;

    for (int c = 0; c < con.cols; c++) {
        unsigned char ch = (unsigned char)text[c];

        /* Stop at the end of the string, then pad; the row is repainted whole so
         * that a shorter line erases a longer one. */
        if (ch == '\0') {
            for (int rest = c; rest < con.cols; rest++) {
                cells[row][rest].ch = ' ';
                cells[row][rest].fg = 0;
                cells[row][rest].bg = 6;
                cells[row][rest].flags = 0;
                cells[row][rest].span = 1;
            }
            break;
        }
        if (ch < CONSOLE_FONT_FIRST || ch > CONSOLE_FONT_LAST) {
            ch = ' ';
        }
        cells[row][c].ch = ch;
        cells[row][c].fg = 0;
        cells[row][c].bg = 6;
        cells[row][c].flags = 0;
        cells[row][c].span = 1;
    }

    for (int c = 0; c < con.cols; c++) {
        draw_glyph(row, c, 1, 1, 0);
    }
}

static int cell_differs(int row, int col)
{
    return cells[row][col].ch != shadow[row][col].ch ||
           cells[row][col].fg != shadow[row][col].fg ||
           cells[row][col].bg != shadow[row][col].bg ||
           cells[row][col].flags != shadow[row][col].flags ||
           cells[row][col].span != shadow[row][col].span;
}

/*
 * Whether a row lies underneath a magnified one and so has no screen of its own.
 *
 * Only the rows a taller line reaches into are hidden. They keep their contents,
 * which is what lets a line written before the magnification above it reappear
 * once that magnification is removed.
 */
static int row_is_covered(const uint8_t *magnify, int row)
{
    for (int above = 1; above < MAX_MAGNIFY && row - above >= 0; above++) {
        if (magnify[row - above] > above) {
            return 1;
        }
    }

    return 0;
}

/* Force a band of rows to be painted again, used where the geometry rather than
 * the text has changed and comparing cells would not notice. */
static void invalidate_rows(int from, int count)
{
    for (int r = from; r < from + count && r < con.rows; r++) {
        if (r < 0) {
            continue;
        }
        for (int c = 0; c < con.cols; c++) {
            shadow[r][c].ch = CELL_UNDRAWN;
        }
    }
}

static void console_flush(void)
{
    for (int r = 0; r < con.rows; r++) {
        int covered = row_is_covered(row_magnify, r);
        int c = 0;

        /* A row that has just been covered, or just uncovered, is nothing the
         * cell comparison can detect: its contents did not change, only whether
         * they are visible. */
        if (covered != row_is_covered(shadow_magnify, r)) {
            invalidate_rows(r, 1);
        }
        shadow_magnify[r] = row_magnify[r];

        if (covered) {
            continue;
        }

        while (c < con.cols) {
            int span = cells[r][c].span;
            int dirty = 0;
            int cursor = 0;
            int had_cursor = 0;

            /* A tail left without its head still owns its cell, and is painted
             * as background rather than skipped. */
            if (span < 1) {
                span = 1;
            }
            if (c + span > con.cols) {
                span = con.cols - c;
            }

            for (int k = 0; k < span; k++) {
                if (cell_differs(r, c + k)) {
                    dirty = 1;
                }
                if (con.cursor_visible && r == con.row && c + k == con.col) {
                    cursor = 1;
                }
                if (shadow_cursor && r == shadow_row && c + k == shadow_col) {
                    had_cursor = 1;
                }
            }
            /* The cursor is not part of a cell, so a character it has just left
             * or arrived at needs repainting although its contents are
             * unchanged. */
            if (cursor != had_cursor) {
                dirty = 1;
            }

            if (dirty) {
                draw_glyph(r, c, span, row_magnify[r], cursor);
                for (int k = 0; k < span; k++) {
                    shadow[r][c + k] = cells[r][c + k];
                }
            }

            c += span;
        }
    }

    shadow_row = con.row;
    shadow_col = con.col;
    shadow_cursor = con.cursor_visible;
}

static void clear_cells(int row, int from, int to)
{
    for (int c = from; c <= to && c < con.cols; c++) {
        cells[row][c].ch = ' ';
        cells[row][c].fg = con.fg;
        cells[row][c].bg = con.bg;
        cells[row][c].flags = 0;
        cells[row][c].span = 1;
    }
}

/* Erasing an entire row takes its magnification with it, so a line that held a
 * title does not keep a title's height once something else is written there. */
static void clear_row(int row)
{
    clear_cells(row, 0, con.cols - 1);
    row_magnify[row] = 1;
}

static void scroll_up(void)
{
    memmove(&cells[0][0], &cells[1][0], (size_t)(con.rows - 1) * sizeof(cells[0]));
    memmove(&row_magnify[0], &row_magnify[1], (size_t)(con.rows - 1) * sizeof(row_magnify[0]));
    clear_row(con.rows - 1);

    /*
     * Rows of ordinary height need no help: their cells moved, and comparing
     * them against what is on the screen finds exactly the ones that changed,
     * which on a mostly blank screen is far fewer than all of them. Magnified
     * rows are not so simple -- a band that was one tall line is now offset by a
     * row -- so when any exist the screen is repainted rather than reasoned
     * about.
     */
    for (int r = 0; r < con.rows; r++) {
        if (row_magnify[r] > 1) {
            invalidate_rows(0, con.rows);
            break;
        }
    }
}

static void line_feed(void)
{
    con.wrap_pending = 0;
    if (con.row + 1 < con.rows) {
        con.row++;
    } else {
        scroll_up();
    }
}

static void put_printable(uint32_t codepoint)
{
    int span = cells_for(codepoint) * con.magnify;
    uint8_t fg;
    uint8_t bg;

    if (con.wrap_pending) {
        con.col = 0;
        line_feed();
    }
    /* A character wider than one cell will not straddle the right margin.
     * Terminals blank the remaining columns and start it on the next line, and
     * Reline's layout assumes exactly that. */
    if (span > 1 && con.col + span > con.cols) {
        clear_cells(con.row, con.col, con.cols - 1);
        con.col = 0;
        line_feed();
    }
    if (span > con.cols) {
        /* Wider than the screen; nothing can be done but draw what fits. */
        span = con.cols;
    }

    /* The magnification belongs to the row, and takes effect the moment the row
     * receives a character. Changing it changes how tall the row is, so the band
     * it reaches over has to be painted again. */
    if (row_magnify[con.row] != (uint8_t)con.magnify) {
        int previous = row_magnify[con.row];
        int affected = previous > con.magnify ? previous : con.magnify;

        row_magnify[con.row] = (uint8_t)con.magnify;
        invalidate_rows(con.row, affected);
    }

    if (con.reverse) {
        fg = con.bg;
        bg = (uint8_t)(con.bold ? (con.fg | 8u) : con.fg);
    } else {
        fg = (uint8_t)(con.bold ? (con.fg | 8u) : con.fg);
        bg = con.bg;
    }

    /* Landing on part of a character that is already there leaves the rest of it
     * orphaned, so what remains on either side is blanked rather than left to
     * render as a fragment of a glyph. */
    if ((cells[con.row][con.col].flags & CELL_TAIL) != 0) {
        for (int c = con.col - 1; c >= 0; c--) {
            cells[con.row][c].ch = ' ';
            cells[con.row][c].span = 1;
            if ((cells[con.row][c].flags & CELL_TAIL) == 0) {
                break;
            }
            cells[con.row][c].flags = 0;
        }
    }
    for (int c = con.col + span; c < con.cols; c++) {
        if ((cells[con.row][c].flags & CELL_TAIL) == 0) {
            break;
        }
        cells[con.row][c].ch = ' ';
        cells[con.row][c].flags = 0;
        cells[con.row][c].span = 1;
    }

    cells[con.row][con.col].ch = (uint16_t)(codepoint > 0xffff ? CELL_MISSING : codepoint);
    cells[con.row][con.col].fg = fg;
    cells[con.row][con.col].bg = bg;
    cells[con.row][con.col].flags = 0;
    cells[con.row][con.col].span = (uint8_t)span;

    for (int k = 1; k < span; k++) {
        cells[con.row][con.col + k].ch = ' ';
        cells[con.row][con.col + k].fg = fg;
        cells[con.row][con.col + k].bg = bg;
        cells[con.row][con.col + k].flags = CELL_TAIL;
        cells[con.row][con.col + k].span = 0;
    }

    if (con.col + span < con.cols) {
        con.col += span;
    } else {
        con.col = con.cols - 1;
        con.wrap_pending = 1;
    }
}

static void move_to(int row, int col)
{
    con.row = row < 0 ? 0 : (row >= con.rows ? con.rows - 1 : row);
    con.col = col < 0 ? 0 : (col >= con.cols ? con.cols - 1 : col);
    con.wrap_pending = 0;
}

static int param(int index, int fallback)
{
    if (index >= con.nparams || con.params[index] == 0) {
        return fallback;
    }

    return con.params[index];
}

static void apply_sgr(void)
{
    if (con.nparams == 0) {
        con.fg = DEFAULT_FG;
        con.bg = DEFAULT_BG;
        con.bold = 0;
        con.reverse = 0;
        return;
    }

    for (int i = 0; i < con.nparams; i++) {
        int p = con.params[i];

        if (p == 0) {
            con.fg = DEFAULT_FG;
            con.bg = DEFAULT_BG;
            con.bold = 0;
            con.reverse = 0;
        } else if (p == 1) {
            con.bold = 1;
        } else if (p == 22) {
            con.bold = 0;
        } else if (p == 7) {
            con.reverse = 1;
        } else if (p == 27) {
            con.reverse = 0;
        } else if (p >= 30 && p <= 37) {
            con.fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            con.fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            con.bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            con.bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            con.fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            con.bg = (uint8_t)(p - 100 + 8);
        }
        /* 38 and 48 introduce 256 colour and true colour selections whose
         * arguments would have to be consumed as well. Nothing here emits them,
         * and ignoring the introducer alone would misread the arguments as
         * further attributes, so they are left out deliberately. */
    }
}

static void erase_in_display(int mode)
{
    if (mode == 0) {
        clear_cells(con.row, con.col, con.cols - 1);
        for (int r = con.row + 1; r < con.rows; r++) {
            clear_row(r);
        }
    } else if (mode == 1) {
        for (int r = 0; r < con.row; r++) {
            clear_row(r);
        }
        clear_cells(con.row, 0, con.col);
    } else {
        for (int r = 0; r < con.rows; r++) {
            clear_row(r);
        }
    }
}

static void erase_in_line(int mode)
{
    if (mode == 0) {
        if (con.col == 0) {
            clear_row(con.row);
        } else {
            clear_cells(con.row, con.col, con.cols - 1);
        }
    } else if (mode == 1) {
        clear_cells(con.row, 0, con.col);
    } else {
        clear_row(con.row);
    }
}

static void insert_lines(int count)
{
    for (int i = 0; i < count; i++) {
        for (int r = con.rows - 1; r > con.row; r--) {
            memcpy(&cells[r][0], &cells[r - 1][0], sizeof(cells[0]));
        }
        clear_row(con.row);
    }
}

static void delete_lines(int count)
{
    for (int i = 0; i < count; i++) {
        for (int r = con.row; r < con.rows - 1; r++) {
            memcpy(&cells[r][0], &cells[r + 1][0], sizeof(cells[0]));
        }
        clear_cells(con.rows - 1, 0, con.cols - 1);
    }
}

static void delete_chars(int count)
{
    int remaining = con.cols - con.col - count;

    if (remaining > 0) {
        memmove(&cells[con.row][con.col], &cells[con.row][con.col + count],
                (size_t)remaining * sizeof(cell_t));
        clear_cells(con.row, con.cols - count, con.cols - 1);
    } else {
        clear_cells(con.row, con.col, con.cols - 1);
    }
}

static void insert_chars(int count)
{
    int remaining = con.cols - con.col - count;

    if (remaining > 0) {
        memmove(&cells[con.row][con.col + count], &cells[con.row][con.col],
                (size_t)remaining * sizeof(cell_t));
    }
    clear_cells(con.row, con.col, con.col + count - 1);
}

static void report_cursor(void)
{
    char buf[24];
    int n = 0;
    int values[2] = { con.row + 1, con.col + 1 };

    buf[n++] = '\033';
    buf[n++] = '[';
    for (int i = 0; i < 2; i++) {
        char digits[8];
        int len = 0;
        int value = values[i];

        do {
            digits[len++] = (char)('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (len > 0) {
            buf[n++] = digits[--len];
        }
        buf[n++] = i == 0 ? ';' : 'R';
    }
    buf[n] = '\0';

    reply_push(buf);
}

static void dispatch_csi(char final)
{
    switch (final) {
    case 'A':
        move_to(con.row - param(0, 1), con.col);
        break;
    case 'B':
        move_to(con.row + param(0, 1), con.col);
        break;
    case 'C':
        move_to(con.row, con.col + param(0, 1));
        break;
    case 'D':
        move_to(con.row, con.col - param(0, 1));
        break;
    case 'E':
        move_to(con.row + param(0, 1), 0);
        break;
    case 'F':
        move_to(con.row - param(0, 1), 0);
        break;
    case 'G':
        move_to(con.row, param(0, 1) - 1);
        break;
    case 'd':
        move_to(param(0, 1) - 1, con.col);
        break;
    case 'H':
    case 'f':
        move_to(param(0, 1) - 1, param(1, 1) - 1);
        break;
    case 'J':
        erase_in_display(con.nparams > 0 ? con.params[0] : 0);
        break;
    case 'K':
        erase_in_line(con.nparams > 0 ? con.params[0] : 0);
        break;
    case 'L':
        insert_lines(param(0, 1));
        break;
    case 'M':
        delete_lines(param(0, 1));
        break;
    case 'P':
        delete_chars(param(0, 1));
        break;
    case '@':
        insert_chars(param(0, 1));
        break;
    case 'S':
        for (int i = param(0, 1); i > 0; i--) {
            scroll_up();
        }
        break;
    case 'm':
        apply_sgr();
        break;
    case 'z':
        /*
         * Magnification, a private sequence with no standard meaning. It exists
         * because a slide deck needs a title larger than its body text and a
         * terminal has no other way to ask for one.
         *
         * The parameter counts from zero for compatibility with the console this
         * was borrowed from, so n selects a size of n + 1: ESC [ 0 z is ordinary
         * text and ESC [ 3 z is four times as large in each direction, covering
         * four rows.
         */
        con.magnify = (con.nparams > 0 ? con.params[0] : 0) + 1;
        if (con.magnify < 1) {
            con.magnify = 1;
        }
        if (con.magnify > MAX_MAGNIFY) {
            con.magnify = MAX_MAGNIFY;
        }
        break;
    case 'n':
        if (con.nparams > 0 && con.params[0] == 6) {
            report_cursor();
        }
        break;
    case 'h':
        if (con.private == '?' && con.nparams > 0 && con.params[0] == 25) {
            con.cursor_visible = 1;
        }
        break;
    case 'l':
        if (con.private == '?' && con.nparams > 0 && con.params[0] == 25) {
            con.cursor_visible = 0;
        }
        break;
    case 's':
        con.saved_row = con.row;
        con.saved_col = con.col;
        break;
    case 'u':
        move_to(con.saved_row, con.saved_col);
        break;
    default:
        break;
    }
}

/*
 * Decode UTF-8 as it arrives.
 *
 * Ruby writes UTF-8 and nothing upstream converts it, so the bytes have to be
 * reassembled here. A write may end in the middle of a character -- stdio
 * flushes on its own buffer boundaries, not on character ones -- so the partial
 * state lives across calls.
 *
 * Only the ground state decodes. Escape sequences are ASCII by construction, and
 * a byte with the high bit set inside one is malformed rather than text.
 */
static void put_ground(char c)
{
    unsigned char byte = (unsigned char)c;

    if (con.utf8_need > 0) {
        if ((byte & 0xc0) == 0x80) {
            con.utf8 = (con.utf8 << 6) | (uint32_t)(byte & 0x3f);
            if (--con.utf8_need == 0) {
                put_printable(con.utf8);
            }
            return;
        }
        /* A truncated character. The byte that ended it is still a byte of text,
         * so it is taken as the start of the next one rather than dropped with
         * the remains of this. */
        con.utf8_need = 0;
    }

    if (byte >= 0x80) {
        if (byte >= 0xc2 && byte <= 0xdf) {
            con.utf8 = byte & 0x1fu;
            con.utf8_need = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            con.utf8 = byte & 0x0fu;
            con.utf8_need = 2;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            con.utf8 = byte & 0x07u;
            con.utf8_need = 3;
        }
        /* Anything else is a stray continuation byte or an overlong lead, and
         * has no character to belong to. */
        return;
    }

    switch (c) {
    case '\n':
        /*
         * A line feed returns the carriage as well.
         *
         * Strictly that is the tty layer's job -- OPOST and ONLCR -- and there is
         * no tty layer here. Doing it unconditionally is what makes the two
         * outputs agree: the debug console this mirrors goes through the kernel's
         * uart_console_putchar, which handles CR/LF itself (pc99/machine/io.c:46),
         * so honouring termios on one side and not the other would leave the
         * framebuffer stepping right on every line while the serial log stayed
         * straight. Reline emits an explicit carriage return before its newlines
         * in raw mode, and a redundant one costs nothing.
         */
        con.col = 0;
        line_feed();
        break;
    case '\r':
        con.col = 0;
        con.wrap_pending = 0;
        break;
    case '\b':
        if (con.col > 0) {
            con.col--;
        }
        con.wrap_pending = 0;
        break;
    case '\t':
        con.wrap_pending = 0;
        do {
            if (con.col + 1 >= con.cols) {
                break;
            }
            con.col++;
        } while (con.col % TAB_WIDTH != 0);
        break;
    case '\a':
        break;
    case '\033':
        con.state = STATE_ESCAPE;
        break;
    case '\017':
    case '\016':
        /* Shift in and shift out select a character set this has only one of. */
        break;
    default:
        if (byte >= 0x20 && byte < 0x7f) {
            put_printable(byte);
        }
        /* Anything else is a control character with no meaning here. */
        break;
    }
}

static void put_escape(char c)
{
    switch (c) {
    case '[':
        con.state = STATE_CSI;
        con.nparams = 0;
        con.have_param = 0;
        con.private = '\0';
        memset(con.params, 0, sizeof(con.params));
        break;
    case ']':
    case 'P':
    case '^':
    case '_':
        con.state = STATE_STRING;
        break;
    case '7':
        con.saved_row = con.row;
        con.saved_col = con.col;
        con.state = STATE_GROUND;
        break;
    case '8':
        move_to(con.saved_row, con.saved_col);
        con.state = STATE_GROUND;
        break;
    case 'M':
        if (con.row > 0) {
            con.row--;
        }
        con.state = STATE_GROUND;
        break;
    case 'c':
        erase_in_display(2);
        move_to(0, 0);
        con.magnify = 1;
        con.fg = DEFAULT_FG;
        con.bg = DEFAULT_BG;
        con.bold = 0;
        con.reverse = 0;
        con.state = STATE_GROUND;
        break;
    default:
        con.state = STATE_GROUND;
        break;
    }
}

static void put_csi(char c)
{
    if (c >= '0' && c <= '9') {
        if (con.nparams < MAX_PARAMS) {
            if (!con.have_param) {
                con.nparams++;
                con.have_param = 1;
            }
            con.params[con.nparams - 1] = con.params[con.nparams - 1] * 10 + (c - '0');
        }
        return;
    }

    if (c == ';') {
        if (!con.have_param && con.nparams < MAX_PARAMS) {
            con.nparams++;
        }
        con.have_param = 0;
        return;
    }

    if (c == '?' || c == '>' || c == '<' || c == '=') {
        con.private = c;
        return;
    }

    /* Intermediate bytes, such as the space in a cursor style request. */
    if (c >= 0x20 && c <= 0x2f) {
        return;
    }

    dispatch_csi(c);
    con.state = STATE_GROUND;
}

void console_fb_write(const char *buf, size_t len)
{
    if (!console_fb_available() || buf == NULL) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        switch (con.state) {
        case STATE_GROUND:
            put_ground(c);
            break;
        case STATE_ESCAPE:
            put_escape(c);
            break;
        case STATE_CSI:
            put_csi(c);
            break;
        case STATE_STRING:
            /* A string sequence ends at BEL or at ESC \, and nothing here acts on
             * one, so only its extent matters. */
            if (c == '\a') {
                con.state = STATE_GROUND;
            } else if (c == '\033') {
                con.state = STATE_STRING_ESC;
            }
            break;
        case STATE_STRING_ESC:
            con.state = c == '\\' ? STATE_GROUND : STATE_STRING;
            break;
        }
    }

    console_flush();
}
