/*
 * Clock syscall shims for running CRuby inside a CAmkES component.
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
 * This is a bring-up placeholder, not wall-clock time. The counter advances on
 * every observation so that code comparing two readings always sees forward
 * progress. Replace it with a TimeServer-backed source before anything depends
 * on real timing.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#define FAKE_CLOCK_STEP_NS 1000000L /* 1ms per observation */
#define NS_PER_S 1000000000L

static uint64_t fake_clock_ns;

static void fake_clock_read(struct timespec *ts)
{
    fake_clock_ns += FAKE_CLOCK_STEP_NS;
    ts->tv_sec = (time_t)(fake_clock_ns / NS_PER_S);
    ts->tv_nsec = (long)(fake_clock_ns % NS_PER_S);
}

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts == NULL) {
        errno = EFAULT;
        return -1;
    }
    fake_clock_read(ts);
    return 0;
}

int clock_getres(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts != NULL) {
        ts->tv_sec = 0;
        ts->tv_nsec = FAKE_CLOCK_STEP_NS;
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
    fake_clock_read(&ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

time_t time(time_t *t)
{
    struct timespec ts;

    fake_clock_read(&ts);
    if (t != NULL) {
        *t = ts.tv_sec;
    }
    return ts.tv_sec;
}
