/*
 * Clock shims for running CRuby inside a CAmkES component.
 *
 * libsel4camkes routes clock_gettime through camkes_sys_clock_gettime
 * (libsel4camkes/src/sys_clock.c), which asserts unless the component supplies a
 * weak clk_get_time(). Even with that hook it only answers CLOCK_REALTIME, while
 * CRuby also asks for CLOCK_MONOTONIC. Overriding the libc entry points here
 * bypasses the syscall path entirely and covers every clock id.
 *
 * musl keeps each of these functions in its own object file, so defining them as
 * strong symbols means the matching libc.a members are never pulled in. The
 * _REDIR_TIME64 indirection in <time.h> is inactive on x86_64, so these are the
 * symbol names callers actually reference.
 *
 * The time comes from the TimeServer, which is the same hardware timer the
 * component already waits on in ruby_syscall_poll.c. A counter that only
 * guaranteed forward progress was enough while nothing depended on the rate, but
 * a slide deck that counts down from five minutes does: with a fake clock the
 * countdown advances by however often something happens to ask the time, which
 * is not a duration at all.
 *
 * There is no wall clock. The TimeServer counts from when it started, so every
 * clock here reports uptime, and a realtime reading is that uptime measured from
 * the epoch -- dates come out in 1970. Nothing on this machine knows better:
 * there is no RTC driver and no network. Differences between two readings, which
 * is what durations and timeouts are made of, are correct.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include <camkes.h>

#define NS_PER_S 1000000000ULL

/*
 * Each reading is an RPC to the TimeServer, which is far more expensive than the
 * vDSO read this replaces. Nothing is cached, because a cached clock is the
 * problem this file exists to fix; code that reads the time in a tight loop will
 * feel the cost rather than get a stale answer.
 */
static void clock_read(struct timespec *ts)
{
    uint64_t now = timeout_time();

    ts->tv_sec = (time_t)(now / NS_PER_S);
    ts->tv_nsec = (long)(now % NS_PER_S);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    /* Every clock id is the same clock. Monotonic is the honest one; realtime is
     * the same count read as seconds since the epoch. */
    (void)clk;
    if (ts == NULL) {
        errno = EFAULT;
        return -1;
    }
    clock_read(ts);
    return 0;
}

int clock_getres(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts != NULL) {
        /* The interface counts in nanoseconds. The underlying timer is coarser,
         * but by how much is the TimeServer's business and it does not say. */
        ts->tv_sec = 0;
        ts->tv_nsec = 1;
    }
    return 0;
}

int gettimeofday(struct timeval *__restrict tv, void *__restrict tz)
{
    struct timespec ts;

    (void)tz;
    if (tv == NULL) {
        errno = EFAULT;
        return -1;
    }
    clock_read(&ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

time_t time(time_t *t)
{
    struct timespec ts;

    clock_read(&ts);
    if (t != NULL) {
        *t = ts.tv_sec;
    }
    return ts.tv_sec;
}
