#!/bin/bash
# repo/pkgs/linux-t2/build.sh
# Stages the Apple T2-patched kernel for the TenebraOS repo.
#
# The t2linux project (https://t2linux.org, github.com/t2linux/T2-Debian-and-Ubuntu-Kernel)
# builds Debian trixie kernels with Apple T2 patches (apple-bce, touchbar, Wi-Fi,
# audio, battery). Devuan daedalus is binary-compatible with Debian trixie, so we
# host their trixie mainline debs in our own repository.
#
# The installer (Calamares hardware_detect -> autoconfig) installs this package
# only on T2 Macs, so the ISO itself stays universal.
#
# Usage:
#   ./repo/pkgs/linux-t2/build.sh            # latest release, trixie, mainline
#   T2_VERSION=v7.1-1 ./repo/pkgs/linux-t2/build.sh
#
# Produces: repo/pool/linux-image-*-t2-trixie*.deb, linux-headers-*-t2-trixie*.deb

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
POOL="$ROOT/pool"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

T2_REPO="t2linux/T2-Debian-and-Ubuntu-Kernel"
T2_VERSION="${T2_VERSION:-latest}"

command -v curl >/dev/null || { echo "curl missing" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "dpkg-deb missing" >&2; exit 1; }

if [ "$T2_VERSION" = "latest" ]; then
    T2_VERSION="$(curl -fsSL "https://api.github.com/repos/${T2_REPO}/releases/latest" \
        | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"\(.*\)".*/\1/')"
fi
echo ">> T2 kernel release: $T2_VERSION"

download() {
    local url="$1"
    local out="$TMP/$(basename "$url")"
    echo ">> Downloading $(basename "$url")"
    curl -fSL --retry 3 -o "$out" "$url"
    echo "$out"
}

# Mainline (non-xanmod) trixie builds — broad CPU support, like the stock kernel.
mapfile -t URLS < <(curl -fsSL "https://api.github.com/repos/${T2_REPO}/releases/tags/${T2_VERSION}" \
    | grep '"browser_download_url"' \
    | sed 's/.*"browser_download_url": *"\([^"]*trixie[^"]*\)".*/\1/' \
    | grep -v -- '-trixie-dbg' \
    | grep -v xanmod \
    | sort -u)

[ "${#URLS[@]}" -gt 0 ] || { echo "no trixie assets found for ${T2_VERSION}" >&2; exit 1; }

mkdir -p "$POOL"
for url in "${URLS[@]}"; do
    deb="$(download "$url")"
    dep="$(dpkg-deb -f "$deb" Depends 2>/dev/null || true)"
    cp "$deb" "$POOL/"
done

echo ""
echo ">> Staged T2 kernel debs in $POOL/:"
ls -lh "$POOL"/linux-*-t2-*trixie*.deb 2>/dev/null || true
echo ""
echo ">> Next: ./repo/publish-repo.sh && ./repo/upload-pool.sh && commit && push"