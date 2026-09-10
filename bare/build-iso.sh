#!/bin/bash
# bare/build-iso.sh
# All-in-one TenebraOS ISO builder
#
# Builds a bootable live ISO from scratch:
#   1. debootstrap a Devuan Excalibur rootfs
#   2. Install kernel, runit init, firmware, desktop
#   3. Add Calamares graphical installer
#   4. Add manual install script (Arch/Void style)
#   5. Create Btrfs snapshot support
#   6. Generate hybrid ISO (BIOS + UEFI)
#
# Usage:
#   sudo ./build-iso.sh
#   sudo ./build-iso.sh --clean     # remove all build artifacts
#   sudo ./build-iso.sh --skip-rootfs  # reuse existing rootfs
#
# Output: /tmp/tenebra-build/TenebraOS-YYYYMMDD.iso

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
BUILD_DIR="/tmp/tenebra-build"
ROOTFS="$BUILD_DIR/rootfs"
STAGING="$BUILD_DIR/iso-staging"
LOG_DIR="$BUILD_DIR/logs"
ISO_OUTPUT="$BUILD_DIR/TenebraOS-$(date +%Y%m%d).iso"

DEVUAN_MIRROR="${DEVUAN_MIRROR:-http://deb.devuan.org/merged}"
SUITE="excalibur"
ARCH="amd64"
LIVE_USER="user"
LIVE_PASS="tenebra"
HOSTNAME="tenebra"

JOBS="${JOBS:-$(nproc)}"

# ─── Colors & Helpers ──────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'

log()   { printf "\n${BLUE}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
ok()    { printf "${GREEN}>>>${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}>>>${NC} %s\n" "$*"; }
err()   { printf "${RED}ERROR:${NC} %s\n" "$*" >&2; exit 1; }

cleanup_mounts() {
    for m in dev/pts dev proc sys run; do
        mountpoint -q "$ROOTFS/$m" 2>/dev/null && umount -lf "$ROOTFS/$m" 2>/dev/null || true
    done
    mountpoint -q "$STAGING/live" 2>/dev/null && umount "$STAGING/live" 2>/dev/null || true
}

# ─── Clean ─────────────────────────────────────────────────────────────────────
do_clean() {
    log "Cleaning all build artifacts"
    cleanup_mounts 2>/dev/null || true
    rm -rf "$BUILD_DIR"
    ok "Clean complete"
    exit 0
}

# ─── Parse Args ────────────────────────────────────────────────────────────────
SKIP_ROOTFS=0
for arg in "$@"; do
    case "$arg" in
        --clean)        do_clean ;;
        --skip-rootfs)  SKIP_ROOTFS=1 ;;
    esac
done

# ─── Pre-flight ────────────────────────────────────────────────────────────────
preflight() {
    log "Pre-flight checks"

    [ "$(id -u)" -eq 0 ] || err "Must run as root"

    # Install mmdebstrap if missing (Arch)
    if ! command -v mmdebstrap >/dev/null 2>&1; then
        if [ -f /etc/arch-release ]; then
            log "Installing dependencies on Arch"
            pacman -S --needed --noconfirm perl curl 2>/dev/null || true

            if ! [ -f /usr/local/bin/mmdebstrap ]; then
                info "Downloading mmdebstrap..."
                curl -fsSL \
                    "https://salsa.debian.org/debian/mmdebstrap/-/raw/master/mmdebstrap" \
                    -o /usr/local/bin/mmdebstrap
                chmod +x /usr/local/bin/mmdebstrap
                ok "mmdebstrap installed"
            fi
        elif [ -f /etc/debian_version ]; then
            export DEBIAN_FRONTEND=noninteractive
            apt-get update -qq
            apt-get install -y -qq mmdebstrap
        else
            err "Install mmdebstrap manually, then re-run this script"
        fi
    fi

    # Verify it works
    mmdebstrap --version >/dev/null 2>&1 || err "mmdebstrap broken — check perl is installed: sudo pacman -S perl"

    for cmd in mksquashfs xorriso rsync; do
        command -v "$cmd" >/dev/null 2>&1 || err "Missing: $cmd — run setup-host-deps.sh first"
    done

    mkdir -p "$BUILD_DIR" "$LOG_DIR"
    ok "All checks passed"
}

# ─── Phase 1: Bootstrap Rootfs with debootstrap ───────────────────────────────
bootstrap_rootfs() {
    if [ "$SKIP_ROOTFS" -eq 1 ] && [ -d "$ROOTFS/bin" ]; then
        log "Reusing existing rootfs (--skip-rootfs)"
        return 0
    fi

    log "Phase 1: Bootstrapping Devuan ${SUITE} rootfs"

    cleanup_mounts 2>/dev/null || true
    rm -rf "$ROOTFS"
    mkdir -p "$ROOTFS"

    local include_pkgs="devuan-keyring,e2fsprogs,btrfs-progs,dosfstools,live-boot,live-config,live-config-sysvinit"

    command -v mmdebstrap >/dev/null 2>&1 || err "mmdebstrap not found"

    mmdebstrap \
        --architectures=amd64 \
        --variant=minbase \
        --include="$include_pkgs" \
        --aptopt='Acquire::Check-Valid-Until "false"' \
        "$SUITE" \
        "$ROOTFS" \
        "${DEVUAN_MIRROR}" \
        2>&1 | tee "$LOG_DIR/debootstrap.log"

    [ -x "$ROOTFS/bin/bash" ] || err "Bootstrap failed — no bash in rootfs"
    ok "Rootfs bootstrapped at $ROOTFS"
}

