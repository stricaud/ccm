#!/usr/bin/env bash
#
# build.sh — build ccm (cacamacs) and its one sibling library (gtcaca) in
# Release, then produce a self-contained bundle for the host OS under dist/.
#
#   GTCACA_SRC=/path/to/gtcaca packaging/build.sh
#
# Default assumes gtcaca is checked out next to this repo (../gtcaca). Requires:
# cmake, a C toolchain, pkg-config, and the external deps libcaca and oniguruma
# (oniguruma is optional but gives gtcaca its TextMate-grammar colourization —
# ship it so `pip install cacamacs` highlights out of the box).
#
# gtcaca is found exactly the way a third party finds it: this script installs
# its library + headers + gtcaca.pc into a staging prefix and puts that on
# PKG_CONFIG_PATH, so ccm's CMake resolves it through pkg-config with no
# source-tree escape hatch. Only the Libraries + Headers components are
# installed, so gtcaca's own apps (cacamacs, rtodo, …) are never built.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${GTCACA_SRC:=$ROOT/../gtcaca}"
: "${BUILD_TYPE:=Release}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Where gtcaca is staged; exported so the bundle scripts can find its dylib/DLL.
export GTCACA_PREFIX="$ROOT/build/gtcaca-prefix"

[ -d "$GTCACA_SRC" ] || { echo "gtcaca source not found at $GTCACA_SRC (set GTCACA_SRC)" >&2; exit 1; }

echo "==> gtcaca ($GTCACA_SRC) -> $GTCACA_PREFIX"
rm -rf "$GTCACA_PREFIX"
cmake -S "$GTCACA_SRC" -B "$GTCACA_SRC/build" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$GTCACA_PREFIX" >/dev/null
cmake --build "$GTCACA_SRC/build" --target gtcaca -j"$JOBS"
# Install just what a consumer needs — the shared library, the headers and the
# generated gtcaca.pc — without pulling in gtcaca's apps.
cmake --install "$GTCACA_SRC/build" --component Libraries
cmake --install "$GTCACA_SRC/build" --component Headers

export PKG_CONFIG_PATH="$GTCACA_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "==> ccm ($ROOT)"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" >/dev/null
cmake --build "$ROOT/build" --target ccm -j"$JOBS"

case "$(uname -s)" in
  Darwin)      "$ROOT/packaging/bundle-macos.sh" ;;
  Linux)       "$ROOT/packaging/bundle-linux.sh" ;;
  MINGW*|MSYS*|CYGWIN*) "$ROOT/packaging/bundle-windows.sh" ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
echo "==> artifacts in $ROOT/dist/"
