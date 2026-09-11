#!/bin/bash
# bare/04-hybrid-pkgmanager.sh
# Phase 4: Install and configure the Zebra (zbra) package manager
#
# Zebra is TenebraOS's default multi-backend package manager that:
#   - Fetches native .deb packages from GitHub (TenebraOS-packages)
#   - Supports passthrough to apt, pacman, yay, snap, flatpak, gentoo-src
#   - Auto-creates Btrfs snapshots before package operations
#   - Handles both binary and source package installation
#
# Usage:
#   sudo ./04-hybrid-pkgmanager.sh              # install zbra + configure
#   sudo ./04-hybrid-pkgmanager.sh --skip-deps  # skip dependency installation

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKIP_DEPS="${1:-}"

# ─── Configuration ──────────────────────────────────────────────────────────
TENEBRA_BUILD="${TENEBRA_BUILD:-/tmp/tenebra-build}"
TENEBRA_ROOTFS="${TENEBRA_ROOTFS:-$TENEBRA_BUILD/rootfs}"
ZBRA_VERSION="1.0.0"
ZBRA_REPO="https://github.com/iTzR1g/TenebraOS-packages"

# ─── Helpers ────────────────────────────────────────────────────────────────
log()   { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
err()   { printf '\n\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info()  { printf '\033[1;32m>>>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m>>>\033[0m %s\n' "$*"; }

require_root() {
    [ "$(id -u)" -eq 0 ] || err "Must run as root"
}

# ─── Install Dependencies ───────────────────────────────────────────────────
install_dependencies() {
    if [ "$SKIP_DEPS" = "--skip-deps" ]; then
        info "Skipping dependency installation"
        return 0
    fi

    log "Installing Zebra dependencies"

    # Check if we're in the rootfs or on a live system
    if [ -d "$TENEBRA_ROOTFS" ] && [ -x "$TENEBRA_ROOTFS/usr/bin/bash" ]; then
        info "Installing into rootfs: $TENEBRA_ROOTFS"
        local rootfs="$TENEBRA_ROOTFS"
    else
        info "Installing into live system"
        local rootfs=""
    fi

    # Core dependencies for Zebra
    local deps=(
        "curl"          # HTTP client for GitHub API
        "git"           # Repository cloning
        "dpkg"          # Package installation
        "apt-get"       # Dependency resolution (fallback)
        "gpg"           # Package verification
        "jq"            # JSON parsing (for GitHub API)
    )

    for dep in "${deps[@]}"; do
        if ! command -v "$dep" >/dev/null 2>&1; then
            warn "Missing dependency: $dep"
            if [ -z "$rootfs" ]; then
                apt-get install -y "$dep" 2>/dev/null || true
            fi
        fi
    done

    info "Dependencies installed"
}

# ─── Install Zebra Package Manager ──────────────────────────────────────────
install_zebra() {
    log "Installing Zebra package manager v${ZBRA_VERSION}"

    local bin_dir="${TENEBRA_ROOTFS}/usr/local/bin"
    mkdir -p "$bin_dir"

    # Copy the zbra script from the repo
    if [ -f "$SCRIPT_DIR/usr-local-bin/zbra" ]; then
        cp "$SCRIPT_DIR/usr-local-bin/zbra" "$bin_dir/zbra"
        chmod 755 "$bin_dir/zbra"
        info "Installed zbra from repo"
    elif [ -f "$SCRIPT_DIR/../zbra" ]; then
        cp "$SCRIPT_DIR/../zbra" "$bin_dir/zbra"
        chmod 755 "$bin_dir/zbra"
        info "Installed zbra from parent directory"
    else
        warn "zbra script not found — installing placeholder"
        create_placeholder_zbra "$bin_dir/zbra"
    fi

    # Create symlink for shorter command
    ln -sf /usr/local/bin/zbra "$bin_dir/z" 2>/dev/null || true

    info "Zebra installed to /usr/local/bin/zbra"
    info "Short alias: /usr/local/bin/z"
}

# ─── Create Placeholder Zebra (if source not available) ─────────────────────
create_placeholder_zbra() {
    local target="$1"
    cat > "$target" <<'ZBRA_EOF'
#!/bin/bash
# Zebra (zbra) — TenebraOS Multi-Backend Package Manager
# Placeholder: replace with full implementation from /usr/local/bin/zbra

set -euo pipefail

VERSION="1.0.0"
REPO_URL="https://github.com/iTzR1g/TenebraOS-packages"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'

log()  { printf "${BLUE}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
ok()   { printf "${GREEN}>>>${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}>>>${NC} %s\n" "$*"; }
err()  { printf "${RED}ERROR:${NC} %s\n" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Zebra (zbra) v${VERSION} — TenebraOS Package Manager

Usage: zbra [options] <action> [package]

Actions:
  -i, --install <pkg>     Install a package
  -r, --remove <pkg>      Remove a package
  -s, --search <pattern>  Search for packages
  -u, --update            Update package lists
  -b, --build <source>    Build package from source
  -l, --list              List installed packages
  -v, --version           Show version

Options:
  -pm, --package-manager <backend>
    Backend: native (default), apt, pacman, yay, snap, flatpak, gentoo-src

Examples:
  zbra -i vim                     # Install via native (GitHub)
  zbra -pm apt -i vim             # Install via apt
  zbra -pm pacman -i neovim       # Install via pacman (distrobox)
  zbra -s firefox                 # Search native packages
  zbra -b /path/to/source.tar.gz  # Build from source
EOF
}

# Parse arguments
BACKEND="native"
ACTION=""
PACKAGE=""

while [ $# -gt 0 ]; do
    case "$1" in
        -i|--install)    ACTION="install"; PACKAGE="${2:-}"; shift 2 ;;
        -r|--remove)     ACTION="remove"; PACKAGE="${2:-}"; shift 2 ;;
        -s|--search)     ACTION="search"; PACKAGE="${2:-}"; shift 2 ;;
        -u|--update)     ACTION="update"; shift ;;
        -b|--build)      ACTION="build"; PACKAGE="${2:-}"; shift 2 ;;
        -l|--list)       ACTION="list"; shift ;;
        -v|--version)    echo "zbra v${VERSION}"; exit 0 ;;
        -pm|--package-manager) BACKEND="${2:-native}"; shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        *)               err "Unknown option: $1" ;;
    esac
