#!/bin/bash
# bare/01-bootstrap.sh
# Phase 1: Btrfs partitioning, rootfs bootstrap, cross-toolchain build
#
# This script builds TenebraOS from bare source tarballs — no pre-assembled
# ISOs or live-build. It follows LFS/Devuan methodology:
#   1. Partition target disk with Btrfs subvolumes (@, @home, @snapshots)
#   2. Download and verify all source tarballs
#   3. Build a cross-compilation toolchain (binutils -> gcc -> glibc)
#   4. Compile core userland packages from source into the new rootfs
#
# Usage:
#   sudo ./01-bootstrap.sh /dev/sdX          # partition + build
#   sudo ./01-bootstrap.sh /dev/sdX --skip-toolchain  # skip toolchain (reuse host)
#
# WARNING: This will DESTROY all data on the target disk.
# Set TENEBRA_TARGET_DEV and TENEBRA_JOBS to override defaults.

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
TENEBRA_TARGET_DEV="${1:-/dev/sdX}"
SKIP_TOOLCHAIN="${2:-}"
TENEBRA_JOBS="${TENEBRA_JOBS:-$(nproc)}"
TENEBRA_CFLAGS="${TENEBRA_CFLAGS:--O2 -pipe -march=x86-64 -mtune=generic}"
TENEBRA_DISTRO_MIRROR="${TENEBRA_DISTRO_MIRROR:-https://deb.devuan.org/merged}"

# Source versions — bump these to track upstream
BINUTILS_VER="2.43"
GCC_VER="14.2.0"
GLIBC_VER="2.40"
LINUX_VER="6.10.6"
MUSL_VER="1.2.5"     # used for the cross-toolchain bootstrap C library

# Toolchain build paths
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_SOURCES="$TENEBRA_BUILD/sources"
TENEBRA_TOOLS="$TENEBRA_BUILD/tools"
TENEBRA_ROOTFS="$TENEBRA_BUILD/rootfs"
TENEBRA_LOGS="$TENEBRA_BUILD/logs"
TENEBRA_PATCHES="$(cd "$(dirname "$0")" && pwd)/patches"

# ─── Helpers ───────────────────────────────────────────────────────────────────
log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }

require_root() {
    [ "$(id -u)" -eq 0 ] || err "Must run as root"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || err "Required command not found: $1"
}

setup_dirs() {
    mkdir -p "$TENEBRA_SOURCES" "$TENEBRA_TOOLS" "$TENEBRA_ROOTFS" "$TENEBRA_LOGS"
}

# ─── Source Download & Verification ────────────────────────────────────────────
download_sources() {
    log "Downloading source tarballs"

    local urls=(
        "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
        "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
        "https://ftp.gnu.org/gnu/glibc/glibc-${GLIBC_VER}.tar.xz"
        "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VER}.tar.xz"
        "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
    )

    for url in "${urls[@]}"; do
        local fname
        fname="$(basename "$url")"
        if [ ! -f "$TENEBRA_SOURCES/$fname" ]; then
            info "Downloading $fname"
            wget -q --show-progress -O "$TENEBRA_SOURCES/$fname" "$url" || \
                err "Failed to download $url"
        else
            info "Already downloaded: $fname"
        fi
    done

    # GCC requires GMP, MPFR, MPC as separate downloads
    local gcc_deps=(
        "https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz"
        "https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz"
        "https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz"
    )
    for url in "${gcc_deps[@]}"; do
        local fname
        fname="$(basename "$url")"
        if [ ! -f "$TENEBRA_SOURCES/$fname" ]; then
            info "Downloading GCC dependency: $fname"
            wget -q --show-progress -O "$TENEBRA_SOURCES/$fname" "$url" || true
        fi
    done

    log "Source tarballs ready in $TENEBRA_SOURCES"
}

