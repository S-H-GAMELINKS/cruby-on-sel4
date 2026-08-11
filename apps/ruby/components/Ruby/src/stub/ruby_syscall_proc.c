/*
 * Process and address space stubs for running CRuby inside a CAmkES component.
 *
 * A CAmkES component is not a process. It has no working directory, no symbolic
 * links, no alternate signal stack and no way to change page protections after
 * the fact. CRuby touches all of these during initialisation, and
 * libsel4muslcsys has no handler for most of them, so each call surfaces as an
 * unimplemented-syscall log line or an assertion.
 *
 * The answers here are the closest truthful ones available: operations with no
 * observable effect report success, and lookups that cannot succeed report that
 * the thing does not exist.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * CRuby calls Linux-specific prctl during initialisation and when naming threads.
 * Nothing in the embedded VM depends on the effect, so reporting success is
 * enough. Without this the call appears as "Error attempting syscall 157".
 */
int prctl(int option, ...)
{
    (void)option;
    return 0;
}

/*
 * There is a single flat namespace with no notion of a current directory. Root is
 * the only answer that keeps path handling coherent.
 */
char *getcwd(char *buf, size_t size)
{
    if (buf == NULL || size < 2) {
        errno = ERANGE;
        return NULL;
    }

    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

/*
 * No symbolic links exist. Reporting ENOENT rather than success matters: CRuby
 * uses readlink to resolve its own executable path, and a bogus success would
 * hand it uninitialised bytes to parse.
 */
ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path;
    (void)buf;
    (void)bufsiz;

    errno = ENOENT;
    return -1;
}

/*
 * CRuby installs an alternate signal stack so it can turn stack overflow into a
 * Ruby-level exception. There are no POSIX signals here, so report that no
 * alternate stack is installed and accept the request.
 */
int sigaltstack(const stack_t *ss, stack_t *old_ss)
{
    (void)ss;

    if (old_ss != NULL) {
        old_ss->ss_sp = NULL;
        old_ss->ss_flags = SS_DISABLE;
        old_ss->ss_size = 0;
    }

    return 0;
}

/*
 * Page protections are fixed at load time by the capDL spec. Ruby's GC calls
 * mprotect when arranging heap pages; since the component-local allocator hands
 * out ordinary read-write memory, the requested protection already holds closely
 * enough to proceed.
 */
int mprotect(void *addr, size_t len, int prot)
{
    (void)addr;
    (void)len;
    (void)prot;
    return 0;
}
