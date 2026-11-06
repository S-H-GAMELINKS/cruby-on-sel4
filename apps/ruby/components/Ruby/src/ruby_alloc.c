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
 * The allocator is deliberately simple: a bump arena whose blocks, once freed,
 * are reused whole. Blocks are never split or coalesced, so what keeps the arena
 * from being eaten away is that each block is reused only for a request of its
 * own size class. With a single free list, a block freed by a large request
 * went to whatever small request came next, the next large request had to be
 * carved afresh, and a program that keeps building and dropping large strings
 * -- a game drawing a frame -- ran the arena dry in minutes.
 *
 * munmap releases a mapping only once all of it has been unmapped; see there.
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

/* Needed to resolve a descriptor back to its CPIO entry for file-backed mmap.
 * That case lives here rather than with the rest of the file surface because
 * mmap is a single entry point serving both kinds of mapping, and the memory it
 * returns comes from the arena below either way. */
#include <muslcsys/io.h>

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

/*
 * Size classes: four to each power of two, so a request is rounded up by at most
 * a quarter. Coarser classes would waste more of an arena that cannot grow;
 * finer ones would leave more blocks stranded in classes nothing asks for.
 */
#define CLASS_MIN_SHIFT 5   /* 32 bytes, the smallest block */
#define CLASS_STEPS 4
#define CLASS_COUNT ((sizeof(size_t) * 8 - CLASS_MIN_SHIFT) * CLASS_STEPS)

typedef struct alloc_header {
    size_t size;
    size_t capacity;
    struct alloc_header *next_free;
    uint16_t is_free;
    uint16_t size_class;
    unsigned int magic;
} alloc_header_t;

static unsigned char ruby_alloc_arena[RUBY_ALLOC_ARENA_SIZE] __attribute__((aligned(4096)));
static size_t ruby_alloc_offset;
static alloc_header_t *free_lists[CLASS_COUNT];

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

/* Round a request up to its class, and say which class that is. Zero when the
 * request is too large to round. */
static size_t size_class_of(size_t size, unsigned int *size_class)
{
    unsigned int shift;
    size_t step;
    size_t rounded;

    if (size <= ((size_t)1 << CLASS_MIN_SHIFT)) {
        *size_class = 0;
        return (size_t)1 << CLASS_MIN_SHIFT;
    }

    /* The power of two at or below the request, and a quarter of it. */
    shift = (unsigned int)(sizeof(size_t) * 8 - 1) - (unsigned int)__builtin_clzl(size);
    step = ((size_t)1 << shift) / CLASS_STEPS;
    rounded = (size + step - 1) & ~(step - 1);
    if (rounded < size) {
        return 0;
    }
    if (rounded == ((size_t)1 << (shift + 1))) {
        /* Rounded up into the next power of two: its first class. */
        shift++;
        step = 0;
    }

    *size_class = (shift - CLASS_MIN_SHIFT) * CLASS_STEPS +
                  (step == 0 ? 0 : (unsigned int)((rounded - ((size_t)1 << shift)) / step));
    return rounded;
}

