#!/usr/bin/env bash
#
# bundle-linux.sh — produce a self-contained Linux bundle: the ccm binary plus
# every non-system shared library it needs (from ldd), with an rpath of
# $ORIGIN/../lib (via patchelf when available, else a launcher sets
# LD_LIBRARY_PATH). glibc core libs are intentionally not bundled — they belong
# to the host. The bundle is what packaging/wheel.py repacks into a wheel.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="$(uname -m)"                         # x86_64
NAME="cacamacs-linux-$ARCH"
OUT="$ROOT/dist/$NAME"
BIN="$ROOT/build/ccm"

[ -x "$BIN" ] || { echo "build first: $BIN missing" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/ccm"
cp "$BIN" "$OUT/bin/ccm"; chmod u+w "$OUT/bin/ccm"

# core libraries that belong to the host, not the package
is_core() {
  case "$1" in
    libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|ld-linux*|\
    libgcc_s.so*|libstdc++.so*|libresolv.so*) return 0 ;;
    *) return 1 ;;
  esac
}

# Copy the non-core dependencies ldd reports (ldd is transitive, so this pulls
# gtcaca and, through it, libcaca + oniguruma). ldd resolves them via the build
# rpath ccm was linked with (the gtcaca staging prefix + pkg-config lib dirs).
ldd "$BIN" | awk '/=>/ {print $3} !/=>/ {print $1}' | while read -r so; do
  [ -f "$so" ] || continue
  base="$(basename "$so")"
  is_core "$base" && continue
  cp -L "$so" "$OUT/lib/$base" 2>/dev/null || true
done

if command -v patchelf >/dev/null 2>&1; then
  patchelf --set-rpath '$ORIGIN/../lib' "$OUT/bin/ccm"
fi

# Ship the sample language extensions so highlighting works on a machine with no
# VSCode installed (the wheel's shim points CCM_BUILTIN_EXTENSIONS here), plus
# the colour-theme sample for reference.
cp -R "$ROOT/examples/extensions" "$OUT/share/ccm/extensions" 2>/dev/null || true
cp "$ROOT/docs/ccm-theme.example" "$OUT/share/ccm/" 2>/dev/null || true

# A shell launcher for the tarball users (the wheel replaces this with a Python
# shim and excludes it — see packaging/wheel.py).
cat > "$OUT/ccm" <<'SH'
#!/usr/bin/env bash
src="$0"
while [ -h "$src" ]; do
  d="$(cd "$(dirname "$src")" && pwd)"; src="$(readlink "$src")"
  case "$src" in /*) ;; *) src="$d/$src" ;; esac
done
here="$(cd "$(dirname "$src")" && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CCM_BUILTIN_EXTENSIONS="${CCM_BUILTIN_EXTENSIONS:-$here/share/ccm/extensions}"
exec "$here/bin/ccm" "$@"
SH
chmod +x "$OUT/ccm"

tar -C "$ROOT/dist" -czf "$ROOT/dist/$NAME.tar.gz" "$NAME"
echo "==> $ROOT/dist/$NAME.tar.gz"
