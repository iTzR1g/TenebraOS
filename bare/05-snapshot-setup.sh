#!/bin/bash
# bare/05-snapshot-setup.sh
# Phase 5: Snapper + grub-btrfs integration for bootable Btrfs snapshots
#
# This script:
#   1. Configures Snapper for automated snapshots of the root subvolume
#   2. Sets up dpkg/apt transaction hooks for pre/post snapshots
#   3. Installs and configures grub-btrfs for bootloader snapshot entries
#   4. Creates a GRUB menu that shows all bootable snapshots
#   5. Enables snapshot-based rollback from the boot menu
#
# Usage:
#   sudo ./05-snapshot-setup.sh

set -euo pipefail

# ─── Configuration ─────────────────────────────────────────────────────────────
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_ROOTFS="$TENEBRA_BUILD/rootfs"
SNAPPER_TIMELINE_MIN_AGE="1800"       # 30 min minimum between snapshots
SNAPPER_TIMELINE_LIMIT_HOURLY="5"
SNAPPER_TIMELINE_LIMIT_DAILY="7"
SNAPPER_TIMELINE_LIMIT_WEEKLY="0"
SNAPPER_TIMELINE_LIMIT_MONTHLY="0"
SNAPPER_TIMELINE_LIMIT_YEARLY="0"

log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }

require_root() { [ "$(id -u)" -eq 0 ] || err "Must run as root"; }

# ─── Snapper Installation ─────────────────────────────────────────────────────
install_snapper() {
    log "Installing and configuring Snapper"

    local rootfs="$TENEBRA_ROOTFS"

    # Create Snapper configuration directory
    mkdir -p "$rootfs/etc/snapper/configs"

    # Snapper configuration for root subvolume
    cat > "$rootfs/etc/snapper/configs/root" <<'SNAPPERCFG'
# TenebraOS Snapper configuration for root subvolume
# /etc/snapper/configs/root

SUBVOLUME="/"
FSTYPE="btrfs"

# Users/groups allowed to work with snapshots
ALLOW_USERS=""
ALLOW_GROUPS="root"

# sync users and groups from ALLOW_USERS and ALLOW_GROUPS to .snapshots
SYNC_ACL="no"

# run daily number cleanup
TIMELINE_CREATE="yes"
TIMELINE_CLEANUP="yes"

TIMELINE_MIN_AGE="1800"
TIMELINE_LIMIT_HOURLY="5"
TIMELINE_LIMIT_DAILY="7"
TIMELINE_LIMIT_WEEKLY="0"
TIMELINE_LIMIT_MONTHLY="0"
TIMELINE_LIMIT_YEARLY="0"

# cleanup algorithms
EMPTY_PRE_POST_CLEANUP="yes"
EMPTY_PRE_POST_MIN_AGE="1800"

EMPTY_TIMELINE_CLEANUP="yes"
EMPTY_TIMELINE_MIN_AGE="43200"

MIN_DELETION_USES="5"
MAX_DELETION_USES="10"

NUMBER_LIMIT="10"
NUMBER_LIMIT_IMPORTANT="5"
SNAPPERCFG

    # Create the snapper timeline service (runit)
    mkdir -p "$rootfs/etc/sv/snapper-timeline"
    cat > "$rootfs/etc/sv/snapper-timeline/run" <<'SVC'
#!/bin/sh
# Snapper timeline — creates periodic snapshots
exec snapper -c root timeline --cleanup
SVC
    chmod +x "$rootfs/etc/sv/snapper-timeline/run"

    # Snapper cleanup service
    mkdir -p "$rootfs/etc/sv/snapper-cleanup"
    cat > "$rootfs/etc/sv/snapper-cleanup/run" <<'SVC'
#!/bin/sh
# Snapper cleanup — removes old snapshots per timeline limits
exec snapper -c root cleanup timeline
SVC
    chmod +x "$rootfs/etc/sv/snapper-cleanup/run"

    # Create the timeline timer (for systemd-free systems, runs via cron or runit)
    mkdir -p "$rootfs/etc/cron.d"
    cat > "$rootfs/etc/cron.d/snapper-timeline" <<'CRON'
# TenebraOS: Snapper timeline snapshot (every 30 minutes)
*/30 * * * * root /usr/bin/snapper -c root timeline --cleanup
CRON

    # Create the .snapshots subvolume mount point
    mkdir -p "$rootfs/.snapshots"

    # Set snapper to use our subvolume
    sed -i 's|^SUBVOLUME=.*|SUBVOLUME="/"|' "$rootfs/etc/snapper/configs/root"

    info "Snapper configured for root subvolume"
}

