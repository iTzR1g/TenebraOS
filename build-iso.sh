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

# Detect container runtime
if command -v docker &>/dev/null; then
    CTR="docker"
elif command -v podman &>/dev/null; then
    CTR="podman"
else
    echo "ERROR: Need docker or podman. Install: sudo pacman -S docker"
    exit 1
fi

DEBIAN_IMAGE="debian:trixie"

echo "============================================================"
echo " TenebraOS ISO Builder (Docker)"
echo " Image: ${DEBIAN_IMAGE}"
echo "============================================================"

# Pull image
echo ">> Pulling ${DEBIAN_IMAGE}..."
${CTR} pull "${DEBIAN_IMAGE}" 2>/dev/null || true

# Build
echo ">> Building ISO inside container (20-40 min)..."
${CTR} run \
    --rm \
    --privileged \
    -t \
    -v "${PROJECT_DIR}":/repo \
    -w /repo \
    "${DEBIAN_IMAGE}" \
    /bin/bash -c '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

echo ">> Installing live-build and dependencies..."
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    live-build cpio gzip xz-utilslz4 zstd \
    devscripts debhelper dpkg-dev fakeroot \
    git curl ca-certificates \
    > /dev/null 2>&1

echo ">> Cleaning old artifacts..."
lb clean 2>/dev/null || true

echo ">> Configuring live-build..."
lb config

echo ">> Building custom packages..."
bash build-packages.sh

echo ">> Building ISO (20-40 min)..."
lb build 2>&1 | tee build.log

if [ -f live-image-amd64.hybrid.iso ]; then
    echo ""
    echo ">> SUCCESS: live-image-amd64.hybrid.iso"
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
