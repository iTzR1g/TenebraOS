#!/bin/bash
# build-iso.sh — Build TenebraOS ISO from Arch Linux using Docker
#
# Usage:
#   ./build-iso.sh           # build ISO
#   ./build-iso.sh clean     # clean build artifacts
#
# Requires: docker or podman

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
CTR=""
if command -v docker &>/dev/null; then
    CTR="docker"
elif command -v podman &>/dev/null; then
    CTR="podman"
else
    echo "ERROR: Need docker or podman. Install: sudo pacman -S docker"
    exit 1
fi

IMAGE="devuan/excalibur"

if [[ "${1:-}" == "clean" ]]; then
    echo ">> Cleaning build artifacts..."
    cd "$PROJECT_DIR"
    rm -rf chroot config/cache build.log
    rm -f live-image-amd64.hybrid.iso
    echo ">> Done."
    exit 0
fi

echo "============================================================"
echo " TenebraOS ISO Builder (Docker)"
echo " Image: ${IMAGE}"
echo "============================================================"

echo ">> Pulling ${IMAGE}..."
${CTR} pull "${IMAGE}" 2>/dev/null || true

echo ">> Building ISO inside container (this will take a while)..."
${CTR} run \
    --rm \
    --privileged \
    -t \
    -v "${PROJECT_DIR}":/repo \
    -w /repo \
    "${IMAGE}" \
    /bin/bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo ">> Installing live-build and dependencies..."
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    live-build \
    cpio gzip xz-utils lz4 zstd \
    devscripts debhelper dpkg-dev fakeroot \
    git curl ca-certificates gnupg \
    > /dev/null 2>&1

echo ">> Cleaning old artifacts..."
for m in dev/pts dev proc sys run; do
    if mountpoint -q "chroot/$m" 2>/dev/null; then
        umount -lf "chroot/$m" || true
    fi
done
lb clean 2>/dev/null || true

echo ">> Configuring live-build (TenebraOS)..."
lb config noauto \
    --distribution excalibur \
    --architectures amd64 \
    --archive-areas "main contrib non-free non-free-firmware" \
    --binary-images iso-hybrid \
    --debian-installer false \
    --initsystem sysvinit \
    --keyring-packages devuan-keyring \
    --mirror-bootstrap http://deb.devuan.org/merged \
    --mirror-chroot http://deb.devuan.org/merged \
    --mirror-binary http://deb.devuan.org/merged \
    --security false \
    --updates false \
    --apt-options "--yes -o Acquire::ForceIPv4=true -o Acquire::Retries=5" \
    --bootappend-live "boot=live components nomodeset quiet splash username=user hostname=tenebra" \
    --iso-application "TenebraOS" \
    --iso-publisher "TenebraOS" \
    --iso-volume "TenebraOS" \
    --linux-flavours amd64 \
    --mode debian \
    --apt-recommends true

# Devuan live-build injects live-config-systemd which does not exist
sed -i "/^live-config-systemd$/d" config/package-lists/live.list.chroot 2>/dev/null || true

echo ">> Building custom packages..."
bash build-packages.sh

echo ">> Building ISO (this may take 20-40 minutes)..."
lb build 2>&1 | tee build.log

if [ -f live-image-amd64.hybrid.iso ]; then
    echo ""
    echo ">> SUCCESS: live-image-amd64.hybrid.iso created"
    ls -lh live-image-amd64.hybrid.iso
else
    echo ""
    echo ">> FAILED. Last 50 lines of build.log:"
    tail -50 build.log 2>/dev/null || true
    exit 1
fi
'

echo ""
echo "============================================================"
echo " ISO: ${PROJECT_DIR}/live-image-amd64.hybrid.iso"
echo "============================================================"