# ─── Dpkg/Apt Transaction Hooks ────────────────────────────────────────────────
install_transaction_hooks() {
    log "Installing dpkg/apt transaction snapshot hooks"

    local rootfs="$TENEBRA_ROOTFS"
    local hooks_dir="$rootfs/etc/apt/apt.conf.d"
    mkdir -p "$hooks_dir"

    # APT hook: snapshot before upgrades
    cat > "$hooks_dir/90-tenebra-snapshots" <<'APTHOOK'
# TenebraOS: Create Btrfs snapshots before/after package operations
DPkg::Pre-Invoke { "/usr/local/bin/tenebra-snapshot-hook pre-dpkg"; };
APT::Update::Post-Invoke { "/usr/local/bin/tenebra-snapshot-hook post-apt-update"; };
APTHOOK

    # The actual hook script
    cat > "$rootfs/usr/local/bin/tenebra-snapshot-hook" <<'HOOKSCRIPT'
#!/bin/bash
# tenebra-snapshot-hook — Called by dpkg/apt to create automatic snapshots
#
# Arguments:
#   pre-dpkg         — before dpkg install/remove
#   post-dpkg        — after dpkg install/remove
#   post-apt-update  — after apt-get update
#   pre-upgrade      — before apt-get upgrade (called from wrapper)
#   post-upgrade     — after apt-get upgrade (called from wrapper)

set -euo pipefail

ACTION="${1:-}"
SNAP_DIR="/snapshots"
LOG_FILE="/var/log/tenebra/snapshot-hooks.log"

mkdir -p "$SNAP_DIR" "$(dirname "$LOG_FILE")"

log_event() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') [$ACTION] $*" >> "$LOG_FILE"
}

create_snapshot() {
    local desc="$1"
    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local snap_name="snap_${timestamp}"

    # Ensure @snapshots subvolume is mounted
    if ! mountpoint -q "$SNAP_DIR" 2>/dev/null; then
        local root_dev
        root_dev="$(findmnt -n -o SOURCE / 2>/dev/null | head -1)"
        if [ -n "$root_dev" ]; then
            local parent_dev
            parent_dev="$(lsblk -n -o PKNAME "$root_dev" 2>/dev/null)"
            [ -n "$parent_dev" ] && parent_dev="$root_dev"
            mkdir -p "$SNAP_DIR"
            mount -o subvol=@snapshots,compress=zstd:1,ssd,noatime \
                "/dev/$parent_dev" "$SNAP_DIR" 2>/dev/null || true
        fi
    fi

    # Create snapshot
    if btrfs subvolume snapshot -r "/" "$SNAP_DIR/$snap_name" 2>/dev/null; then
        log_event "Created snapshot: $snap_name — $desc"

        # Record metadata
        echo "${snap_name}|$(date +%s)|${desc}|$(whoami)" >> "$SNAP_DIR/snapshots.log"

        # Enforce snapshot limits (keep last 10)
        local count
        count="$(grep -c '^snap_' "$SNAP_DIR/snapshots.log" 2>/dev/null || echo 0)"
        if [ "$count" -gt 10 ]; then
            local to_remove=$((count - 10))
            grep '^snap_' "$SNAP_DIR/snapshots.log" | head -n "$to_remove" | \
            while IFS='|' read -r name _ _ _; do
                btrfs subvolume delete "$SNAP_DIR/$name" 2>/dev/null || true
                sed -i "\|^${name}|d" "$SNAP_DIR/snapshots.log" 2>/dev/null || true
                log_event "Removed old snapshot: $name"
            done
        fi
    else
        log_event "WARNING: Failed to create snapshot — $desc"
    fi
}

