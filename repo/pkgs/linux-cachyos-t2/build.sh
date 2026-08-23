#!/bin/bash
# repo/pkgs/linux-cachyos-t2/build.sh
# Builds the TenebraOS kernel (Apple T2 patches + CachyOS tuning) as .debs.
#
#   On Devuan/Debian  -> runs natively (deps: sudo ./setup-build-deps.sh)
#   On any other host -> runs inside a debian:trixie container (docker/podman)
#
# Usage:
#   sudo ./repo/pkgs/linux-cachyos-t2/build.sh
#   KERNEL_VERSION=6.18 ./repo/pkgs/linux-cachyos-t2/build.sh
#   CACHYOS_SCHED=eevdf ./repo/pkgs/linux-cachyos-t2/build.sh
#   NATIVE=0 ...        # force container even on Debian-family hosts

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
POOL="$ROOT/repo/pool"

KERNEL_VERSION="${KERNEL_VERSION:-7.2}"
PKGREL="${PKGREL:-1}"
CODENAME="${CODENAME:-tenebra}"
CACHYOS_SCHED="${CACHYOS_SCHED:-bore}"

mkdir -p "$POOL"

IS_DEBIAN_FAMILY=0
[ -f /etc/debian_version ] && IS_DEBIAN_FAMILY=1

USE_NATIVE=0
if [ "${NATIVE:-auto}" = "1" ] || { [ "${NATIVE:-auto}" = "auto" ] && [ "$IS_DEBIAN_FAMILY" = "1" ]; }; then
    USE_NATIVE=1
fi

echo "============================================================"
echo " TenebraOS kernel builder — v${KERNEL_VERSION} / ${CACHYOS_SCHED}"
echo " T2 patches + CachyOS tuning -> .debs in repo/pool/"
if [ "$USE_NATIVE" = "1" ]; then
    echo " mode: native ($(cat /etc/debian_version 2>/dev/null || echo debian-family))"
else
    CTR=""
    if command -v docker &>/dev/null; then CTR="docker"
    elif command -v podman &>/dev/null; then CTR="podman"
    else
        echo " ERROR: not a Debian-family host and no docker/podman found." >&2
        exit 1
    fi
    echo " mode: container (${CTR} debian:trixie)"
fi
echo "============================================================"

run_inner() {
    KERNEL_VERSION="$KERNEL_VERSION" PKGREL="$PKGREL" \
    CODENAME="$CODENAME" CACHYOS_SCHED="$CACHYOS_SCHED" \
    OUTPUT_DIR="$POOL" bash "$HERE/inner-build.sh"
}

if [ "$USE_NATIVE" = "1" ]; then
    if [ "$(id -u)" != "0" ]; then
        exec sudo env \
            KERNEL_VERSION="$KERNEL_VERSION" PKGREL="$PKGREL" \
            CODENAME="$CODENAME" CACHYOS_SCHED="$CACHYOS_SCHED" \
            OUTPUT_DIR="$POOL" bash "$HERE/inner-build.sh"
    else
        run_inner
    fi
else
    "$CTR" pull debian:trixie >/dev/null 2>&1 || true
    "$CTR" run --rm -t \
        -e KERNEL_VERSION="$KERNEL_VERSION" \
        -e PKGREL="$PKGREL" \
        -e CODENAME="$CODENAME" \
        -e CACHYOS_SCHED="$CACHYOS_SCHED" \
        -e OUTPUT_DIR=/output \
        -v "$POOL":/output \
        -v "$HERE/inner-build.sh":/inner.sh:ro \
        debian:trixie /bin/bash /inner.sh || {
        echo ">> Container build failed. Log: repo/pool/build.log"
        exit 1
    }
fi

echo ""
echo "============================================================"
echo " Done. Packages staged in repo/pool/:"
ls -lh "$POOL"/linux-*"${CODENAME}"*.deb 2>/dev/null \
    || ls -lh "$POOL"/linux-*t2*.deb 2>/dev/null \
    || true
echo ""
echo " Publish:"
echo "   ./repo/publish-repo.sh && ./repo/upload-pool.sh"
echo "============================================================"
