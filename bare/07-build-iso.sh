#!/bin/bash
# bare/07-build-iso.sh
# Build a bootable ISO from the bare-source rootfs
#
# This script takes the rootfs built by phases 01-06 and creates a
# hybrid ISO (BIOS + UEFI) that can:
#   - Boot from USB (dd'ed)
#   - Boot from CD/DVD
#   - Boot from UEFI firmware
#
# The ISO contains a compressed squashfs of the rootfs, the kernel,
# initramfs, and GRUB/ISOLINUX bootloaders.
#
# Usage:
#   sudo ./07-build-iso.sh
#   sudo ./07-build-iso.sh --output /path/to/output.iso
#   sudo ./07-build-iso.sh --rootfs /custom/rootfs/path

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_ROOTFS="${TENEBRA_ROOTFS:-$TENEBRA_BUILD/rootfs}"
ISO_LABEL="TenebraOS"
ISO_APPLICATION="TenebraOS Live"
ISO_OUTPUT="${ISO_OUTPUT:-$TENEBRA_BUILD/TenebraOS-$(date +%Y%m%d).iso}"
STAGING="$TENEBRA_BUILD/iso-staging"
ISO_TEMP="$TENEBRA_BUILD/iso-temp"
DISC_ID="TenebraOS-$(date +%Y%m%d)"
COMPRESSOR="xz"
COMPRESSOR_OPTS="-Xdict-size 1M"

log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m>>>\033[0m %s\n' "$*"; }

require_root() { [ "$(id -u)" -eq 0 ] || err "Must run as root"; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || err "Required command not found: $1"
}

cleanup() {
    log "Cleaning up staging areas"
    umount "$STAGING/live" 2>/dev/null || true
    umount "$STAGING/proc" 2>/dev/null || true
    umount "$STAGING/sys" 2>/dev/null || true
    umount "$STAGING/dev" 2>/dev/null || true
    rm -rf "$STAGING" "$ISO_TEMP"
}
trap cleanup EXIT

# ─── Parse Arguments ───────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --output)  ISO_OUTPUT="$2"; shift 2 ;;
        --rootfs)  TENEBRA_ROOTFS="$2"; shift 2 ;;
        --compress)
            COMPRESSOR="$2"
            case "$COMPRESSOR" in
                gzip)    COMPRESSOR_OPTS="-9" ;;
                xz)      COMPRESSOR_OPTS="-Xdict-size 1M" ;;
                zstd)    COMPRESSOR_OPTS="-19" ;;
                *)       COMPRESSOR_OPTS="" ;;
            esac
            shift 2
            ;;
        --help)
            echo "Usage: $0 [--output <path>] [--rootfs <path>] [--compress <gzip|xz|zstd>]"
            exit 0
            ;;
        *) err "Unknown option: $1" ;;
    esac
done