# ─── Phase 2: Mount Pseudo-Filesystems ────────────────────────────────────────
mount_pseudo() {
    log "Mounting pseudo-filesystems"

    cleanup_mounts 2>/dev/null || true

    for m in dev dev/pts proc sys run; do
        case "$m" in
            dev)    mount --bind /dev  "$ROOTFS/dev" ;;
            dev/pts) mount --bind /dev/pts "$ROOTFS/dev/pts" ;;
            proc)   mount -t proc proc "$ROOTFS/proc" ;;
            sys)    mount -t sysfs sysfs "$ROOTFS/sys" ;;
            run)    mount -t tmpfs tmpfs "$ROOTFS/run" ;;
        esac
    done

    ok "Pseudo-filesystems mounted"
}

# ─── Phase 3: Configure Rootfs ────────────────────────────────────────────────
configure_rootfs() {
    log "Phase 2: Configuring rootfs"

    # --- APT sources (Devuan, no systemd) ---
    cat > "$ROOTFS/etc/apt/sources.list" <<SOURCES
deb ${DEVUAN_MIRROR} ${SUITE} main contrib non-free non-free-firmware
deb-src ${DEVUAN_MIRROR} ${SUITE} main contrib non-free non-free-firmware
SOURCES

    # --- apt preferences: prefer Devuan, no systemd ---
    mkdir -p "$ROOTFS/etc/apt/preferences.d"
    cat > "$ROOTFS/etc/apt/preferences.d/99-tenebra" <<'PREF'
Package: systemd*
Pin: release o=Devuan
Pin-Priority: -1
PREF

    # --- Base system packages ---
    chroot "$ROOTFS" bash -c '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update

        # Core packages
        apt-get install -y --no-install-recommends \
            linux-image-amd64 \
            linux-headers-amd64 \
            linux-image-amd64-dbg \
            dkms \
            firmware-linux \
            firmware-linux-nonfree \
            firmware-misc-nonfree \
            intel-microcode \
            amd64-microcode

        # Init system: runit + elogind (no systemd)
        apt-get install -y --no-install-recommends \
            runit \
            runit-init \
            elogind \
            dbus \
            dbus-user-session \
            network-manager \
            network-manager-gnome

        # Desktop: KDE Plasma
        apt-get install -y --no-install-recommends \
            plasma-desktop \
            plasma-nm \
            sddm \
            konsole \
            kate

        # Audio (no systemd requirement)
        apt-get install -y --no-install-recommends \
            pipewire \
            pipewire-pulse \
            wireplumber \
            alsa-utils

        # Installer
        apt-get install -y --no-install-recommends \
            calamares \
            calamares-settings-debconf \
            calamares-settings-l10n

        # Utilities
        apt-get install -y --no-install-recommends \
            sudo \
            curl \
            wget \
            git \
            rsync \
            htop \
            neofetch \
            xorg \
            xserver-xorg-video-all \
            xserver-xorg-input-all \
            xinit \
            open-ssh \
            gnome-disk-utility \
            gparted

        # Filesystem tools
        apt-get install -y --no-install-recommends \
            btrfs-progs \
            snapper \
            grub-pc-bin \
            grub-efi-amd64-bin \
            grub2-common \
            efibootmgr

        # Firmware
        apt-get install -y --no-install-recommends \
            udisks2 \
            network-manager-gnome \
            bluedevil || true

        apt-get clean
    ' 2>&1 | tee "$LOG_DIR/packages.log"

    ok "Base packages installed"
}

