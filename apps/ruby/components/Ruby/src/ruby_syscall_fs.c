/*
 * Read-only file surface for running CRuby inside a CAmkES component.
 *
 * Scripts are embedded as a CPIO archive built by MakeCPIO, which exposes the
 * bytes as _cpio_archive / _cpio_archive_end. libsel4muslcsys already implements
 * open, close, read and lseek against such an archive; this file supplies the
 * three pieces it does not.
 *
 * 1. Registration. libsel4muslcsys can register the archive itself, but only when
 *    LIB_SEL4_MUSLC_SYS_CPIO_FS is enabled, and that is unusable in a CAmkES
 *    build: libsel4muslcsys is linked into every binary in the system, including
 *    capdl-loader, so the constructor in vsyscall.c leaves _cpio_archive
 *    undefined everywhere except this component and the loader fails to link.
 *    Calling muslcsys_install_cpio_interface here has the same effect with no
 *    effect on any other binary. The dispatch in sys_open_impl tests the
 *    installed function pointers rather than the config macro, so this is enough.
 *
 * 2. Open flags. sys_open_impl masks only O_LARGEFILE and then asserts that the
 *    flags are exactly O_RDONLY. CRuby opens files with O_RDONLY | O_CLOEXEC, so
 *    passing its flags through unmodified aborts the component. Nothing here
 *    execs, so dropping everything but the access mode is safe.
 *
 * 3. stat. libsel4muslcsys installs no stat, lstat, fstat or fstatat handler at
 *    all, and CRuby needs a file's size before it will read it.
 *
 * Paths are normalised because CPIO entries carry no leading slash, and
 * sys_open_impl only retries after stripping a "./" prefix.
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdarg.h>
#include <stdlib.h>

#include <cpio/cpio.h>
#include <muslcsys/io.h>
#include <muslcsys/vsyscall.h>
#include <utils/util.h>

/* Emitted by MakeCPIO's generated assembly stub. */
extern char _cpio_archive[];
extern char _cpio_archive_end[];

static unsigned long archive_len(void)
{
    return (unsigned long)(_cpio_archive_end - _cpio_archive);
}


static const char *normalize_path(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    while (path[0] == '/') {
        path++;
    }
    while (path[0] == '.' && path[1] == '/') {
        path += 2;
    }
    return path;
}

static const void *find_file(const char *path, unsigned long *size)
{
    const char *name = normalize_path(path);

    if (name == NULL) {
        return NULL;
    }

    return cpio_get_file(_cpio_archive, archive_len(), name, size);
}

static void fill_regular(struct stat *st, unsigned long size)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
    st->st_size = (off_t)size;
    st->st_blksize = 4096;
    st->st_blocks = (blkcnt_t)((size + 511) / 512);
}

static void fill_directory(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    st->st_nlink = 2;
    st->st_blksize = 4096;
}

/*
 * Report whether any archive entry lives underneath `name`, which is what makes
 * `name` a directory.
 *
 * The archive holds files only -- the generator lists regular files and nothing
 * else -- so directories have to be inferred from entry names. Ruby depends on
 * them existing: rb_realpath_internal walks a path one component at a time and
 * stats each prefix, so requiring reline/version fails on the missing /reline
 * long before the file itself is opened.
 *
 * cpio_get_entry restarts its scan on every call, so this is quadratic in the
 * entry count. With an archive of a few dozen files that is irrelevant, and it
 * keeps the lookup free of any state to invalidate.
 */
static int is_archive_directory(const char *name)
{
    size_t len;
    int index;

    if (name == NULL) {
        return 0;
    }

    len = strlen(name);
    if (len == 0) {
        /* The archive root, reached as "/" or ".". */
        return 1;
    }

    for (index = 0;; index++) {
        const char *entry = NULL;
        unsigned long size = 0;

        if (cpio_get_entry(_cpio_archive, archive_len(), index, &entry, &size) == NULL) {
            return 0;
        }
        /* Entry names are NUL terminated inside the archive, so indexing one past
         * the compared prefix is safe. */
        if (entry != NULL && strncmp(entry, name, len) == 0 && entry[len] == '/') {
            return 1;
        }
    }
}