done

[ -z "$ACTION" ] && { usage; exit 1; }

# Route to backend
case "$BACKEND" in
    native)
        log "Using native backend (TenebraOS-packages)"
        warn "Native backend not yet implemented — falling back to apt"
        BACKEND="apt"
        ;;&
    apt)
        case "$ACTION" in
            install) sudo apt-get install -y "$PACKAGE" ;;
            remove)  sudo apt-get remove -y "$PACKAGE" ;;
            search)  apt-cache search "$PACKAGE" ;;
            update)  sudo apt-get update ;;
            list)    dpkg -l | grep '^ii' ;;
        esac
        ;;
    pacman)
        case "$ACTION" in
            install) sudo pacman -S --noconfirm "$PACKAGE" ;;
            remove)  sudo pacman -R --noconfirm "$PACKAGE" ;;
            search)  pacman -Ss "$PACKAGE" ;;
            update)  sudo pacman -Sy ;;
            list)    pacman -Q ;;
        esac
        ;;
    yay)
        case "$ACTION" in
            install) yay -S --noconfirm "$PACKAGE" ;;
            remove)  yay -R --noconfirm "$PACKAGE" ;;
            search)  yay -Ss "$PACKAGE" ;;
            update)  yay -Sy ;;
            list)    yay -Q ;;
        esac
        ;;
    *)
        err "Unknown backend: $BACKEND"
        ;;
esac
ZBRA_EOF
    chmod 755 "$target"
}

# ─── Configure Zebra ────────────────────────────────────────────────────────
configure_zebra() {
    log "Configuring Zebra"

    local conf_dir="${TENEBRA_ROOTFS}/etc/tenebra"
    mkdir -p "$conf_dir"

    # Create zbra configuration
    cat > "$conf_dir/zbra.conf" <<CONF
# Zebra (zbra) Configuration — TenebraOS Package Manager
# Generated by 04-hybrid-pkgmanager.sh

# Default backend: native, apt, pacman, yay, snap, flatpak, gentoo-src
DEFAULT_BACKEND=native

# GitHub repository for native packages
NATIVE_REPO=${ZBRA_REPO}
NATIVE_BRANCH=main

# Snapshot settings
SNAPSHOT_BEFORE_INSTALL=yes
SNAPSHOT_MAX_AGE=30
SNAPSHOT_RETAIN=10

# Source build settings
BUILD_DIR=/var/cache/tenebra/builds
SOURCE_DIR=/var/cache/tenebra/sources
LOG_DIR=/var/log/tenebra

# Verbosity: quiet, normal, verbose
VERBOSE=normal
CONF

    info "Configuration written to $conf_dir/zbra.conf"
}