# ─── Phase 4: Runit Init Setup ────────────────────────────────────────────────
setup_runit() {
    log "Phase 3: Setting up runit init system"

    # Install runit-init (replaces sysvinit-core)
    chroot "$ROOTFS" bash -c '
        export DEBIAN_FRONTEND=noninteractive
        if ! [ -x /sbin/runit-init ]; then
            apt-get install -y runit-init 2>/dev/null || true
        fi
    ' 2>&1 || true

    # Symlink runit as /sbin/init
    chroot "$ROOTFS" ln -sf /sbin/runit-init /sbin/init 2>/dev/null || true

    # Service directories
    mkdir -p "$ROOTFS/etc/runit/runsvdir/default"
    mkdir -p "$ROOTFS/run/runit/service"
    ln -sfn /etc/runit/runsvdir/default "$ROOTFS/run/runit/service" 2>/dev/null || true
    ln -sfn /etc/runit/runsvdir/default "$ROOTFS/etc/service" 2>/dev/null || true

    # --- Service definitions ---
    for svc in dbus NetworkManager sddm; do
        mkdir -p "$ROOTFS/etc/sv/$svc"
    done

    # dbus
    cat > "$ROOTFS/etc/sv/dbus/run" <<'SVC'
#!/bin/sh
mkdir -p /run/dbus
exec /usr/bin/dbus-daemon --system --nofork
SVC
    chmod +x "$ROOTFS/etc/sv/dbus/run"

    # NetworkManager
    cat > "$ROOTFS/etc/sv/NetworkManager/run" <<'SVC'
#!/bin/sh
while [ ! -S /run/dbus/system_bus_socket ]; do sleep 0.1; done
exec /usr/sbin/NetworkManager --no-daemon
SVC
    chmod +x "$ROOTFS/etc/sv/NetworkManager/run"

    # SDDM (resolve session type at boot)
    cat > "$ROOTFS/etc/sv/sddm/run" <<'SVC'
#!/bin/sh
while [ ! -S /run/dbus/system_bus_socket ]; do sleep 0.1; done
SESSION=""
for s in plasmax11 plasma; do
    if [ -f "/usr/share/xsessions/$s.desktop" ]; then
        SESSION="$s"; break
    fi
done
[ -z "$SESSION" ] && SESSION=$(ls /usr/share/xsessions/*.desktop 2>/dev/null | head -1 | xargs -r basename .desktop 2>/dev/null)
[ -n "$SESSION" ] && printf '[Autologin]\nUser=user\nSession=%s\nRelogin=false\n' "$SESSION" \
    > /etc/sddm.conf.d/autologin.conf 2>/dev/null || true
exec /usr/bin/sddm
SVC
    chmod +x "$ROOTFS/etc/sv/sddm/run"

    # Snapper timeline
    mkdir -p "$ROOTFS/etc/sv/snapper-timeline"
    cat > "$ROOTFS/etc/sv/snapper-timeline/run" <<'SVC'
#!/bin/sh
exec snapper -c root timeline --cleanup
SVC
    chmod +x "$ROOTFS/etc/sv/snapper-timeline/run"

    # Enable default services
    for svc in dbus NetworkManager; do
        ln -sf "/etc/sv/$svc" "$ROOTFS/etc/runit/runsvdir/default/$svc"
    done

    ok "runit init configured"
}

# ─── Phase 5: Live User + Desktop Config ──────────────────────────────────────
setup_live_user() {
    log "Phase 4: Creating live user and desktop config"

    # Create live user
    chroot "$ROOTFS" bash -c "
        useradd -m -s /bin/bash '$LIVE_USER' 2>/dev/null || true
        echo '$LIVE_USER:$LIVE_PASS' | chpasswd
        for g in sudo adm lpadmin autologin nopasswdlogin video audio plugdev; do
            usermod -aG \"\$g\" '$LIVE_USER' 2>/dev/null || true
        done
    "

    # Sudoers: NOPASSWD for live user
    mkdir -p "$ROOTFS/etc/sudoers.d"
    cat > "$ROOTFS/etc/sudoers.d/tenebra-live" <<SUDOERS
$LIVE_USER ALL=(ALL) NOPASSWD: ALL
SUDOERS
    chmod 440 "$ROOTFS/etc/sudoers.d/tenebra-live"

    # Auto-login on SDDM
    mkdir -p "$ROOTFS/etc/sddm.conf.d"
    cat > "$ROOTFS/etc/sddm.conf.d/autologin.conf" <<SDDM
[Autologin]
User=$LIVE_USER
Session=plasma
Relogin=false
SDDM

    # SDDM X11 config
    cat > "$ROOTFS/etc/sddm.conf.d/x11.conf" <<X11
[General]
DisplayServer=x11
X11

    # Calamares auto-start on desktop login
    mkdir -p "$ROOTFS/etc/xdg/autostart"
    cat > "$ROOTFS/etc/xdg/autostart/tenebra-installer.desktop" <<'AUTO'
[Desktop Entry]
Type=Application
Name=TenebraOS Installer
Comment=Install TenebraOS to your hard drive
Exec=/usr/local/bin/tenebra-installer.sh
Icon=calamares
Terminal=false
Categories=System;
X-GNOME-Autostart-enabled=true
AUTO

    # Calamares installer launcher
    cat > "$ROOTFS/usr/local/bin/tenebra-installer.sh" <<'INSTALLER'
#!/bin/bash
if ! command -v calamares >/dev/null 2>&1; then
    echo "Calamares is not installed" >&2
    exit 1
fi
exec sudo -E /usr/bin/calamares "$@"
INSTALLER
    chmod +x "$ROOTFS/usr/local/bin/tenebra-installer.sh"

    # Calamares polkit rule
    mkdir -p "$ROOTFS/etc/polkit-1/localauthority/50-local.d"
    cat > "$ROOTFS/etc/polkit-1/localauthority/50-local.d/calamares.pkla" <<'POLKIT'
[Install Calamares]
Identity=unix-group:sudo
Action=org.debian.calamares.*;org.freedesktop.accounts.set-locale;org.freedesktop.accounts.set-timezone;org.freedesktop.accounts.set-user-geometry
ResultAny=yes
ResultInactive=yes
ResultActive=yes
POLKIT

    # Calamares sudoers
    cat > "$ROOTFS/etc/sudoers.d/calamares" <<'CALAMARES'
Defaults env_keep += "DISPLAY XAUTHORITY WAYLAND_DISPLAY"
$LIVE_USER ALL=(root) NOPASSWD: /usr/bin/calamares
CALAMARES
    chmod 440 "$ROOTFS/etc/sudoers.d/calamares"

    ok "Live user created: $LIVE_USER"
}

# ─── Phase 6: Calamares Configuration ─────────────────────────────────────────
setup_calamares() {
    log "Phase 5: Configuring Calamares installer"

    mkdir -p "$ROOTFS/etc/calamares/modules"

    # Calamares settings.conf
    cat > "$ROOTFS/etc/calamares/settings.conf" <<'CALCONF'
# TenebraOS Calamares settings
---
modules-search:
    - /usr/share/calamares/modules
    - /usr/lib/calamares/modules

instances:
    - id: profileselect
      module: packagechooser
      config: profileselect.conf

branding: tenebra

oem-setup: false
hide-back-and-next-during-exec: false
prompt-install: true
dont-chroot: false
disable-cancel: false
disable-cancel-during-exec: true
quit-at-end: true

requirements:
    requiredStorage: 15
    requiredRam: 2.0

sequence:
    - show:
        - welcome
        - packagechooser@profileselect
        - locale
        - keyboard
        - partition
        - users
        - summary
    - exec:
        - partition
        - mount
        - unpackfs
        - machineid
        - fstab
        - locale
        - keyboard
        - localecfg
        - users
        - networkcfg
        - hwclock
        - grubcfg
        - bootloader
        - initramfscfg
        - initramfs
        - umount
    - show:
        - finished
CALCONF

    # Bootloader config
    cat > "$ROOTFS/etc/calamares/modules/bootloader.conf" <<'BOOTCONF'
---
efiBootLoader: "grub"
kernel: "/vmlinuz-*"
img: "/initrd.img-*"
timeout: "5"
grubInstall: "grub-install"
grubMkconfig: "grub-mkconfig"
grubCfg: "/boot/grub/grub.cfg"
grubProbe: "grub-probe"
efiBootMgr: "efibootmgr"
bootloaderEntryName: "TenebraOS"
installEFIFallback: true
BOOTCONF

    # Users config
    cat > "$ROOTFS/etc/calamares/modules/users.conf" <<'USRCONF'
---
defaultGroup: sudo
defaultShell: /bin/bash
userGroups:
    - sudo
    - audio
    - video
    - plugdev
    - netdev
doAutologin: false
USRCONF

    # Branding
    mkdir -p "$ROOTFS/usr/share/calamares/branding/tenebra"
    cat > "$ROOTFS/usr/share/calamares/branding/tenebra/branding.desc" <<'BRAND'
---
type: "Constitute"
name: "TenebraOS"
version: "1.0"
publisher: "TenebraOS"
www: "https://itzr1g.github.io/tenebra.github.io/"
BRAND

    ok "Calamares configured"
}

# ─── Phase 7: TenebraOS Manual Install Script ─────────────────────────────────
setup_manual_install() {
    log "Phase 6: Adding manual install script (Arch/Void style)"

    cat > "$ROOTFS/usr/local/bin/tenebra-install" <<'INSTALL'
#!/bin/bash
# tenebra-install — Manual installer for TenebraOS (Arch/Void style)
#
# Usage:
#   tenebra-install              # guided mode
#   tenebra-install --help       # show options

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'

log()   { printf "\n${BLUE}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
ok()    { printf "${GREEN}>>>${NC} %s\n" "$*"; }
warn()  { printf "${YELLOW}>>>${NC} %s\n" "$*"; }
err()   { printf "${RED}ERROR:${NC} %s\n" "$*" >&2; exit 1; }

ROOT=""

usage() {
    cat <<EOF
TenebraOS Manual Installer

Usage:
  tenebra-install                  Guided installation
  tenebra-install --disk <dev>     Target disk (e.g. /dev/sda)
  tenebra-install --user <name>    Username (default: user)
  tenebra-install --pass <pass>    User password
  tenebra-install --hostname <h>   Hostname (default: tenebra)
  tenebra-install --help           Show this help

Example:
  tenebra-install --disk /dev/sda --user rigby --pass mypass --hostname mypc
EOF
}

# Parse arguments
DISK=""
USER_NAME="user"
USER_PASS=""
HOST="tenebra"

while [ $# -gt 0 ]; do
    case "$1" in
        --disk)      DISK="$2"; shift 2 ;;
        --user)      USER_NAME="$2"; shift 2 ;;
        --pass)      USER_PASS="$2"; shift 2 ;;
        --hostname)  HOST="$2"; shift 2 ;;
        --help|-h)   usage; exit 0 ;;
        *)           err "Unknown option: $1" ;;
    esac
done

# Interactive prompts if not provided
if [ -z "$DISK" ]; then
    echo ""
    echo "Available disks:"
    lsblk -d -o NAME,SIZE,MODEL | grep -v loop
    echo ""
    read -rp "Target disk (e.g. /dev/sda): " DISK
fi
[ -b "$DISK" ] || err "Not a block device: $DISK"

if [ -z "$USER_PASS" ]; then
    read -srp "Password for $USER_NAME: " USER_PASS
    echo ""
    [ -n "$USER_PASS" ] || err "Password cannot be empty"
fi

if [ -z "$USER_NAME" ] || [ "$USER_NAME" = "root" ]; then
    read -rp "Username (default: user): " USER_NAME
    USER_NAME="${USER_NAME:-user}"
fi

read -rp "Hostname (default: tenebra): " HOST
HOST="${HOST:-tenebra}"

echo ""
log "Installing TenebraOS to $DISK"
echo "  User:     $USER_NAME"
echo "  Hostname: $HOST"
echo "  Disk:     $DISK"
echo ""
warn "THIS WILL DESTROY ALL DATA ON $DISK"
read -rp "Type 'YES' to continue: " confirm
[ "$confirm" = "YES" ] || { echo "Aborted."; exit 0; }

# --- Partition ---
log "Partitioning $DISK"
wipefs -af "$DISK"
sgdisk --zap-all "$DISK"
parted -s "$DISK" \
    mklabel gpt \
    mkpart ESP fat32 1MiB 513MiB \
    set 1 esp on \
    mkpart primary 513MiB 100%

partprobe "$DISK"
sleep 2

ESP="${DISK}1"
ROOT_PART="${DISK}2"
[[ "$DISK" == *"nvme"* ]] && ESP="${DISK}p1" && ROOT_PART="${DISK}p2"

mkfs.fat -F32 -n TENEBOOT "$ESP"
mkfs.btrfs -f -L TEBRA_ROOT "$ROOT_PART"

# --- Btrfs subvolumes ---
log "Creating Btrfs subvolumes"
mkdir -p /mnt
mount "$ROOT_PART" /mnt
btrfs subvolume create /mnt/@
btrfs subvolume create /mnt/@home
btrfs subvolume create /mnt/@snapshots
btrfs subvolume create /mnt/@var_log
umount /mnt

mount -o subvol=@,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt
mkdir -p /mnt/{home,snapshots,boot,var/log}
mount -o subvol=@home,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt/home
mount -o subvol=@snapshots,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt/snapshots
mount -o subvol=@var_log,compress=zstd:1,ssd,noatime "$ROOT_PART" /mnt/var/log
mount "$ESP" /mnt/boot

# --- Copy live rootfs to target ---
log "Copying live system to target (this takes a while)"
rsync -aHA --exclude='/dev/*' --exclude='/proc/*' --exclude='/sys/*' \
    --exclude='/run/*' --exclude='/tmp/*' --exclude='/mnt/*' \
    --exclude='/media/*' --exclude='/snapshots/*' \
    / "$ROOT"

# --- fstab ---
ROOT_UUID="$(blkid -s UUID -o value "$ROOT_PART")"
ESP_UUID="$(blkid -s UUID -o value "$ESP")"

cat > "$ROOT/etc/fstab" <<FSTAB
UUID=$ROOT_UUID   /           btrfs   subvol=@,compress=zstd:1,ssd,noatime  0 1
UUID=$ROOT_UUID   /home       btrfs   subvol=@home,compress=zstd:1,ssd      0 2
UUID=$ROOT_UUID   /snapshots  btrfs   subvol=@snapshots,compress=zstd:1,ssd 0 2
UUID=$ROOT_UUID   /var/log    btrfs   subvol=@var_log,compress=zstd:1,ssd   0 2
UUID=$ESP_UUID    /boot       vfat    umask=0077                            0 2
proc              /proc       proc    nosuid,noexec,nodev                   0 0
sysfs             /sys        sysfs   nosuid,noexec,nodev,ro                0 0
tmpfs             /tmp        tmpfs   nosuid,nodev,size=2G                   0 0
tmpfs             /run        tmpfs   nosuid,nodev,size=2G                   0 0
FSTAB

# --- Chroot and configure ---
log "Configuring installed system"

# Bind mounts for chroot
mount --bind /dev  "$ROOT/dev"
mount --bind /dev/pts "$ROOT/dev/pts"
mount -t proc proc "$ROOT/proc"
mount -t sysfs sysfs "$ROOT/sys"

# Hostname
echo "$HOST" > "$ROOT/etc/hostname"
cat > "$ROOT/etc/hosts" <<HOSTS
127.0.0.1   localhost
127.0.1.1   $HOST
::1         localhost ip6-localhost ip6-loopback
HOSTS

# Users
chroot "$ROOT" bash -c "
    useradd -m -s /bin/bash '$USER_NAME'
    echo '$USER_NAME:$USER_PASS' | chpasswd
    echo 'root:$(openssl passwd -6 "$USER_PASS")' | chpasswd -e 2>/dev/null || true
    for g in sudo adm audio video plugdev netdev; do
        usermod -aG \"\$g\" '$USER_NAME' 2>/dev/null || true
    done
"

# Disable live user autologin, enable login manager
chroot "$ROOT" bash -c '
    rm -f /etc/sddm.conf.d/autologin.conf
    rm -f /etc/xdg/autostart/tenebra-installer.desktop
    rm -f /etc/sudoers.d/tenebra-live
'

# GRUB
log "Installing bootloader"
chroot "$ROOT" bash -c "
    grub-install --target=x86_64-efi --efi-directory=/boot --recheck 2>/dev/null || true
    grub-install --target=i386-pc --recheck '$DISK' 2>/dev/null || true
    update-grub
"

# Enable SDDM
mkdir -p "$ROOT/etc/sv/sddm/run" 2>/dev/null || true
chroot "$ROOT" bash -c '
    mkdir -p /etc/runit/runsvdir/default 2>/dev/null || true
    ln -sf /etc/sv/sddm /etc/runit/runsvdir/default/sddm 2>/dev/null || true
' 2>/dev/null || true

# Cleanup installer
rm -f "$ROOT/usr/local/bin/tenebra-install"

# --- Unmount ---
sync
umount "$ROOT/dev/pts" 2>/dev/null || true
umount "$ROOT/dev" 2>/dev/null || true
umount "$ROOT/proc" 2>/dev/null || true
umount "$ROOT/sys" 2>/dev/null || true

ok ""
ok "Installation complete!"
ok "  Reboot and remove the live media."
ok "  Login: $USER_NAME"
ok ""
INSTALL
    chmod +x "$ROOTFS/usr/local/bin/tenebra-install"

    # Desktop shortcut for manual install
    mkdir -p "$ROOTFS/home/$LIVE_USER/Desktop"
    cat > "$ROOTFS/home/$LIVE_USER/Desktop/manual-install.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Manual Install
Comment=Install TenebraOS from terminal
Exec=sudo /usr/local/bin/tenebra-install
Icon=utilities-terminal
Terminal=true
Categories=System;
DESKTOP
    chmod 644 "$ROOTFS/home/$LIVE_USER/Desktop/manual-install.desktop"
    chown -R 1000:1000 "$ROOTFS/home/$LIVE_USER/Desktop"

    # Calamares desktop shortcut
    cat > "$ROOTFS/home/$LIVE_USER/Desktop/install-tenebra.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Install TenebraOS
Comment=Graphical installer for TenebraOS
Exec=/usr/local/bin/tenebra-installer.sh
Icon=calamares
Terminal=false
Categories=System;
DESKTOP
    chmod 644 "$ROOTFS/home/$LIVE_USER/Desktop/install-tenebra.desktop"
    chown -R 1000:1000 "$ROOTFS/home/$LIVE_USER/Desktop"

    ok "Install options added:"
    ok "  Calamares — /usr/local/bin/tenebra-installer.sh"
    ok "  Manual    — /usr/local/bin/tenebra-install"
}

# ─── Phase 8: Btrfs Snapper Setup ─────────────────────────────────────────────
setup_snapshots() {
    log "Phase 7: Configuring Btrfs snapshots"

    mkdir -p "$ROOTFS/etc/snapper/configs"
    cat > "$ROOTFS/etc/snapper/configs/root" <<'SNAPCFG'
SUBVOLUME="/"
FSTYPE="btrfs"
ALLOW_USERS=""
ALLOW_GROUPS="root"
SYNC_ACL="no"
TIMELINE_CREATE="yes"
TIMELINE_CLEANUP="yes"
TIMELINE_MIN_AGE="1800"
TIMELINE_LIMIT_HOURLY="5"
TIMELINE_LIMIT_DAILY="7"
TIMELINE_LIMIT_WEEKLY="0"
TIMELINE_LIMIT_MONTHLY="0"
TIMELINE_LIMIT_YEARLY="0"
EMPTY_PRE_POST_CLEANUP="yes"
EMPTY_PRE_POST_MIN_AGE="1800"
EMPTY_TIMELINE_CLEANUP="yes"
EMPTY_TIMELINE_MIN_AGE="43200"
NUMBER_LIMIT="10"
NUMBER_LIMIT_IMPORTANT="5"
SNAPCFG

    # Cron job for timeline snapshots
    mkdir -p "$ROOTFS/etc/cron.d"
    cat > "$ROOTFS/etc/cron.d/snapper" <<'CRON'
*/30 * * * * root /usr/bin/snapper -c root timeline --cleanup
CRON

    # Snapshot hook for apt transactions
    cat > "$ROOTFS/usr/local/bin/tenebra-snapshot-hook" <<'HOOK'
#!/bin/bash
set -euo pipefail
ACTION="${1:-}"
SNAP_DIR="/.snapshots"
LOG_FILE="/var/log/tenebra/snapshot-hooks.log"
mkdir -p "$SNAP_DIR" "$(dirname "$LOG_FILE")"
echo "$(date '+%Y-%m-%d %H:%M:%S') [$ACTION]" >> "$LOG_FILE"

create_snap() {
    local desc="$1"
    local ts
    ts="$(date +%Y%m%d_%H%M%S)"
    local name="snap_${ts}"
    btrfs subvolume snapshot -r "/" "$SNAP_DIR/$name" 2>/dev/null && {
        echo "${name}|$(date +%s)|${desc}|$(whoami)" >> "$SNAP_DIR/snapshots.log"
        # Keep only last 10
        local count
        count="$(grep -c '^snap_' "$SNAP_DIR/snapshots.log" 2>/dev/null || echo 0)"
        [ "$count" -gt 10 ] && {
            local rm=$((count - 10))
            grep '^snap_' "$SNAP_DIR/snapshots.log" | head -n "$rm" | \
            while IFS='|' read -r n _ _ _; do
                btrfs subvolume delete "$SNAP_DIR/$n" 2>/dev/null || true
                sed -i "\|^${n}|d" "$SNAP_DIR/snapshots.log" 2>/dev/null || true
            done
        }
    }
}

case "$ACTION" in
    pre-dpkg)    [ -f /tmp/tenebra-snap-trigger ] && create_snap "Pre-dpkg: $(cat /tmp/tenebra-snap-trigger)" && rm -f /tmp/tenebra-snap-trigger ;;
    post-dpkg)   create_snap "Post-dpkg" ;;
    pre-upgrade) create_snap "Pre-upgrade" ;;
