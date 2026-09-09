#!/bin/bash
# bare/02-kernel-initramfs.sh
# Phase 2: Compile the Linux kernel from source and generate a Btrfs-aware initramfs
#
# This script:
#   1. Downloads the kernel source if not already present
#   2. Applies a TenebraOS default config (or uses the previous .config)
#   3. Compiles the kernel + modules
#   4. Installs modules into the rootfs
#   5. Generates a minimal Btrfs-aware initramfs with switch_root support
#   6. Installs GRUB as the bootloader
#
# Usage:
#   sudo ./02-kernel-initramfs.sh

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_ROOTFS="$TENEBRA_BUILD/rootfs"
TENEBRA_SOURCES="$TENEBRA_BUILD/sources"
TENEBRA_LOGS="$TENEBRA_BUILD/logs"
TENEBRA_JOBS="${TENEBRA_JOBS:-$(nproc)}"
TENEBRA_TARGET_DEV="${TENEBRA_TARGET_DEV:-/dev/sdX}"

LINUX_VER="${LINUX_VER:-6.10.6}"
KERNEL_CONFIG="${KERNEL_CONFIG:-generic}"  # generic | t2 | lowlatency

log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }

require_root() { [ "$(id -u)" -eq 0 ] || err "Must run as root"; }

# ─── Kernel Compilation ────────────────────────────────────────────────────────
build_kernel() {
    log "Compiling Linux kernel ${LINUX_VER}"

    local KSRC="$TENEBRA_BUILD/linux-${LINUX_VER}"

    if [ ! -d "$KSRC" ]; then
        if [ -f "$TENEBRA_SOURCES/linux-${LINUX_VER}.tar.xz" ]; then
            tar xf "$TENEBRA_SOURCES/linux-${LINUX_VER}.tar.xz" -C "$TENEBRA_BUILD"
        else
            wget -q -O "$TENEBRA_SOURCES/linux-${LINUX_VER}.tar.xz" \
                "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VER}.tar.xz"
            tar xf "$TENEBRA_SOURCES/linux-${LINUX_VER}.tar.xz" -C "$TENEBRA_BUILD"
        fi
    fi

    cd "$KSRC"

    # Use existing .config or generate from defconfig
    if [ -f .config ]; then
        info "Reusing existing .config"
        make olddefconfig 2>&1 | tee "$TENEBRA_LOGS/kernel-config.log"
    else
        info "Generating config: ${KERNEL_CONFIG}"
        case "$KERNEL_CONFIG" in
            t2)
                make defconfig 2>&1 | tee "$TENEBRA_LOGS/kernel-config.log"
                # Enable T2-specific options
                scripts/config --enable CONFIG_APPLE_T2
                scripts/config --enable CONFIG_APPLE_T2_SIMPLEDRM
                scripts/config --enable CONFIG_SPI_APPLE
                scripts/config --enable CONFIG_APPLE_GMUX
                ;;
            lowlatency)
                make defconfig 2>&1 | tee "$TENEBRA_LOGS/kernel-config.log"
                scripts/config --disable PREEMPT_VOLUNTARY
                scripts/config --enable PREEMPT
                scripts/config --enable PREEMPT_RT
                scripts/config --set-val HZ 1000
                ;;
            *)
                make defconfig 2>&1 | tee "$TENEBRA_LOGS/kernel-config.log"
                ;;
        esac

        # Essential options for TenebraOS
        scripts/config --enable CONFIG_BTRFS_FS
        scripts/config --enable CONFIG_BTRFS_FS_POSIX_ACL
        scripts/config --enable CONFIG_BTRFS_FS_SECURITY_LABEL
        scripts/config --enable CONFIG_ZRAM
        scripts/config --enable CONFIG_ZRAM_DEF_COMPRESSOR_ZSTD
        scripts/config --enable CONFIG_ZSTD_COMPRESS
        scripts/config --enable CONFIG_SQUASHFS
        scripts/config --enable CONFIG_SQUASHFS_ZSTD
        scripts/config --enable CONFIG_OVERLAY_FS
        scripts/config --enable CONFIG_VFAT_FS
        scripts/config --enable CONFIG_NTFS3_FS
        scripts/config --enable CONFIG_EXT4_FS

        # Auto-load modules
        scripts/config --enable CONFIG_MODULES
        scripts/config --enable CONFIG_MODULE_UNLOAD
        scripts/config --enable CONFIG_MODULE_FORCE_UNLOAD

        # Sound
        scripts/config --enable CONFIG_SND_HDA_INTEL
        scripts/config --enable CONFIG_SND_HDA_CODEC_REALTEK
        scripts/config --enable CONFIG_SND_HDMI_AUDIO

        # DRM / GPU
        scripts/config --enable CONFIG_DRM_I915
        scripts/config --enable CONFIG_DRM_AMDGPU
        scripts/config --enable CONFIG_DRM_NOUVEAU
        scripts/config --enable CONFIG_DRM_VIRTIO_GPU

        # Networking
        scripts/config --enable CONFIG_NETFILTER
        scripts/config --enable CONFIG_NF_CONNTRACK
        scripts/config --enable CONFIG_IP_NF_IPTABLES

        # USB
        scripts/config --enable CONFIG_USB_XHCI_HCD
        scripts/config --enable CONFIG_USB_EHCI_HCD
        scripts/config --enable CONFIG_USB_STORAGE

        # NVMe
        scripts/config --enable CONFIG_BLK_DEV_NVME

        # EFI
        scripts/config --enable CONFIG_EFI_STUB

        make olddefconfig 2>&1 | tee -a "$TENEBRA_LOGS/kernel-config.log"
    fi

    # Build kernel
    log "  Compiling kernel (this will take a while)..."
    make -j"$TENEBRA_JOBS" bzImage 2>&1 | tee "$TENEBRA_LOGS/kernel-build.log"

    # Build modules
    log "  Building kernel modules..."
    make -j"$TENEBRA_JOBS" modules 2>&1 | tee "$TENEBRA_LOGS/kernel-modules.log"

    log "Kernel compilation complete"
}

