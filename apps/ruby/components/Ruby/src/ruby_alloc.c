/*
 * Component-local allocator for running CRuby inside a CAmkES component.
 *
 * This replaces musl's allocator outright rather than sitting on top of it. Two
 * reasons:
 *
 *  - Ruby's GC allocates heap pages through mmap directly, bypassing malloc.
 *    Without local mmap/munmap the component falls into libsel4muslcsys'
 *    sys_mmap_impl_static, which carves downwards from morecore_area +
 *    morecore_size, so the topmost mapping ends flush against the last byte of
 *    the CAmkES heap array with nothing but unrelated symbols beyond it.
 *  - mremap has no implementation at all on this target: once heap_size is
 *    configured, sys_mremap always dispatches to sys_mremap_static, which is an
 *    assert(!"not implemented").
 *
 * Owning malloc, mmap and mremap together keeps every allocation inside one
 * arena whose bounds this file controls, so none of those paths are reachable.
 *
 * The allocator is deliberately simple: a bump arena with a free list that reuses
 * whole blocks. It does not coalesce adjacent free blocks, so fragmentation is
 * possible. munmap is accepted without releasing anything, because Ruby's GC
 * calls it on page-trimming ranges derived from a larger mapping; treating those
 * addresses as allocation pointers would be unsafe without per-mapping metadata.
 *
 * musl's own allocator reaches the internal names rather than the public ones
 * (src/malloc/mallocng/glue.h defines mmap to __mmap, src/malloc/oldmalloc calls
 * __mmap and __munmap directly, and src/mman exposes the public names only as
 * weak_alias), so both spellings are defined here.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

/* Sized to carry CRuby through initialisation and a small script. Every 4 KiB
 * costs one capDL frame object, so raising this may require a larger
 * CapDLLoaderMaxObjects. Conversely, with this arena in place the CAmkES
 * heap_size attribute is no longer used by anything and can be dropped. */
#define RUBY_ALLOC_ARENA_SIZE (64 * 1024 * 1024)

#define DEFAULT_ALIGNMENT (sizeof(void *) * 2)
#define MMAP_ALIGNMENT 4096

/* Marks a block as one of ours, so free() on a foreign pointer is ignored rather
 * than corrupting the free list. */
#define ALLOC_MAGIC 0x52414c43u

typedef struct alloc_header {
    size_t size;
    size_t capacity;
    struct alloc_header *next_free;
    unsigned int is_free;
    unsigned int magic;
} alloc_header_t;

static unsigned char ruby_alloc_arena[RUBY_ALLOC_ARENA_SIZE] __attribute__((aligned(4096)));
static size_t ruby_alloc_offset;
static alloc_header_t *free_list;

static int is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static void *header_payload(alloc_header_t *header)
{
    return (void *)(header + 1);
}

static alloc_header_t *payload_header(void *ptr)
{
    return (alloc_header_t *)ptr - 1;
}

static int block_matches(alloc_header_t *header, size_t size, size_t alignment)
{
    return header->capacity >= size && ((uintptr_t)header_payload(header) % alignment) == 0;
}

static void *reuse_free_block(size_t size, size_t alignment)
{
    alloc_header_t *prev = NULL;
    alloc_header_t *current = free_list;

    while (current != NULL) {
        if (block_matches(current, size, alignment)) {
            if (prev == NULL) {
                free_list = current->next_free;
            } else {
                prev->next_free = current->next_free;
            }
            current->size = size;
            current->next_free = NULL;
            current->is_free = 0;
            return header_payload(current);
        }

        prev = current;
        current = current->next_free;
    }

    return NULL;
}

static void *arena_alloc(size_t size, size_t alignment)
{
    uintptr_t base;
    uintptr_t header_addr;
    uintptr_t next;
    alloc_header_t *header;
    void *reused;

    if (size == 0) {
        size = 1;
    }
    if (alignment < DEFAULT_ALIGNMENT) {
        alignment = DEFAULT_ALIGNMENT;
    }
    if (!is_power_of_two(alignment)) {
        errno = EINVAL;
        return NULL;
    }

    reused = reuse_free_block(size, alignment);
    if (reused != NULL) {
        return reused;
    }

    /* Place the header so that the payload, not the header, lands on the
     * requested alignment. */
    base = (uintptr_t)ruby_alloc_arena + ruby_alloc_offset;
    header_addr = align_up(base + sizeof(*header), alignment) - sizeof(*header);
    next = header_addr + sizeof(*header) + size;

    if (next < header_addr || next > (uintptr_t)ruby_alloc_arena + sizeof(ruby_alloc_arena)) {
        errno = ENOMEM;
        return NULL;
    }

    header = (alloc_header_t *)header_addr;
    header->size = size;
    header->capacity = size;
    header->next_free = NULL;
    header->is_free = 0;
    header->magic = ALLOC_MAGIC;
    ruby_alloc_offset = next - (uintptr_t)ruby_alloc_arena;
    return header_payload(header);
}

