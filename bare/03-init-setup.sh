#!/bin/bash
# bare/03-init-setup.sh
# Phase 3: Init Freedom — install and configure a non-systemd init system
#
# This script sets up one of three init systems as PID 1:
#   - runit   (recommended: lightweight, reliable, easy to supervise)
#   - OpenRC  (feature-rich, Gentoo-derived, dependency-based)
#   - s6/s6-rc (skarnet's supervision suite, maximal control)
#
# It also installs service definitions for:
#   eudev (device manager), dbus, elogind, NetworkManager, SDDM, PipeWire
#
# Usage:
#   sudo ./03-init-setup.sh [runit|openrc|s6]
#   sudo ./03-init-setup.sh openrc    # force OpenRC

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_ROOTFS="$TENEBRA_BUILD/rootfs"
TENEBRA_LOGS="$TENEBRA_BUILD/logs"
TENEBRA_SOURCES="$TENEBRA_BUILD/sources"
TENEBRA_JOBS="${TENEBRA_JOBS:-$(nproc)}"
INIT_SYSTEM="${1:-runit}"

# Source versions
EDEVICED_VER="${EDEVICED_VER:-255}"
RUNIT_VER="${RUNIT_VER:-2.1.2}"
OPENRC_VER="${OPENRC_VER:-0.55}"
S6_VER="${S6_VER:-2.11.0.0}"
S6_RC_VER="${S6_RC_VER:-0.5.3.0}"
S6_LINUX_INIT_VER="${S6_LINUX_INIT_VER:-1.1.1.0}"

log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }

require_root() { [ "$(id -u)" -eq 0 ] || err "Must run as root"; }

# ─── Sysvinit Base (required by all inits for boot glue) ───────────────────────
install_sysvinit_base() {
    log "Installing sysvinit boot scripts (base layer)"

    local rootfs="$TENEBRA_ROOTFS"

    # /etc/init.d/rcS — the early boot script that runs before PID 1
    cat > "$rootfs/etc/init.d/rcS" <<'RCS'
#!/bin/sh
# /etc/init.d/rcS — System V early boot (runs before init system)
# Mounts, hostname, keymaps, udev, etc.

PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH

echo "TenebraOS booting..."

# Mount virtual filesystems (initramfs already mounted root)
mount -t proc     proc     /proc    2>/dev/null || true
mount -t sysfs    sysfs    /sys     2>/dev/null || true
mount -t devtmpfs devtmpfs /dev     2>/dev/null || true
mount -t tmpfs    tmpfs    /run     2>/dev/null || true
mkdir -p /dev/pts
mount -t devpts   devpts   /dev/pts 2>/dev/null || true

# Hostname
if [ -f /etc/hostname ]; then
    hostname "$(cat /etc/hostname)"
fi

# Keymaps
if [ -f /etc/default/keyboard ]; then
    . /etc/default/keyboard
    [ -n "${XKBLAYOUT:-}" ] && loadkeys "$XKBLAYOUT" 2>/dev/null || true
fi

# Check filesystems
echo "Checking filesystems..."
fsck -a 2>/dev/null || true

# Mount remaining filesystems from /etc/fstab
mount -a 2>/dev/null || true

# Device nodes
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/null ]    || mknod /dev/null    c 1 3
[ -c /dev/zero ]    || mknod /dev/zero    c 1 5
[ -c /dev/random ]  || mknod /dev/random  c 1 8
[ -c /dev/urandom ] || mknod /dev/urandom c 1 9
[ -c /dev/tty ]     || mknod /dev/tty     c 5 0
[ -c /dev/tty1 ]    || mknod /dev/tty1    c 4 1

echo "Early boot complete."
RCS
    chmod +x "$rootfs/etc/init.d/rcS"

    # /etc/rc.local — user-defined startup commands
    cat > "$rootfs/etc/rc.local" <<'RCLOCAL'
#!/bin/sh
# /etc/rc.local — TenebraOS user startup (runs after services)
exit 0
RCLOCAL
    chmod +x "$rootfs/etc/rc.local"
}