# ─── Install Kernel + Modules ──────────────────────────────────────────────────
install_kernel() {
    log "Installing kernel and modules into rootfs"

    local KSRC="$TENEBRA_BUILD/linux-${LINUX_VER}"
    local KREL="$(cd "$KSRC" && make -s kernelrelease)"
    local BOOT_DIR="$TENEBRA_ROOTFS/boot"

    mkdir -p "$BOOT_DIR"

    # Install kernel image
    cp "$KSRC/arch/x86/boot/bzImage" "$BOOT_DIR/vmlinuz-${KREL}"

    # Install modules
    make -C "$KSRC" \
        INSTALL_MOD_PATH="$TENEBRA_ROOTFS/usr" \
        modules_install 2>&1 | tee "$TENEBRA_LOGS/kernel-install.log"

    # Save config for reference
    cp "$KSRC/.config" "$BOOT_DIR/config-${KREL}"

    # Save System.map for reference
    cp "$KSRC/System.map" "$BOOT_DIR/System.map-${KREL}"

    # Store the kernel release for initramfs generation
    echo "$KREL" > "$TENEBRA_BUILD/.kernel-release"

    log "Kernel ${KREL} installed to rootfs"
}

# ─── Btrfs-Aware Initramfs Generator ───────────────────────────────────────────
generate_initramfs() {
    log "Generating Btrfs-aware initramfs"

    local KREL
    KREL="$(cat "$TENEBRA_BUILD/.kernel-release")"
    local INITRAMFS="$TENEBRA_ROOTFS/boot/initrd.img-${KREL}"
    local TMPDIR="$TENEBRA_BUILD/initramfs-tmp"

    rm -rf "$TMPDIR"
    mkdir -p "$TMPDIR"/{bin,dev,etc,lib,lib64,mnt,proc,root,run,sbin,sys,tmp,usr,var}

    # --- The Init Script (PID 1) ---
    cat > "$TMPDIR/init" <<'INITSCRIPT'
#!/bin/sh
# TenebraOS initramfs init — Btrfs-aware, switch_root capable
# This is the first userspace process. It:
#   1. Mounts essential pseudo-filesystems
#   2. Detects the root device
#   3. Mounts Btrfs subvolumes or falls back to ext4
#   4. switch_root into the real rootfs

export PATH=/sbin:/bin:/usr/sbin:/usr/bin

log() { echo "[initramfs] $*"; }

emergency_shell() {
    log "Dropping to emergency shell..."
    exec /bin/sh
}

mount_pseudo() {
    log "Mounting pseudo-filesystems"
    mount -t proc     proc     /proc
    mount -t sysfs    sysfs    /sys
    mount -t devtmpfs devtmpfs /dev
    mkdir -p /dev/pts
    mount -t devpts   devpts   /dev/pts
    mount -t tmpfs    tmpfs    /run
}

wait_for_device() {
    local dev="$1" timeout=30
    log "Waiting for $dev (timeout: ${timeout}s)"
    while [ ! -b "$dev" ] && [ "$timeout" -gt 0 ]; do
        sleep 0.5
        timeout=$((timeout - 1))
    done
    [ -b "$dev" ] || return 1
}

find_root_device() {
    # Check kernel command line for root= parameter
    local root_param=""
    for param in $(cat /proc/cmdline); do
        case "$param" in
            root=*) root_param="${param#root=}" ;;
        esac
    done

    if [ -n "$root_param" ]; then
        # Resolve UUID if needed
        case "$root_param" in
            UUID=*)
                local uuid="${root_param#UUID=}"
                for dev in /dev/disk/by-uuid/*; do
                    if [ "$(readlink -f "$dev")" != "$dev" ] || [ -e "$dev" ]; then
                        if [ "$(basename "$dev")" = "$uuid" ]; then
                            echo "$dev"
                            return 0
                        fi
                    fi
                done
                ;;
            LABEL=*)
                local label="${root_param#LABEL=}"
                for dev in /dev/disk/by-label/*; do
                    if [ "$(basename "$dev")" = "$label" ]; then
                        echo "$dev"
                        return 0
                    fi
                done
                ;;
            /dev/*)
                echo "$root_param"
                return 0
                ;;
        esac
    fi

    # Fallback: find the TEBRA_ROOT partition
    for dev in /dev/sd? /dev/nvme?n?p? /dev/vd?; do
        [ -b "$dev" ] || continue
        local label
        label="$(blkid -s LABEL -o value "$dev" 2>/dev/null || true)"
        if [ "$label" = "TEBRA_ROOT" ]; then
            echo "$dev"
            return 0
        fi
    done

    return 1
}

mount_root() {
    local root_dev="$1"
    local mountpoint="/mnt"

    log "Root device: $root_dev"

    # Detect filesystem type
    local fstype
    fstype="$(blkid -s TYPE -o value "$root_dev" 2>/dev/null || echo "unknown")"

    case "$fstype" in
        btrfs)
            log "Detected Btrfs filesystem"
            # Try subvolume @ first
            mount -o subvol=@,compress=zstd:1,ssd,noatime "$root_dev" "$mountpoint" 2>/dev/null && {
                log "Mounted Btrfs subvol=@ at $mountpoint"
                return 0
            }
            # Fallback: mount default subvolume
            mount -o compress=zstd:1,ssd,noatime "$root_dev" "$mountpoint" 2>/dev/null && {
                log "Mounted Btrfs default subvol at $mountpoint"
                return 0
            }
            # Last resort: mount without options
            mount "$root_dev" "$mountpoint" 2>/dev/null && {
                log "Mounted Btrfs (raw) at $mountpoint"
                return 0
            }
            ;;
        ext4)
            log "Detected ext4 filesystem"
            mount -o errors=remount-ro "$root_dev" "$mountpoint" && return 0
            ;;
        *)
            log "Unknown filesystem: $fstype — trying generic mount"
            mount "$root_dev" "$mountpoint" && return 0
            ;;
    esac

    return 1
}

mount_subvolumes() {
    local root_dev="$1"
    local mountpoint="/mnt"

    # Mount remaining Btrfs subvolumes
    if [ -d "$mountpoint/@home" ] || btrfs subvolume list "$mountpoint" 2>/dev/null | grep -q '@home'; then
        mount -o subvol=@home,compress=zstd:1,ssd,noatime "$root_dev" "$mountpoint/home" 2>/dev/null || true
    fi
    if [ -d "$mountpoint/@snapshots" ] || btrfs subvolume list "$mountpoint" 2>/dev/null | grep -q '@snapshots'; then
        mount -o subvol=@snapshots,compress=zstd:1,ssd,noatime "$root_dev" "$mountpoint/snapshots" 2>/dev/null || true
    fi
}

# --- Main init flow ---
log "TenebraOS initramfs starting..."

mount_pseudo

# Load essential modules
for mod in btrfs ext4 vfat nvme xhci_pci ehci_pci usb_storage; do
    modprobe "$mod" 2>/dev/null || true
done

# Find and mount root
ROOT_DEV="$(find_root_device)" || {
    log "ERROR: Could not find root device"
    emergency_shell
}

wait_for_device "$ROOT_DEV" || {
    log "ERROR: Root device $ROOT_DEV not found"
    emergency_shell
}

mount_root "$ROOT_DEV" || {
    log "ERROR: Failed to mount root"
    emergency_shell
}

mount_subvolumes "$ROOT_DEV"

# Bind mount necessary pseudo-filesystems into the new root
mkdir -p /mnt/{dev,proc,sys,run}
mount --move /dev  /mnt/dev
mount --move /proc /mnt/proc
mount --move /sys  /mnt/sys
mount --move /run  /mnt/run

log "Switching root to /mnt"
exec switch_root -c /dev/console /mnt /sbin/init
INITSCRIPT
    chmod +x "$TMPDIR/init"

    # --- Busybox (static) for essential tools ---
    log "  Installing busybox (static) into initramfs"
    if [ ! -f "$TENEBRA_SOURCES/busybox-${BUSYBOX_VER:-1.36.1}.tar.xz" ]; then
        BUSYBOX_VER="${BUSYBOX_VER:-1.36.1}"
        wget -q -O "$TENEBRA_SOURCES/busybox-${BUSYBOX_VER}.tar.xz" \
            "https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.xz" || true
    fi

    # If busybox is available, build static; otherwise use host busybox
    if command -v busybox >/dev/null 2>&1; then
        BUSYBOX_BIN="$(command -v busybox)"
    else
        BUSYBOX_VER="${BUSYBOX_VER:-1.36.1}"
        if [ -f "$TENEBRA_SOURCES/busybox-${BUSYBOX_VER}.tar.xz" ]; then
            local BBUILD="$TENEBRA_BUILD/busybox-${BUSYBOX_VER}"
            if [ ! -d "$BBUILD" ]; then
                tar xf "$TENEBRA_SOURCES/busybox-${BUSYBOX_VER}.tar.xz" -C "$TENEBRA_BUILD"
            fi
            cd "$BBUILD"
            if [ ! -f .config ]; then
                make defconfig 2>&1 | tee "$TENEBRA_LOGS/busybox-config.log"
                sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
                make olddefconfig 2>&1 | tee -a "$TENEBRA_LOGS/busybox-config.log"
            fi
            make -j"$TENEBRA_JOBS" 2>&1 | tee "$TENEBRA_LOGS/busybox-build.log"
            BUSYBOX_BIN="$BBUILD/busybox"
        fi
    fi

    if [ -n "${BUSYBOX_BIN:-}" ] && [ -f "$BUSYBOX_BIN" ]; then
        # Copy busybox and create symlinks
        cp "$BUSYBOX_BIN" "$TMPDIR/bin/busybox"
        chmod +x "$TMPDIR/bin/busybox"
        for cmd in sh bash mount umount switch_root modprobe blkid cat ls mkdir \
                   mknod mount_proc mount_sys mount_dev chroot sleep grep; do
            ln -sf busybox "$TMPDIR/bin/$cmd"
        done
    else
        # Fallback: copy essential binaries from host
        for bin in sh mount umount switch_root modprobe blkid sleep; do
            local binpath
            binpath="$(command -v "$bin" 2>/dev/null || true)"
            if [ -n "$binpath" ]; then
                cp "$binpath" "$TMPDIR/bin/" 2>/dev/null || true
                # Copy required libraries
                ldd "$binpath" 2>/dev/null | awk '/=>/ {print $3}' | while read -r lib; do
                    [ -f "$lib" ] && cp "$lib" "$TMPDIR/lib/" 2>/dev/null || true
                done
            fi
        done
    fi

    # Copy required libraries from rootfs into initramfs
    for lib in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
               libgcc_s.so.1 libresolv.so.2; do
        local libsrc
        libsrc="$(find "$TENEBRA_ROOTFS/lib" "$TENEBRA_ROOTFS/usr/lib" -name "$lib" 2>/dev/null | head -1)"
        if [ -n "$libsrc" ] && [ -f "$libsrc" ]; then
            cp "$libsrc" "$TMPDIR/lib/" 2>/dev/null || true
        fi
    done

    # Copy kernel modules needed for boot
    local KREL
    KREL="$(cat "$TENEBRA_BUILD/.kernel-release")"
    local MODDIR="$TENEBRA_ROOTFS/lib/modules/$KREL"
    if [ -d "$MODDIR" ]; then
        mkdir -p "$TMPDIR/lib/modules"
        # Copy only essential boot modules (btrfs, storage, filesystem)
        for mod in btrfs crc32c libcrc32c crypto_simd cryptd ext4 vfat fat \
                   nvme nvme-core xhci-pci ehci-pci usb-storage sd_mod \
                   ahci libahci; do
            find "$MODDIR" -name "${mod}.ko*" -exec cp {} "$TMPDIR/lib/modules/" \; 2>/dev/null || true
        done
        # Copy modules.dep for modprobe
        if [ -f "$MODDIR/modules.dep" ]; then
            cp "$MODDIR/modules.dep" "$TMPDIR/lib/modules/" 2>/dev/null || true
        fi
    fi

    # Build the initramfs cpio archive
    log "  Packing initramfs"
    cd "$TMPDIR"
    find . -print0 | cpio --null -o --format=newc --quiet 2>/dev/null | \
        gzip -9 > "$INITRAMFS"

    log "Initramfs created: $INITRAMFS ($(du -h "$INITRAMFS" | cut -f1))"
}

# ─── GRUB Bootloader Installation ──────────────────────────────────────────────
install_grub() {
    log "Installing GRUB bootloader"

    local KREL
    KREL="$(cat "$TENEBRA_BUILD/.kernel-release")"

    # Determine boot directory and device
    local BOOT_DIR="$TENEBRA_ROOTFS/boot"
    local MOUNT_DEV="$TENEBRA_TARGET_DEV"

    if [ "$MOUNT_DEV" = "/dev/sdX" ]; then
        log "No target device — generating GRUB config only"
    fi

    # Generate grub.cfg
    cat > "$BOOT_DIR/grub/grub.cfg" <<GRUBCFG
# TenebraOS GRUB configuration
# Auto-generated by 02-kernel-initramfs.sh

set default=0
set timeout=5

loadfont unicode

set gfxmode=auto
set gfxpayload=keep

insmod efi_gop
insmod efi_uga
insmod gfxterm
insmod gfxmenu
insmod all_video
insmod jpeg
insmod png

terminal_output gfxterm

set theme=\${prefix}/themes/tenebra/theme.txt
export theme

menuentry "TenebraOS ${KREL}" {
    search --no-floppy --fs-uuid --set=root ROOT_UUID_PLACEHOLDER
    linux /boot/vmlinuz-${KREL} root=ROOT_UUID_PLACEHOLDER subvol=@ rootflags=subvol=@ rw quiet splash
    initrd /boot/initrd.img-${KREL}
}

menuentry "TenebraOS ${KREL} (recovery)" {
    search --no-floppy --fs-uuid --set=root ROOT_UUID_PLACEHOLDER
    linux /boot/vmlinuz-${KREL} root=ROOT_UUID_PLACEHOLDER subvol=@ rootflags=subvol=@ rw single
    initrd /boot/initrd.img-${KREL}
}

menuentry "TenebraOS (previous kernel)" {
    search --no-floppy --fs-uuid --set=root ROOT_UUID_PLACEHOLDER
    linux /boot/vmlinuz-* root=ROOT_UUID_PLACEHOLDER subvol=@ rootflags=subvol=@ rw quiet splash
    initrd /boot/initrd.img-*
}
GRUBCFG

    # Replace ROOT_UUID_PLACEHOLDER with actual UUID if device is known
    if [ "$MOUNT_DEV" != "/dev/sdX" ] && [ -b "$MOUNT_DEV" ]; then
        local ROOT_PART="${MOUNT_DEV}2"
        if [[ "$MOUNT_DEV" == *"nvme"* ]]; then
            ROOT_PART="${MOUNT_DEV}p2"
        fi
        local ROOT_UUID
        ROOT_UUID="$(blkid -s UUID -o value "$ROOT_PART" 2>/dev/null || echo "ROOT_UUID_PLACEHOLDER")"
        sed -i "s/ROOT_UUID_PLACEHOLDER/$ROOT_UUID/g" "$BOOT_DIR/grub/grub.cfg"
    fi

    # Generate GRUB themes directory
    mkdir -p "$BOOT_DIR/grub/themes/tenebra"

    # Install GRUB if target device is specified
    if [ "$MOUNT_DEV" != "/dev/sdX" ]; then
        # Chroot and install GRUB
        mount --bind /dev  "$TENEBRA_ROOTFS/dev"  2>/dev/null || true
        mount --bind /dev/pts "$TENEBRA_ROOTFS/dev/pts" 2>/dev/null || true
        mount --bind /proc "$TENEBRA_ROOTFS/proc" 2>/dev/null || true
        mount --bind /sys  "$TENEBRA_ROOTFS/sys"  2>/dev/null || true

        chroot "$TENEBRA_ROOTFS" /bin/bash -c "grub-install --target=x86_64-efi \
            --efi-directory=/boot --bootloader-id=TenebraOS --recheck" 2>&1 | \
            tee "$TENEBRA_LOGS/grub-efi.log" || \
            log "GRUB EFI install skipped (no EFI variables available)"

        chroot "$TENEBRA_ROOTFS" /bin/bash -c "grub-install --target=i386-pc \
            --recheck ${MOUNT_DEV}" 2>&1 | \
            tee "$TENEBRA_LOGS/grub-bios.log" || \
            log "GRUB BIOS install skipped"

        umount "$TENEBRA_ROOTFS/dev/pts" 2>/dev/null || true
        umount "$TENEBRA_ROOTFS/dev" 2>/dev/null || true
        umount "$TENEBRA_ROOTFS/proc" 2>/dev/null || true
        umount "$TENEBRA_ROOTFS/sys" 2>/dev/null || true
    fi

    log "GRUB installed and configured"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    [ "$(id -u)" -eq 0 ] || { echo "Must run as root" >&2; exit 1; }
    [ -d "$TENEBRA_ROOTFS" ] || err "Rootfs not found at $TENEBRA_ROOTFS — run 01-bootstrap.sh first"

    build_kernel
    install_kernel
    generate_initramfs
    install_grub

    log "Phase 2 complete: kernel + initramfs + GRUB installed"
    log "Next: sudo ./03-init-setup.sh"
}

main "$@"