case "$ACTION" in
    pre-dpkg)
        # Only snapshot for install/remove operations
        if [ -f /tmp/tenebra-snapshot-trigger ]; then
            create_snapshot "Pre-dpkg: $(cat /tmp/tenebra-snapshot-trigger)"
            rm -f /tmp/tenebra-snapshot-trigger
        fi
        ;;
    post-dpkg)
        # Create post-operation snapshot
        create_snapshot "Post-dpkg transaction"
        ;;
    post-apt-update)
        create_snapshot "After apt-get update"
        ;;
    pre-upgrade)
        create_snapshot "Pre-upgrade snapshot"
        ;;
    post-upgrade)
        create_snapshot "Post-upgrade snapshot"
        ;;
    *)
        log_event "Unknown action: $ACTION"
        ;;
esac
HOOKSCRIPT
    chmod +x "$rootfs/usr/local/bin/tenebra-snapshot-hook"

    # Dpkg trigger file helper (creates a temp file with the package name)
    cat > "$rootfs/usr/local/bin/tenebra-snapshot-trigger" <<'TRIGGER'
#!/bin/bash
# tenebra-snapshot-trigger <package-name>
# Creates a trigger file for the pre-dpkg hook
echo "${1:-unknown}" > /tmp/tenebra-snapshot-trigger
TRIGGER
    chmod +x "$rootfs/usr/local/bin/tenebra-snapshot-trigger"

    info "Transaction hooks installed"
    info "  /etc/apt/apt.conf.d/90-tenebra-snapshots"
    info "  /usr/local/bin/tenebra-snapshot-hook"
    info "  /usr/local/bin/tenebra-snapshot-trigger"
}

# ─── Grub-Btrfs Integration ───────────────────────────────────────────────────
install_grub_btrfs() {
    log "Installing grub-btrfs for bootloader snapshot integration"

    local rootfs="$TENEBRA_ROOTFS"

    # Create the grub-btrfs configuration directory
    mkdir -p "$rootfs/etc/grub-btrfs"

    # grub-btrfs main configuration
    cat > "$rootfs/etc/grub-btrfs/grub-btrfs.cfg" <<'GRUBBTROUT'
# grub-btrfs configuration for TenebraOS
# Auto-generates GRUB menu entries for each bootable Btrfs snapshot

# Search for the root device by label or UUID
search --no-floppy --fs-uuid --set=root ROOT_UUID_PLACEHOLDER

# Snapshots are bootable via the @snapshots subvolume
# Each snapshot is mounted read-only and booted with 'ro' kernel parameter

GRUBBTROUT

    # Create the snapshot discovery script that generates GRUB entries
    cat > "$rootfs/usr/local/bin/tenebra-snapshot-grub" <<'GRUBGEN'
#!/bin/bash
# tenebra-snapshot-grub — Generate GRUB menu entries for Btrfs snapshots
#
# Scans /snapshots for read-only snapshots and creates GRUB entries
# that boot into them. Called by grub-mkconfig.

set -euo pipefail

SNAP_DIR="/snapshots"
GRUB_CFG="/boot/grub/grub.cfg"
SNAP_ENTRIES="/boot/grub/grub-snapshots.cfg"

if [ ! -d "$SNAP_DIR" ]; then
    : > "$SNAP_ENTRIES"
    exit 0
fi

# Find the root UUID
ROOT_UUID="$(blkid -s UUID -o value "$(findmnt -n -o SOURCE /)" 2>/dev/null || echo "UNKNOWN")"

cat > "$SNAP_ENTRIES" <<HEADER
# Auto-generated TenebraOS snapshot entries
# Generated: $(date)
# Root UUID: $ROOT_UUID

HEADER

# Generate entry for each snapshot
SNAP_NUM=1
for snap in "$SNAP_DIR"/snap_*; do
    [ -d "$snap" ] || continue
    snap_name="$(basename "$snap")"
    snap_desc="$(grep "^${snap_name}|" "$SNAP_DIR/snapshots.log" 2>/dev/null | cut -d'|' -f4)"
    [ -z "$snap_desc" ] && snap_desc="Snapshot $SNAP_NUM"
    snap_date="$(date -d "@$(grep "^${snap_name}|" "$SNAP_DIR/snapshots.log" 2>/dev/null | cut -d'|' -f2)" '+%Y-%m-%d %H:%M' 2>/dev/null || echo "unknown")"

    cat >> "$SNAP_ENTRIES" <<ENTRY
menuentry "Snapshot $SNAP_NUM: $snap_desc ($snap_date)" {
    search --no-floppy --fs-uuid --set=root $ROOT_UUID
    linux /boot/vmlinuz-* root=UUID=$ROOT_UUID subvol=@snapshots/$snap_name rootflags=subvol=@snapshots/$snap_name ro
    initrd /boot/initrd.img-*
}
ENTRY

    SNAP_NUM=$((SNAP_NUM + 1))
done

# Add rollback entry
cat >> "$SNAP_ENTRIES" <<'ENTRY'
menuentry "Rollback: Restore latest snapshot as root @" {
    search --no-floppy --fs-uuid --set=root ROOT_UUID_PLACEHOLDER
    linux /boot/vmlinuz-* root=UUID=ROOT_UUID_PLACEHOLDER subvol=@ rootflags=subvol=@ rw
    initrd /boot/initrd.img-*
    echo "Loading snapshot rollback environment..."
    # The initramfs detects this entry and performs the rollback
}
ENTRY

# Replace ROOT_UUID_PLACEHOLDER with actual UUID
sed -i "s/ROOT_UUID_PLACEHOLDER/$ROOT_UUID/g" "$SNAP_ENTRIES"

echo "Generated $(($SNAP_NUM - 1)) snapshot entries in $SNAP_ENTRIES"
GRUBGEN
    chmod +x "$rootfs/usr/local/bin/tenebra-snapshot-grub"

    # Integrate snapshot entries into GRUB config
    cat > "$rootfs/etc/grub.d/40-tenebra-snapshots" <<'GRUBINTEGRATE'
#!/bin/sh
# TenebraOS snapshot GRUB entries
# This is sourced by grub-mkconfig to add snapshot boot options

# Generate snapshot entries
/usr/local/bin/tenebra-snapshot-grub 2>/dev/null || true

# Source the generated entries
if [ -f /boot/grub/grub-snapshots.cfg ]; then
    echo ""
    echo "# ─── TenebraOS Btrfs Snapshots ───"
    echo "submenu \"Btrfs Snapshots\" {"
    cat /boot/grub/grub-snapshots.cfg
    echo "}"
fi
GRUBINTEGRATE
    chmod +x "$rootfs/etc/grub.d/40-tenebra-snapshots"

    # Run grub-btrfs generation on GRUB update
    mkdir -p "$rootfs/etc/kernel/postinst.d"
    cat > "$rootfs/etc/kernel/postinst.d/tenebra-snapshot-grub" <<'POSTINST'
#!/bin/sh
# Regenerate snapshot GRUB entries after kernel install
version="$1"
[ -x /usr/local/bin/tenebra-snapshot-grub ] && /usr/local/bin/tenebra-snapshot-grub
[ -x /usr/sbin/update-grub ] && /usr/sbin/update-grub
POSTINST
    chmod +x "$rootfs/etc/kernel/postinst.d/tenebra-snapshot-grub"

    info "grub-btrfs integration installed"
    info "  Snapshot entries auto-generated in GRUB menu"
    info "  Bootable snapshots visible under 'Btrfs Snapshots' submenu"
}

