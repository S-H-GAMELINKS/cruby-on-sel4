/*
 * Descriptor-level I/O for running CRuby inside a CAmkES component: fake eventfd
 * and epoll, plus SerialServer-backed standard input.
 *
 * The serial input path lives here rather than in a file of its own even though
 * it is a different mechanism. read, readv, close and fcntl each have to decide
 * which kind of descriptor they were handed, and splitting that decision across
 * two translation units would mean two places to look when a descriptor behaves
 * unexpectedly. The fake descriptor space is one thing; the kinds within it are
 * eventfd, epoll and stdin.
 *
 * The eventfd and epoll parts:
 *
 * CRuby's thread_pthread.c builds a "communication pipe" during initialisation so
 * that one thread can wake another. On this target none of the mechanisms it tries
 * exist: eventfd2, pipe2 and pipe all reach libsel4muslcsys with no handler, and
 * CRuby then aborts with "can not create communication pipe".
 *
 * The descriptors handed out here are not kernel objects. They are entries in the
 * tables below, numbered from a base well above any real descriptor so that
 * anything unrecognised can be passed through to the real syscall. An eventfd is a
 * plain counter, and epoll_wait reports a watched eventfd as readable when its
 * counter is non-zero.
 *
 * This is enough for a single-threaded VM, where nothing ever needs to be woken.
 * epoll_wait never blocks and ignores its timeout: with one thread there is no
 * other party that could make a descriptor ready, so waiting could only hang.
 * Replacing this means backing wakeups with seL4 notifications.
 *
 * The standard input part:
 *
 * libsel4muslcsys installs no handler for reading fd 0, so Ruby's STDIN.gets has
 * nothing to call. SerialServer publishes typed characters into a shared ring
 * buffer and signals a notification, and readv below drains that buffer.
 *
 * This depends on the assembly declaring `uses GetChar stdin_getchar` and
 * connecting it to SerialServer with seL4SerialServer; without that connection
 * the generated accessors referenced here do not exist and this file will not
 * compile.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* Generated declarations for stdin_getchar_get_buf() and
 * stdin_getchar_notification(), from the seL4SerialServer connection. */
#include <camkes.h>
#include <sel4/sel4.h>

#define FAKE_EVENTFD_BASE 1000
#define FAKE_EVENTFD_COUNT 4
#define FAKE_EPOLL_BASE 1100
#define FAKE_EPOLL_COUNT 2
#define FAKE_EPOLL_WATCH_COUNT 8

typedef struct fake_eventfd {
    int used;
    int fd;
    int flags;
    uint64_t counter;
} fake_eventfd_t;

typedef struct fake_epoll_watch {
    int used;
    int fd;
    struct epoll_event event;
} fake_epoll_watch_t;

typedef struct fake_epoll {
    int used;
    int fd;
    int flags;
    fake_epoll_watch_t watches[FAKE_EPOLL_WATCH_COUNT];
} fake_epoll_t;

static fake_eventfd_t fake_eventfds[FAKE_EVENTFD_COUNT];
static fake_epoll_t fake_epolls[FAKE_EPOLL_COUNT];

static fake_eventfd_t *fake_eventfd_from_fd(int fd)
{
    size_t i;

    for (i = 0; i < FAKE_EVENTFD_COUNT; i++) {
        if (fake_eventfds[i].used && fake_eventfds[i].fd == fd) {
            return &fake_eventfds[i];
        }
    }

    return NULL;
}

static fake_epoll_t *fake_epoll_from_fd(int fd)
{
    size_t i;

    for (i = 0; i < FAKE_EPOLL_COUNT; i++) {
        if (fake_epolls[i].used && fake_epolls[i].fd == fd) {
            return &fake_epolls[i];
        }
    }

    return NULL;
}

int eventfd(unsigned int initval, int flags)
{
    size_t i;

    for (i = 0; i < FAKE_EVENTFD_COUNT; i++) {
        if (!fake_eventfds[i].used) {
            fake_eventfds[i].used = 1;
            fake_eventfds[i].fd = FAKE_EVENTFD_BASE + (int)i;
            fake_eventfds[i].flags = flags;
            fake_eventfds[i].counter = initval;
            return fake_eventfds[i].fd;
        }
    }

    errno = EMFILE;
    return -1;
}

