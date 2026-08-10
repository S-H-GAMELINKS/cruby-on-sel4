#!/bin/sh
# Bootstrap the CAmkES checkout and expose this repository's app to it.
#
# Run this inside the build container, which must be started from the
# repository root so that /host maps to the repository root.
set -eu

REPO_ROOT=$(cd "$(dirname "$0")" && pwd)
CAMKES_DIR="$REPO_ROOT/vendor/camkes-project"

# Need baseruby: CRuby's cross-compiling configure requires a host ruby
if ! command -v ruby >/dev/null 2>&1; then
    sudo apt update
    sudo apt install -y ruby
fi

if [ ! -d "$CAMKES_DIR/.repo" ]; then
    mkdir -p "$CAMKES_DIR"
    (cd "$CAMKES_DIR" \
        && repo init -u https://github.com/seL4/camkes-manifest.git \
        && repo sync)
fi

# CAmkES resolves apps/<name> relative to projects/camkes, so the app has to be
# reachable from there. The link target must be relative: an absolute path would
# point outside the container's /host mount and silently dangle.
ln -sfn ../../../../../apps/ruby "$CAMKES_DIR/projects/camkes/apps/ruby"

# Verify the link resolves in *this* execution context. A link that works on the
# host can still be broken inside the container, so fail loudly here instead of
# letting CMake report a confusing missing-include error later.
if [ ! -f "$CAMKES_DIR/projects/camkes/apps/ruby/CMakeLists.txt" ]; then
    echo "ERROR: apps/ruby is not reachable from the CAmkES tree." >&2
    echo "Are you running this inside the container, started from the repository root?" >&2
    exit 1
fi

echo "setup complete"
