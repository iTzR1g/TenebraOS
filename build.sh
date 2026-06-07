#!/bin/bash
# build.sh
# TenebraOS - Build and test helper
# Debian 13 (Trixie)
# Run from the TenebraOS/ root directory.

set -e
DISTRO_ROOT="$(cd "$(dirname "$0")" && pwd)"
LB_DIR="$DISTRO_ROOT/live-build"

usage() {
    echo $DISTRO_ROOT
    echo "TenebraOS build script (Debian 13 Trixie)"
    echo ""
    echo "Usage: $0 [init|build|clean|test-qemu|flash]"
    echo ""
    echo "  init       Initialize live-build config (run once)"
    echo "  build      Build the ISO (~20-40 min)"
    echo "  clean      Clean previous build artifacts"
    echo "  test-qemu  Boot the ISO in QEMU with UEFI"
    echo "  flash      Write ISO to USB (prompts for device)"
    exit 1
}

cmd_init() {
    echo "==> Initializing live-build for Trixie..."
    cd "$LB_DIR"
    bash init-lb.sh
}

cmd_clean() {
    cd "$LB_DIR"
    sudo lb clean
    echo "Build artifacts cleaned."
}

cmd_build() {
    echo "==> Building TenebraOS ISO (Debian 13 Trixie)..."
    cd "$LB_DIR"
    sudo lb build 2>&1 | tee build.log
    echo ""
    echo "=== Build complete ==="
    ls -lh live-image-amd64.hybrid.iso 2>/dev/null \
        || echo "ISO not found — check build.log for errors."
}

cmd_test_qemu() {
    ISO="$LB_DIR/live-image-amd64.hybrid.iso"
    if [ ! -f "$ISO" ]; then
        echo "ISO not found at $ISO — run './build.sh build' first."
        exit 1
    fi
    echo "==> Booting TenebraOS in QEMU (UEFI)..."
    qemu-system-x86_64 \
        -enable-kvm \
        -m 4096 \
        -cpu host \
        -drive if=pflash,format=raw,readonly=on,\
file=/usr/share/OVMF/OVMF_CODE.fd \
        -cdrom "$ISO" \
        -boot d
}

cmd_flash() {
    ISO="$LB_DIR/live-image-amd64.hybrid.iso"
    if [ ! -f "$ISO" ]; then
        echo "ISO not found — run './build.sh build' first."
        exit 1
    fi
    echo "Available block devices:"
    lsblk -d -o NAME,SIZE,MODEL
    echo ""
    read -rp "Enter target device (e.g. sdb — NO /dev/ prefix): " DEV
    echo ""
    echo "WARNING: This will DESTROY all data on /dev/$DEV"
    read -rp "Type YES to confirm: " CONFIRM
    if [ "$CONFIRM" = "YES" ]; then
        sudo dd if="$ISO" of="/dev/$DEV" bs=4M status=progress
        sync
        echo "Done. TenebraOS written to /dev/$DEV."
    else
        echo "Aborted."
    fi
}

case "${1:-}" in
    init)       cmd_init ;;
    build)      cmd_build ;;
    clean)      cmd_clean ;;
    test-qemu)  cmd_test_qemu ;;
    flash)      cmd_flash ;;
    *)          usage ;;
esac