# ─── Pre-flight Checks ────────────────────────────────────────────────────────
preflight() {
    log "Pre-flight checks"

    require_root
    require_cmd mksquashfs
    require_cmd xorriso
    require_cmd cpio

    # GRUB tools — try host paths, warn if missing
    if ! command -v grub-mkimage >/dev/null 2>&1; then
        if [ -x /usr/lib/grub/x86_64-efi/grub-mkimage ]; then
            GRUB_MKIMAGE="/usr/lib/grub/x86_64-efi/grub-mkimage"
        elif [ -x /usr/bin/grub2-mkimage ]; then
            GRUB_MKIMAGE="grub2-mkimage"
        else
            warn "grub-mkimage not found — UEFI boot may not work"
        fi
    fi
    if ! command -v grub-install >/dev/null 2>&1; then
        if command -v grub2-install >/dev/null 2>&1; then
            GRUB_INSTALL="grub2-install"
        else
            warn "grub-install not found — UEFI boot may not work"
        fi
    fi

    [ -d "$TENEBRA_ROOTFS" ] || err "Rootfs not found at $TENEBRA_ROOTFS"
    [ -f "$TENEBRA_ROOTFS/boot/vmlinuz-"* ] || {
        local kernel
        kernel="$(ls "$TENEBRA_ROOTFS/boot/vmlinuz-"* 2>/dev/null | head -1)"
        [ -n "$kernel" ] || err "No kernel found in $TENEBRA_ROOTFS/boot/"
    }

    # Check for enough disk space (squashfs + ISO = ~1.5x rootfs size)
    local rootfs_size
    rootfs_size="$(du -sb "$TENEBRA_ROOTFS" | awk '{print $1}')"
    local avail
    avail="$(df -B1 "$(dirname "$ISO_OUTPUT")" | tail -1 | awk '{print $4}')"
    local needed=$((rootfs_size + rootfs_size / 3))

    if [ "$avail" -lt "$needed" ]; then
        local need_gb=$((needed / 1073741824))
        local avail_gb=$((avail / 1073741824))
        err "Insufficient disk space: need ~${need_gb}GB, have ${avail_gb}GB"
    fi

    info "Rootfs: $TENEBRA_ROOTFS"
    info "Output: $ISO_OUTPUT"
    info "Compressor: $COMPRESSOR"
}

# ─── Prepare Staging Directory ─────────────────────────────────────────────────
prepare_staging() {
    log "Preparing ISO staging directory"

    rm -rf "$STAGING" "$ISO_TEMP"
    mkdir -p "$STAGING"/{live,boot/grub,boot/isolinux,EFI/BOOT}
    mkdir -p "$ISO_TEMP"

    # Copy kernel and initramfs
    local kernel
    kernel="$(ls "$TENEBRA_ROOTFS/boot/vmlinuz-"* 2>/dev/null | head -1)"
    local initrd
    initrd="$(ls "$TENEBRA_ROOTFS/boot/initrd.img-"* 2>/dev/null | head -1)"

    cp "$kernel" "$STAGING/boot/vmlinuz"
    [ -n "$initrd" ] && cp "$initrd" "$STAGING/boot/initrd.img" || true

    info "Kernel: $(basename "$kernel")"
    [ -n "$initrd" ] && info "Initrd: $(basename "$initrd")"
}