esac
HOOK
    chmod +x "$ROOTFS/usr/local/bin/tenebra-snapshot-hook"

    cat > "$ROOTFS/etc/apt/apt.conf.d/90-tenebra-snapshots" <<'APTHOOK'
DPkg::Pre-Invoke { "/usr/local/bin/tenebra-snapshot-hook pre-dpkg"; };
APTHOOK

    ok "Snapshots configured"
}

# ─── Phase 9: TenebraOS Branding ──────────────────────────────────────────────
setup_branding() {
    log "Phase 8: TenebraOS branding"

    # os-release
    cat > "$ROOTFS/etc/os-release" <<'OSRELEASE'
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

    # GRUB theme
    mkdir -p "$ROOTFS/boot/grub/themes/tenebra"
    cat > "$ROOTFS/boot/grub/themes/tenebra/theme.txt" <<'THEME'
set color_normal=white/black
set color_highlight=red/black
desktop-color: "#1a1a2e"
title-text: "TenebraOS"
title-color: "#e94560"
title-font: "DejaVu Sans Bold 18"
THEME

    ok "Branding applied"
}

# ─── Phase 10: Create SquashFS ────────────────────────────────────────────────
create_squashfs() {
    log "Phase 9: Creating SquashFS"

    cleanup_mounts 2>/dev/null || true

    rm -rf "$STAGING"
    mkdir -p "$STAGING"/{live,boot/grub}

    # Create clean copy for squashfs
    local sqroot="$BUILD_DIR/squashfs-root"
    rm -rf "$sqroot"
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
        "$ROOTFS/" "$sqroot/"

    mkdir -p "$sqroot/home" "$sqroot/snapshots"

    # Copy kernel + initramfs to staging
    cp "$sqroot/boot/vmlinuz-"* "$STAGING/boot/vmlinuz" 2>/dev/null || \
        cp "$sqroot/boot/vmlinuz"* "$STAGING/boot/vmlinuz" 2>/dev/null || true
    cp "$sqroot/boot/initrd.img-"* "$STAGING/boot/initrd.img" 2>/dev/null || \
        cp "$sqroot/boot/initrd.img"* "$STAGING/boot/initrd.img" 2>/dev/null || true

    # Compress
    local sqfs="$STAGING/live/filesystem.squashfs"
    info "Compressing rootfs..."
    mksquashfs "$sqroot" "$sqfs" \
        -comp xz -b 1M -Xdict-size 1M \
        -noappend -quiet 2>&1 | tail -1

    ok "SquashFS: $(du -h "$sqfs" | cut -f1)"
    rm -rf "$sqroot"
}

