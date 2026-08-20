#!/bin/bash
# repo/pkgs/tenebraos-fastfetch/build.sh
# Builds the TenebraOS-customized fastfetch .deb and drops it into repo/pool/.
#
# The binary is upstream fastfetch; the "custom" part is the TenebraOS logo
# + a preloaded TenebraOS preset config shipped in /etc/skel and /etc/xdg.
#
# Requires: git, cmake, ninja (or make), gcc, pkg-config, dpkg-deb
# Usage: ./repo/pkgs/tenebraos-fastfetch/build.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
POOL="$ROOT/pool"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FASTFETCH_VERSION="${FASTFETCH_VERSION:-2.41.1}"
PKG_VERSION="2.41.1-1+tenebraos"
NAME="tenebraos-fastfetch"
DEB="$POOL/${NAME}_${PKG_VERSION}_amd64.deb"

for cmd in git cmake make gcc dpkg-deb; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done

echo ">> Cloning fastfetch ${FASTFETCH_VERSION}..."
git clone --depth 1 --branch "$FASTFETCH_VERSION" \
    https://github.com/fastfetch-cli/fastfetch.git "$WORK/src" 2>/dev/null \
    || git clone --depth 1 https://github.com/fastfetch-cli/fastfetch.git "$WORK/src"

echo ">> Building..."
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build "$WORK/build" -j"$(nproc)"

echo ">> Packaging..."
DESTDIR="$WORK/pkg" cmake --install "$WORK/build" >/dev/null

mkdir -p "$WORK/pkg/usr/share/fastfetch/presets/"
mkdir -p "$WORK/pkg/etc/skel/.config/fastfetch/"
mkdir -p "$WORK/pkg/etc/xdg/fastfetch/"
mkdir -p "$WORK/pkg/DEBIAN"

cp "$HERE/tenebraos.jsonc" "$WORK/pkg/usr/share/fastfetch/presets/"
cp "$HERE/tenebra-logo.txt" "$WORK/pkg/usr/share/fastfetch/presets/"
cp "$HERE/tenebraos.jsonc" "$WORK/pkg/etc/xdg/fastfetch/config.jsonc"
cp "$HERE/tenebraos.jsonc" "$WORK/pkg/etc/skel/.config/fastfetch/config.jsonc"

cat > "$WORK/pkg/DEBIAN/control" <<EOF
Package: tenebraos-fastfetch
Version: ${PKG_VERSION}
Architecture: amd64
Maintainer: TenebraOS Team <tenebraos@lists.local>
Installed-Size: $(du -sk "$WORK/pkg" | cut -f1)
Depends: libc6 (>= 2.34)
Section: utils
Priority: optional
Homepage: https://github.com/fastfetch-cli/fastfetch
Description: Custom TenebraOS build of fastfetch
 Fastfetch system information tool with the TenebraOS logo preset
 applied by default for all users.
EOF

dpkg-deb --build --root-owner-group "$WORK/pkg" "$DEB" >/dev/null

echo ">> Built $DEB"
ls -lh "$DEB"