/*
 * Replace the open syscalls rather than only the libc wrappers.
 *
 * libsel4muslcsys' sys_open_impl masks O_LARGEFILE and then asserts that nothing
 * else is set, which turns an ordinary open into a dead component: musl's fopen
 * passes O_CLOEXEC (its "e" mode flag), and fopen does not go through the public
 * open() at all -- it issues SYS_openat directly, so overriding open() cannot
 * catch it. musl's getpwuid reaching for /etc/passwd is one such caller, by way of
 * Ruby expanding '~'.
 *
 * Failing to find a file also has to be an error rather than an assertion. A
 * missing file is a completely ordinary thing for Ruby to probe for.
 *
 * The descriptor is registered exactly as sys_open_impl would, so the read, lseek
 * and close handlers in libsel4muslcsys continue to serve it.
 */
static long open_cpio(const char *pathname, int flags)
{
    unsigned long size = 0;
    const void *file;
    muslcsys_fd_t *fds;
    cpio_file_data_t *data;
    int fd;

    if (pathname == NULL) {
        return -EFAULT;
    }
    /* The archive is read only, so anything but a plain read has to fail. */
    if ((flags & O_ACCMODE) != O_RDONLY) {
        return -EROFS;
    }

    file = find_file(pathname, &size);
    if (file == NULL) {
        return -ENOENT;
    }

    fd = allocate_fd();
    if (fd == -EMFILE) {
        return -EMFILE;
    }

    fds = get_fd_struct(fd);
    fds->filetype = FILE_TYPE_CPIO;
    fds->data = malloc(sizeof(*data));
    if (fds->data == NULL) {
        add_free_fd(fd);
        return -ENOMEM;
    }

    data = (cpio_file_data_t *)fds->data;
    data->start = (const char *)file;
    data->size = (uint32_t)size;
    data->current = 0;
    return fd;
}

static long ruby_sys_open(va_list ap)
{
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);

    return open_cpio(pathname, flags);
}

static long ruby_sys_openat(va_list ap)
{
    /* One flat namespace, so the directory descriptor carries no meaning. */
    (void)va_arg(ap, int);
    const char *pathname = va_arg(ap, const char *);
    int flags = va_arg(ap, int);

    return open_cpio(pathname, flags);
}

/* Runs after libsel4muslcsys has its own table in place, so these replacements
 * are not overwritten. */
static void CONSTRUCTOR(MUSLCSYS_WITH_VSYSCALL_PRIORITY + 1) install_ruby_fs(void)
{
    /* Kept as a fallback for any caller that still reaches sys_open_impl. */
    muslcsys_install_cpio_interface(_cpio_archive, archive_len(), cpio_get_file);

#ifdef __NR_open
    muslcsys_install_syscall(__NR_open, ruby_sys_open);
#endif
#ifdef __NR_openat
    muslcsys_install_syscall(__NR_openat, ruby_sys_openat);
#endif
}

int open(const char *path, int flags, ...)
{
    if (path == NULL) {
        errno = EFAULT;
        return -1;
    }
    /* The archive is read only; there is nothing to create or truncate. */
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
    }

    return (int)syscall(SYS_openat, AT_FDCWD, normalize_path(path), O_RDONLY, 0);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    /* There is one flat namespace, so a directory descriptor has no meaning. */
    (void)dirfd;
    return open(path, flags);
}

int stat(const char *__restrict path, struct stat *__restrict st)
{
    unsigned long size = 0;

    if (st == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (find_file(path, &size) != NULL) {
        fill_regular(st, size);
        return 0;
    }
    if (is_archive_directory(normalize_path(path))) {
        fill_directory(st);
        return 0;
    }

    errno = ENOENT;
    return -1;
}

/* No symbolic links exist in a CPIO archive read this way. */
int lstat(const char *__restrict path, struct stat *__restrict st)
{
    return stat(path, st);
}

int fstatat(int dirfd, const char *__restrict path, struct stat *__restrict st, int flags)
{
    (void)dirfd;
    (void)flags;
    return stat(path, st);
}

int fstat(int fd, struct stat *st)
{
    if (st == NULL) {
        errno = EFAULT;
        return -1;
    }

    /*
     * CRuby stats the standard streams while setting up its IO layer. Reporting a
     * character device keeps it from treating them as regular files, which would
     * imply a size and a seekable offset that the debug console does not have.
     */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | S_IRUSR | S_IWUSR;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return 0;
    }

    if (valid_fd(fd)) {
        muslcsys_fd_t *fds = get_fd_struct(fd);

        if (fds != NULL && fds->filetype == FILE_TYPE_CPIO && fds->data != NULL) {
            const cpio_file_data_t *data = (const cpio_file_data_t *)fds->data;

            fill_regular(st, data->size);
            return 0;
        }
    }

    errno = EBADF;
    return -1;
}