# ─── Phase 11: Bootloader Config ──────────────────────────────────────────────
setup_bootloaders() {
    log "Phase 10: Bootloader configuration"

    # --- ISOLINUX (BIOS boot) ---
    cat > "$STAGING/boot/isolinux/isolinux.cfg" <<'ISOLINUX'
UI vesamenu.c32
MENU TITLE TenebraOS
TIMEOUT 50
DEFAULT tenebra

LABEL tenebra
    MENU LABEL TenebraOS Live
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components quiet splash

LABEL safe
    MENU LABEL TenebraOS (Safe Graphics)
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components nomodeset quiet

LABEL ram
    MENU LABEL TenebraOS (RAM Mode)
    LINUX /boot/vmlinuz
    INITRD /boot/initrd.img
    APPEND boot=live components toram=filesystem.squashfs quiet splash

LABEL hdd
    MENU LABEL Boot from hard disk
    LOCALBOOT 0x80
ISOLINUX

    # Copy isolinux binaries
    for dir in /usr/lib/ISOLINUX /usr/lib/syslinux /usr/share/syslinux; do
        if [ -d "$dir" ]; then
            cp "$dir/isolinux.bin" "$STAGING/boot/isolinux/" 2>/dev/null || true
            cp "$dir/ldlinux.c32" "$STAGING/boot/isolinux/" 2>/dev/null || true
            cp "$dir/vesamenu.c32" "$STAGING/boot/isolinux/" 2>/dev/null || true
            cp "$dir/libcom32.c32" "$STAGING/boot/isolinux/" 2>/dev/null || true
            cp "$dir/libutil.c32" "$STAGING/boot/isolinux/" 2>/dev/null || true
            break
        fi
    done

    # --- GRUB EFI ---
    cat > "$STAGING/boot/grub/grub.cfg" <<'GRUB'
set default="0"
set timeout=5
set gfxmode=auto
set gfxpayload=keep
loadfont unicode
insmod efi_gop efi_uga gfxterm gfxmenu all_video jpeg png iso9660
terminal_output gfxterm
search --no-floppy --set=root --label TenebraOS

set theme=${prefix}/themes/tenebra/theme.txt
export theme

menuentry "TenebraOS Live" {
    linux /boot/vmlinuz boot=live components quiet splash
    initrd /boot/initrd.img
}
menuentry "TenebraOS (Safe Graphics)" {
    linux /boot/vmlinuz boot=live components nomodeset quiet
    initrd /boot/initrd.img
}
menuentry "TenebraOS (RAM Mode)" {
    linux /boot/vmlinuz boot=live components toram=filesystem.squashfs quiet splash
    initrd /boot/initrd.img
}
menuentry "Boot from hard disk" {
    set root=(hd0)
    chainloader +1
}
GRUB

    # GRUB theme
    mkdir -p "$STAGING/boot/grub/themes/tenebra"
    cat > "$STAGING/boot/grub/themes/tenebra/theme.txt" <<'THEME'
set color_normal=white/black
set color_highlight=red/black
desktop-color: "#1a1a2e"
title-text: "TenebraOS"
title-color: "#e94560"
title-font: "DejaVu Sans Bold 18"
THEME

    ok "Bootloaders configured"
}

