#!/bin/bash
# TenebraOS - Initialize live-build configuration
# Run this once from inside the live-build/ directory
# Base: Debian 13 (Trixie)

cd "$(dirname "$0")"

lb config \
    --distribution trixie \
    --archive-areas "main contrib non-free non-free-firmware" \
    --architectures amd64 \
    --binary-images iso-hybrid \
    --bootloader grub-efi \
    --debian-installer live \
    --memtest none
    
echo "live-build initialized for TenebraOS (Debian 13 Trixie)."
