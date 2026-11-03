/*
 * Stand-in for libcpio's header. On the host there is no archive: render.c
 * answers lookups from the scripts directory instead.
 */

#pragma once

const void *cpio_get_file(const void *archive, unsigned long len, const char *name,
                          unsigned long *size);
