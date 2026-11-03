# frozen_string_literal: true
#
# Loaded ahead of slides.rb (ruby -r) when it runs on the host for render.c.
#
# Two things differ from the device, and both are answered here so that
# slides.rb itself runs unchanged.
#
# The window size. On the device it is the framebuffer console's; here stdout is
# a pipe, which has none, and slides.rb would fall back to 80x25.
#
# Absolute paths. On the device / is the CPIO archive, which holds the scripts
# directory, so a deck naming /camkes.ppm means scripts/camkes.ppm. A path that
# exists there is redirected; anything else is left alone.

SLIDES_ROWS = Integer(ENV.fetch('SLIDES_ROWS'))
SLIDES_COLS = Integer(ENV.fetch('SLIDES_COLS'))
SLIDES_ROOT = ENV.fetch('SLIDES_ROOT')

def $stdout.winsize
  [SLIDES_ROWS, SLIDES_COLS]
end

module ArchivePaths
  def open(path, *args, **kwargs, &block)
    super(archive_path(path), *args, **kwargs, &block)
  end

  def read(path, *args, **kwargs)
    super(archive_path(path), *args, **kwargs)
  end

  private

  def archive_path(path)
    return path unless path.is_a?(String) && path.start_with?('/')

    candidate = File.join(SLIDES_ROOT, path)
    File.exist?(candidate) ? candidate : path
  end
end

File.singleton_class.prepend(ArchivePaths)
