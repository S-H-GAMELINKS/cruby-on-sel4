/*
 * Send standard output and standard error to the framebuffer as well as to the
 * debug console.
 *
 * Both, rather than either. The debug console is a write to port 0x3f8, which
 * QEMU turns into the terminal this is developed in and which the Steam Deck
 * simply discards -- the machine has no serial port. The framebuffer is the
 * reverse only in degree: it is the one thing the Deck can show, and under QEMU
 * it is a window that is easy to ignore. Writing to both means one build behaves
 * correctly in both places, and the serial log stays available on hardware that
 * has it.
 *
 * The interception is at the syscall layer rather than on write(), because musl's
 * stdio does not go through the public write(). __stdout_write issues SYS_writev
 * directly, so printf and everything built on it would bypass a libc level
 * override -- the same reason the open handlers in ruby_syscall_fs.c sit here.
 *
 * Only writev is taken. Everything that writes arrives there eventually:
 * libsel4muslcsys' write handler wraps its buffer in an iovec and calls writev
 * (sys_io.c:321-330), so intercepting both would draw every such write twice.
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <muslcsys/vsyscall.h>
#include <utils/util.h>

#include "../console/console.h"

static muslcsys_syscall_t next_writev;

static int is_console_fd(int fd)
{
    return fd == STDOUT_FILENO || fd == STDERR_FILENO;
}

static long ruby_sys_writev(va_list ap)
{
    va_list chained;
    int fd;
    const struct iovec *iov;
    int iovcnt;
    long result;

    va_copy(chained, ap);

    fd = va_arg(ap, int);
    iov = va_arg(ap, const struct iovec *);
    iovcnt = va_arg(ap, int);

    if (is_console_fd(fd) && iov != NULL && iovcnt > 0) {
        for (int i = 0; i < iovcnt; i++) {
            if (iov[i].iov_len != 0 && iov[i].iov_base != NULL) {
                console_fb_write((const char *)iov[i].iov_base, iov[i].iov_len);
            }
        }
    }

    result = next_writev != NULL ? next_writev(chained) : -ENOSYS;
    va_end(chained);

    return result;
}

/* Runs after libsel4muslcsys has installed its own handlers, so that the ones
 * they replace are the ones chained to. */
static void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY + 1) install_ruby_console(void)
{
#ifdef __NR_writev
    next_writev = muslcsys_install_syscall(__NR_writev, ruby_sys_writev);
#endif
}