# ─── Phase 12: Build ISO ──────────────────────────────────────────────────────
build_iso() {
    log "Phase 11: Building ISO"

    rm -f "$ISO_OUTPUT"

    # Find isolinux/isohdpfx.bin
    local isohdpfx=""
    for path in /usr/lib/ISOLINUX/isohdpfx.bin \
                /usr/lib/syslinux/isohdpfx.bin \
                /usr/share/syslinux/isohdpfx.bin; do
        [ -f "$path" ] && isohdpfx="$path" && break
    done

    # Create EFI image
    local efi_img="$STAGING/boot/grub/efi.img"
    dd if=/dev/zero of="$efi_img" bs=1M count=10 2>/dev/null
    mkfs.fat -F 32 -n EFI "$efi_img" 2>/dev/null

    local efimnt="$BUILD_DIR/efi-mnt"
    mkdir -p "$efimnt"
    mount -o loop "$efi_img" "$efimnt"

    mkdir -p "$efimnt/EFI/BOOT" "$efimnt/boot/grub"

    # Build GRUB EFI
    local grub_modules="part_gpt part_msdos fat iso9660 normal boot linux configfile loopback chain efifwsetup efi_gop efi_uga search_label search_fs_uuid search"
    grub-mkimage \
        --format=x86_64-efi \
        --output="$efimnt/EFI/BOOT/BOOTX64.EFI" \
        --prefix="/boot/grub" \
        --modules="$grub_modules" \
        --compression=xz \
        /usr/lib/grub/x86_64-efi/kernel.img 2>/dev/null || \
        warn "GRUB EFI build failed — BIOS boot still works"

    cp "$STAGING/boot/grub/grub.cfg" "$efimnt/boot/grub/" 2>/dev/null || true
    cp -r "$STAGING/boot/grub/themes" "$efimnt/boot/grub/" 2>/dev/null || true
    cp "$STAGING/boot/vmlinuz" "$efimnt/boot/" 2>/dev/null || true
    cp "$STAGING/boot/initrd.img" "$efimnt/boot/" 2>/dev/null || true

    umount "$efimnt"
    rmdir "$efimnt"

    # Assemble ISO
    if [ -n "$isohdpfx" ] && [ -f "$STAGING/boot/isolinux/isolinux.bin" ]; then
        info "Building hybrid ISO (BIOS + UEFI)"
        xorriso -as mkisofs \
            -iso-level 3 \
            -full-iso9660-filenames \
            -volid "TenebraOS-$(date +%Y%m%d)" \
            -output "$ISO_OUTPUT" \
            -J -joliet-long \
            -rational-rock \
            -append_partition 2 0xef "$efi_img" \
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
            "$STAGING" 2>&1 | tee "$BUILD_DIR/iso-build.log"
    else
        warn "BIOS boot not available (isolinux missing) — UEFI only"
        xorriso -as mkisofs \
            -iso-level 3 \
            -full-iso9660-filenames \
            -volid "TenebraOS-$(date +%Y%m%d)" \
            -output "$ISO_OUTPUT" \
            -J -joliet-long \
            -rational-rock \
            -append_partition 2 0xef "$efi_img" \
            -appended_part_as_gpt \
            -eltorito-boot boot/grub/efi.img \
                -no-emul-boot \
                -isohybrid-gpt-basdat \
            "$STAGING" 2>&1 | tee "$BUILD_DIR/iso-build.log"
    fi

    [ -f "$ISO_OUTPUT" ] || err "ISO creation failed — check $BUILD_DIR/iso-build.log"
}

