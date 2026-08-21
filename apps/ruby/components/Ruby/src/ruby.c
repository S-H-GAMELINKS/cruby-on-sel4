#include <ruby.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void trace(const char* message) {
  printf("%s\n", message);
  fflush(stdout);
}

static char *read_script(const char *path)
{
    int fd;
    struct stat st;
    char *buffer;
    size_t used = 0;
    size_t buffer_size;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("failed to open %s: %s\n", path, strerror(errno));
        return 0;
    }

    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        printf("failed to stat %s: %s\n", path, strerror(errno));
        close(fd);
        return 0;
    }

    buffer_size = (size_t)st.st_size + 1;
    buffer = malloc(buffer_size);
    if (buffer == 0) {
        printf("failed to allocate script buffer\n");
        close(fd);
        return 0;
    }

    while (used + 1 < buffer_size) {
        ssize_t n = read(fd, buffer + used, buffer_size - used - 1);

        if (n < 0) {
            printf("failed to read %s: %s\n", path, strerror(errno));
            close(fd);
            free(buffer);
            return 0;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }

    close(fd);
    buffer[used] = 0;
    return buffer;
}

int run(void) {
  int state = 0;
  char *script;

  trace("CRuby initialize");
  ruby_init();

  /* Register the statically linked extensions. ruby_opt_init() does this at
   * ruby.c:1828 -- "load statically linked extensions before rubygems" -- and
   * that is the same command-line path that also performs the builtin loading
   * below, so an embedded VM reaches neither. Without it require 'io/console'
   * fails with LoadError even though the extension is linked in: extinit.c
   * registers each one through ruby_init_ext, and nothing calls it.
   *
   * Neither symbol appears in an installed header, so declare both here. */
  extern void Init_ext(void);
  Init_ext();

  /* Load the parts of core implemented in Ruby rather than C: kernel.rb,
   * numeric.rb, io.rb and friends. rb_call_builtin_inits() is normally reached
   * through ruby_process_options -> ruby_opt_init (ruby.c:1815), which is the
   * command-line path an embedded VM never takes. ruby_init() only enables
   * builtin loading; it does not perform it.
   *
   * Without this, methods defined in those files are simply absent --
   * Kernel#class lives in kernel.rb:18 -- and NameError message construction,
   * which is also Ruby-level, degrades to inspecting the raw symbol. The symbol
   * is not declared in any installed header, so declare it here. */
  extern void rb_call_builtin_inits(void);
  rb_call_builtin_inits();
  trace("ruby_init done");

  rb_eval_string_protect("$stdout.sync = true", &state);

  script = read_script("/shell.rb");
  if (script != 0) {
      rb_eval_string_protect(script, &state);
      free(script);
      if (state != 0) {
          trace("ruby exception from CPIO script");
          rb_p(rb_errinfo());
      }
  }

  trace("Finalize CRuby");
  ruby_finalize();

  return 0;
}
