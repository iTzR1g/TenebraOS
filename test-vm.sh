#!/bin/bash
# test-vm.sh — Boot the live-build chroot/ directly in QEMU, no ISO needed.
#
# Uses QEMU direct kernel boot (-kernel/-initrd), so no bootloader and no
# squashfs are involved: the chroot filesystem becomes the VM's root.
# Tests everything userspace: runit services, SDDM autologin, Calamares,
# apt sources — everything except live-boot's ISO-specific boot params.
#
# Requires: qemu-system-x86_64, rsync (see setup-build-deps.sh)
# Usage:    ./test-vm.sh [size-in-GiB]     # default 12G

set -euo pipefail
cd "$(dirname "$0")"

GIB="${1:-12}"
IMG="/tmp/tenebra-test.img"
MNT=/mnt/tenebra-test

[ -x chroot/bin/bash ] || { echo "ERROR: no chroot/ — run ./build.sh once first" >&2; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "ERROR: need qemu-system-x86_64" >&2; exit 1; }
command -v rsync >/dev/null || { echo "ERROR: need rsync" >&2; exit 1; }

# newest kernel/initrd pair in the chroot (skip debug variants)
K=$(ls chroot/boot/vmlinuz-*    2>/dev/null | grep -vi dbg | sort -V | tail -1 || true)
I=$(ls chroot/boot/initrd.img-* 2>/dev/null | grep -vi dbg | sort -V | tail -1 || true)
[ -n "$K" ] && [ -n "$I" ] || { echo "ERROR: no kernel/initrd in chroot/" >&2; exit 1; }
echo ">> kernel: $(basename "$K")"

# make sure no stale pseudo-fs mounts break the rsync
for m in dev/pts dev proc sys run; do
    mountpoint -q "chroot/$m" && sudo umount -lf "chroot/$m" || true
done

echo ">> creating $IMG"
rm -f "$IMG"
truncate -s "${GIB}G" "$IMG"

# No partition table: ext4 on the whole disk -> root=/dev/vda, one less
# thing to rescan (partprobe) or get wrong.
DEV=$(sudo losetup --find --show "$IMG")
cleanup() {
    sudo umount "$MNT" 2>/dev/null || true
    sudo losetup -d "$DEV" 2>/dev/null || true
}
trap cleanup EXIT

sudo mkfs.ext4 -F -q "$DEV"
sudo mkdir -p "$MNT"
sudo mount "$DEV" "$MNT"

echo ">> copying chroot -> VM disk (a few minutes)"
sudo rsync -aHA chroot/ "$MNT"/
printf '/dev/vda / ext4 errors=remount-ro 0 1\n' | sudo tee "$MNT/etc/fstab" >/dev/null

cleanup
trap - EXIT

echo ">> booting VM (close window to quit)"
exec qemu-system-x86_64 \
    -m 4096 -enable-kvm -cpu host -smp "$(nproc)" \
    -drive file="$IMG",format=raw,if=virtio \
    -kernel "$K" -initrd "$I" \
    -append "root=/dev/vda rw quiet splash" \
    -vga virtio -display gtk
