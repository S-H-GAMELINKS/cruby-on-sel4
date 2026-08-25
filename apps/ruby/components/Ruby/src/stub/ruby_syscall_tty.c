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
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* The fallback reline uses when the size is unavailable, so a terminal that never
 * reports its own dimensions still gets a consistent answer. */
#define CONSOLE_ROWS 24
#define CONSOLE_COLS 80

static struct termios console_termios;
static int console_termios_ready;

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
        ws->ws_row = CONSOLE_ROWS;
        ws->ws_col = CONSOLE_COLS;
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