# ─── Eudev (Device Manager) ───────────────────────────────────────────────────
install_eudev() {
    log "Installing eudev from source"

    if [ ! -f "$TENEBRA_SOURCES/eudev-${EDEVICED_VER}.tar.gz" ]; then
        wget -q -O "$TENEBRA_SOURCES/eudev-${EDEVICED_VER}.tar.gz" \
            "https://github.com/eudev-project/eudev/archive/refs/tags/v${EDEVICED_VER}.tar.gz" || true
    fi

    if [ -f "$TENEBRA_SOURCES/eudev-${EDEVICED_VER}.tar.gz" ]; then
        local BD="$TENEBRA_BUILD/eudev-${EDEVICED_VER}"
        [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/eudev-${EDEVICED_VER}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$BD"
        ./configure \
            --prefix=/usr \
            --sysconfdir=/etc \
            --localstatedir=/var \
            --sbinddir=/sbin \
            --libexecdir=/lib/udev \
            --disable-manpages \
            --disable-gtk-doc \
            2>&1 | tee "$TENEBRA_LOGS/eudev.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/eudev.log"
        make DESTDIR="$TENEBRA_ROOTFS" install 2>&1 | tee -a "$TENEBRA_LOGS/eudev.log"
    else
        info "eudev source not available — installing udev from rootfs"
    fi

    # Create udevd service (run by the active init system)
    create_udev_service
}

create_udev_service() {
    local rootfs="$TENEBRA_ROOTFS"

    # udevadm settle helper
    cat > "$rootfs/usr/bin/udev-settle.sh" <<'UDEVSETTLE'
#!/bin/sh
# Wait for udev to finish processing events
udevadm settle --timeout=30 2>/dev/null || true
UDEVSETTLE
    chmod +x "$rootfs/usr/bin/udev-settle.sh"
}

# ─── Runit Setup ───────────────────────────────────────────────────────────────
setup_runit() {
    log "Setting up runit as PID 1"

    local rootfs="$TENEBRA_ROOTFS"

    # Install runit from source if not available
    if ! chroot "$rootfs" which runit-init >/dev/null 2>&1; then
        if [ ! -f "$TENEBRA_SOURCES/runit-${RUNIT_VER}.tar.gz" ]; then
            wget -q -O "$TENEBRA_SOURCES/runit-${RUNIT_VER}.tar.gz" \
                "https://smarden.org/runit/runit-${RUNIT_VER}.tar.gz" || true
        fi

        if [ -f "$TENEBRA_SOURCES/runit-${RUNIT_VER}.tar.gz" ]; then
            local BD="$TENEBRA_BUILD/runit-${RUNIT_VER}"
            [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/runit-${RUNIT_VER}.tar.gz" -C "$TENEBRA_BUILD"
            cd "$BD/command"
            sed -i 's/gcc /cc /' Makefile
            make -j"$TENEBRA_JOBS" 2>&1 | tee "$TENEBRA_LOGS/runit.log"
            make install prefix="$rootfs/usr" 2>&1 | tee -a "$TENEBRA_LOGS/runit.log"

            # Create runit-init symlink
            ln -sf "$rootfs/usr/bin/runit" "$rootfs/sbin/runit-init"
        fi
    fi

    # runit stage 1: system initialization
    mkdir -p "$rootfs/etc/runit"
    cat > "$rootfs/etc/runit/1" <<'RUNIT1'
#!/bin/sh
# runit stage 1: system initialization
# This runs once at boot and hands off to stage 2

export PATH=/sbin:/bin:/usr/sbin:/usr/bin

# Run early boot scripts
[ -x /etc/init.d/rcS ] && /etc/init.d/rcS

# udev coldplug (load modules for existing devices)
if [ -x /sbin/udevadm ]; then
    /sbin/udevadm trigger --type=subsystems --action=add
    /sbin/udevadm trigger --type=devices --action=add
    /sbin/udevadm settle --timeout=30
fi

# Mount pseudo-filesystems not yet mounted
mount -t proc     proc     /proc    2>/dev/null || true
mount -t sysfs    sysfs    /sys     2>/dev/null || true
mount -t devtmpfs devtmpfs /dev     2>/dev/null || true
mount -t tmpfs    tmpfs    /run     2>/dev/null || true

exit 0
RUNIT1
    chmod +x "$rootfs/etc/runit/1"

    # runit stage 2: supervised services
    cat > "$rootfs/etc/runit/2" <<'RUNIT2'
#!/bin/sh
# runit stage 2: supervised services
# Starts runsvdir which supervises all services in /run/runit/service

export PATH=/sbin:/bin:/usr/sbin:/usr/bin

exec /usr/bin/runsvdir -P /run/runit/service \
    'log: s2: 100,,t;/run/runit/service '
RUNIT2
    chmod +x "$rootfs/etc/runit/2"

    # runit stage 3: shutdown
    cat > "$rootfs/etc/runit/3" <<'RUNIT3'
#!/bin/sh
# runit stage 3: shutdown

# Kill supervised services
for svc in /run/runit/service/*; do
    [ -d "$svc" ] && sv force-stop "$svc" 2>/dev/null || true
done

# Sync filesystems
sync

# Power off / reboot / halt based on command line
case "$(cat /proc/cmdline)" in
    *reboot*)  reboot -f ;;
    *halt*)    halt -f ;;
    *)         poweroff -f ;;
esac
RUNIT3
    chmod +x "$rootfs/etc/runit/3"

    # Service directories
    mkdir -p "$rootfs/etc/runit/runsvdir/default"
    mkdir -p "$rootfs/run/runit/service"

    # Symlink default services
    ln -sfn /etc/runit/runsvdir/default "$rootfs/run/runit/service"

    # Create runit service definitions for essential services
    create_runit_services

    # /sbin/init -> runit-init
    if [ -f "$rootfs/sbin/runit-init" ] || [ -f "$rootfs/usr/bin/runit" ]; then
        ln -sf runit-init "$rootfs/sbin/init" 2>/dev/null || \
        ln -sf ../usr/bin/runit "$rootfs/sbin/init" 2>/dev/null || true
    fi

    log "runit installed as PID 1"
}

create_runit_services() {
    local svdir="$TENEBRA_ROOTFS/etc/sv"
    mkdir -p "$svdir"

    # --- eudev/udevd ---
    mkdir -p "$svdir/udevd"
    cat > "$svdir/udevd/run" <<'SVC'
#!/bin/sh
exec /sbin/udevd --daemon
SVC
    chmod +x "$svdir/udevd/run"

    cat > "$svdir/udevd/finish" <<'FIN'
#!/bin/sh
/sbin/udevadm control --stop-exec-queue 2>/dev/null || true
/sbin/udevadm info --cleanup-db 2>/dev/null || true
FIN
    chmod +x "$svdir/udevd/finish"

    # --- dbus ---
    mkdir -p "$svdir/dbus"
    cat > "$svdir/dbus/run" <<'SVC'
#!/bin/sh
mkdir -p /run/dbus
exec /usr/bin/dbus-daemon --system --nofork
SVC
    chmod +x "$svdir/dbus/run"

    # --- elogind ---
    mkdir -p "$svdir/elogind"
    cat > "$svdir/elogind/run" <<'SVC'
#!/bin/sh
exec /usr/bin/elogind --daemon
SVC
    chmod +x "$svdir/elogind/run"

    # --- NetworkManager ---
    mkdir -p "$svdir/NetworkManager"
    cat > "$svdir/NetworkManager/run" <<'SVC'
#!/bin/sh
while [ ! -S /run/dbus/system_bus_socket ]; do
    sleep 0.1
done
exec /usr/sbin/NetworkManager --no-daemon
SVC
    chmod +x "$svdir/NetworkManager/run"

    # --- SDDM (display manager) ---
    mkdir -p "$svdir/sddm"
    cat > "$svdir/sddm/run" <<'SVC'
#!/bin/sh
while [ ! -S /run/dbus/system_bus_socket ]; do
    sleep 0.1
done

# Resolve session type
SESSION=""
for s in plasmax11 plasma; do
    if [ -f "/usr/share/xsessions/$s.desktop" ]; then
        SESSION="$s"
        break
    fi
done
[ -z "$SESSION" ] && SESSION=$(ls /usr/share/xsessions/*.desktop 2>/dev/null | head -1 | xargs -r basename .desktop 2>/dev/null)
[ -n "$SESSION" ] && printf '[Autologin]\nUser=root\nSession=%s\nRelogin=false\n' "$SESSION" \
    > /etc/sddm.conf.d/autologin.conf 2>/dev/null || true

exec /usr/bin/sddm
SVC
    chmod +x "$svdir/sddm/run"

    # --- PipeWire (audio — runs per-user, but we create a system service template) ---
    mkdir -p "$svdir/pipewire"
    cat > "$svdir/pipewire/run" <<'SVC'
#!/bin/sh
# PipeWire system service — runs pipewire as a dedicated user
# Users should also start their own pipewire session via their DE
exec /usr/bin/pipewire
SVC
    chmod +x "$svdir/pipewire/run"

    # --- Dhcpcd (fallback DHCP if NM is not used) ---
    mkdir -p "$svdir/dhcpcd"
    cat > "$svdir/dhcpcd/run" <<'SVC'
#!/bin/sh
exec /usr/sbin/dhcpcd -b
SVC
    chmod +x "$svdir/dhcpcd/run"

    # Enable essential services (symlink into runsvdir)
    for svc in udevd dbus elogind NetworkManager; do
        ln -sf "$svdir/$svc" "$TENEBRA_ROOTFS/etc/runit/runsvdir/default/$svc"
    done

    # SDDM only enabled in graphical mode — user can enable manually:
    #   ln -s /etc/sv/sddm /etc/runit/runsvdir/default/
    info "runit services created in $svdir"
    info "Enabled by default: udevd, dbus, elogind, NetworkManager"
    info "To enable SDDM:  ln -s /etc/sv/sddm /etc/runit/runsvdir/default/"
    info "To enable PipeWire: ln -s /etc/sv/pipewire /etc/runit/runsvdir/default/"
}

# ─── OpenRC Setup ──────────────────────────────────────────────────────────────
setup_openrc() {
    log "Setting up OpenRC as PID 1"

    local rootfs="$TENEBRA_ROOTFS"

    # Build OpenRC from source
    if [ ! -f "$TENEBRA_SOURCES/openrc-${OPENRC_VER}.tar.xz" ]; then
        wget -q -O "$TENEBRA_SOURCES/openrc-${OPENRC_VER}.tar.xz" \
            "https://github.com/OpenRC/openrc/archive/refs/tags/${OPENRC_VER}.tar.xz" || true
    fi

    if [ -f "$TENEBRA_SOURCES/openrc-${OPENRC_VER}.tar.xz" ]; then
        local BD="$TENEBRA_BUILD/openrc-${OPENRC_VER}"
        [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/openrc-${OPENRC_VER}.tar.xz" -C "$TENEBRA_BUILD"
        cd "$BD"
        ./configure \
            --prefix=/usr \
            --sysconfdir=/etc \
            --libdir=/usr/lib/openrc \
            --sbindir=/sbin \
            --mandir=/usr/share/man \
            --enable-linux-mtab \
            --disable-examples \
            --with-dns=systemd-resolved \
            2>&1 | tee "$TENEBRA_LOGS/openrc.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/openrc.log"
        make DESTDIR="$rootfs" install 2>&1 | tee -a "$TENEBRA_LOGS/openrc.log"
    fi

    # OpenRC init script: /sbin/init (uses sysvinit as PID 1, OpenRC as init manager)
    # OpenRC works as an init *manager* alongside a PID 1 like sysvinit or s6-linux-init.
    # For pure OpenRC-as-PID-1, we use OpenRC's built-in sysvinit compatibility.

    # Create OpenRC service definitions
    create_openrc_services

    # /etc/inittab for OpenRC
    cat > "$rootfs/etc/inittab" <<'INITTAB'
# TenebraOS OpenRC inittab
# System initialization
si::sysinit:/etc/init.d/rcS

# TTYs
1:2345:respawn:/sbin/getty 38400 tty1
2:2345:respawn:/sbin/getty 38400 tty2
3:2345:respawn:/sbin/getty 38400 tty3
4:2345:respawn:/sbin/getty 38400 tty4
5:2345:respawn:/sbin/getty 38400 tty5
6:2345:respawn:/sbin/getty 38400 tty6

# Serial console (optional)
#S0:2345:respawn:/sbin/getty -L ttyS0 115200 vt100

# Ctrl-Alt-Delete handler
ca::ctrlaltdel:/sbin/reboot

# Shutdown/reboot
l0:0:wait:/etc/rc.d/rc 0
l1:1:wait:/etc/rc.d/rc 1
l2:2:wait:/etc/rc.d/rc 2
l3:3:wait:/etc/rc.d/rc 3
l4:4:wait:/etc/rc.d/rc 4
l5:5:wait:/etc/rc.d/rc 5
l6:6:wait:/etc/rc.d/rc 6
INITTAB

    # rcS script for OpenRC
    mkdir -p "$rootfs/etc/rc.d"
    cat > "$rootfs/etc/rc.d/rcS" <<'RCS'
#!/bin/sh
# OpenRC early boot
export PATH=/sbin:/bin:/usr/sbin:/usr/bin

# Mount pseudo-filesystems
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts

# Hostname
[ -f /etc/hostname ] && hostname "$(cat /etc/hostname)"

# Device nodes
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/null ]    || mknod /dev/null    c 1 3
[ -c /dev/urandom ] || mknod /dev/urandom c 1 9

# udev coldplug
if [ -x /sbin/udevadm ]; then
    /sbin/udevd --daemon
    /sbin/udevadm trigger --type=subsystems --action=add
    /sbin/udevadm trigger --type=devices --action=add
    /sbin/udevadm settle --timeout=30
fi

# Mount filesystems
mount -a

# Start system services via OpenRC
if [ -x /sbin/openrc ]; then
    openrc sysinit
    openrc boot
    openrc default
fi
RCS
    chmod +x "$rootfs/etc/rc.d/rcS"
    ln -sf /etc/rc.d/rcS "$rootfs/etc/init.d/rcS"

    # /sbin/init for OpenRC
    ln -sf /sbin/init.openrc "$rootfs/sbin/init" 2>/dev/null || true

    log "OpenRC installed"
}

create_openrc_services() {
    local rootfs="$TENEBRA_ROOTFS"
    local svdir="$rootfs/etc/init.d"

    # --- udevd ---
    cat > "$svdir/udevd" <<'SVC'
#!/sbin/openrc-run
# eudev device manager

description="eudev device manager"

depend() {
    need localmount
    before dbus elogind NetworkManager
    keyword -stop
}

start() {
    ebegin "Starting eudev"
    /sbin/udevd --daemon
    /sbin/udevadm trigger --type=subsystems --action=add
    /sbin/udevadm trigger --type=devices --action=add
    /sbin/udevadm settle --timeout=30
    eend $?
}

stop() {
    ebegin "Stopping eudev"
    /sbin/udevadm control --stop-exec-queue
    /sbin/udevadm info --cleanup-db
    eend $?
}
SVC
    chmod +x "$svdir/udevd"

    # --- dbus ---
    cat > "$svdir/dbus" <<'SVC'
#!/sbin/openrc-run
# D-Bus system message bus

description="D-Bus system message bus"

depend() {
    use logger
    need udevd
    before NetworkManager sddm elogind
    keyword -stop
}

start() {
    ebegin "Starting D-Bus"
    mkdir -p /run/dbus
    /usr/bin/dbus-daemon --system --nofork &
    eend $?
}

stop() {
    ebegin "Stopping D-Bus"
    killall dbus-daemon 2>/dev/null
    rm -f /run/dbus/pid
    eend $?
}
SVC
    chmod +x "$svdir/dbus"

    # --- elogind ---
    cat > "$svdir/elogind" <<'SVC'
#!/sbin/openrc-run
# elogind (logind without systemd)

description="elogind session manager"

depend() {
    need dbus
    after dbus
    keyword -stop
}

start() {
    ebegin "Starting elogind"
    /usr/bin/elogind --daemon
    eend $?
}

stop() {
    ebegin "Stopping elogind"
    killall elogind 2>/dev/null
    eend $?
}
SVC
    chmod +x "$svdir/elogind"

    # --- NetworkManager ---
    cat > "$svdir/NetworkManager" <<'SVC'
#!/sbin/openrc-run
# NetworkManager

description="NetworkManager network management daemon"

depend() {
    need dbus elogind
    after dbus elogind
}

start() {
    ebegin "Starting NetworkManager"
    /usr/sbin/NetworkManager --no-daemon &
    eend $?
}

stop() {
    ebegin "Stopping NetworkManager"
    killall NetworkManager 2>/dev/null
    eend $?
}
SVC
    chmod +x "$svdir/NetworkManager"

    # --- SDDM ---
    cat > "$svdir/sddm" <<'SVC'
#!/sbin/openrc-run
# SDDM display manager

description="SDDM display manager"

depend() {
    need dbus elogind
    after elogind
    keyword -stop
}

start() {
    ebegin "Starting SDDM"
    /usr/bin/sddm &
    eend $?
}

stop() {
    ebegin "Stopping SDDM"
    killall sddm 2>/dev/null
    eend $?
}
SVC
    chmod +x "$svdir/sddm"

    # Enable essential services
    for svc in udevd dbus elogind NetworkManager; do
        rc-update add "$svc" default 2>/dev/null || true
    done

    info "OpenRC services created in $svdir"
}

# ─── S6 Setup ──────────────────────────────────────────────────────────────────
setup_s6() {
    log "Setting up s6/s6-rc as PID 1"

    local rootfs="$TENEBRA_ROOTFS"

    # Build s6 from source
    if [ ! -f "$TENEBRA_SOURCES/s6-${S6_VER}.tar.gz" ]; then
        wget -q -O "$TENEBRA_SOURCES/s6-${S6_VER}.tar.gz" \
            "https://skarnet.org/software/s6/s6-${S6_VER}.tar.gz" || true
    fi

    if [ -f "$TENEBRA_SOURCES/s6-${S6_VER}.tar.gz" ]; then
        local BD="$TENEBRA_BUILD/s6-${S6_VER}"
        [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/s6-${S6_VER}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$BD"
        ./configure \
            --prefix="$rootfs/usr" \
            --libdir="$rootfs/usr/lib" \
            --bindir="$rootfs/usr/bin" \
            --sbindir="$rootfs/usr/sbin" \
            --enable-absolute-paths \
            2>&1 | tee "$TENEBRA_LOGS/s6.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/s6.log"
        make install 2>&1 | tee -a "$TENEBRA_LOGS/s6.log"
    fi

    # s6-linux-init (the actual PID 1)
    if [ ! -f "$TENEBRA_SOURCES/s6-linux-init-${S6_LINUX_INIT_VER}.tar.gz" ]; then
        wget -q -O "$TENEBRA_SOURCES/s6-linux-init-${S6_LINUX_INIT_VER}.tar.gz" \
            "https://skarnet.org/software/s6-linux-init/s6-linux-init-${S6_LINUX_INIT_VER}.tar.gz" || true
    fi

    if [ -f "$TENEBRA_SOURCES/s6-linux-init-${S6_LINUX_INIT_VER}.tar.gz" ]; then
        local BD="$TENEBRA_BUILD/s6-linux-init-${S6_LINUX_INIT_VER}"
        [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/s6-linux-init-${S6_LINUX_INIT_VER}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$BD"
        ./configure \
            --prefix="$rootfs/usr" \
            --libdir="$rootfs/usr/lib" \
            --bindir="$rootfs/usr/bin" \
            --sbindir="$rootfs/usr/sbin" \
            --enable-absolute-paths \
            --with-rcenv="PATH=/usr/bin:/bin:/usr/sbin:/sbin" \
            2>&1 | tee "$TENEBRA_LOGS/s6-linux-init.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/s6-linux-init.log"
        make install 2>&1 | tee -a "$TENEBRA_LOGS/s6-linux-init.log"
    fi

    # s6-rc (service manager)
    if [ ! -f "$TENEBRA_SOURCES/s6-rc-${S6_RC_VER}.tar.gz" ]; then
        wget -q -O "$TENEBRA_SOURCES/s6-rc-${S6_RC_VER}.tar.gz" \
            "https://skarnet.org/software/s6-rc/s6-rc-${S6_RC_VER}.tar.gz" || true
    fi

    if [ -f "$TENEBRA_SOURCES/s6-rc-${S6_RC_VER}.tar.gz" ]; then
        local BD="$TENEBRA_BUILD/s6-rc-${S6_RC_VER}"
        [ -d "$BD" ] || tar xf "$TENEBRA_SOURCES/s6-rc-${S6_RC_VER}.tar.gz" -C "$TENEBRA_BUILD"
        cd "$BD"
        ./configure \
            --prefix="$rootfs/usr" \
            --libdir="$rootfs/usr/lib" \
            --bindir="$rootfs/usr/bin" \
            --sbindir="$rootfs/usr/sbin" \
            --enable-absolute-paths \
            2>&1 | tee "$TENEBRA_LOGS/s6-rc.log"
        make -j"$TENEBRA_JOBS" 2>&1 | tee -a "$TENEBRA_LOGS/s6-rc.log"
        make install 2>&1 | tee -a "$TENEBRA_LOGS/s6-rc.log"
    fi

    # Create s6 service definitions
    create_s6_services

    # s6-linux-init stage 1 script
    mkdir -p "$rootfs/etc/s6-linux-init"
    cat > "$rootfs/etc/s6-linux-init/rc.init" <<'S6INIT'
#!/bin/sh
# s6-linux-init rc.init — early boot before services

export PATH=/usr/bin:/bin:/usr/sbin:/sbin

# Mount pseudo-filesystems
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /run
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts

# Hostname
[ -f /etc/hostname ] && hostname "$(cat /etc/hostname)"

# Device nodes
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/null ]    || mknod /dev/null    c 1 3
[ -c /dev/urandom ] || mknod /dev/urandom c 1 9

# udev coldplug
if [ -x /usr/sbin/udevd ]; then
    /usr/sbin/udevd --daemon
    /usr/sbin/udevadm trigger --type=subsystems --action=add
    /usr/sbin/udevadm trigger --type=devices --action=add
    /usr/sbin/udevadm settle --timeout=30
fi

# Mount filesystems
mount -a 2>/dev/null || true

# Hand off to s6-rc
exec /usr/bin/s6-rc -u change default
S6INIT
    chmod +x "$rootfs/etc/s6-linux-init/rc.init"

    # s6-linux-init stage 3 (shutdown)
    cat > "$rootfs/etc/s6-linux-init/rc.shutdown" <<'S6SHUTDOWN'
#!/bin/sh
# s6-linux-init shutdown
sync
for svc in /run/service/*; do
    [ -d "$svc" ] && /usr/bin/s6-svc -Od "$svc" 2>/dev/null || true
done
sleep 1
umount -a 2>/dev/null || true
poweroff -f
S6SHUTDOWN
    chmod +x "$rootfs/etc/s6-linux-init/rc.shutdown"

    # /sbin/init -> s6-linux-init
    ln -sf /usr/bin/s6-linux-init "$rootfs/sbin/init" 2>/dev/null || true

    log "s6/s6-rc installed as PID 1"
}

create_s6_services() {
    local rootfs="$TENEBRA_ROOTFS"
    local svdir="$rootfs/etc/s6-rc-sources"

    mkdir -p "$svdir"/{udevd,dbus,elogind,NetworkManager,sddm,pipewire}

    # --- udevd ---
    cat > "$svdir/udevd/type" <<'TYPE'
oneshot
TYPE
    cat > "$svdir/udevd/up" <<'UP'
#!/bin/sh
exec /usr/sbin/udevd --daemon
/usr/sbin/udevadm trigger --type=subsystems --action=add
/usr/sbin/udevadm trigger --type=devices --action=add
/usr/sbin/udevadm settle --timeout=30
UP
    chmod +x "$svdir/udevd/up"

    # --- dbus ---
    cat > "$svdir/dbus/type" <<'TYPE'
longrun
TYPE
    cat > "$svdir/dbus/run" <<'RUN'
#!/bin/sh
mkdir -p /run/dbus
exec /usr/bin/dbus-daemon --system --nofork
RUN
    chmod +x "$svdir/dbus/run"

    # Dependencies
    mkdir -p "$svdir/dbus/dependencies.d"
    touch "$svdir/dbus/dependencies.d/udevd"

    # --- elogind ---
    cat > "$svdir/elogind/type" <<'TYPE'
longrun
TYPE
    cat > "$svdir/elogind/run" <<'RUN'
#!/bin/sh
exec /usr/bin/elogind --daemon
RUN
    chmod +x "$svdir/elogind/run"

    mkdir -p "$svdir/elogind/dependencies.d"
    touch "$svdir/elogind/dependencies.d/dbus"

    # --- NetworkManager ---
    cat > "$svdir/NetworkManager/type" <<'TYPE'
longrun
TYPE
    cat > "$svdir/NetworkManager/run" <<'RUN'
#!/bin/sh
exec /usr/sbin/NetworkManager --no-daemon
RUN
    chmod +x "$svdir/NetworkManager/run"

    mkdir -p "$svdir/NetworkManager/dependencies.d"
    touch "$svdir/NetworkManager/dependencies.d/dbus"
    touch "$svdir/NetworkManager/dependencies.d/elogind"

    # --- SDDM ---
    cat > "$svdir/sddm/type" <<'TYPE'
longrun
TYPE
    cat > "$svdir/sddm/run" <<'RUN'
#!/bin/sh
exec /usr/bin/sddm
RUN
    chmod +x "$svdir/sddm/run"

    mkdir -p "$svdir/sddm/dependencies.d"
    touch "$svdir/sddm/dependencies.d/dbus"
    touch "$svdir/sddm/dependencies.d/elogind"

    # Default bundle
    mkdir -p "$svdir/default/contents.d"
    for svc in udevd dbus elogind NetworkManager; do
        touch "$svdir/default/contents.d/$svc"
    done

    info "s6-rc service definitions created in $svdir"
}

# ─── Install Init System Switcher ──────────────────────────────────────────────
install_init_switcher() {
    log "Installing init-switcher utility"

    local rootfs="$TENEBRA_ROOTFS"

    cat > "$rootfs/usr/local/bin/tenebra-init-switch" <<'INITSWITCH'
#!/bin/bash
# tenebra-init-switch — Switch between init systems on TenebraOS
#
# Usage:
#   tenebra-init-switch status        # show current init system
#   tenebra-init-switch list          # list available init systems
#   tenebra-init-switch switch <name> # switch init system (requires reboot)
#
# WARNING: Switching init systems requires a reboot. Services from the
# previous init system will be stopped and the new one will take over.

set -euo pipefail

ROOTFS="${TENEBRA_ROOTFS:-/}"
SERVICE_DIR=""

case "$1" in
    status)
        echo "Current init system:"
        if [ -x /sbin/runit-init ] && readlink /sbin/init 2>/dev/null | grep -q runit; then
            echo "  runit"
        elif [ -f /sbin/openrc ]; then
            echo "  openrc"
        elif [ -x /usr/bin/s6-linux-init ] && readlink /sbin/init 2>/dev/null | grep -q s6; then
            echo "  s6"
        else
            echo "  sysvinit (default)"
        fi
        echo ""
        echo "Available init scripts:"
        for init in runit openrc s6; do
            printf "  %-12s " "$init"
            if [ -f "/etc/tenebra/init-${init}.available" ]; then
                echo "[available]"
            else
                echo "[not installed]"
            fi
        done
        ;;
    list)
        echo "Available init systems:"
        for init in runit openrc s6 sysvinit; do
            echo "  $init"
        done
        ;;
    switch)
        TARGET="${2:-}"
        [ -n "$TARGET" ] || { echo "Usage: $0 switch <runit|openrc|s6>" >&2; exit 1; }

        case "$TARGET" in
            runit)
                if [ -f /sbin/runit-init ]; then
                    ln -sf runit-init /sbin/init
                    echo "Switched to runit. Reboot to apply."
                else
                    echo "runit-init not installed" >&2
                    exit 1
                fi
                ;;
            openrc)
                if [ -f /sbin/openrc ]; then
                    ln -sf /sbin/init.openrc /sbin/init
                    echo "Switched to OpenRC. Reboot to apply."
                else
                    echo "OpenRC not installed" >&2
                    exit 1
                fi
                ;;
            s6)
                if [ -x /usr/bin/s6-linux-init ]; then
                    ln -sf /usr/bin/s6-linux-init /sbin/init
                    echo "Switched to s6. Reboot to apply."
                else
                    echo "s6-linux-init not installed" >&2
                    exit 1
                fi
                ;;
            sysvinit)
                ln -sf /sbin/init.sysvinit /sbin/init 2>/dev/null || \
                ln -sf /sbin/init.d/rc 2>/dev/null || true
                echo "Switched to sysvinit. Reboot to apply."
                ;;
            *)
                echo "Unknown init system: $TARGET" >&2
                echo "Available: runit, openrc, s6, sysvinit" >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "Usage: $0 {status|list|switch <init>}"
        exit 1
        ;;
esac
INITSWITCH
    chmod +x "$rootfs/usr/local/bin/tenebra-init-switch"

    # Mark which init systems are available
    touch "$rootfs/etc/tenebra/init-${INIT_SYSTEM}.available"

    log "Init switcher installed at /usr/local/bin/tenebra-init-switch"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    require_root
    [ -d "$TENEBRA_ROOTFS" ] || err "Rootfs not found — run 01-bootstrap.sh first"

    install_sysvinit_base

    case "$INIT_SYSTEM" in
        runit)
            install_eudev
            setup_runit
            ;;
        openrc)
            install_eudev
            setup_openrc
            ;;
        s6)
            install_eudev
            setup_s6
            ;;
        *)
            err "Unknown init system: $INIT_SYSTEM (use: runit, openrc, s6)"
            ;;
    esac

    install_init_switcher

    log "Phase 3 complete: ${INIT_SYSTEM} installed as init system"
    log "Next: sudo ./04-hybrid-pkgmanager.sh"
}

main "$@"
