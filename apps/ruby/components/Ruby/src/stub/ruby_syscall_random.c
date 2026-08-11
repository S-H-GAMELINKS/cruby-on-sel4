/*
 * Randomness stubs for running CRuby inside a CAmkES component.
 *
 * CRuby tries getrandom() first and falls back to reading /dev/urandom when it
 * fails. That fallback is worse than useless here: libsel4muslcsys' sys_open_impl
 * only supports O_RDONLY and asserts on the flags CRuby passes, so the fallback
 * aborts the component. Answering getrandom() keeps CRuby away from it.
 *
 * This is a deterministic xorshift sequence with a fixed seed, which means it is
 * NOT random. It is suitable only for getting the VM up. Replace it with a real
 * entropy source before running any security-sensitive Ruby code: with a fixed
 * seed, hash seeds, SecureRandom and anything else built on this are predictable.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>
#include <sys/types.h>

/* First word of the SHA-512 initial state: an arbitrary constant with no special
 * meaning here beyond being a non-zero seed. */
static uint64_t random_state = 0x6a09e667f3bcc909ULL;

static uint64_t next_random64(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags)
{
    unsigned char *bytes = (unsigned char *)buf;
    size_t offset = 0;

    /* GRND_NONBLOCK and GRND_RANDOM make no difference: this never blocks and
     * there is only one source. */
    (void)flags;

    if (buf == NULL && buflen != 0) {
        errno = EFAULT;
        return -1;
    }

    while (offset < buflen) {
        uint64_t value = next_random64();
        size_t chunk = buflen - offset;
        size_t i;

        if (chunk > sizeof(value)) {
            chunk = sizeof(value);
        }

        for (i = 0; i < chunk; i++) {
            bytes[offset + i] = (unsigned char)(value >> (i * 8));
        }
        offset += chunk;
    }

    return (ssize_t)buflen;
}

int getentropy(void *buf, size_t buflen)
{
    /* getentropy is specified to fail rather than truncate above 256 bytes. */
    if (buflen > 256) {
        errno = EIO;
        return -1;
    }

    return getrandom(buf, buflen, 0) == (ssize_t)buflen ? 0 : -1;
}
