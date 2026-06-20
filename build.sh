#!/bin/bash
set -eo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

build() {
    echo "==> Cleaning old build artifacts..."
    cd "$PROJECT_DIR"
    rm -f live-image-amd64.hybrid.iso
    sudo lb clean --purge 2>/dev/null || true

    echo "==> Configuring live-build..."
    sudo lb config \
        --apt-recommends true \
        --architecture amd64 \
        --archive-areas "main contrib non-free non-free-firmware" \
        --bootappend-live "boot=live components quiet splash username=user hostname=tenebra" \
        --debian-installer false \
        --distribution trixie \
        --iso-application "TenebraOS" \
        --iso-publisher "TenebraOS" \
        --iso-volume "TenebraOS" \
        --linux-flavours amd64 \
        --mode debian

    echo "==> Building ISO (this may take 20-40 minutes)..."
    sudo lb build 2>&1 | tee build.log

    if [ -f live-image-amd64.hybrid.iso ]; then
        echo "==> SUCCESS: live-image-amd64.hybrid.iso created"
        ls -lh live-image-amd64.hybrid.iso
    else
        echo "==> FAILED: ISO not created. Check build.log for errors."
        echo "    Last 50 lines of build.log:"
        tail -50 build.log 2>/dev/null || true
        exit 1
    fi
}

test_qemu() {
    ISO="$PROJECT_DIR/live-image-amd64.hybrid.iso"
    if [ ! -f "$ISO" ]; then
        echo "ISO not found at $ISO — run './build.sh build' first."
        exit 1
    fi
    echo "==> Booting TenebraOS in QEMU (UEFI)..."
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"
    if [ ! -f "$OVMF_CODE" ]; then
        OVMF_CODE="/usr/share/OVMF/OVMF_CODE.fd"
    fi
    if [ ! -f "$OVMF_VARS" ]; then
        OVMF_VARS=$(mktemp /tmp/OVMF_VARS.XXXX.fd)
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$OVMF_VARS" 2>/dev/null || \
        cp /usr/share/qemu/OVMF.fd "$OVMF_VARS" 2>/dev/null || true
    fi
    qemu-system-x86_64 \
        -m 4096 \
        -smp 4 \
        -enable-kvm \
        -cpu host \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$OVMF_VARS" \
        -cdrom "$ISO" \
        -vga virtio \
        -display gtk
}

flash() {
    echo "==> Available devices:"
    lsblk -d -o NAME,SIZE,MODEL | grep -v loop
    echo ""
    read -p "Enter device name (e.g. sdb): " DEV
    ISO="$PROJECT_DIR/live-image-amd64.hybrid.iso"
    if [ ! -f "$ISO" ]; then
        echo "ISO not found — run './build.sh build' first."
        exit 1
    fi
    echo "==> Writing ISO to /dev/$DEV..."
    sudo dd if="$ISO" of="/dev/$DEV" bs=4M status=progress
    sync
    echo "==> Done."
}

case "${1:-build}" in
    build) build ;;
    test-qemu) test_qemu ;;
    flash) flash ;;
    *)
        echo "Usage: $0 {build|test-qemu|flash}"
        echo "  build      Build the ISO (~20-40 min)"
        echo "  test-qemu  Boot the ISO in QEMU with UEFI"
        echo "  flash      Write ISO to USB (prompts for device)"
        exit 1
        ;;
esac
