/*
 * Terminal probing stubs for running CRuby inside a CAmkES component.
 *
 * libsel4muslcsys' sys_ioctl answers only requests on stdout and asserts on
 * everything else, so any tty probe CRuby makes against another descriptor aborts
 * the component. Answering here removes that path.
 *
 * Reporting "not a terminal" is also the honest answer for the current setup:
 * output goes to the seL4 debug console, and there is no line discipline, no
 * window size and no terminal to query. Note that this makes musl treat stdout as
 * fully buffered rather than line buffered, so anything that must appear before a
 * subsequent hang needs an explicit fflush.
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ioctl(int fd, int request, ...)
{
    (void)fd;
    (void)request;

    errno = ENOTTY;
    return -1;
}

int isatty(int fd)
{
    (void)fd;

    errno = ENOTTY;
    return 0;
}
