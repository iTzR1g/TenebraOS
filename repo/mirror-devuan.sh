#!/bin/bash
# repo/mirror-devuan.sh
# Mirrors essential Devuan packages into repo/pool/ for the TenebraOS apt repo.
#
# Downloads packages from Devuan Excalibur (amd64) and places them in
# pool/ with the standard naming. Only downloads packages listed in
# the TenebraOS package lists (tenebra.list.chroot, desktop.list.chroot).
#
# Usage:
#   sudo ./repo/mirror-devuan.sh
#   sudo ./repo/mirror-devuan.sh --all    # mirror ALL packages in pool/
#
# Requires: apt-get, dpkg-dev (for apt-get download)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
POOL_DIR="$REPO_ROOT/pool"
PKG_LISTS_DIR="$REPO_ROOT/../config/package-lists"

mkdir -p "$POOL_DIR"

# Essential packages from TenebraOS package lists (base system + desktop)
ESSENTIAL_PKGS=(
    # --- Base system (tenebra.list.chroot) ---
    wget git pciutils usbutils dkms
    linux-image-amd64 linux-headers-amd64
    # --- Desktop (desktop.list.chroot) ---
    plasma-desktop sddm plasma-nm plasma-discover plasma-discover-backend-flatpak
    konsole kate
    pipewire pipewire-pulse wireplumber
    flatpak distrobox podman
    calamares
    live-boot live-config live-config-sysvinit
    runit elogind dbus
    udisks2 sudo network-manager
    curl dosfstools e2fsprogs ntfs-3g
    grub-efi-amd64-bin grub-pc-bin grub2-common efibootmgr
    firmware-linux firmware-linux-nonfree firmware-misc-nonfree
    intel-microcode amd64-microcode
    devuan-keyring
    # --- Calamares deps ---
    calamares-settings-debian
)

# Resolve package names from apt cache (handles virtual packages)
echo ">> Resolving package list..."
RESOLVED=()
for pkg in "${ESSENTIAL_PKGS[@]}"; do
    # Try direct name first, then resolve virtual
    RESOLVED_PKG=$(apt-cache show "$pkg" 2>/dev/null | awk '/^Package:/{print $2; exit}' || true)
    if [ -n "$RESOLVED_PKG" ]; then
        RESOLVED+=("$RESOLVED_PKG")
    else
        echo "   WARN: $pkg not found, skipping"
    fi
done

echo ">> Downloading ${#RESOLVED[@]} packages to $POOL_DIR/..."
cd "$POOL_DIR"

# Download each package (skip if already present)
for pkg in "${RESOLVED[@]}"; do
    # Check if any version of this package is already in pool
    PKG_BASE=$(echo "$pkg" | sed 's/:amd64$//')
    if ls "${PKG_BASE}"_*.deb >/dev/null 2>&1; then
        echo "   skip: $PKG_BASE (already present)"
        continue
    fi
    echo "   download: $pkg"
    apt-get download "$pkg" 2>/dev/null || echo "   WARN: failed to download $pkg"
done

echo ""
echo ">> Pool contents:"
ls -lh "$POOL_DIR"/*.deb 2>/dev/null | awk '{print "   "$NF}'
echo ""
echo ">> Done. Run ./repo/publish-repo.sh to regenerate the index."