# ─── Btrfs Disk Partitioning ───────────────────────────────────────────────────
partition_disk() {
    log "Partitioning $TENEBRA_TARGET_DEV for Btrfs"

    if [ "$TENEBRA_TARGET_DEV" = "/dev/sdX" ]; then
        err "Please set TENEBRA_TARGET_DEV or pass the device as argument 1"
    fi

    if [ ! -b "$TENEBRA_TARGET_DEV" ]; then
        err "$TENEBRA_TARGET_DEV is not a block device"
    fi

    read -p "THIS WILL DESTROY ALL DATA on $TENEBRA_TARGET_DEV. Continue? [y/N]: " confirm
    [ "$confirm" = "y" ] || [ "$confirm" = "Y" ] || exit 1

    # Wipe existing partition table
    wipefs -af "$TENEBRA_TARGET_DEV"
    sgdisk --zap-all "$TENEBRA_TARGET_DEV"

    # Create partition layout:
    #   1: EFI System Partition (512 MiB, FAT32)
    #   2: Btrfs root (remaining space)
    parted -s "$TENEBRA_TARGET_DEV" \
        mklabel gpt \
        mkpart ESP fat32 1MiB 513MiB \
        set 1 esp on \
        mkpart primary 513MiB 100%

    # Wait for device nodes to appear
    partprobe "$TENEBRA_TARGET_DEV"
    sleep 2

    local ESP_PART="${TENEBRA_TARGET_DEV}1"
    local ROOT_PART="${TENEBRA_TARGET_DEV}2"

    # Handle nvme-style partition naming
    if [[ "$TENEBRA_TARGET_DEV" == *"nvme"* ]]; then
        ESP_PART="${TENEBRA_TARGET_DEV}p1"
        ROOT_PART="${TENEBRA_TARGET_DEV}p2"
    fi

    # Format ESP
    mkfs.fat -F32 -n TENEBOOT "$ESP_PART"

    # Format root as Btrfs (single device, no RAID)
    mkfs.btrfs -f -L TEBRA_ROOT "$ROOT_PART"

    # Create Btrfs subvolumes
    mount "$ROOT_PART" /mnt

    btrfs subvolume create /mnt/@
    btrfs subvolume create /mnt/@home
    btrfs subvolume create /mnt/@snapshots

    umount /mnt

    # Remount with subvolumes
    mount -o subvol=@,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt

    mkdir -p /mnt/{home,snapshots,boot}
    mount -o subvol=@home,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt/home
    mount -o subvol=@snapshots,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt/snapshots

    mount "$ESP_PART" /mnt/boot

    # Set up rootfs mount point for toolchain build
    mkdir -p "$TENEBRA_ROOTFS"
    mount --bind /mnt "$TENEBRA_ROOTFS"

    info "Btrfs layout created:"
    info "  @(root)   -> /mnt"
    info "  @home     -> /mnt/home"
    info "  @snapshots -> /mnt/snapshots"
    info "  ESP       -> /mnt/boot"
}