# ─── Summary ───────────────────────────────────────────────────────────────────
print_summary() {
    local iso_size
    iso_size="$(du -h "$ISO_OUTPUT" | cut -f1)"

    log "BUILD COMPLETE"
    echo ""
    echo "  ISO:    $ISO_OUTPUT"
    echo "  Size:   $iso_size"
    echo ""
    echo "  Boot to live desktop:"
    echo "    User: $LIVE_USER  Pass: $LIVE_PASS"
    echo ""
    echo "  Install options:"
    echo "    1. Calamares — double-click 'Install TenebraOS' on desktop"
    echo "    2. Manual    — run 'sudo tenebra-install' in terminal"
    echo ""
    echo "  Test in QEMU:"
    echo "    sudo qemu-system-x86_64 -m 4096 -smp 4 -enable-kvm \\"
    echo "      -cpu host -cdrom $ISO_OUTPUT -vga virtio -display gtk"
    echo ""
    echo "  Write to USB:"
    echo "    sudo dd if=$ISO_OUTPUT of=/dev/sdX bs=4M status=progress && sync"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    preflight
    bootstrap_rootfs
    mount_pseudo
    configure_rootfs
    setup_runit
    setup_live_user
    setup_calamares
    setup_manual_install
    setup_snapshots
    setup_branding
    create_squashfs
    setup_bootloaders
    build_iso
    print_summary
}

main "$@"
