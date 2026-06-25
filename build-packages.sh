#!/bin/bash
set -eo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$PROJECT_DIR/repo"
PACKAGES_DIR="$PROJECT_DIR/packages"
TARGET_DIR="$PROJECT_DIR/config/includes.chroot/opt/tenebra-packages"

mkdir -p "$REPO_DIR" "$TARGET_DIR"

echo "==> Building tenebra-wallpapers..."
dpkg-deb --root-owner-group --build "$PACKAGES_DIR/tenebra-wallpapers" "$REPO_DIR/tenebra-wallpapers_1.0_all.deb"

echo "==> Building tenebra-defaults..."
dpkg-deb --root-owner-group --build "$PACKAGES_DIR/tenebra-defaults" "$REPO_DIR/tenebra-defaults_1.0_all.deb"

echo "==> Building tenebra-grub-theme..."
dpkg-deb --root-owner-group --build "$PACKAGES_DIR/tenebra-grub-theme" "$REPO_DIR/tenebra-grub-theme_1.0_all.deb"

echo "==> Building tenebra-branding..."
dpkg-deb --root-owner-group --build "$PACKAGES_DIR/tenebra-branding" "$REPO_DIR/tenebra-branding_1.0_all.deb"

echo "==> Building tenebra-calamares..."
dpkg-deb --root-owner-group --build "$PACKAGES_DIR/tenebra-calamares" "$REPO_DIR/tenebra-calamares_1.0_all.deb"

echo "==> Copying packages to ISO includes..."
cp "$REPO_DIR"/*.deb "$TARGET_DIR/"

echo "==> Done. Packages built:"
ls -lh "$REPO_DIR"/*.deb
