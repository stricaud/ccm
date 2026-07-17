#!/usr/bin/env bash
#
# bundle-windows.sh — produce a self-contained Windows bundle: ccm.exe plus
# every non-system DLL it needs (from ldd, which is transitive under MSYS2),
# with the sample extensions alongside the exe.
#
# Run from an MSYS2 MinGW shell (UCRT64), after packaging/build.sh has built
# build/ccm.exe.
#
# The layout is deliberately flat: Windows looks for DLLs in the exe's directory
# first, so the bundle runs from anywhere with no launcher, no installer and no
# PATH changes. The wheel's Python shim points CCM_BUILTIN_EXTENSIONS at the
# extensions/ folder shipped beside the exe.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${GTCACA_PREFIX:=$ROOT/build/gtcaca-prefix}"
ARCH="${MSYSTEM_CARCH:-x86_64}"
NAME="cacamacs-windows-$ARCH"
OUT="$ROOT/dist/$NAME"
BIN="$ROOT/build/ccm.exe"

[ -f "$BIN" ] || { echo "build first: $BIN missing" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$BIN" "$OUT/ccm.exe"

# ldd resolves DLLs through PATH; gtcaca.dll lives in the staging prefix rather
# than the MSYS2 prefix, so without this it would come back "not found" and we
# would ship a bundle that cannot start.
export PATH="$GTCACA_PREFIX/lib:$GTCACA_PREFIX/bin:$PATH"

# Copy the non-system DLLs ldd reports (ldd is transitive). Anything under
# /c/Windows belongs to the OS (kernel32, msvcrt, …) and must never be shipped;
# the rest come from the MSYS2 prefix or the staging prefix, and do.
missing=0
while read -r name arrow dll rest; do
  [ "$arrow" = "=>" ] || continue
  case "$(printf '%s' "$dll" | tr 'A-Z' 'a-z')" in
    /c/windows/*) continue ;;                      # system DLL — never bundle
    ""|"not")                                      # "libfoo.dll => not found"
      echo "  ! unresolved: $name" >&2; missing=1; continue ;;
  esac
  [ -f "$dll" ] || { echo "  ! unresolved: $name" >&2; missing=1; continue; }
  cp -f "$dll" "$OUT/$(basename "$dll")"
done < <(ldd "$BIN")
[ "$missing" -eq 0 ] || { echo "refusing to ship an incomplete bundle" >&2; exit 1; }

# Sample language extensions (highlighting) + the colour-theme sample.
cp -R "$ROOT/examples/extensions" "$OUT/extensions" 2>/dev/null || true
cp "$ROOT/docs/ccm-theme.example" "$OUT/" 2>/dev/null || true

cat > "$OUT/README.txt" <<'TXT'
ccm — cacamacs, a small terminal text editor with Emacs key bindings

Run ccm.exe from a terminal, optionally with a file or directory:

    ccm.exe path\to\file

Everything needed is in this folder — no installation required. Keep ccm.exe
together with the DLLs and the extensions\ folder.

For the best display, run ccm in Windows Terminal.

https://github.com/stricaud/ccm
TXT

( cd "$ROOT/dist" && zip -qr "$NAME.zip" "$NAME" )
echo "==> $ROOT/dist/$NAME.zip"