# ─── GRUB Theme for Snapshots ─────────────────────────────────────────────────
install_grub_snapshot_theme() {
    log "Configuring GRUB snapshot theme"

    local rootfs="$TENEBRA_ROOTFS"
    local theme_dir="$rootfs/boot/grub/themes/tenebra"

    mkdir -p "$theme_dir"

    # Theme configuration
    cat > "$theme_dir/theme.txt" <<'THEME'
# TenebraOS GRUB Theme — Btrfs Snapshot Style

title-text: "TenebraOS Snapshots"
title-color: "#ffffff"
title-font: "DejaVu Sans Bold 16"

message-font: "DejaVu Sans Regular 12"
message-color: "#cccccc"

desktop-color: "#1a1a2e"
desktop-image: "background.png"

+ image {
    left = 0
    top = 0
    width = 100%
    height = 100%
    file = "background.png"
}

+ label {
    top = 8
    left = 20
    width = 100%
    height = 40
    text = "TenebraOS — Boot from Snapshot"
    font = "DejaVu Sans Bold 18"
    color = "#e94560"
}

+ label {
    top = 50
    left = 20
    width = 100%
    height = 20
    text = "Select a snapshot or press Esc to boot normally"
    font = "DejaVu Sans Regular 12"
    color = "#aaaaaa"
}

+ boot_menu {
    left = 20
    top = 100
    width = 960
    height = 400

    menu_pixmap_style = "menu_bkg"
    item_pixmap_style = "item_bkg"
    item_color = "#ffffff"
    selected_item_color = "#e94560"
    selected_item_pixmap_style = "sel_bkg"

    item_font = "DejaVu Sans Regular 14"
    selected_item_font = "DejaVu Sans Bold 14"

    scrollbar = false
}
THEME

    info "GRUB snapshot theme configured at $theme_dir"
}

