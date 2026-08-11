#include <ruby.h>
#include <stdio.h>

static void trace(const char* message) {
  printf("%s\n", message);
  fflush(stdout);
}

int run(void) {
  trace("CRuby initialize");
  ruby_init();
  trace("ruby_init done");

  rb_eval_string("puts 'Hello World from CRuby on seL4!'");

  trace("Finalize CRuby");
  ruby_finalize();

  return 0;
}