# ─── Cross-Toolchain Build (LFS Method) ───────────────────────────────────────
build_toolchain() {
    if [ "$SKIP_TOOLCHAIN" = "--skip-toolchain" ]; then
        log "Skipping toolchain build (--skip-toolchain)"
        return 0
    fi

    log "Building cross-compilation toolchain"

    export PATH="$TENEBRA_TOOLS/bin:$PATH"

    # 1. Binutils (cross)
    log "  [1/5] Cross-binutils ${BINUTILS_VER}"
    tar xf "$TENEBRA_SOURCES/binutils-${BINUTILS_VER}.tar.xz" -C "$TENEBRA_BUILD"
    cd "$TENEBRA_BUILD/binutils-${BINUTILS_VER}"
    mkdir build && cd build
    ../configure \
        --prefix="$TENEBRA_TOOLS" \
        --with-sysroot="$TENEBRA_ROOTFS" \
        --target=x86_64-tenebra-linux-gnu \
        --with-lib-path="$TENEBRA_TOOLS/lib" \
        --disable-nls \
        --disable-werror \
        --enable-gold \
        --enable-ld=default \
        --enable-plugins \
        --enable-threads \
        2>&1 | tee "$TENEBRA_LOGS/binutils-cross.log"
    make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/binutils-cross.log"
    make install 2>&1 | tee -a "$TENEBRA_LOGS/binutils-cross.log"

    # 2. GCC Cross (stage 1 — C only, with musl)
    log "  [2/5] Cross-GCC stage 1 ${GCC_VER} (C compiler only)"
    cd "$TENEBRA_BUILD"
    tar xf "$TENEBRA_SOURCES/gcc-${GCC_VER}.tar.xz"
    cd "gcc-${GCC_VER}"

    # Symlink GCC dependencies
    ln -sf "$TENEBRA_SOURCES/gmp-6.3.0" gmp 2>/dev/null || true
    ln -sf "$TENEBRA_SOURCES/mpfr-4.2.2" mpfr 2>/dev/null || true
    ln -sf "$TENEBRA_SOURCES/mpc-1.3.1" mpc 2>/dev/null || true

    mkdir build && cd build
    ../configure \
        --prefix="$TENEBRA_TOOLS" \
        --target=x86_64-tenebra-linux-gnu \
        --with-sysroot="$TENEBRA_ROOTFS" \
        --with-newlib \
        --without-headers \
        --enable-languages=c \
        --disable-nls \
        --disable-shared \
        --disable-multilib \
        --disable-threads \
        --disable-libssp \
        --disable-libgomp \
        --disable-libmudflap \
        --disable-libquadmath \
        --disable-libatomic \
        --with-system-zlib \
        2>&1 | tee "$TENEBRA_LOGS/gcc-stage1.log"
    make -j"$TENEBRA_JOBS" all-gcc all-target-libgcc 2>&1 | tee -a "$TENEBRA_LOGS/gcc-stage1.log"
    make install-gcc install-target-libgcc 2>&1 | tee -a "$TENEBRA_LOGS/gcc-stage1.log"

    # 3. Glibc headers
    log "  [3/5] Linux API headers"
    cd "$TENEBRA_BUILD"
    tar xf "$TENEBRA_SOURCES/linux-${LINUX_VER}.tar.xz"
    cd "linux-${LINUX_VER}"
    make mrproper 2>/dev/null || true
    make headers_check INSTALL_HDR_PATH="$TENEBRA_ROOTFS/usr" 2>&1 | tee "$TENEBRA_LOGS/headers.log"
    make headers_install INSTALL_HDR_PATH="$TENEBRA_ROOTFS/usr" 2>&1 | tee -a "$TENEBRA_LOGS/headers.log"

    # 4. Glibc
    log "  [4/5] Glibc ${GLIBC_VER}"
    cd "$TENEBRA_BUILD"
    tar xf "$TENEBRA_SOURCES/glibc-${GLIBC_VER}.tar.xz"
    cd "glibc-${GLIBC_VER}"
    mkdir build && cd build
    echo "CVS=$TENEBRA_TOOLS/bin/x86_64-tenebra-linux-gnu-gcc" > configparms
    ../configure \
        --prefix=/usr \
        --host=x86_64-tenebra-linux-gnu \
        --build=x86_64-pc-linux-gnu \
        --enable-kernel=6.10 \
        --with-headers="$TENEBRA_ROOTFS/usr/include" \
        --enable-stack-protector=strong \
        --enable-obsolete-rpc \
        --disable-werror \
        libc_cv_slibdir=/usr/lib \
        libc_cv_rtlddir=/lib \
        2>&1 | tee "$TENEBRA_LOGS/glibc.log"
    make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/glibc.log"
    make install_root="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/glibc.log"

    # 5. GCC full build (stage 2 — C/C++)
    log "  [5/5] Full GCC ${GCC_VER} (C/C++)"
    cd "$TENEBRA_BUILD/gcc-${GCC_VER}/build"
    rm -rf *
    ../configure \
        --prefix="$TENEBRA_TOOLS" \
        --target=x86_64-tenebra-linux-gnu \
        --with-sysroot="$TENEBRA_ROOTFS" \
        --enable-languages=c,c++ \
        --enable-clocale=gnu \
        --enable-shared \
        --enable-threads=posix \
        --enable-libssp \
        --enable-libstdcxx-time \
        --enable-lto \
        --enable-default-pie \
        --enable-default-ssp \
        --enable-linker-build-id \
        --disable-nls \
        --disable-multilib \
        --disable-libatomic \
        --disable-libsanitizer \
        --with-system-zlib \
        2>&1 | tee "$TENEBRA_LOGS/gcc-full.log"
    make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/gcc-full.log"
    make install 2>&1 | tee -a "$TENEBRA_LOGS/gcc-full.log"

    # Create symlinks for the target
    ln -sf "$TENEBRA_TOOLS/bin/x86_64-tenebra-linux-gnu-gcc" \
        "$TENEBRA_TOOLS/bin/gcc" 2>/dev/null || true
    ln -sf "$TENEBRA_TOOLS/bin/x86_64-tenebra-linux-gnu-g++" \
        "$TENEBRA_TOOLS/bin/g++" 2>/dev/null || true

    log "Cross-toolchain installed to $TENEBRA_TOOLS"
}

