#!/bin/bash
# bare/setup-host-deps.sh
# Install all host dependencies for building TenebraOS
#
# Detects the package manager (pacman/apt/dnf) and installs
# the equivalent packages for the current distro.
#
# Usage:
#   sudo ./setup-host-deps.sh

set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "Run as root" >&2; exit 1; }

install_arch() {
    echo "==> Detected Arch Linux (pacman)"

    # Core build tools
    pacman -S --needed --noconfirm \
        base-devel gcc make git wget curl rsync \
        || true

    # Cross-compilation + kernel build
    pacman -S --needed --noconfirm \
        gmp mpfr libmpc \
        libelf ncurses bison flex openssl bc cpio perl pahole python \
        || true

    # Filesystem tools
    pacman -S --needed --noconfirm \
        btrfs-progs dosfstools e2fsprogs parted util-linux \
        || true

    # ISO creation
    pacman -S --needed --noconfirm \
        squashfs-tools xorriso mtools syslinux \
        || true

    # debootstrap / mmdebstrap (AUR — install manually)
    if ! command -v mmdebstrap >/dev/null 2>&1 && ! command -v debootstrap >/dev/null 2>&1; then
        echo ""
        echo "  Bootstrap tool not found. Install one from AUR:"
        echo "    yay -S mmdebstrap    (recommended)"
        echo "    yay -S debootstrap   (may have arch issues on Arch)"
        echo ""
    fi

    # GRUB
    pacman -S --needed --noconfirm \
        grub \
        || true

    # Optional: QEMU for testing
    pacman -S --needed --noconfirm \
        qemu-full edk2-ovmf \
        || echo "  (skipped — QEMU testing unavailable)"
}

install_debian() {
    echo "==> Detected Debian/Ubuntu (apt)"

    export DEBIAN_FRONTEND=noninteractive

    apt-get update

    apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        make \
        git \
        wget \
        curl \
        rsync

    apt-get install -y --no-install-recommends \
        gawk \
        bison \
        flex \
        texinfo \
        gettext

    apt-get install -y --no-install-recommends \
        libncurses-dev \
        libssl-dev \
        libelf-dev \
        libgmp-dev \
        libmpfr-dev \
        libmpc-dev

    apt-get install -y --no-install-recommends \
        libudev-dev \
        libpci-dev \
        libiberty-dev \
        autoconf \
        bc \
        cpio \
        perl \
        dwarves \
        python3

    apt-get install -y --no-install-recommends \
        btrfs-progs \
        dosfstools \
        e2fsprogs \
        parted \
        fdisk \
        uuid-runtime

    apt-get install -y --no-install-recommends \
        squashfs-tools \
        xorriso \
        mtools \
        syslinux-common

    apt-get install -y --no-install-recommends \
        grub-pc-bin \
        grub-efi-amd64-bin \
        grub2-common

    apt-get install -y --no-install-recommends \
        qemu-system-x86 \
        ovmf \
        || echo "  (skipped — QEMU testing unavailable)"
}

install_fedora() {
    echo "==> Detected Fedora (dnf)"

    dnf groupinstall -y "Development Tools"

    dnf install -y \
        gcc \
        make \
        git \
        wget \
        curl \
        rsync \
        gawk \
        bison \
        flex \
        texinfo \
        gettext

    dnf install -y \
        ncurses-devel \
        openssl-devel \
        elfutils-libelf-devel \
        gmp-devel \
        mpfr-devel \
        libmpc-devel \
        systemd-devel \
        pciutils-devel \
        autoconf \
        bc \
        cpio \
        perl \
        python3

    dnf install -y \
        btrfs-progs \
        dosfstools \
        e2fsprogs \
        parted \
        util-linux

    dnf install -y \
        squashfs-tools \
        xorriso \
        mtools \
        syslinux

    dnf install -y \
        grub2-efi-x64-modules \
        grub2-pc-modules \
        || echo "  (skipped — GRUB not available)"

    dnf install -y \
        qemu-system-x86 \
        edk2-ovmf \
        || echo "  (skipped — QEMU testing unavailable)"
}

# Detect distro
if [ -f /etc/arch-release ]; then
    install_arch
elif [ -f /etc/debian_version ]; then
    install_debian
elif [ -f /etc/fedora-release ]; then
    install_fedora
else
    echo "ERROR: Unknown distro. Install these packages manually:"
    echo ""
    echo "Build: gcc, make, git, wget, curl, rsync"
    echo "Kernel: libelf, libncurses-dev, bison, flex, openssl, bc, cpio, perl, dwarves"
    echo "Filesystem: btrfs-progs, dosfstools, parted"
    echo "ISO: squashfs-tools, xorriso, mtools"
    echo "Boot: grub (EFI + BIOS modules)"
    exit 1
fi

echo ""
echo "==> All dependencies installed."
echo ""
echo "Build TenebraOS:"
echo "  cd $(dirname "$0")"
echo "  sudo ./01-bootstrap.sh /dev/sdX"
echo "  sudo ./02-kernel-initramfs.sh"
echo "  sudo ./03-init-setup.sh runit"
echo "  sudo ./05-snapshot-setup.sh"
echo "  sudo ./06-master-build.sh --target /dev/sdX"
echo "  sudo ./07-build-iso.sh"
