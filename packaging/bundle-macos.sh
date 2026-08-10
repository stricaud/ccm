#!/usr/bin/env bash
#
# bundle-macos.sh — produce a self-contained macOS bundle: the ccm binary plus
# every non-system dylib it needs (gtcaca, libcaca, oniguruma, …), with
# install-names rewritten to @executable_path/../lib so it runs anywhere with no
# Homebrew and no DYLD_LIBRARY_PATH. The bundle is what packaging/wheel.py
# repacks into a wheel.
set -euo pipefail
export COPYFILE_DISABLE=1                 # no ._ AppleDouble files in the tarball

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${GTCACA_PREFIX:=$ROOT/build/gtcaca-prefix}"
ARCH="$(uname -m)"                         # arm64 on Apple Silicon
NAME="cacamacs-macos-$ARCH"
OUT="$ROOT/dist/$NAME"
BIN="$ROOT/build/ccm"

# Where @rpath/@loader_path deps may live: gtcaca's staging prefix, then Homebrew.
SEARCH=("$GTCACA_PREFIX/lib" /opt/homebrew/lib /usr/local/lib)

[ -x "$BIN" ] || { echo "build first: $BIN missing" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/share/ccm"
cp "$BIN" "$OUT/bin/ccm"; chmod u+w "$OUT/bin/ccm"

resolve() {                                # echo absolute path of a dependency, or fail
  local dep="$1" base; base="$(basename "$dep")"
  case "$dep" in
    /usr/lib/*|/System/*) return 1 ;;                 # system — never bundle
    /*) [ -f "$dep" ] && { echo "$dep"; return 0; }; return 1 ;;
    *)  for d in "${SEARCH[@]}"; do [ -f "$d/$base" ] && { echo "$d/$base"; return 0; }; done
        return 1 ;;
  esac
}

# Walk the dependency graph transitively (ccm → gtcaca → libcaca/oniguruma …),
# copying each non-system dylib once and repointing every edge at @rpath.
worklist=("$OUT/bin/ccm")
while [ ${#worklist[@]} -gt 0 ]; do
  f="${worklist[0]}"; worklist=("${worklist[@]:1}")
  while IFS= read -r dep; do
    case "$dep" in /usr/lib/*|/System/*) continue ;; esac
    base="$(basename "$dep")"
    [ "$base" = "$(basename "$f")" ] && continue       # self-reference (the -id line)
    if [ ! -f "$OUT/lib/$base" ]; then
      real="$(resolve "$dep")" || { echo "  ! unresolved: $dep" >&2; continue; }
      cp "$real" "$OUT/lib/$base"; chmod u+w "$OUT/lib/$base"
      install_name_tool -id "@executable_path/../lib/$base" "$OUT/lib/$base"
      worklist+=("$OUT/lib/$base")
    fi
    install_name_tool -change "$dep" "@executable_path/../lib/$base" "$f"
  done < <(otool -L "$f" | tail -n +2 | awk '{print $1}')
done

install_name_tool -add_rpath "@executable_path/../lib" "$OUT/bin/ccm" 2>/dev/null || true

# re-sign (install_name_tool invalidates the signature; required on Apple Silicon)
codesign -f -s - "$OUT/bin/ccm" 2>/dev/null || true
for l in "$OUT"/lib/*.dylib; do codesign -f -s - "$l" 2>/dev/null || true; done

# Ship the sample language extensions so syntax highlighting works on a machine
# with no VSCode installed (the wheel's shim points CCM_BUILTIN_EXTENSIONS here),
# plus the colour-theme sample for reference.
cp -R "$ROOT/examples/extensions" "$OUT/share/ccm/extensions" 2>/dev/null || true
cp -R "$ROOT/themes" "$OUT/share/ccm/themes" 2>/dev/null || true
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
export CCM_BUILTIN_EXTENSIONS="${CCM_BUILTIN_EXTENSIONS:-$here/share/ccm/extensions}"
exec "$here/bin/ccm" "$@"
SH
chmod +x "$OUT/ccm"

tar -C "$ROOT/dist" -czf "$ROOT/dist/$NAME.tar.gz" "$NAME"
echo "==> $ROOT/dist/$NAME.tar.gz"