int eventfd_read(int fd, eventfd_t *value)
{
    return read(fd, value, sizeof(*value)) == (ssize_t)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value)
{
    return write(fd, &value, sizeof(value)) == (ssize_t)sizeof(value) ? 0 : -1;
}

int epoll_create1(int flags)
{
    size_t i;

    for (i = 0; i < FAKE_EPOLL_COUNT; i++) {
        if (!fake_epolls[i].used) {
            fake_epolls[i].used = 1;
            fake_epolls[i].fd = FAKE_EPOLL_BASE + (int)i;
            fake_epolls[i].flags = flags;
            memset(fake_epolls[i].watches, 0, sizeof(fake_epolls[i].watches));
            return fake_epolls[i].fd;
        }
    }

    errno = EMFILE;
    return -1;
}

int epoll_create(int size)
{
    if (size <= 0) {
        errno = EINVAL;
        return -1;
    }

    return epoll_create1(0);
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    fake_epoll_t *epoll = fake_epoll_from_fd(epfd);
    size_t i;

    if (epoll == NULL) {
        return (int)syscall(SYS_epoll_ctl, epfd, op, fd, event);
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        if (event == NULL) {
            errno = EINVAL;
            return -1;
        }
        for (i = 0; i < FAKE_EPOLL_WATCH_COUNT; i++) {
            if (epoll->watches[i].used && epoll->watches[i].fd == fd) {
                errno = EEXIST;
                return -1;
            }
        }
        for (i = 0; i < FAKE_EPOLL_WATCH_COUNT; i++) {
            if (!epoll->watches[i].used) {
                epoll->watches[i].used = 1;
                epoll->watches[i].fd = fd;
                epoll->watches[i].event = *event;
                return 0;
            }
        }
        errno = ENOSPC;
        return -1;

    case EPOLL_CTL_MOD:
        if (event == NULL) {
            errno = EINVAL;
            return -1;
        }
        for (i = 0; i < FAKE_EPOLL_WATCH_COUNT; i++) {
            if (epoll->watches[i].used && epoll->watches[i].fd == fd) {
                epoll->watches[i].event = *event;
                return 0;
            }
        }
        errno = ENOENT;
        return -1;

    case EPOLL_CTL_DEL:
        for (i = 0; i < FAKE_EPOLL_WATCH_COUNT; i++) {
            if (epoll->watches[i].used && epoll->watches[i].fd == fd) {
                epoll->watches[i].used = 0;
                return 0;
            }
        }
        errno = ENOENT;
        return -1;

    default:
        errno = EINVAL;
        return -1;
    }
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    fake_epoll_t *epoll = fake_epoll_from_fd(epfd);
    int ready = 0;
    size_t i;

    if (epoll == NULL) {
        return (int)syscall(SYS_epoll_wait, epfd, events, maxevents, timeout);
    }
    if (events == NULL || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < FAKE_EPOLL_WATCH_COUNT && ready < maxevents; i++) {
        fake_eventfd_t *event_source;

        if (!epoll->watches[i].used) {
            continue;
        }

        event_source = fake_eventfd_from_fd(epoll->watches[i].fd);
        if (event_source != NULL && event_source->counter > 0 &&
            (epoll->watches[i].event.events & EPOLLIN) != 0) {
            events[ready] = epoll->watches[i].event;
            events[ready].events = EPOLLIN;
            ready++;
        }
    }

    /* Never wait. Nothing else runs that could make a descriptor ready. */
    (void)timeout;
    return ready;
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout, const sigset_t *sigmask)
{
    (void)sigmask;
    return epoll_wait(epfd, events, maxevents, timeout);
}

/*
 * The descriptor operations below only claim the fake descriptors and pass
 * everything else to the real syscall, so stdio and the seL4 debug console are
 * untouched.
 */

ssize_t read(int fd, void *buf, size_t count)
{
    fake_eventfd_t *event = fake_eventfd_from_fd(fd);

    if (event != NULL) {
        uint64_t value;

        if (buf == NULL || count < sizeof(value)) {
            errno = EINVAL;
            return -1;
        }
        if (event->counter == 0) {
            /* A blocking read here could never be satisfied. */
            errno = EAGAIN;
            return -1;
        }

        if ((event->flags & EFD_SEMAPHORE) != 0) {
            value = 1;
            event->counter--;
        } else {
            value = event->counter;
            event->counter = 0;
        }

        memcpy(buf, &value, sizeof(value));
        return (ssize_t)sizeof(value);
    }

    return syscall(SYS_read, fd, buf, count);
}

