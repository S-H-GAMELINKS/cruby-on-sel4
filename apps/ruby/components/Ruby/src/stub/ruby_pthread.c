/*
 * Single-threaded pthread stubs for running CRuby inside a CAmkES component.
 *
 * musl's pthread_create reaches __clone (src/thread/x86_64/clone.s), which issues
 * a raw `syscall` instruction instead of going through the seL4 vsyscall hook. On
 * seL4 that instruction is not a Linux syscall at all, so the call never reaches
 * sel4_vsyscall, never logs an unimplemented-syscall message, and the component
 * simply stops making progress. Neither libsel4muslcsys nor libsel4camkes
 * overrides that path.
 *
 * Everything here therefore assumes exactly one thread. Locks always succeed
 * because there is nothing to contend with, and pthread_create reports EAGAIN so
 * that CRuby takes its "cannot spawn a native thread" path. Running a start
 * routine on the caller's stack instead would be worse: CRuby's timer thread
 * expects a genuinely separate thread and would deadlock or corrupt state.
 *
 * Replacing this means backing threads with real seL4 threads. Until then Ruby's
 * own threading is unavailable.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/* musl may expose pthread_equal as a macro; the definition below needs the name. */
#ifdef pthread_equal
#undef pthread_equal
#endif

#define MAX_KEYS 16

static void *key_values[MAX_KEYS];
static unsigned next_key;
static char main_stack;

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    (void)attr;
    (void)start;
    (void)arg;

    if (thread != NULL) {
        *thread = (pthread_t)0;
    }

    return EAGAIN;
}

int pthread_detach(pthread_t thread)
{
    (void)thread;
    return 0;
}

void pthread_exit(void *value)
{
    (void)value;
    /* There is no other thread to return control to, and returning from a
     * _Noreturn function is undefined, so stop here. */
    for (;;) {
    }
}

int pthread_join(pthread_t thread, void **value)
{
    (void)thread;
    if (value != NULL) {
        *value = NULL;
    }
    /* No thread was ever created, so there is nothing to join. */
    return ESRCH;
}

pthread_t pthread_self(void)
{
    return (pthread_t)1;
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_kill(pthread_t thread, int sig)
{
    (void)thread;
    (void)sig;
    return 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    (void)how;
    (void)set;
    if (oldset != NULL) {
        sigemptyset(oldset);
    }
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    (void)destructor;

    if (key == NULL || next_key >= MAX_KEYS) {
        return EAGAIN;
    }

    *key = next_key++;
    return 0;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key < MAX_KEYS) {
        key_values[key] = NULL;
    }
    return 0;
}

/* With one thread, thread-specific data is just a flat array. */
void *pthread_getspecific(pthread_key_t key)
{
    if (key >= MAX_KEYS) {
        return NULL;
    }

    return key_values[key];
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    if (key >= MAX_KEYS) {
        return EINVAL;
    }

    key_values[key] = (void *)value;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (mutex != NULL) {
        memset(mutex, 0, sizeof(*mutex));
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (cond != NULL) {
        memset(cond, 0, sizeof(*cond));
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

/* Nothing can signal a condition variable in a single-threaded system, so
 * returning immediately is the only option that does not hang. */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *timeout)
{
    (void)cond;
    (void)mutex;
    (void)timeout;
    return ETIMEDOUT;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
{
    (void)attr;
    if (lock != NULL) {
        memset(lock, 0, sizeof(*lock));
    }
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
    (void)lock;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
    (void)lock;
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock)
{
    (void)lock;
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
    (void)lock;
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    if (attr != NULL) {
        memset(attr, 0, sizeof(*attr));
    }
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{
    (void)attr;
    (void)state;
    return 0;
}

int pthread_attr_setinheritsched(pthread_attr_t *attr, int inherit)
{
    (void)attr;
    (void)inherit;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    (void)attr;
    (void)size;
    return 0;
}

/*
 * CRuby inspects the current thread's stack to decide where to scan for GC roots
 * and how much recursion it can afford. Report the address of a component-local
 * object and the configured control thread stack size; there is only one stack,
 * so a single answer suffices.
 */
int pthread_attr_getstack(const pthread_attr_t *attr, void **addr, size_t *size)
{
    (void)attr;
    if (addr != NULL) {
        *addr = &main_stack;
    }
    if (size != NULL) {
        *size = 1024 * 1024;
    }
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size)
{
    (void)attr;
    if (size != NULL) {
        *size = 0;
    }
    return 0;
}

int pthread_getattr_np(pthread_t thread, pthread_attr_t *attr)
{
    (void)thread;
    return pthread_attr_init(attr);
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (attr != NULL) {
        memset(attr, 0, sizeof(*attr));
    }
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    (void)attr;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{
    (void)attr;
    (void)clock;
    return 0;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    (void)thread;
    (void)name;
    return 0;
}

int pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param)
{
    (void)thread;
    if (policy != NULL) {
        *policy = SCHED_OTHER;
    }
    if (param != NULL) {
        memset(param, 0, sizeof(*param));
    }
    return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
    (void)thread;
    (void)policy;
    (void)param;
    return 0;
}
