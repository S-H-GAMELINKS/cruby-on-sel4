/*
 * Pieces of C library start-up that a CAmkES component never reaches.
 *
 * musl performs these in __libc_start_main, which is the entry point of an
 * ordinary process. A CAmkES component starts at _camkes_start instead, so
 * whatever that path would have set up stays at its zero-initialised value.
 */

#define _GNU_SOURCE

#include <locale.h>

#include <autoconf.h>
#include <utils/util.h>

/*
 * Give the thread a locale.
 *
 * musl reaches the current locale through CURRENT_LOCALE, defined in
 * src/internal/locale_impl.h as __pthread_self()->locale, and dereferences it
 * without a null check: LCTRANS expands to (loc)->cat[lc]. Nothing in
 * __libc_start_main.c assigns that field, so it is NULL here and any locale
 * dependent function faults on first use.
 *
 * strerror is the one that bites immediately. It calls
 * __strerror_l(e, CURRENT_LOCALE), which evaluates (NULL)->cat[LC_MESSAGES] --
 * a read at offset 5 * sizeof(void *) = 0x28. Since CRuby builds every
 * SystemCallError message through strerror, the first failing syscall turns into
 * a data fault at 0x28 rather than a Ruby exception.
 *
 * uselocale(LC_GLOBAL_LOCALE) is the public way to point the field at
 * libc.global_locale: src/locale/uselocale.c assigns
 * self->locale = &libc.global_locale for that argument. This fixes every locale
 * consumer, not just strerror, and needs no musl-internal headers.
 */
static void CONSTRUCTOR(CONSTRUCTOR_MIN_PRIORITY) init_thread_locale(void)
{
    (void)uselocale(LC_GLOBAL_LOCALE);
}
