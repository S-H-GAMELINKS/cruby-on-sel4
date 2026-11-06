#!/bin/sh
# Build the CPIO archive embedded in the Ruby component and wrap it in an object
# exposing _cpio_archive / _cpio_archive_end.
#
# A path-preserving archive is required: `require 'reline/config'` looks for
# reline/config.rb, so entry names have to keep their directories. The upstream
# MakeCPIO helper archives each input by basename and cannot express that.
set -eu

cruby_src=$1    # extracted CRuby source tree
cruby_build=$2  # CRuby build directory, which holds the generated rbconfig.rb
termfront=$3    # extracted Termfront source tree
script_dir=$4   # this component's scripts/ directory
stage=$5        # staging directory assembled below
out_cpio=$6
out_asm=$7
out_obj=$8
cc=$9

rm -rf "$stage"
mkdir -p "$stage"

# Everything this repository puts in the archive: scripts, and the data they
# read. The slide deck is a file the presentation tool opens at run time, so it
# has to travel with it -- there is no filesystem to put it on later.
cp "$script_dir"/* "$stage/"

# reline ships as a bundled gem, and its lib directory is already laid out the
# way require expects. The version is globbed rather than pinned so that a CRuby
# upgrade does not silently produce an archive with no reline in it.
reline_lib=$(echo "$cruby_src"/.bundle/gems/reline-*/lib)
cp -R "$reline_lib"/. "$stage/"

# What reline pulls in from the standard library at load time. etc and socket are
# deliberately absent: tmpdir.rb wraps `require 'etc.so'` in rescue LoadError,
# and fileutils.rb requires socket lazily inside a method body.
for f in forwardable tempfile delegate tmpdir fileutils; do
    cp "$cruby_src/lib/$f.rb" "$stage/"
done

# Termfront, under a directory of its own so that its lib/ and data/ keep the
# relationship mission/base.rb relies on: it finds data/events by going up from
# its own __dir__. The audio is left out -- it is most of the repository by size,
# and there is nothing here to play it with. Without a manifest the game simply
# stays silent.
mkdir -p "$stage/termfront/data"
cp -R "$termfront/lib" "$stage/termfront/"
cp -R "$termfront/data/events" "$stage/termfront/data/"
cp "$termfront/LICENSE.txt" "$stage/termfront/"

# json's Ruby half. The C half is linked in; json/ext.rb requires it by the
# names extinit.c registers.
cp -R "$cruby_src/ext/json/lib/." "$stage/"

# What Termfront pulls in from the standard library besides json. set and thread
# need nothing: Set is a core class in Ruby 4.0 and both are provided features.
cp "$cruby_src/lib/shellwords.rb" "$stage/"

# Generated per build, so it comes from the build tree, not the source tree.
cp "$cruby_build/rbconfig.rb" "$stage/"

# Sorted, ./-stripped names keep the archive stable across builds.
(cd "$stage" && find . -type f | sed 's#^\./##' | sort \
    | cpio --quiet --create -H newc) > "$out_cpio"

# Match the section and symbol names upstream MakeCPIO uses, so that the archive
# lands where libsel4muslcsys expects it. An absolute .incbin path avoids
# depending on the assembler's working directory.
cat > "$out_asm" <<EOF
.section ._archive_cpio,"aw"
.globl _cpio_archive, _cpio_archive_end
_cpio_archive:
.incbin "$out_cpio"
_cpio_archive_end:
EOF

"$cc" -c -o "$out_obj" "$out_asm"