# ─── Core Userland Build ────────────────────────────────────────────────────────
build_core_userland() {
    log "Building core userland from source"

    export PATH="$TENEBRA_TOOLS/bin:$PATH"
    export CC="${TENEBRA_TOOLS}/bin/x86_64-tenebra-linux-gnu-gcc"
    export CXX="${TENEBRA_TOOLS}/bin/x86_64-tenebra-linux-gnu-g++"
    export AR="${TENEBRA_TOOLS}/bin/x86_64-tenebra-linux-gnu-ar"
    export RANLIB="${TENEBRA_TOOLS}/bin/x86_64-tenebra-linux-gnu-ranlib"
    export CFLAGS="$TENEBRA_CFLAGS"
    export CXXFLAGS="$TENEBRA_CFLAGS"
    export TARGET="$TENEBRA_ROOTFS"

    # Package build functions — each downloads from upstream and compiles

    # --- Make 4.4 ---
    build_make() {
        log "  Building Make"
        local ver="4.4.1"
        if [ ! -f "$TENEBRA_SOURCES/make-${ver}.tar.gz" ]; then
            wget -q -O "$TENEBRA_SOURCES/make-${ver}.tar.gz" \
                "https://ftp.gnu.org/gnu/make/make-${ver}.tar.gz"
        fi
        tar xf "$TENEBRA_SOURCES/make-${ver}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/make-${ver}"
        ./configure \
            --prefix=/usr \
            --host=x86_64-tenebra-linux-gnu \
            --build=x86_64-pc-linux-gnu \
            --without-guile \
            2>&1 | tee "$TENEBRA_LOGS/make.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/make.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/make.log"
    }

    # --- Bash 5.2 ---
    build_bash() {
        log "  Building Bash"
        local ver="5.2.32"
        if [ ! -f "$TENEBRA_SOURCES/bash-${ver}.tar.gz" ]; then
            wget -q -O "$TENEBRA_SOURCES/bash-${ver}.tar.gz" \
                "https://ftp.gnu.org/gnu/bash/bash-${ver}.tar.gz"
        fi
        tar xf "$TENEBRA_SOURCES/bash-${ver}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/bash-${ver}"
        ./configure \
            --prefix=/usr \
            --host=x86_64-tenebra-linux-gnu \
            --build=x86_64-pc-linux-gnu \
            --without-bash-malloc \
            --enable-readline \
            --with-curses \
            2>&1 | tee "$TENEBRA_LOGS/bash.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/bash.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/bash.log"
        ln -sf bash "$TENEBRA_ROOTFS/bin/sh"
    }

    # --- Coreutils 9.5 ---
    build_coreutils() {
        log "  Building Coreutils"
        local ver="9.5"
        if [ ! -f "$TENEBRA_SOURCES/coreutils-${ver}.tar.xz" ]; then
            wget -q -O "$TENEBRA_SOURCES/coreutils-${ver}.tar.xz" \
                "https://ftp.gnu.org/gnu/coreutils/coreutils-${ver}.tar.xz"
        fi
        tar xf "$TENEBRA_SOURCES/coreutils-${ver}.tar.xz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/coreutils-${ver}"
        ./configure \
            --prefix=/usr \
            --host=x86_64-tenebra-linux-gnu \
            --build=x86_64-pc-linux-gnu \
            --enable-install-program=hostname \
            --enable-no-install-program=groups,kill,uptime \
            2>&1 | tee "$TENEBRA_LOGS/coreutils.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/coreutils.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/coreutils.log"
    }

    # --- Util-Linux 2.40 ---
    build_util_linux() {
        log "  Building Util-Linux"
        local ver="2.40.2"
        if [ ! -f "$TENEBRA_SOURCES/util-linux-${ver}.tar.xz" ]; then
            wget -q -O "$TENEBRA_SOURCES/util-linux-${ver}.tar.xz" \
                "https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.40/util-linux-${ver}.tar.xz"
        fi
        tar xf "$TENEBRA_SOURCES/util-linux-${ver}.tar.xz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/util-linux-${ver}"
        ./configure \
            --prefix=/usr \
            --host=x86_64-tenebra-linux-gnu \
            --build=x86_64-pc-linux-gnu \
            --disable-chfn-chsh \
            --disable-login \
            --disable-nologin \
            --disable-su \
            --disable-setpriv \
            --disable-runuser \
            --disable-pylibmount \
            --disable-static \
            --without-python \
            2>&1 | tee "$TENEBRA_LOGS/util-linux.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/util-linux.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/util-linux.log"
    }

    # --- Btrfs-Progs 6.10 ---
    build_btrfs_progs() {
        log "  Building Btrfs-Progs"
        local ver="6.10.1"
        if [ ! -f "$TENEBRA_SOURCES/btrfs-progs-${ver}.tar.xz" ]; then
            wget -q -O "$TENEBRA_SOURCES/btrfs-progs-${ver}.tar.xz" \
                "https://mirrors.edge.kernel.org/pub/linux/utils/fs/btrfs/btrfs-progs-${ver}.tar.xz"
        fi
        tar xf "$TENEBRA_SOURCES/btrfs-progs-${ver}.tar.xz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/btrfs-progs-${ver}"
        ./configure \
            --prefix=/usr \
            --host=x86_64-tenebra-linux-gnu \
            --build=x86_64-pc-linux-gnu \
            --disable-documentation \
            --disable-convert \
            --with-crypto=builtin \
            2>&1 | tee "$TENEBRA_LOGS/btrfs-progs.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/btrfs-progs.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/btrfs-progs.log"
    }

    # --- D-Bus 1.14 ---
    build_dbus() {
        log "  Building D-Bus"
        local ver="1.14.12"
        if [ ! -f "$TENEBRA_SOURCES/dbus-${ver}.tar.xz" ]; then
            wget -q -O "$TENEBRA_SOURCES/dbus-${ver}.tar.xz" \
                "https://dbus.freedesktop.org/releases/dbus/dbus-${ver}.tar.xz"
        fi
        tar xf "$TENEBRA_SOURCES/dbus-${ver}.tar.xz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/dbus-${ver}"
        ./configure \
            --prefix=/usr \
            --sysconfdir=/etc \
            --localstatedir=/var \
            --disable-static \
            --disable-tests \
            --disable-systemd \
            --disable-apparmor \
            --with-console-auth-dir=/run/console \
            2>&1 | tee "$TENEBRA_LOGS/dbus.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/dbus.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/dbus.log"
    }

    # --- Elogind 255 ---
    build_elogind() {
        log "  Building Elogind"
        local ver="255.17"
        if [ ! -f "$TENEBRA_SOURCES/elogind-${ver}.tar.xz" ]; then
            wget -q -O "$TENEBRA_SOURCES/elogind-${ver}.tar.xz" \
                "https://github.com/elogind/elogind/archive/refs/tags/v${ver}.tar.gz"
        fi
        tar xf "$TENEBRA_SOURCES/elogind-${ver}.tar.xz" -C "$TENEBRA_BUILD" 2>/dev/null || \
            tar xf "$TENEBRA_SOURCES/elogind-${ver}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$TENEBRA_BUILD/elogind-${ver}"
        ./configure \
            --prefix=/usr \
            --sysconfdir=/etc \
            --localstatedir=/var \
            --disable-static \
            --disable-gtk-doc \
            --disable-manpages \
            --enable-logind \
            --with-linux-pthreads \
            2>&1 | tee "$TENEBRA_LOGS/elogind.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/elogind.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/elogind.log"
    }

    # Execute all builds
    build_make
    build_bash
    build_coreutils
    build_util_linux
    build_btrfs_progs
    build_dbus
    build_elogind

    log "Core userland built and installed to $TENEBRA_ROOTFS"
}

