#!/bin/sh
#
# Turn the slide deck into a PDF, one page per slide, as the device draws it.
#
# Nothing here boots seL4. slides.rb runs on the host Ruby and its output goes
# through fb_console.c compiled for the host (slides-pdf/render.c), so the pages
# are drawn by the same code as the screen: the same fonts, the same layout, the
# same images. What the device adds -- the kernel, the drivers -- does not change
# what a slide looks like.
#
# The deck is stepped through by feeding slides.rb more "next" keys than it has
# slides and then a quit; once on the last slide a further "next" draws nothing.
#
# Requires, on the host: a C compiler, Ruby, ImageMagick.
#
# Environment:
#   DECK       the deck to render     (default: scripts/slides.md)
#   OUT        the PDF to write       (default: build/slides.pdf at the repository root)
#   FB_WIDTH, FB_HEIGHT, FB_SCALE
#              the screen geometry    (default: 1920 1080 2, as ruby.camkes has it)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
component=$(dirname -- "$here")
repo=$(CDPATH= cd -- "$component/../../../.." && pwd)

scripts=$component/scripts
deck=${DECK:-$scripts/slides.md}
out=${OUT:-$repo/build/slides.pdf}
width=${FB_WIDTH:-1920}
height=${FB_HEIGHT:-1080}
scale=${FB_SCALE:-2}

if command -v magick >/dev/null 2>&1; then
    convert=magick
elif command -v convert >/dev/null 2>&1; then
    convert=convert
else
    echo "missing tool: ImageMagick (apt install imagemagick)" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

${CC:-cc} -O2 -std=gnu11 \
    -DFB_WIDTH="$width" -DFB_HEIGHT="$height" -DFB_SCALE="$scale" \
    -I"$here/slides-pdf/include" -I"$component/src/console" \
    -o "$work/render" \
    "$here/slides-pdf/render.c" \
    "$component/src/console/fb_console.c" \
    "$component/src/console/font_8x16.c" \
    "$component/src/console/font_shinonome.c"

# The console's size in cells, as fb_console.c works it out: the last row is the
# status line, not the terminal's.
cols=$((width / (8 * scale)))
rows=$((height / (16 * scale) - 1))

{
    i=0
    while [ $i -lt 1000 ]; do
        printf ' '
        i=$((i + 1))
    done
    printf 'q'
} | SLIDES_ROWS=$rows SLIDES_COLS=$cols SLIDES_ROOT=$scripts \
    ruby -r"$here/slides-pdf/host.rb" "$scripts/slides.rb" "$deck" \
  | "$work/render" "$scripts" "$work"

mkdir -p "$(dirname -- "$out")"
"$convert" "$work"/slide-*.ppm "$out"
echo "wrote $out"
