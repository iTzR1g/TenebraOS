#!/bin/bash
# setup-build-deps.sh
# Installs every package needed to build TenebraOS natively on Devuan/Debian:
#   - ISO (live-build pipeline)          ./build.sh
#   - custom .debs                        ./build-packages.sh
#   - kernel (T2 + CachyOS)               ./repo/pkgs/linux-cachyos-t2/build.sh
#   - apt repo publish/upload             ./repo/publish-repo.sh, upload-pool.sh
#
# Usage: sudo ./setup-build-deps.sh

set -euo pipefail

[ "$(id -u)" = "0" ] || { echo "run with sudo" >&2; exit 1; }
command -v apt-get >/dev/null || { echo "needs apt (Devuan/Debian)" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive

echo "==> Updating package index..."
apt-get update

echo "==> Installing ISO build dependencies (live-build)..."
apt-get install -y --no-install-recommends \
    live-build \
    debootstrap \
    squashfs-tools \
    xorriso \
    dpkg-dev \
    fakeroot \
    dosfstools \
    mtools \
    syslinux-common

echo "==> Installing kernel build dependencies..."
apt-get install -y --no-install-recommends \
    build-essential \
    fakeroot \
    libncurses-dev \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    openssl \
    dkms \
    libudev-dev \
    libpci-dev \
    libiberty-dev \
    autoconf \
    wget \
    xz-utils \
    git \
    bc \
    rsync \
    cpio \
    debhelper \
    kernel-wedge \
    curl \
    gawk \
    dwarves \
    zstd \
    python3 \
    libdw-dev \
    lsb-release \
    perl

echo "==> Installing repo publishing dependencies..."
apt-get install -y --no-install-recommends \
    apt-utils \
    gnupg

echo "==> Installing test/QEMU dependencies (optional but recommended)..."
apt-get install -y --no-install-recommends \
    qemu-system-x86 \
    ovmf \
    || echo "    (skipped — QEMU testing unavailable)"

echo ""
echo "==> Checking gh CLI (needed for ./repo/upload-pool.sh)..."
if ! command -v gh >/dev/null; then
    if apt-cache show gh >/dev/null 2>&1; then
        apt-get install -y gh
    else
        echo "    gh not in repos — install manually:"
        echo "      curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg | dd of=/usr/share/keyrings/githubcli-archive-keyring.gpg"
        echo "      echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main' > /etc/apt/sources.list.d/github-cli.list"
        echo "      apt-get update && apt-get install gh"
    fi
else
    echo "    gh already installed"
fi

echo ""
echo "==> Done. Build with:"
echo "     ./build.sh                              # ISO"
echo "     sudo ./repo/pkgs/linux-cachyos-t2/build.sh   # custom kernel"
echo "     ./repo/publish-repo.sh && ./repo/upload-pool.sh  # ship packages"