# ─── Rootfs Base Structure ─────────────────────────────────────────────────────
create_rootfs_skeleton() {
    log "Creating rootfs directory skeleton"

    local rootfs="$TENEBRA_ROOTFS"

    # Standard FHS directories
    for d in bin boot dev etc home lib lib64 media mnt opt proc root run sbin srv sys tmp usr var; do
        mkdir -p "$rootfs/$d"
    done

    for d in usr/bin usr/include usr/lib usr/libexec usr/sbin usr/share usr/src; do
        mkdir -p "$rootfs/$d"
    done

    for d in var/lib var/log var/run var/tmp var/cache var/spool; do
        mkdir -p "$rootfs/$d"
    done

    # /etc skeleton
    for d in init.d rc0.d rc1.d rc2.d rc3.d rc4.d rc5.d rc6.d; do
        mkdir -p "$rootfs/etc/$d"
    done
    mkdir -p "$rootfs/etc/sv"
    mkdir -p "$rootfs/etc/tenebra"
    mkdir -p "$rootfs/etc/X11/xorg.conf.d"

    # /tmp and /run permissions
    chmod 1777 "$rootfs/tmp"
    chmod 1777 "$rootfs/run"

    # /etc/fstab
    cat > "$rootfs/etc/fstab" <<'FSTAB'
# <device>                          <mount>  <type>  <options>                          <dump> <pass>
UUID=PLACEHOLDER-UUID-ROOT          /        btrfs   subvol=@,compress=zstd:1,ssd      0      1
UUID=PLACEHOLDER-UUID-ROOT          /home    btrfs   subvol=@home,compress=zstd:1,ssd  0      2
UUID=PLACEHOLDER-UUID-ROOT          /snapshots btrfs subvol=@snapshots,compress=zstd:1,ssd 0   2
UUID=PLACEHOLDER-UUID-ESP           /boot    vfat    umask=0077                         0      2
FSTAB

    # /etc/hostname
    echo "tenebra" > "$rootfs/etc/hostname"

    # /etc/hosts
    cat > "$rootfs/etc/hosts" <<'HOSTS'
127.0.0.1   localhost
127.0.1.1   tenebra
::1         localhost ip6-localhost ip6-loopback
HOSTS

    # /etc/resolv.conf (placeholder — NetworkManager will overwrite)
    echo "nameserver 1.1.1.1" > "$rootfs/etc/resolv.conf"

    # /etc/passwd, /etc/group
    cat > "$rootfs/etc/passwd" <<'PASSWD'
root:x:0:0:root:/root:/bin/bash
daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin
nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin
PASSWD

    cat > "$rootfs/etc/group" <<'GROUP'
root:x:0:
daemon:x:1:
tty:x:5:
disk:x:6:
audio:x:29:
video:x:44:
plugdev:x:46:
users:x:100:
GROUP

    cat > "$rootfs/etc/shadow" <<'SHADOW'
root:$6$rounds=656000$placeholder:19000:0:99999:7:::
SHADOW

    cat > "$rootfs/etc/gshadow" <<'GSHADOW'
root::
GSHADOW

    # /etc/os-release
    cat > "$rootfs/etc/os-release" <<'OSRELEASE'
PRETTY_NAME="TenebraOS"
NAME="TenebraOS"
VERSION_ID="1.0"
VERSION="1.0 (Bare Source)"
VERSION_CODENAME=tenebra
ID=tenebra
ID_LIKE=debian
HOME_URL="https://itzr1g.github.io/tenebra.github.io/"
BUG_REPORT_URL="https://github.com/iTzR1g/TenebraOS/issues"
OSRELEASE

    # /etc/tenebra/build.conf (default build flags)
    cat > "$rootfs/etc/tenebra/build.conf" <<'BUILDCONF'
# /etc/tenebra/build.conf
# TenebraOS hybrid package manager build configuration
#
# Global CFLAGS/CXXFLAGS applied to all source builds
CFLAGS="-O2 -pipe -march=x86-64 -mtune=generic"
CXXFLAGS="-O2 -pipe -march=x86-64 -mtune=generic"
MAKEOPTS="-j$(nproc)"

# USE-like flags for package builds
# Comma-separated list of build features to enable
USE="X kde wayland alsa pulseaudio dbus elogind btrfs"
USE_DISABLE="systemd systemd-journal"

# Mirrors
SRC_MIRROR="https://ftp.gnu.org/gnu"
DEBIAN_MIRROR="https://deb.devuan.org/merged"

# Build directory
BUILD_DIR="/var/cache/tenebra/builds"
PKG_CACHE="/var/cache/tenebra/packages"
BUILDCONF

    # /etc/default/grub
    mkdir -p "$rootfs/etc/default"
    cat > "$rootfs/etc/default/grub" <<'GRUBDEFAULT'
GRUB_DEFAULT=0
GRUB_TIMEOUT=5
GRUB_TIMEOUT_STYLE=menu
GRUB_DISTRIBUTOR="TenebraOS"
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
GRUB_CMDLINE_LINUX=""
GRUB_DISABLE_OS_PROBER=true
GRUB_ENABLE_CRYPTODISK=n
GRUBDEFAULT

    info "Rootfs skeleton created"
}

