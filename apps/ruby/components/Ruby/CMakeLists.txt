cmake_minimum_required(VERSION 3.16.0)

project(ComponentRuby C)

include(cpio)
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

# Statically linked extensions are not part of libruby-static.a: common.mk:400
# builds that archive from the core objects only, and common.mk:398 links the
# extensions separately. extinit.c registers each one under its ".so" name via
# ruby_init_ext, so `require 'io/console'` resolves without touching the
# filesystem.
set(CRUBY_EXT_INIT_OBJ "${CRUBY_BUILD_DIR}/ext/extinit.o")
set(CRUBY_EXT_CONSOLE_LIB "${CRUBY_BUILD_DIR}/ext/io/console/console.a")
set(CRUBY_EXT_WAIT_LIB "${CRUBY_BUILD_DIR}/ext/io/wait/wait.a")


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
            # reline needs io/console and io/wait. Building every extension would
            # fail on the ones wanting zlib, openssl and friends, none of which
            # exist in this sysroot. ext/Setup is no longer consulted in Ruby 4.0;
            # exts.mk is generated from configure's selection instead.
            --with-ext=io/console,io/wait
    BUILD_COMMAND make -j${CRUBY_JOBS} exts
    COMMAND make -j${CRUBY_JOBS} libruby-static.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
      "${CRUBY_STATIC_LIB}"
      "${CRUBY_EXT_INIT_OBJ}"
      "${CRUBY_EXT_CONSOLE_LIB}"
      "${CRUBY_EXT_WAIT_LIB}"
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

add_library(cruby_ext_init OBJECT IMPORTED GLOBAL)
add_dependencies(cruby_ext_init cruby_external)
set_property(TARGET cruby_ext_init PROPERTY IMPORTED_OBJECTS "${CRUBY_EXT_INIT_OBJ}")

add_library(cruby_ext_console STATIC IMPORTED GLOBAL)
add_dependencies(cruby_ext_console cruby_external)
set_property(TARGET cruby_ext_console PROPERTY IMPORTED_LOCATION "${CRUBY_EXT_CONSOLE_LIB}")

add_library(cruby_ext_wait STATIC IMPORTED GLOBAL)
add_dependencies(cruby_ext_wait cruby_external)
set_property(TARGET cruby_ext_wait PROPERTY IMPORTED_LOCATION "${CRUBY_EXT_WAIT_LIB}")

# MakeCPIO emits archive.<name>.cpio, <name>.S and <name> into the build tree and
# nothing into the source tree, so no generated embedding code is ever committed.
# Its default symbol pair _cpio_archive/_cpio_archive_end is exactly what
# libsel4muslcsys registers when LibSel4MuslcSysCPIOFS is enabled
# (libsel4muslcsys/src/vsyscall.c:241-248), so no symbol name needs overriding.
set(RUBY_SCRIPT_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts")
set(RUBY_ARCHIVE_OBJ "${CMAKE_CURRENT_BINARY_DIR}/ruby_archive.o")

MakeCPIO(ruby_archive.o "${RUBY_SCRIPT_DIR}/shell.rb")

# MakeCPIO declares an add_custom_command; something has to depend on its output
# for the rule to run.
add_custom_target(ruby_archive_gen DEPENDS "${RUBY_ARCHIVE_OBJ}")

# Same pattern as cruby_static above, and as muslc_imported upstream: an IMPORTED
# target can be named in LIBS, and add_dependencies gives CMake the ordering.
add_library(ruby_archive OBJECT IMPORTED GLOBAL)
add_dependencies(ruby_archive ruby_archive_gen)
set_property(TARGET ruby_archive PROPERTY IMPORTED_OBJECTS "${RUBY_ARCHIVE_OBJ}")

# These include directories do not exist until cruby_external has built. That is
# fine: CAmkES resolves absolute include paths without requiring them to exist.
DeclareCAmkESComponent(
    Ruby
    SOURCES
        src/ruby.c
        src/ruby_alloc.c
        src/ruby_syscall_fs.c
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
        # extinit.o first: it needs Init_io_console and Init_io_wait from the
        # archives after it, and libruby's undefined Init_ext is satisfied by an
        # object regardless of position.
        cruby_ext_init
        cruby_ext_console
        cruby_ext_wait
        cruby_static
        ruby_archive
)
