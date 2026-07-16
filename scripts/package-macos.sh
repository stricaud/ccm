#!/bin/bash
#
# Build a SELF-CONTAINED macOS installer package for ccm (cacamacs).
#
# The resulting ccm-<ver>.pkg installs, under /usr/local:
#   bin/ccm                        the editor
#   lib/libgtcaca.*.dylib          the toolkit  (bundled from wherever it links)
#   lib/libcaca.0.dylib            bundled      (so Homebrew is NOT required)
#   share/ccm/ccm-theme.example    the colour-theme sample
#
# A postinstall script then seeds ~/.ccm/theme for the installing user from that
# sample (only if they don't already have one, so it never clobbers).
#
# Unlike the old gtcaca-tree packager, this one does not assume gtcaca was built
# next door: it bundles every non-system dylib `ccm` links (resolved with otool)
# and repoints each at @rpath, so it works whether gtcaca came from Homebrew,
# `make install`, or a local build.
#
# Usage:  scripts/package-macos.sh [build-dir]
set -euo pipefail
export COPYFILE_DISABLE=1          # don't copy ._ AppleDouble metadata files

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
VERSION="$(awk -F'"' '/set\(CCM_VERSION/{print $2}' "$ROOT/CMakeLists.txt")"
PREFIX="/usr/local"
STAGE="$(mktemp -d)"
DEST="$STAGE$PREFIX"
OUT="$ROOT/ccm-$VERSION.pkg"

echo ">> building ccm"
cmake -S "$ROOT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target ccm >/dev/null

echo ">> staging into $DEST"
mkdir -p "$DEST/bin" "$DEST/lib" "$DEST/share/ccm"
cp "$BUILD/ccm"                       "$DEST/bin/ccm"
cp "$ROOT/docs/ccm-theme.example"     "$DEST/share/ccm/ccm-theme.example"

# Bundle every non-system dylib ccm links (gtcaca, libcaca, and anything caca
# itself drags in that isn't already in /usr/lib or /System).
echo ">> bundling linked dylibs"
otool -L "$DEST/bin/ccm" | awk 'NR>1{print $1}' | while read -r dylib; do
  case "$dylib" in
    /usr/lib/*|/System/*) continue ;;                 # system: always present
    @*) continue ;;                                   # already relocated
  esac
  base="$(basename "$dylib")"
  # resolve symlinks (Homebrew's opt/… points into Cellar/…)
  real="$(cd "$(dirname "$dylib")" && pwd -P)/$base"
  [ -f "$real" ] || { echo "   !! cannot find $dylib" >&2; continue; }
  echo "   + $base"
  cp "$real" "$DEST/lib/$base"
  chmod u+w "$DEST/lib/$base"
  install_name_tool -id "@rpath/$base"          "$DEST/lib/$base"
  install_name_tool -change "$dylib" "@rpath/$base" "$DEST/bin/ccm"
done

# The toolkit dylib in turn links libcaca; repoint that edge at @rpath too.
GLIB="$(ls "$DEST"/lib/libgtcaca.*.dylib 2>/dev/null | head -1 || true)"
if [ -n "${GLIB:-}" ]; then
  otool -L "$GLIB" | awk 'NR>1{print $1}' | while read -r dep; do
    case "$dep" in
      *libcaca*) install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$GLIB" || true ;;
    esac
  done
fi

# ccm finds ../lib via rpath
install_name_tool -add_rpath @executable_path/../lib "$DEST/bin/ccm" 2>/dev/null || true

echo ">> verifying the staged binary resolves only @rpath + system libs"
otool -L "$DEST/bin/ccm" | sed 's/^/   /'

# postinstall: seed ~/.ccm/theme for the installing user (never clobber)
SCRIPTS="$(mktemp -d)"
cat > "$SCRIPTS/postinstall" <<'POST'
#!/bin/sh
# Give the installing user a starter ~/.ccm/theme from the bundled sample, but
# only if they don't already have one. Runs as root, so target the real user.
set -e
EXAMPLE="/usr/local/share/ccm/ccm-theme.example"

user="$(stat -f%Su /dev/console 2>/dev/null || true)"
if [ -z "$user" ] || [ "$user" = "root" ]; then user="${SUDO_USER:-${USER:-}}"; fi
[ -n "$user" ] && [ "$user" != "root" ] || exit 0

home="$(dscl . -read "/Users/$user" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
[ -n "$home" ] || home="$(eval echo "~$user")"
[ -n "$home" ] && [ -d "$home" ] || exit 0

theme="$home/.ccm/theme"
if [ ! -e "$theme" ] && [ -f "$EXAMPLE" ]; then
  mkdir -p "$home/.ccm"
  cp "$EXAMPLE" "$theme"
  chown "$user" "$home/.ccm" "$theme" 2>/dev/null || true
fi
exit 0
POST
chmod +x "$SCRIPTS/postinstall"

echo ">> building $OUT"
pkgbuild --root "$STAGE" --install-location / --scripts "$SCRIPTS" \
         --identifier org.gtcaca.ccm --version "$VERSION" "$OUT" >/dev/null

rm -rf "$STAGE" "$SCRIPTS"
echo ">> done: $OUT"
echo "   install it (double-click or: sudo installer -pkg \"$OUT\" -target /), then run: ccm"
