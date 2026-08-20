#!/bin/bash
# repo/pkgs/tenebraos-fastfetch/build.sh
# Builds the TenebraOS-customized fastfetch .deb into repo/pool/.
#
# The binary is the upstream fastfetch release .deb; the "custom" part is the
# TenebraOS logo + a preloaded TenebraOS preset config shipped in /etc/skel
# and /etc/xdg — injected with repo/repack-deb.py (no toolchain needed).
#
# Requires: curl, python3
# Usage: ./repo/pkgs/tenebraos-fastfetch/build.sh        # latest release
#        FASTFETCH_VERSION=2.67.1 ./repo/pkgs/tenebraos-fastfetch/build.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
POOL="$ROOT/pool"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

FF_REPO="fastfetch-cli/fastfetch"
FF_VERSION="${FASTFETCH_VERSION:-latest}"

for cmd in curl python3; do
    command -v "$cmd" >/dev/null || { echo "missing: $cmd" >&2; exit 1; }
done

if [ "$FF_VERSION" = "latest" ]; then
    FF_VERSION="$(curl -fsSL "https://api.github.com/repos/${FF_REPO}/releases/latest" \
        | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"\(.*\)".*/\1/')"
fi
echo ">> fastfetch release: $FF_VERSION"

URL="https://github.com/${FF_REPO}/releases/download/${FF_VERSION}/fastfetch-linux-amd64.deb"
echo ">> Downloading upstream .deb"
curl -fSL --retry 3 -o "$WORK/upstream.deb" "$URL"

PKG_VERSION="${FF_VERSION}-1+tenebraos"
NAME="tenebraos-fastfetch"
OUT="$POOL/${NAME}_${PKG_VERSION}_amd64.deb"

mkdir -p "$POOL" "$WORK/presets"
cp "$HERE/tenebraos.jsonc" "$WORK/presets/"
cp "$HERE/tenebra-logo.txt" "$WORK/presets/"

echo ">> Repacking with TenebraOS preset..."
python3 "$ROOT/repack-deb.py" \
    --in "$WORK/upstream.deb" \
    --out "$OUT" \
    --control "Package=${NAME}" \
    --control "Version=${PKG_VERSION}" \
    --control "Homepage=https://github.com/fastfetch-cli/fastfetch" \
    --control "Description=Custom TenebraOS build of fastfetch" \
    --control "Depends=libgcc-s1 (>= 3.0), libc6 (>= 2.34)" \
    --add "$WORK/presets/tenebraos.jsonc:/usr/share/fastfetch/presets/tenebraos.jsonc" \
    --add "$WORK/presets/tenebra-logo.txt:/usr/share/fastfetch/presets/tenebra-logo.txt" \
    --add "$WORK/presets/tenebraos.jsonc:/etc/skel/.config/fastfetch/config.jsonc" \
    --add "$WORK/presets/tenebraos.jsonc:/etc/xdg/fastfetch/config.jsonc"

echo ">> Built $OUT"
ls -lh "$OUT"