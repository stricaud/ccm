#!/bin/sh
#
# Build (if needed) and run the local ccm.
#
# ccm links against the gtcaca you installed (Homebrew / `make install` / .pkg),
# and its install_name points straight at that dylib, so — unlike when ccm lived
# inside the gtcaca build tree — there is no stale-library shuffle to do here.
#
# Usage:  ./run.sh [args…]     e.g.  ./run.sh .    ./run.sh file.c    ./run.sh --test
#         BUILD=/path/to/build ./run.sh …    to point at a different build dir
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$HERE/build}"

if [ ! -x "$BUILD/ccm" ]; then
  echo "run.sh: $BUILD/ccm not found — building it first…" >&2
  cmake -S "$HERE" -B "$BUILD" >/dev/null
  cmake --build "$BUILD" >/dev/null
fi

exec "$BUILD/ccm" "$@"