# ─── Create SquashFS ──────────────────────────────────────────────────────────
create_squashfs() {
    log "Creating SquashFS of rootfs (this may take a while)"

    # Prepare the rootfs for squashfs — exclude live/non-essential paths
    local sqroot="$TENEBRA_BUILD/squashfs-root"
    rm -rf "$sqroot"

    # Use rsync to create a clean copy without pseudo-fs
    rsync -aAX \
        --exclude='./dev/*' \
        --exclude='./proc/*' \
        --exclude='./sys/*' \
        --exclude='./run/*' \
        --exclude='./tmp/*' \
        --exclude='./mnt/*' \
        --exclude='./media/*' \
        --exclude='./snapshots/*' \
        --exclude='./home/*' \
        "$TENEBRA_ROOTFS/" "$sqroot/"

    # Create an empty /home and /snapshots for the live system
    mkdir -p "$sqroot/home" "$sqroot/snapshots"

    # Clean up build artifacts from the rootfs
    rm -rf "$sqroot/tmp"/* 2>/dev/null || true
    rm -rf "$sqroot/var/cache/apt/archives/*.deb" 2>/dev/null || true

    local sqfs="$STAGING/live/filesystem.squashfs"
    info "Compressing rootfs with $COMPRESSOR..."

    case "$COMPRESSOR" in
        gzip)
            mksquashfs "$sqroot" "$sqfs" \
                -comp gzip -b 1M -Xcompression-level 9 \
                -noappend -quiet \
                2>&1 | tail -1
            ;;
        xz)
            mksquashfs "$sqroot" "$sqfs" \
                -comp xz -b 1M -Xdict-size 1M -noappend -quiet \
                2>&1 | tail -1
            ;;
        zstd)
            mksquashfs "$sqroot" "$sqfs" \
                -comp zstd -b 1M -compression-level 19 -noappend -quiet \
                2>&1 | tail -1
            ;;
        *)
            mksquashfs "$sqroot" "$sqfs" \
                -comp xz -b 1M -noappend -quiet \
                2>&1 | tail -1
            ;;
    esac

    local sqsize
    sqsize="$(du -h "$sqfs" | cut -f1)"
    info "SquashFS created: $sqfs ($sqsize)"

    # Clean up the uncompressed copy
    rm -rf "$sqroot"
}

# ─── Create Live Boot Configuration ────────────────────────────────────────────
create_live_boot() {
    log "Creating live boot configuration"

    # /etc/live/boot.conf
    cat > "$STAGING/live/boot.conf" <<'BOOTCONF'
# TenebraOS live boot configuration
BOOT_AUFS=disabled
BOOT_MERGE=disabled
BOOT_PRIVATE_UNIONFS=disabled
BOOT_COW_NAME="tenebra-cow"
BOOT_COW_FS="ext4"
BOOT_COW_DEVICE=""
BOOT_COW_USER_NAME="user"
BOOT_CONFIGS=""
BOOT_CLONE=""
BOOT_IP=""
BOOT_MODULES=""
BOOT_NFS_SERVER=""
BOOT_NFS_DIR=""
BOOT_NFS_OPTS=""
BOOT_NBD_SERVER=""
BOOT_NBD_DIR=""
BOOT_NBD_OPTS=""
BOOT_USERNAME="user"
BOOT_USERFULLNAME="Live User"
BOOT_HOSTNAME="tenebra-live"
BOOT_LOCALES="en_US.UTF-8"
BOOT_KEYMAP="us"
BOOT_timezone="UTC"
BOOTCONF

    # Create a live-boot helper script
    cat > "$STAGING/boot/live-init" <<'LIVEINIT'
#!/bin/sh
# TenebraOS live init helper — used by live-boot
# This is sourced by live-boot's /init

export PATH=/sbin:/bin:/usr/sbin:/usr/bin

echo "TenebraOS live boot starting..."

# Set hostname
echo "tenebra-live" > /etc/hostname

# Create live user
useradd -m -s /bin/bash -G sudo,video,audio,plugdev user 2>/dev/null || true
echo "user:user" | chpasswd 2>/dev/null || true

# Auto-login on tty1
mkdir -p /etc/sv/sddm
if [ -f /usr/bin/sddm ]; then
    mkdir -p /etc/sv/sddm
fi
LIVEINIT
    chmod +x "$STAGING/boot/live-init"
}

# ─── Create ISOLINUX Configuration (BIOS Boot) ────────────────────────────────
create_isolinux() {
    log "Creating ISOLINUX configuration (BIOS boot)"

    cat > "$STAGING/boot/isolinux/isolinux.cfg" <<'ISOLINUXCFG'
# TenebraOS ISOLINUX configuration — BIOS boot

UI vesamenu.c32
MENU TITLE TenebraOS Boot Menu
MENU BACKGROUND /boot/grub/themes/tenebra/background.png
MENU COLOR TITLE    1;36;44
MENU COLOR SEL       5;31;44
MENU COLOR UNSel     37;44
MENU COLOR HELP      37;44
TIMEOUT 50

DEFAULT tenebra-live

LABEL tenebra-live
    MENU LABEL TenebraOS Live
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components quiet splash

LABEL tenebra-safe
    MENU LABEL TenebraOS Live (Safe Graphics)
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components nomodeset quiet

LABEL tenebra-ram
    MENU LABEL TenebraOS Live (RAM mode)
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components toram=filesystem.squashfs quiet splash

LABEL memtest
    MENU LABEL Memory Test
    LINUX /boot/memtest86+

LABEL hdd
    MENU LABEL Boot from first hard disk
    LOCALBOOT 0x80
ISOLINUXCFG

    # Copy ISOLINUX binaries if available (from syslinux-common)
    local isolinux_dir="/usr/lib/ISOLINUX"
    [ -d "$isolinux_dir" ] || isolinux_dir="/usr/share/syslinux"
    if [ -d "$isolinux_dir" ]; then
        for f in isolinux.bin ldlinux.c32 vesamenu.c32 libcom32.c32 \
                 libutil.c32 menu.c32 libui.c32; do
            cp "$isolinux_dir/$f" "$STAGING/boot/isolinux/" 2>/dev/null || true
        done
    fi
}

# ─── Create GRUB EFI Boot Configuration ────────────────────────────────────────
create_grub_efi() {
    log "Creating GRUB EFI configuration"

    cat > "$STAGING/boot/grub/grub.cfg" <<'GRUBCFG'
# TenebraOS GRUB EFI configuration

set default="0"
set timeout=5
set gfxmode=auto
set gfxpayload=keep

loadfont unicode

insmod efi_gop
insmod efi_uga
insmod gfxterm
insmod gfxmenu
insmod all_video
insmod jpeg
insmod png
insmod iso9660

terminal_output gfxterm

# Look for the squashfs
search --no-floppy --set=root --label TenebraOS

set theme=${prefix}/themes/tenebra/theme.txt
export theme

menuentry "TenebraOS Live" {
    linux /boot/vmlinuz boot=live components quiet splash
    initrd /boot/initrd.img
}

menuentry "TenebraOS Live (Safe Graphics)" {
    linux /boot/vmlinuz boot=live components nomodeset quiet
    initrd /boot/initrd.img
}

menuentry "TenebraOS Live (RAM mode)" {
    linux /boot/vmlinuz boot=live components toram=filesystem.squashfs quiet splash
    initrd /boot/initrd.img
}

menuentry "Boot from first hard disk" {
    set root=(hd0)
    chainloader +1
}
GRUBCFG

    # Create GRUB theme directory
    mkdir -p "$STAGING/boot/grub/themes/tenebra"

    # Use existing theme if available from the main project
    local theme_src="$(dirname "$0")/../config/includes.binary/boot/grub/themes/tenebra"
    if [ -d "$theme_src" ]; then
        cp "$theme_src"/* "$STAGING/boot/grub/themes/tenebra/" 2>/dev/null || true
    else
        # Create a minimal theme
        cat > "$STAGING/boot/grub/themes/tenebra/theme.txt" <<'THEME'
set color_normal=white/black
set color_highlight=red/black

desktop-color: "#1a1a2e"
title-text: "TenebraOS"
title-color: "#e94560"
title-font: "DejaVu Sans Bold 14"

+ image {
    top=0
    left=0
    width=100%
    height=100%
    file = "background.png"
}
THEME
    fi
}

# ─── Create GRUB EFI Image ────────────────────────────────────────────────────
create_grub_efi_image() {
    log "Creating GRUB EFI image"

    local efi_img="$STAGING/boot/grub/efi.img"
    local efi_img_mb=$((10))  # 10 MB EFI image

    # Create a FAT EFI image
    dd if=/dev/zero of="$efi_img" bs=1M count=$efi_img_mb 2>/dev/null
    mkfs.fat -F 32 -n EFI "$efi_img" 2>/dev/null

    # Mount and populate
    local efimnt="$TENEBRA_BUILD/efi-mnt"
    mkdir -p "$efimnt"
    mount -o loop "$efi_img" "$efimnt"

    # GRUB EFI structure
    mkdir -p "$efimnt/EFI/BOOT"
    mkdir -p "$efimnt/EFI/TENEBRA"
    mkdir -p "$efimnt/boot/grub"

    # Build GRUB EFI binary
    local grub_modules="part_gpt part_msdos fat iso9660 normal boot linux configfile loopback chain efifwsetup efi_gop efi_uga search_label search_fs_uuid search"
    local grub_core="$efimnt/EFI/BOOT/BOOTX64.EFI"

    grub-mkimage \
        --format=x86_64-efi \
        --output="$grub_core" \
        --prefix="/boot/grub" \
        --modules="$grub_modules" \
        --compression=xz \
        /usr/lib/grub/x86_64-efi/kernel.img 2>/dev/null || \
    grub-mkimage \
        --format=x86_64-efi \
        --output="$grub_core" \
        --prefix="(,gpt2)/boot/grub" \
        --modules="$grub_modules" \
        /usr/lib/grub/x86_64-efi/kernel.img 2>/dev/null || \
        warn "GRUB EFI image creation failed (non-fatal for BIOS boot)"

    # Copy GRUB modules
    if [ -d "/usr/lib/grub/x86_64-efi" ]; then
        cp -r /usr/lib/grub/x86_64-efi "$efimnt/boot/grub/" 2>/dev/null || true
    fi

    # Copy grub.cfg and theme
    cp "$STAGING/boot/grub/grub.cfg" "$efimnt/boot/grub/"
    cp -r "$STAGING/boot/grub/themes" "$efimnt/boot/grub/" 2>/dev/null || true

    # Copy kernel and initramfs into EFI image (for EFI-only boot)
    cp "$STAGING/boot/vmlinuz" "$efimnt/boot/" 2>/dev/null || true
    cp "$STAGING/boot/initrd.img" "$efimnt/boot/" 2>/dev/null || true

    umount "$efimnt"
    rmdir "$efimnt"

    info "GRUB EFI image: $efi_img ($(du -h "$efi_img" | cut -f1))"
}

# ─── Create BIOS Boot Image (El Torito) ───────────────────────────────────────
create_bios_boot() {
    log "Creating BIOS boot image (isolinux-based)"

    local boot_cat="$STAGING/boot/boot.cat"
    local boot_img="$STAGING/boot/isolinux/boot.img"

    # ISOLINUX boot image is used as the El Torito boot catalog
    # xorriso handles this automatically with -eltorito-boot

    info "BIOS boot: ISOLINUX at $STAGING/boot/isolinux/"
}

# ─── Assemble ISO ──────────────────────────────────────────────────────────────
assemble_iso() {
    log "Assembling ISO image"

    # Clean up the output path
    rm -f "$ISO_OUTPUT"
    mkdir -p "$(dirname "$ISO_OUTPUT")"

    # Calculate squashfs size for the initramfs
    local sqfs_size
    sqfs_size="$(stat -c%s "$STAGING/live/filesystem.squashfs" 2>/dev/null || echo 0)"
    local sqfs_mb=$((sqfs_size / 1048576 + 1))

    # Assemble the ISO with xorriso
    # -joliet:on       — Joliet extensions for Windows compat
    # -rational-rock   — Rock Ridge with long filenames
    # -boot_image      — BIOS + UEFI hybrid

    # Find isolinux MBR binary for hybrid boot (BIOS + UEFI)
    local isohdpfx=""
    for path in /usr/lib/ISOLINUX/isohdpfx.bin \
                /usr/lib/syslinux/isohdpfx.bin \
                /usr/share/syslinux/isohdpfx.bin; do
        [ -f "$path" ] && isohdpfx="$path" && break
    done

    local isolinux_bin=""
    for path in /usr/lib/ISOLINUX/isolinux.bin \
                /usr/lib/syslinux/isolinux.bin \
                /usr/share/syslinux/isolinux.bin; do
        [ -f "$path" ] && isolinux_bin="$path" && break
    done

    # Copy isolinux binaries to staging if found
    if [ -n "$isolinux_bin" ]; then
        local iso_dir
        iso_dir="$(dirname "$isolinux_bin")"
        cp "$iso_dir"/isolinux.bin "$STAGING/boot/isolinux/" 2>/dev/null || true
        for f in ldlinux.c32 vesamenu.c32 libcom32.c32 libutil.c32; do
            cp "$iso_dir/$f" "$STAGING/boot/isolinux/" 2>/dev/null || true
        done
    fi

    if [ -n "$isohdpfx" ] && [ -f "$STAGING/boot/isolinux/isolinux.bin" ]; then
        info "BIOS+UEFI hybrid: using isolinux MBR"
        xorriso -as mkisofs \
            -iso-level 3 \
            -full-iso9660-filenames \
            -volid "$DISC_ID" \
            -output "$ISO_OUTPUT" \
            -J -joliet-long \
            -rational-rock \
            -append_partition 2 0xef "$STAGING/boot/grub/efi.img" \
            -appended_part_as_gpt \
            -eltorito-boot boot/isolinux/isolinux.bin \
                -no-emul-boot \
                -boot-load-size 4 \
                -boot-info-table \
                --eltorito-catalog boot/boot.cat \
            -eltorito-alt-boot \
                -e boot/grub/efi.img \
                -no-emul-boot \
                -isohybrid-gpt-basdat \
            -isohybrid-mbr "$isohdpfx" \
            "$STAGING" 2>&1 | tee "$TENEBRA_BUILD/iso-build.log"
    else
        # UEFI-only mode (no BIOS isolinux available)
        warn "isolinux not found — BIOS boot will not be available"
        warn "  Install syslinux-common for BIOS boot support"
        xorriso -as mkisofs \
            -iso-level 3 \
            -full-iso9660-filenames \
            -volid "$DISC_ID" \
            -output "$ISO_OUTPUT" \
            -J -joliet-long \
            -rational-rock \
            -append_partition 2 0xef "$STAGING/boot/grub/efi.img" \
            -appended_part_as_gpt \
            -eltorito-boot boot/grub/efi.img \
                -no-emul-boot \
                -isohybrid-gpt-basdat \
            "$STAGING" 2>&1 | tee "$TENEBRA_BUILD/iso-build.log"
    fi

    [ -f "$ISO_OUTPUT" ] || err "ISO creation failed — check $TENEBRA_BUILD/iso-build.log"
}

# ─── Verify ISO ────────────────────────────────────────────────────────────────
verify_iso() {
    log "Verifying ISO"

    local iso_size
    iso_size="$(du -h "$ISO_OUTPUT" | cut -f1)"

    # Check ISO magic bytes
    if file "$ISO_OUTPUT" | grep -q "ISO 9660"; then
        info "ISO format: valid ISO 9660"
    else
        warn "ISO may not be valid — check file output"
    fi

    # Check for UEFI partition
    if fdisk -l "$ISO_OUTPUT" 2>/dev/null | grep -q "EFI System"; then
        info "UEFI boot: present"
    else
        warn "UEFI boot partition not detected"
    fi

    info "ISO size: $iso_size"
    info "ISO path: $ISO_OUTPUT"
}

# ─── Print Summary ─────────────────────────────────────────────────────────────
print_summary() {
    log "ISO Build Complete"
    echo ""
    echo "  ISO:       $ISO_OUTPUT"
    echo "  Size:      $(du -h "$ISO_OUTPUT" | cut -f1)"
    echo "  Label:     $ISO_LABEL"
    echo "  Compressor: $COMPRESSOR"
    echo ""
    echo "Boot options:"
    echo "  UEFI:  Boot from USB/DVD in UEFI mode"
    echo "  BIOS:  Boot from USB/DVD in legacy BIOS mode"
    echo "  USB:   sudo dd if=$ISO_OUTPUT of=/dev/sdX bs=4M status=progress && sync"
    echo ""
    echo "Test in QEMU:"
    echo "  sudo qemu-system-x86_64 \\"
    echo "    -m 4096 -smp 4 -enable-kvm -cpu host \\"
    echo "    -cdrom $ISO_OUTPUT \\"
    echo "    -vga virtio -display gtk"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    preflight
    prepare_staging
    create_squashfs
    create_live_boot
    create_isolinux
    create_grub_efi
    create_grub_efi_image
    create_bios_boot
    assemble_iso
    verify_iso
    print_summary
}

main "$@"