static void *reuse_free_block(unsigned int size_class, size_t size, size_t alignment)
{
    alloc_header_t *prev = NULL;
    alloc_header_t *current = free_lists[size_class];

    while (current != NULL) {
        if (((uintptr_t)header_payload(current) % alignment) == 0) {
            if (prev == NULL) {
                free_lists[size_class] = current->next_free;
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
    unsigned int size_class;
    size_t capacity;
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

    capacity = size_class_of(size, &size_class);
    if (capacity == 0 || size_class >= CLASS_COUNT) {
        errno = ENOMEM;
        return NULL;
    }

    reused = reuse_free_block(size_class, size, alignment);
    if (reused != NULL) {
        return reused;
    }

    /* Place the header so that the payload, not the header, lands on the
     * requested alignment. */
    base = (uintptr_t)ruby_alloc_arena + ruby_alloc_offset;
    header_addr = align_up(base + sizeof(*header), alignment) - sizeof(*header);
    next = header_addr + sizeof(*header) + capacity;

    if (next < header_addr || next > (uintptr_t)ruby_alloc_arena + sizeof(ruby_alloc_arena)) {
        errno = ENOMEM;
        return NULL;
    }

    header = (alloc_header_t *)header_addr;
    header->size = size;
    header->capacity = capacity;
    header->next_free = NULL;
    header->is_free = 0;
    header->size_class = (uint16_t)size_class;
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
    header->next_free = free_lists[header->size_class];
    free_lists[header->size_class] = header;
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
    /* Kept in place unless it has shrunk to less than half the block: a string
     * built large and then trimmed would otherwise hold its largest size for as
     * long as it lives. */
    if (payload_header(ptr)->capacity >= size && size > payload_header(ptr)->capacity / 2) {
        payload_header(ptr)->size = size;
        return ptr;
    }

    new_ptr = malloc(size);
    if (new_ptr == NULL) {
        if (payload_header(ptr)->capacity >= size) {
            /* Only shrinking; staying put is still an answer. */
            payload_header(ptr)->size = size;
            return ptr;
        }
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
/* Every mapping handed out, with how much of it has not been unmapped yet. A
 * mapping that does not fit is still handed out, just never reclaimed. */
#define MAX_MAPPINGS 2048

typedef struct {
    uintptr_t base;
    size_t length;
    size_t remaining;
} mapping_t;

static mapping_t mappings[MAX_MAPPINGS];
static size_t mapping_count;

static void *record_mapping(void *ptr, size_t length)
{
    if (mapping_count < MAX_MAPPINGS) {
        mappings[mapping_count].base = (uintptr_t)ptr;
        mappings[mapping_count].length = length;
        mappings[mapping_count].remaining = length;
        mapping_count++;
    }
    return ptr;
}

static const cpio_file_data_t *cpio_file_for_fd(int fd)
{
    muslcsys_fd_t *fds;

    if (fd < 0 || !valid_fd(fd)) {
        return NULL;
    }
    fds = get_fd_struct(fd);
    if (fds == NULL || fds->filetype != FILE_TYPE_CPIO || fds->data == NULL) {
        return NULL;
    }

    return (const cpio_file_data_t *)fds->data;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    size_t rounded;
    void *ptr;

    (void)addr;
    (void)prot;

    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    rounded = (length + MMAP_ALIGNMENT - 1) & ~(size_t)(MMAP_ALIGNMENT - 1);
    if (rounded < length) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    /*
     * File-backed mappings have to carry the file's contents, not blank memory.
     * CRuby loads every script this way: prism/util/pm_string.c:190 and
     * prism_compile.c's pm_read_file both do
     *
     *     mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0)
     *
     * and neither falls back to read() when that fails -- they return
     * PM_STRING_INIT_ERROR_GENERIC. Handing back zeroed memory instead makes
     * every required file parse as an empty program: require succeeds, the
     * feature is recorded, and nothing is defined.
     *
     * The contents are already resident in the embedded CPIO archive, so this
     * copies from there. Copying rather than aliasing the archive directly keeps
     * the two guarantees callers rely on: the result is page aligned, and the
     * remainder of the final page reads as zero.
     */
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        const cpio_file_data_t *file = cpio_file_for_fd(fd);
        size_t available;

        if (file == NULL) {
            /* Only the CPIO-backed descriptors have contents to map. */
            errno = ENODEV;
            return MAP_FAILED;
        }
        if (offset < 0 || (size_t)offset > file->size) {
            errno = EINVAL;
            return MAP_FAILED;
        }

        available = file->size - (size_t)offset;
        if (available > length) {
            available = length;
        }

        ptr = arena_alloc(rounded, MMAP_ALIGNMENT);
        if (ptr == NULL) {
            return MAP_FAILED;
        }
        memset(ptr, 0, rounded);
        memcpy(ptr, file->start + offset, available);
        return record_mapping(ptr, rounded);
    }

    ptr = arena_alloc(rounded, MMAP_ALIGNMENT);
    if (ptr == NULL) {
        return MAP_FAILED;
    }

    memset(ptr, 0, rounded);
    return record_mapping(ptr, rounded);
}

void *__mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return mmap(addr, length, prot, flags, fd, offset);
}

/*
 * Release a mapping once all of it has been unmapped.
 *
 * Ruby's GC maps and unmaps heap pages continuously, so ignoring munmap entirely
 * means every page the collector releases is lost for the rest of the run.
 *
 * Nor can munmap simply free what it is given. The GC maps twice the 64 KiB it
 * needs and unmaps the excess on either side to leave an aligned page
 * (heap_page_body_allocate in gc/default.c); later it unmaps that page alone.
 * The first trim starts at the mapping itself, so freeing on it would hand the
 * whole block, the page in use included, to the next allocation; and the page's
 * own address is not one this allocator ever returned.
 *
 * So each mapping is recorded with how much of it is still mapped, and an unmap
 * anywhere inside it counts against that. The block goes back to the allocator
 * when nothing of it is left. Ranges inside no recorded mapping are ignored, as
 * all of them were before.
 */
static int munmap_ranges(void *addr, size_t length)
{
    uintptr_t start = (uintptr_t)addr;

    for (size_t i = 0; i < mapping_count; i++) {
        mapping_t *mapping = &mappings[i];

        if (start < mapping->base || start >= mapping->base + mapping->length) {
            continue;
        }

        length = (length + MMAP_ALIGNMENT - 1) & ~(size_t)(MMAP_ALIGNMENT - 1);
        mapping->remaining = length >= mapping->remaining ? 0 : mapping->remaining - length;
        if (mapping->remaining == 0) {
            free((void *)mapping->base);
            mappings[i] = mappings[--mapping_count];
        }
        return 0;
    }

    return 0;
}

int munmap(void *addr, size_t length)
{
    if (addr == NULL || length == 0) {
        return 0;
    }

    return munmap_ranges(addr, length);
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
