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

#include <cpio/cpio.h>
#include <muslcsys/io.h>
#include <utils/util.h>

/* Emitted by MakeCPIO's generated assembly stub. */
extern char _cpio_archive[];
extern char _cpio_archive_end[];

static unsigned long archive_len(void)
{
    return (unsigned long)(_cpio_archive_end - _cpio_archive);
}

static void CONSTRUCTOR(CONSTRUCTOR_MIN_PRIORITY) install_ruby_cpio(void)
{
    muslcsys_install_cpio_interface(_cpio_archive, archive_len(), cpio_get_file);
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
    if (find_file(path, &size) == NULL) {
        errno = ENOENT;
        return -1;
    }

    fill_regular(st, size);
    return 0;
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