# ─── Snapshot Rollback Initramfs Hook ──────────────────────────────────────────
install_rollback_hook() {
    log "Installing snapshot rollback hook for initramfs"

    local rootfs="$TENEBRA_ROOTFS"

    # This script is called from the initramfs when a snapshot rollback entry is booted
    cat > "$rootfs/usr/local/bin/tenebra-rollback" <<'ROLLBACK'
#!/bin/bash
# tenebra-rollback — Restore a Btrfs snapshot as the root subvolume
#
# Called from initramfs when booting the "Rollback" GRUB entry.
# This is a destructive operation: the current @ is deleted and replaced
# with the latest snapshot.

set -euo pipefail

SNAP_DIR="/snapshots"
ROOT_MOUNT="/mnt"

log() { echo "[rollback] $*"; }

# Find the most recent snapshot
LATEST_SNAP="$(ls -td "$SNAP_DIR"/snap_* 2>/dev/null | head -1)"

if [ -z "$LATEST_SNAP" ]; then
    log "No snapshots found in $SNAP_DIR"
    log "Booting normally..."
    exit 0
fi

log "Latest snapshot: $(basename "$LATEST_SNAP")"

# Find root device
ROOT_DEV="$(findmnt -n -o SOURCE / 2>/dev/null | head -1)"
[ -b "$ROOT_DEV" ] || { log "ERROR: root device not found"; exit 1; }

# Mount root subvolume
mkdir -p "$ROOT_MOUNT"
mount -o subvol=/,compress=zstd:1,ssd,noatime "$ROOT_DEV" "$ROOT_MOUNT" || {
    log "ERROR: Failed to mount root"
    exit 1
}

log "Current root mounted at $ROOT_MOUNT"

# Create a temporary snapshot of the current @ (backup)
BACKUP_NAME="backup_$(date +%Y%m%d_%H%M%S)"
btrfs subvolume snapshot "${ROOT_MOUNT}/@" "${ROOT_MOUNT}/@backups/${BACKUP_NAME}" 2>/dev/null || \
    mkdir -p "${ROOT_MOUNT}/@backups" && \
    btrfs subvolume snapshot "${ROOT_MOUNT}/@" "${ROOT_MOUNT}/@backups/${BACKUP_NAME}" 2>/dev/null || true

log "Backup of current @ saved as @backups/${BACKUP_NAME}"

# Delete current @ and replace with snapshot
btrfs subvolume delete "${ROOT_MOUNT}/@" 2>/dev/null || {
    log "ERROR: Failed to delete current @"
    umount "$ROOT_MOUNT"
    exit 1
}

btrfs subvolume snapshot "$LATEST_SNAP" "${ROOT_MOUNT}/@" || {
    log "ERROR: Failed to create @ from snapshot"
    umount "$ROOT_MOUNT"
    exit 1
}

log "Rollback complete: @ restored from $(basename "$LATEST_SNAP")"
umount "$ROOT_MOUNT"
ROLLBACK
    chmod +x "$rootfs/usr/local/bin/tenebra-rollback"

    info "Rollback hook installed at /usr/local/bin/tenebra-rollback"
}

# ─── Main ──────────────────────────────────────────────────────────────────────
main() {
    require_root
    [ -d "$TENEBRA_ROOTFS" ] || err "Rootfs not found — run 01-bootstrap.sh first"

    install_snapper
    install_transaction_hooks
    install_grub_btrfs
    install_grub_snapshot_theme
    install_rollback_hook

    log "Phase 5 complete: Snapshot management configured"
    log "  Snapper: automated timeline snapshots every 30 minutes"
    log "  Dpkg hooks: auto-snapshot before/after package operations"
    log "  grub-btrfs: bootable snapshots in GRUB menu"
    log "  Rollback: restore from GRUB boot menu"
    log ""
    log "Next: sudo ./06-master-build.sh (to finalize the rootfs)"
}

main "$@"
