#!/bin/bash
set -eo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$PROJECT_DIR/repo"
PACKAGES_DIR="$PROJECT_DIR/packages"
TARGET_DIR="$PROJECT_DIR/config/packages.chroot"

mkdir -p "$REPO_DIR" "$TARGET_DIR"

# dpkg-deb enforces maintainer-script permissions (>=0555, no world-writable);
# copies/rsync can mangle these, so normalize every time.
find "$PACKAGES_DIR" -type f -path '*/DEBIAN/*' ! -name control ! -name md5sums \
    -exec chmod 755 {} +
find "$PACKAGES_DIR" -type f \( -name control -o -name md5sums \) -path '*/DEBIAN/*' \
    -exec chmod 644 {} +

# The native zbra is built from source here rather than shipped prebuilt: the
# binary is an artifact, and building it against the build host's libc is what
# makes it runnable in the image that live-build is about to create. Those two
# are the same release in this project (excalibur), and the check below exists
# because if they ever diverge the ISO will build cleanly and then fail to
# launch /usr/local/bin/zbra on the target, which is the worst place to find out.
build_zbra() {
    local zdir="$PROJECT_DIR/zbra-c"
    local pkg="$PACKAGES_DIR/tenebra-zbra"
    local bin="$pkg/usr/local/bin/zbra"

    echo "==> Building tenebra-zbra..."
    [ -d "$zdir" ] || { echo "error: zbra-c not found at $zdir" >&2; return 1; }

    make -C "$zdir" zbra >/dev/null

    [ -x "$zdir/zbra" ] || { echo "error: zbra did not build" >&2; return 1; }

    mkdir -p "$(dirname "$bin")"
    install -m 755 "$zdir/zbra" "$bin"
    ln -sf zbra "$pkg/usr/local/bin/z"

    # Fails the build now rather than shipping an image whose package manager
    # cannot start.
    if ! "$bin" --version >/dev/null 2>&1; then
        echo "error: the built zbra does not run on this host" >&2
        echo "       this usually means its libc is not the one the ISO ships" >&2
        return 1
    fi

    local target host
    target=$(sed -n 's/.*--distribution[[:space:]]\+\([^[:space:]\\]*\).*/\1/p' \
                 "$PROJECT_DIR/auto/config" 2>/dev/null | head -1)
    host=$(. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-$ID}")

    if [ -n "$target" ] && [ -n "$host" ] && [ "$target" != "$host" ]; then
        echo "warning: building zbra for '$target' on '$host'." >&2
        echo "warning: it may reference a newer libc than the ISO provides." >&2
        echo "warning: build it in a $target container if the image boots to" >&2
        echo "warning: 'GLIBC_x.x not found' when running zbra." >&2
    fi

    dpkg-deb --root-owner-group --build "$pkg" "$REPO_DIR/tenebra-zbra_$(cat "$zdir/VERSION" 2>/dev/null || echo 0.1.0)_amd64.deb"
}

build_zbra

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