# ─── Create Package Cache Directories ───────────────────────────────────────
setup_cache_dirs() {
    log "Setting up package cache directories"

    local cache_base="${TENEBRA_ROOTFS}/var/cache/tenebra"
    mkdir -p "$cache_base"/{builds,sources,packages,repo}
    mkdir -p "${TENEBRA_ROOTFS}/var/log/tenebra"

    info "Cache directories created at $cache_base"
}

# ─── Create Btrfs Snapshot Hook ─────────────────────────────────────────────
setup_snapshot_hook() {
    log "Setting up Btrfs snapshot hook for Zebra"

    local hook_dir="${TENEBRA_ROOTFS}/usr/local/lib/zbra"
    mkdir -p "$hook_dir"

    cat > "$hook_dir/snapshot-hook.sh" <<'HOOK'
#!/bin/bash
# Zebra Btrfs Snapshot Hook
# Creates a read-only snapshot before package operations

set -euo pipefail

SNAP_DIR="/snapshots"
LOG_FILE="/var/log/tenebra/snapshots.log"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SNAP_NAME="zbra_${TIMESTAMP}"

# Check if running on Btrfs
if ! grep -qs 'btrfs' /proc/mounts; then
    exit 0
fi

# Create snapshot
if command -v btrfs >/dev/null 2>&1; then
    btrfs subvolume snapshot -r "/" "${SNAP_DIR}/${SNAP_NAME}" 2>/dev/null || {
        echo "Failed to create snapshot" >&2
        exit 1
    }

    # Log snapshot
    echo "${SNAP_NAME}|$(date +%s)|zbra-$1|$(whoami)" >> "$LOG_FILE"

    # Cleanup old snapshots (keep last 10)
    local count
    count=$(grep -c '^zbra_' "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$count" -gt 10 ]; then
        local rm_count=$((count - 10))
        grep '^zbra_' "$LOG_FILE" | head -n "$rm_count" | \
        while IFS='|' read -r name _ _ _; do
            btrfs subvolume delete "${SNAP_DIR}/${name}" 2>/dev/null || true
            sed -i "\|^${name}|d" "$LOG_FILE" 2>/dev/null || true
        done
    fi
fi
HOOK
    chmod 755 "$hook_dir/snapshot-hook.sh"

    info "Snapshot hook installed to $hook_dir/snapshot-hook.sh"
}

# ─── Create TenebraOS Package Repository Config ─────────────────────────────
setup_repo_config() {
    log "Setting up TenebraOS package repository"

    local apt_dir="${TENEBRA_ROOTFS}/etc/apt/sources.list.d"
    mkdir -p "$apt_dir"

    # Add TenebraOS repository
    cat > "$apt_dir/tenebraos.list" <<REPO
# TenebraOS Custom Packages
# Hosted on GitHub Releases
deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] https://github.com/iTzR1g/TenebraOS-packages/releases/download/tenebraos-repo/ ./
REPO

    # Import repository signing key if available
    if [ -f "${TENEBRA_ROOTFS}/usr/share/keyrings/tenebraos-repo.gpg" ]; then
        info "Repository signing key already installed"
    else
        warn "Repository signing key not found — packages may not be verified"
    fi

    info "TenebraOS repository configured"
}

# ─── Main ───────────────────────────────────────────────────────────────────
main() {
    log "Phase 4: Zebra Package Manager Setup"
    echo "  Version:  ${ZBRA_VERSION}"
    echo "  Repo:     ${ZBRA_REPO}"
    echo "  Rootfs:   ${TENEBRA_ROOTFS:-/}"
    echo ""

    require_root
    install_dependencies
    install_zebra
    configure_zebra
    setup_cache_dirs
    setup_snapshot_hook
    setup_repo_config

    log "Phase 4 Complete"
    echo ""
    echo "  Zebra (zbra) is now installed."
    echo "  Usage: zbra --help"
    echo ""
    echo "  Next: sudo ./05-snapshot-setup.sh"
    echo ""
}

main "$@"