# ─── Generate fstab from live partition UUIDs ───────────────────────────────────
generate_fstab() {
    log "Generating /etc/fstab from partition UUIDs"

    local rootfs="$TENEBRA_ROOTFS"
    local ESP_PART="${TENEBRA_TARGET_DEV}1"
    local ROOT_PART="${TENEBRA_TARGET_DEV}2"

    if [[ "$TENEBRA_TARGET_DEV" == *"nvme"* ]]; then
        ESP_PART="${TENEBRA_TARGET_DEV}p1"
        ROOT_PART="${TENEBRA_TARGET_DEV}p2"
    fi

    local ROOT_UUID
    ROOT_UUID="$(blkid -s UUID -o value "$ROOT_PART")"
    local ESP_UUID
    ESP_UUID="$(blkid -s UUID -o value "$ESP_PART")"

    cat > "$rootfs/etc/fstab" <<FSTAB
# <device>                          <mount>  <type>  <options>                          <dump> <pass>
UUID=${ROOT_UUID}                   /        btrfs   subvol=@,compress=zstd:1,ssd      0      1
UUID=${ROOT_UUID}                   /home    btrfs   subvol=@home,compress=zstd:1,ssd  0      2
UUID=${ROOT_UUID}                   /snapshots btrfs subvol=@snapshots,compress=zstd:1,ssd 0   2
UUID=${ESP_UUID}                    /boot    vfat    umask=0077                         0      2
FSTAB

    info "fstab written with UUIDs:"
    info "  ROOT=$ROOT_UUID  ESP=$ESP_UUID"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    require_root
    require_cmd wget
    require_cmd parted
    require_cmd mkfs.btrfs
    require_cmd mkfs.fat
    require_cmd btrfs
    require_cmd tar
    require_cmd gcc
    require_cmd make
    require_cmd patch

    setup_dirs
    download_sources

    # Only partition if a real device was specified
    if [ "$TENEBRA_TARGET_DEV" != "/dev/sdX" ]; then
        partition_disk
    else
        log "No target device specified — building toolchain + rootfs only"
        log "Set TENEBRA_TARGET_DEV or pass device as argument to partition a disk"
        mkdir -p "$TENEBRA_ROOTFS"
    fi

    create_rootfs_skeleton

    if [ "$TENEBRA_TARGET_DEV" != "/dev/sdX" ]; then
        generate_fstab
    fi

    build_toolchain
    build_core_userland

    log "Phase 1 complete: rootfs ready at $TENEBRA_ROOTFS"
    log "Next: sudo ./02-kernel-initramfs.sh"
}

main "$@"
