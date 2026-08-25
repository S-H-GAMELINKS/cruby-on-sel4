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
script_dir=$3   # this component's scripts/ directory
stage=$4        # staging directory assembled below
out_cpio=$5
out_asm=$6
out_obj=$7
cc=$8

rm -rf "$stage"
mkdir -p "$stage"

# Scripts owned by this repository.
cp "$script_dir"/*.rb "$stage/"

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