/*
 * SerialServer's client ring buffer. The layout is fixed by
 * global-components/components/SerialServer/src/serial.c, which asserts at compile
 * time that the structure fills exactly one 4 KiB dataport, so the assembly must
 * configure stdin_getchar_shmem_size = 0x1000 to match.
 *
 * The server writes at tail and the client advances head, both modulo the size of
 * the character array rather than the whole structure.
 */
typedef struct serial_ring {
    uint32_t head;
    uint32_t tail;
    char buf[4096 - 8];
} serial_ring_t;

static volatile serial_ring_t *serial_ring(void)
{
    return (volatile serial_ring_t *)stdin_getchar_get_buf();
}

/*
 * Blocking read of one character.
 *
 * The wait has to be a loop with the emptiness check inside it. SerialServer only
 * emits a notification when the client has consumed something since the previous
 * one (serial.c:329-331), so a notification can arrive for characters already
 * drained, and conversely no fresh notification arrives while the client sits on
 * an unread buffer. Re-checking head against tail on every wakeup covers both.
 */
static char serial_getchar(void)
{
    volatile serial_ring_t *ring = serial_ring();
    char c;

    while (ring->head == ring->tail) {
        seL4_Wait(stdin_getchar_notification(), NULL);
    }

    c = ring->buf[ring->head];
    ring->head = (ring->head + 1) % sizeof(ring->buf);
    return c;
}

#define SERIAL_EOT 0x04 /* Ctrl-D */
#define SERIAL_ESC 0x1b
#define SERIAL_DEL 0x7f

static void serial_write(const char *s, size_t len)
{
    (void)write(STDOUT_FILENO, s, len);
}

/* There is no line discipline behind the debug console, so echo has to be done
 * here or typing is invisible.
 *
 * A bare newline suffices: the kernel console driver prepends a carriage return
 * to every '\n' (kernel/include/drivers/uart.h:16-20), so echoing "\r\n" would
 * put two carriage returns on the wire. */
static void serial_echo(char c)
{
    if (c == '\r' || c == '\n') {
        serial_write("\n", 1);
    } else {
        serial_write(&c, 1);
    }
}

/* Step back over the last echoed character and blank it. */
static void serial_erase(void)
{
    serial_write("\b \b", 3);
}

/*
 * Swallow a CSI or SS3 sequence. Arrow and function keys arrive as ESC followed
 * by '[' or 'O', optional parameter bytes, and a final byte in 0x40..0x7e.
 * Acting on cursor movement would need a full line editor and a model of what is
 * on screen; discarding the sequence at least keeps the escape bytes out of the
 * line Ruby receives.
 */
static void serial_discard_escape(void)
{
    char c = serial_getchar();

    if (c != '[' && c != 'O') {
        return;
    }
    do {
        c = serial_getchar();
    } while ((unsigned char)c < 0x40 || (unsigned char)c > 0x7e);
}

/*
 * Read one edited line.
 *
 * Ruby's STDIN.gets wants a line at a time, so this returns as soon as Enter
 * arrives rather than filling the caller's buffer. Backspace has to be handled
 * here rather than merely echoed: echoing the erase without dropping the
 * character leaves both the mistake and a literal 0x08 in what Ruby parses.
 *
 * Console input is line oriented, so a scatter read collapses into the first
 * usable buffer. Letting one edited line span several buffers would make
 * backspace cross buffer boundaries for no gain, and Ruby's IO layer passes a
 * single buffer here regardless.
 */
