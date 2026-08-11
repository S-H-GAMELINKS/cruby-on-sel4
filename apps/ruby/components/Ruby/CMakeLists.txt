cmake_minimum_required(VERSION 3.16.0)

project(ComponentRuby C)

include(ExternalProject)
include(ProcessorCount)

set(CRUBY_VERSION "4.0.6" CACHE STRING "CRuby release to build")
set(CRUBY_SHA256
    "837d299e8f7ddf2be31a229a7a7e019d354979825117989acb3b32b1a9be262a"
    CACHE STRING "SHA256 of the CRuby release tarball"
)

if(NOT TARGET muslc_imported)
    message(FATAL_ERROR "muslc_imported not found; CAmkES build environment is not set up")
endif()

# Derive the staged musl install from the target CAmkES already exports, rather
# than hardcoding a path into the build tree.
get_target_property(CRUBY_MUSL_LIBC muslc_imported IMPORTED_LOCATION)
get_filename_component(CRUBY_MUSL_LIBDIR "${CRUBY_MUSL_LIBC}" DIRECTORY)
get_filename_component(CRUBY_MUSL_STAGE "${CRUBY_MUSL_LIBDIR}" DIRECTORY)

ProcessorCount(CRUBY_JOBS)
if(CRUBY_JOBS EQUAL 0)
    set(CRUBY_JOBS 1)
endif()

set(CRUBY_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/cruby")
set(CRUBY_SRC_DIR "${CRUBY_PREFIX}/src/cruby_external")
set(CRUBY_BUILD_DIR "${CRUBY_PREFIX}/build")
set(CRUBY_STATIC_LIB "${CRUBY_BUILD_DIR}/libruby-static.a")

ExternalProject_Add(
    cruby_external
    PREFIX "${CRUBY_PREFIX}"
    URL "https://cache.ruby-lang.org/pub/ruby/4.0/ruby-${CRUBY_VERSION}.tar.gz"
    URL_HASH "SHA256=${CRUBY_SHA256}"
    SOURCE_DIR "${CRUBY_SRC_DIR}"
    BINARY_DIR "${CRUBY_BUILD_DIR}"
    # CRuby configure probes the staged musl headers and libc.a, so musl has to
    # be staged first.
    DEPENDS muslc_gen
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "CPPFLAGS=--sysroot=${CMAKE_SYSROOT} -I${CRUBY_MUSL_STAGE}/include"
            "CFLAGS=--sysroot=${CMAKE_SYSROOT} -m64 -D__KERNEL_64__ -march=nehalem"
            "LDFLAGS=--sysroot=${CMAKE_SYSROOT} -L${CRUBY_MUSL_STAGE}/lib -static"
            "LIBS=${CRUBY_MUSL_STAGE}/lib/libc.a"
        "${CRUBY_SRC_DIR}/configure"
            --host=x86_64-linux-musl
            # Force cross-compilation mode so configure never runs target binaries.
            --build=x86_64-pc-linux-gnu
            --disable-shared
            --disable-install-doc
            --with-compress-debug-sections=no
            --without-gmp
            --with-static-linked-ext
            # Without this, miniruby fails with 'RUBY_FUNCTION_NAME_STRING' undeclared.
            rb_cv_function_name_string=__func__
            # The seL4 sysroot provides neither zlib.h nor libz.a.
            ac_cv_lib_z_uncompress=no
            # The CAmkES static morecore path does not implement mremap.
            ac_cv_func_mremap=no
            # No process control in a CAmkES component.
            ac_cv_func_fork=no
            ac_cv_func_vfork=no
            ac_cv_func_popen=no
            ac_cv_func_getpgrp=no
            ac_cv_func_setpgid=no
            # configure runs without -nostdinc and finds gcc's C23 stdckdint.h,
            # but CAmkES components compile with -nostdinc, where only the staged
            # musl headers are visible and stdckdint.h is absent. Force CRuby onto
            # its __builtin_*_overflow fallback instead.
            ac_cv_header_stdckdint_h=no
    BUILD_COMMAND make -j${CRUBY_JOBS} libruby-static.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${CRUBY_STATIC_LIB}"
    # Stream configure/build output instead of capturing it to log files. During
    # bring-up the failures are the point.
    USES_TERMINAL_DOWNLOAD TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
)

# Mirror the muslc_imported pattern: wrapping the archive in an IMPORTED target
# lets DeclareCAmkESComponent take it by name and lets CMake order the build.
add_library(cruby_static STATIC IMPORTED GLOBAL)
add_dependencies(cruby_static cruby_external)
set_property(TARGET cruby_static PROPERTY IMPORTED_LOCATION "${CRUBY_STATIC_LIB}")

# These include directories do not exist until cruby_external has built. That is
# fine: CAmkES resolves absolute include paths without requiring them to exist.
DeclareCAmkESComponent(
    Ruby
    SOURCES
        src/ruby.c
        src/ruby_alloc.c
        src/stub/ruby_pthread.c
        src/stub/ruby_syscall_clock.c
        src/stub/ruby_syscall_poll.c
        src/stub/ruby_syscall_proc.c
        src/stub/ruby_syscall_random.c
        src/stub/ruby_syscall_tty.c
    INCLUDES
        "${CRUBY_BUILD_DIR}/.ext/include/x86_64-linux-musl"
        "${CRUBY_SRC_DIR}/include"
    LIBS
        cruby_static
)