void *malloc(size_t size)
{
    return arena_alloc(size, DEFAULT_ALIGNMENT);
}

void free(void *ptr)
{
    alloc_header_t *header;

    if (ptr == NULL) {
        return;
    }

    header = payload_header(ptr);
    if (header->magic != ALLOC_MAGIC || header->is_free) {
        return;
    }

    header->is_free = 1;
    header->next_free = free_list;
    free_list = header;
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    void *ptr;

    if (count != 0 && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return NULL;
    }

    total = count * size;
    ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    void *new_ptr;
    size_t old_size;

    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    old_size = payload_header(ptr)->size;
    if (payload_header(ptr)->capacity >= size) {
        payload_header(ptr)->size = size;
        return ptr;
    }

    new_ptr = malloc(size);
    if (new_ptr == NULL) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    free(ptr);
    return new_ptr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *ptr;

    if (memptr == NULL || alignment < sizeof(void *) || !is_power_of_two(alignment)) {
        return EINVAL;
    }

    ptr = arena_alloc(size, alignment);
    if (ptr == NULL) {
        return errno == EINVAL ? EINVAL : ENOMEM;
    }

    *memptr = ptr;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || !is_power_of_two(alignment)) {
        errno = EINVAL;
        return NULL;
    }

    return arena_alloc(size, alignment);
}

size_t malloc_usable_size(void *ptr)
{
    if (ptr == NULL) {
        return 0;
    }

    return payload_header(ptr)->capacity;
}

/* musl reaches malloc through these internal aliases in places. */
void *__libc_malloc(size_t size)
{
    return malloc(size);
}

void *__libc_malloc_impl(size_t size)
{
    return malloc(size);
}

void __libc_free(void *ptr)
{
    free(ptr);
}

void *__libc_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

/*
 * Anonymous mappings come from the same arena, page aligned. Protection flags and
 * file offsets are ignored: there is one flat address space here and no file
 * backing, so there is nothing to honour.
 *
 * Two things mmap guarantees that plain malloc does not, and both matter to
 * Ruby's GC:
 *
 *  - The mapping is zeroed. arena_alloc may satisfy a request from the free list,
 *    which holds whatever the previous owner left behind, so the block has to be
 *    cleared here. Skipping this corrupts freshly allocated GC heap pages in ways
 *    that surface much later as unresolvable symbols or bogus objects.
 *  - The mapping spans whole pages. Callers may legitimately touch every byte up
 *    to the page boundary, so round the request up rather than handing back a
 *    block that ends mid-page.
 */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    size_t rounded;
    void *ptr;

    (void)addr;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;

    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    rounded = (length + MMAP_ALIGNMENT - 1) & ~(size_t)(MMAP_ALIGNMENT - 1);
    if (rounded < length) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    ptr = arena_alloc(rounded, MMAP_ALIGNMENT);
    if (ptr == NULL) {
        return MAP_FAILED;
    }

    memset(ptr, 0, rounded);
    return ptr;
}

void *__mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length)
{
    (void)addr;
    (void)length;
    return 0;
}

int __munmap(void *addr, size_t length)
{
    return munmap(addr, length);
}

/*
 * Growing in place is never possible here. Reporting failure is the supported
 * answer rather than merely a quiet one: src/malloc/mallocng/realloc.c treats
 * MAP_FAILED as "could not grow" and falls back to malloc + memcpy + free.
 */
void *mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...)
{
    (void)old_addr;
    (void)old_len;
    (void)new_len;
    (void)flags;

    errno = ENOMEM;
    return MAP_FAILED;
}

void *__mremap(void *old_addr, size_t old_len, size_t new_len, int flags, ...)
{
    (void)old_addr;
    (void)old_len;
    (void)new_len;
    (void)flags;

    errno = ENOMEM;
    return MAP_FAILED;
}
