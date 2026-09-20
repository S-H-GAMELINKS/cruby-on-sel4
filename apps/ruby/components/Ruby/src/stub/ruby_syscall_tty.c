/*
 * Console shims for running CRuby inside a CAmkES component.
 *
 * The serial console reached through SerialServer is a real ANSI terminal on the
 * other side -- QEMU hands it to the host's terminal -- so this reports it as a
 * tty and answers the terminal queries that go with that. What is missing is the
 * kernel side of a tty: no line discipline, no window size, no session.
 *
 * Reporting a tty is what lets Reline edit a line in place. reline/io/ansi.rb
 * asks for the cursor position (cursor_pos, ansi.rb:206) only when both_tty?
 * holds, and without an answer it assumes the cursor sits at the origin, so every
 * redraw lands in the wrong place and the line appears again below instead of
 * being rewritten.
 *
 * io/console reaches raw mode through libc rather than ioctl -- ext/io/console
 * uses tcgetattr and tcsetattr (console.c:30,35) -- so the termios pair below is
 * enough for IO#raw. The flags are stored and handed back, not acted on: reads
 * are already unbuffered and unechoed (see ruby_syscall_poll.c), which is what
 * raw mode asks for, and cooked mode has no line discipline to restore.
 */

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* Used until the terminal has been asked, and if it never answers. Matches
 * reline's own fallback, so behaviour is unchanged when measuring fails. */
#define CONSOLE_ROWS 24
#define CONSOLE_COLS 80

/* Long enough for a terminal on the other end of a serial line to answer,
 * short enough not to stall startup if nothing is listening. */
#define CONSOLE_QUERY_TIMEOUT_MS 200

static struct termios console_termios;
static int console_termios_ready;

static unsigned short console_rows = CONSOLE_ROWS;
static unsigned short console_cols = CONSOLE_COLS;
static int console_size_known;

static int is_console(int fd)
{
    return fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO;
}

/* A plausible cooked state, so the first tcgetattr does not report a terminal
 * with every flag cleared. */
static void console_termios_init(void)
{
    if (console_termios_ready) {
        return;
    }

    memset(&console_termios, 0, sizeof(console_termios));
    console_termios.c_iflag = ICRNL;
    console_termios.c_oflag = OPOST | ONLCR;
    console_termios.c_cflag = CS8 | CREAD;
    console_termios.c_lflag = ISIG | ICANON | ECHO;
    console_termios.c_cc[VMIN] = 1;
    console_termios.c_cc[VTIME] = 0;
    console_termios_ready = 1;
}

/*
 * Parse a cursor position report: ESC [ rows ; cols R
 */
static int parse_cursor_report(const char *buf, size_t len, unsigned int *rows, unsigned int *cols)
{
    size_t i = 0;
    unsigned int value[2] = { 0, 0 };
    int field = 0;
    int digits = 0;

    while (i + 1 < len && !(buf[i] == '\033' && buf[i + 1] == '[')) {
        i++;
    }
    if (i + 1 >= len) {
        return -1;
    }
    i += 2;

    for (; i < len; i++) {
        char c = buf[i];

        if (c >= '0' && c <= '9') {
            value[field] = value[field] * 10u + (unsigned int)(c - '0');
            digits++;
        } else if (c == ';' && field == 0 && digits > 0) {
            field = 1;
            digits = 0;
        } else if (c == 'R' && field == 1 && digits > 0) {
            *rows = value[0];
            *cols = value[1];
            return 0;
        } else {
            return -1;
        }
    }

    return -1;
}

/*
 * Ask the terminal how large it is.
 *
 * seL4 cannot know: the console is a serial line and nothing on this side is told
 * what sits on the other end. The terminal does know, and will say so if asked.
 * Driving the cursor far past any real screen clamps it to the bottom right
 * corner, and a cursor position report then carries the dimensions. The position
 * is saved and restored around the query so that whatever the caller was drawing
 * is left alone.
 *
 * Only stdin is measured, and only once. musl probes TIOCGWINSZ on stdout the
 * first time anything is printed -- __stdout_write, deciding line versus full
 * buffering -- which is far too early to be exchanging escape sequences.
 *
 * Anything the user types during the exchange is consumed as part of the reply.
 * The query happens on reline's first winsize call, before it reads a key, so the
 * window is small.
 */
static void measure_console(void)
{
    static const char query[] = "\033[s\033[999;999H\033[6n";
    static const char restore[] = "\033[u";
    char buf[32];
    size_t used = 0;
    unsigned int rows = 0;
    unsigned int cols = 0;

    console_size_known = 1;

    (void)write(STDOUT_FILENO, query, sizeof(query) - 1);

    while (used < sizeof(buf) - 1) {
        struct pollfd pfd;
        ssize_t n;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, CONSOLE_QUERY_TIMEOUT_MS) <= 0) {
            break;
        }
        n = read(STDIN_FILENO, buf + used, 1);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        if (buf[used - 1] == 'R') {
            break;
        }
    }

    (void)write(STDOUT_FILENO, restore, sizeof(restore) - 1);

    if (parse_cursor_report(buf, used, &rows, &cols) == 0 && rows > 0 && cols > 0) {
        console_rows = (unsigned short)rows;
        console_cols = (unsigned short)cols;
    }
}

int isatty(int fd)
{
    if (is_console(fd)) {
        return 1;
    }

    errno = ENOTTY;
    return 0;
}

int tcgetattr(int fd, struct termios *t)
{
    if (!is_console(fd)) {
        errno = ENOTTY;
        return -1;
    }
    if (t == NULL) {
        errno = EFAULT;
        return -1;
    }

    console_termios_init();
    *t = console_termios;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *t)
{
    /* Nothing is queued, so draining or flushing first makes no difference. */
    (void)optional_actions;

    if (!is_console(fd)) {
        errno = ENOTTY;
        return -1;
    }
    if (t == NULL) {
        errno = EFAULT;
        return -1;
    }

    console_termios_init();
    console_termios = *t;
    return 0;
}

int ioctl(int fd, int request, ...)
{
    va_list ap;
    void *arg;

    if (!is_console(fd)) {
        errno = ENOTTY;
        return -1;
    }

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize *)arg;

        if (ws == NULL) {
            errno = EFAULT;
            return -1;
        }
        if (fd == STDIN_FILENO && !console_size_known) {
            measure_console();
        }
        ws->ws_row = console_rows;
        ws->ws_col = console_cols;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }

    case TIOCSWINSZ:
        /* There is no window to resize; accepting keeps callers on their normal
         * path. */
        return 0;

    default:
        errno = ENOTTY;
        return -1;
    }
}