static ssize_t serial_readv(const struct iovec *iov, int iovcnt)
{
    char *base = NULL;
    size_t capacity = 0;
    size_t used = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len != 0) {
            if (iov[i].iov_base == NULL) {
                errno = EFAULT;
                return -1;
            }
            base = (char *)iov[i].iov_base;
            capacity = iov[i].iov_len;
            break;
        }
    }
    if (base == NULL) {
        return 0;
    }

    while (used < capacity) {
        char c = serial_getchar();

        if (c == SERIAL_ESC) {
            serial_discard_escape();
            continue;
        }
        if (c == '\b' || c == SERIAL_DEL) {
            if (used > 0) {
                used--;
                serial_erase();
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            serial_echo('\n');
            base[used++] = '\n';
            return (ssize_t)used;
        }
        if (c == SERIAL_EOT) {
            /* Ctrl-D ends input only on an empty line, as a terminal does. */
            if (used == 0) {
                return 0;
            }
            continue;
        }
        /* Drop the remaining control characters instead of letting them into the
         * line; none of them mean anything without a real terminal. */
        if ((unsigned char)c < 0x20) {
            continue;
        }

        serial_echo(c);
        base[used++] = c;
    }

    return (ssize_t)used;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    int i;

    if (iov == NULL || iovcnt < 0) {
        errno = EINVAL;
        return -1;
    }

    if (fd == STDIN_FILENO) {
        return serial_readv(iov, iovcnt);
    }

    /* Fake descriptors have no syscall behind them, so scatter reads have to be
     * assembled out of the single-buffer path above. */
    if (fake_eventfd_from_fd(fd) != NULL) {
        for (i = 0; i < iovcnt; i++) {
            ssize_t n;

            if (iov[i].iov_len == 0) {
                continue;
            }
            n = read(fd, iov[i].iov_base, iov[i].iov_len);
            if (n < 0) {
                return total > 0 ? total : -1;
            }
            total += n;
            if ((size_t)n != iov[i].iov_len) {
                break;
            }
        }
        return total;
    }

    return syscall(SYS_readv, fd, iov, iovcnt);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    fake_eventfd_t *event = fake_eventfd_from_fd(fd);

    if (event != NULL) {
        uint64_t value;

        if (buf == NULL || count < sizeof(value)) {
            errno = EINVAL;
            return -1;
        }

        memcpy(&value, buf, sizeof(value));
        event->counter += value;
        return (ssize_t)sizeof(value);
    }

    return syscall(SYS_write, fd, buf, count);
}

int close(int fd)
{
    fake_eventfd_t *event = fake_eventfd_from_fd(fd);
    fake_epoll_t *epoll = fake_epoll_from_fd(fd);

    if (event != NULL) {
        event->used = 0;
        event->fd = -1;
        event->flags = 0;
        event->counter = 0;
        return 0;
    }
    if (epoll != NULL) {
        epoll->used = 0;
        epoll->fd = -1;
        epoll->flags = 0;
        memset(epoll->watches, 0, sizeof(epoll->watches));
        return 0;
    }

    return (int)syscall(SYS_close, fd);
}

int fcntl(int fd, int cmd, ...)
{
    fake_eventfd_t *event = fake_eventfd_from_fd(fd);
    fake_epoll_t *epoll = fake_epoll_from_fd(fd);
    va_list ap;
    long arg = 0;

    if (cmd == F_SETFL || cmd == F_SETFD || cmd == F_DUPFD
#ifdef F_DUPFD_CLOEXEC
        || cmd == F_DUPFD_CLOEXEC
#endif
    ) {
        va_start(ap, cmd);
        arg = va_arg(ap, long);
        va_end(ap);
    }

    if (event != NULL || epoll != NULL) {
        int *flags = event != NULL ? &event->flags : &epoll->flags;

        switch (cmd) {
        case F_GETFL:
            return *flags;
        case F_SETFL:
            *flags = (int)arg;
            return 0;
        case F_GETFD:
            return (*flags & O_CLOEXEC) != 0 ? FD_CLOEXEC : 0;
        case F_SETFD:
            if ((arg & FD_CLOEXEC) != 0) {
                *flags |= O_CLOEXEC;
            } else {
                *flags &= ~O_CLOEXEC;
            }
            return 0;
        case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
        case F_DUPFD_CLOEXEC:
#endif
            /* Duplicating a fake descriptor would need a second table entry
             * aliasing the same counter, which nothing needs yet. */
            errno = EMFILE;
            return -1;
        default:
            errno = EINVAL;
            return -1;
        }
    }

    return (int)syscall(SYS_fcntl, fd, cmd, arg);
}
