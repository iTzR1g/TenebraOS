#!/bin/bash
# build-tenebra-kernel.sh — Standalone TenebraOS kernel build script
#
# Fetches mainline Linux kernel, applies Apple T2 + CachyOS patches,
# compiles, and outputs standard Debian packages (.deb).
#
# Variants:
#   tenebra-core   — Stock kernel with CachyOS performance patches
#   tenebra-t2     — Core + Apple T2 hardware support
#   tenebra-cachy  — Core + full CachyOS tuning (BORE, BBRv3, -O3)
#
# Usage:
#   sudo ./build-tenebra-kernel.sh                          # default: bore
#   sudo ./build-tenebra-kernel.sh --variant tenebra-t2     # T2 variant
#   sudo ./build-tenebra-kernel.sh --version 6.18 --jobs 8
#   KERNEL_VERSION=6.18 CACHYOS_SCHED=eevdf ./build-tenebra-kernel.sh
#
# Output: ../repo/pool/linux-image-*.deb, linux-headers-*.deb

set -euo pipefail

# ─── Defaults ────────────────────────────────────────────────────────────────
KERNEL_VERSION="${KERNEL_VERSION:-7.2}"
PKGREL="${PKGREL:-1}"
CODENAME="${CODENAME:-tenebra}"
CACHYOS_SCHED="${CACHYOS_SCHED:-bore}"
VARIANT="${VARIANT:-tenebra-cachy}"
JOBS="${JOBS:-$(nproc)}"
OUTPUT_DIR="${OUTPUT_DIR:-$(cd "$(dirname "$0")" && pwd)/repo/pool}"
KEEP_WORK="${KEEP_WORK:-0}"

KERNEL_GIT="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
KERNEL_GIT_ALT="https://cdn.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
T2_PATCHES="https://github.com/t2linux/linux-t2-patches.git"
CACHY_PATCHES="https://github.com/CachyOS/kernel-patches.git"
T2_CONFIG="https://raw.githubusercontent.com/t2linux/T2-Debian-and-Ubuntu-Kernel/Mainline/templates/default-config-debian"

# ─── Colors ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'

log()  { printf "\n${BLUE}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
ok()   { printf "${GREEN}>>>${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}>>>${NC} %s\n" "$*"; }
err()  { printf "${RED}ERROR:${NC} %s\n" "$*" >&2; exit 1; }

# ─── Parse args ──────────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --variant)   VARIANT="$2"; shift 2 ;;
        --version)   KERNEL_VERSION="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        --scheduler) CACHYOS_SCHED="$2"; shift 2 ;;
        --keep)      KEEP_WORK=1; shift ;;
        --help|-h)
            cat <<EOF
TenebraOS Kernel Builder

Usage: sudo $0 [OPTIONS]

Options:
  --variant <name>    Kernel variant: tenebra-core, tenebra-t2, tenebra-cachy
  --version <ver>     Kernel version (default: $KERNEL_VERSION)
  --jobs <n>          Parallel jobs (default: $(nproc))
  --scheduler <name>  Scheduler: bore, eevdf, bmq (default: $CACHYOS_SCHED)
  --keep              Keep build directory after completion

Environment:
  KERNEL_VERSION      Same as --version
  CACHYOS_SCHED       Same as --scheduler
  CODENAME            Package version suffix (default: tenebra)
  PKGREL              Package release number (default: 1)
  OUTPUT_DIR          Output directory (default: repo/pool/)

Examples:
  sudo $0                                        # Default: bore scheduler
  sudo $0 --variant tenebra-t2                   # Apple T2 support
  sudo $0 --variant tenebra-cachy --version 6.18 # Specific version
  sudo $0 --scheduler eevdf --jobs 8             # EEVDF scheduler, 8 threads
EOF
            exit 0
            ;;
        *) err "Unknown option: $1 (use --help)" ;;
    esac
done

# ─── Validate variant ────────────────────────────────────────────────────────
case "$VARIANT" in
    tenebra-core|tenebra-t2|tenebra-cachy) ;;
    *) err "Invalid variant: $VARIANT (use tenebra-core, tenebra-t2, or tenebra-cachy)" ;;
esac

# ─── Preflight ───────────────────────────────────────────────────────────────
[ "$(id -u)" -eq 0 ] || err "Must run as root (or use sudo)"

for cmd in make gcc patch curl git bc perl; do
    command -v "$cmd" >/dev/null 2>&1 || err "Missing: $cmd"
done

mkdir -p "$OUTPUT_DIR"

# ─── Work directory ──────────────────────────────────────────────────────────
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tenebra-kern-XXXXXX")"
if [ "$KEEP_WORK" = "1" ]; then
    trap 'echo "Work directory preserved: $WORK"' EXIT
else
    trap 'rm -rf "$WORK"' EXIT
fi

log "TenebraOS Kernel Builder"
echo "  Variant:    $VARIANT"
echo "  Version:    $KERNEL_VERSION"
echo "  Scheduler:  $CACHYOS_SCHED"
echo "  Jobs:       $JOBS"
echo "  Output:     $OUTPUT_DIR"
echo "  Work:       $WORK"

# ─── [1/7] Clone kernel source ──────────────────────────────────────────────
log "[1/7] Cloning kernel v${KERNEL_VERSION}..."
cd "$WORK"
git clone -q --depth 1 --single-branch --branch "v${KERNEL_VERSION}" \
    "${KERNEL_GIT}" "$WORK/linux" 2>/dev/null || \
git clone -q --depth 1 --single-branch --branch "v${KERNEL_VERSION}" \
    "${KERNEL_GIT_ALT}" "$WORK/linux"
cd "$WORK/linux"
ok "Kernel source: $(make kernelversion)"

# ─── [2/7] Fetch patches ────────────────────────────────────────────────────
log "[2/7] Fetching T2 + CachyOS patches..."
mkdir -p "$WORK/patches"

# Apple T2 patches (always include for tenebra-t2 and tenebra-cachy)
T2_COUNT=0
if [[ "$VARIANT" == *"t2"* ]] || [[ "$VARIANT" == *"cachy"* ]]; then
    git clone -q --depth 1 "${T2_PATCHES}" "$WORK/src-t2" 2>/dev/null || true
    if [ -d "$WORK/src-t2" ]; then
        cp "$WORK/src-t2"/*.patch "$WORK/patches/" 2>/dev/null || true
        T2_COUNT=$(find "$WORK/patches" -name '*.patch' | wc -l)
    fi
fi
ok "T2 patches: $T2_COUNT"

# CachyOS patches (scheduler + performance tuning)
CACHY_COUNT=0
SCHED_PATCH=""
if [[ "$VARIANT" == *"cachy"* ]] || [[ "$CACHYOS_SCHED" != "stock" ]]; then
    git clone -q --depth 1 "${CACHY_PATCHES}" "$WORK/src-cachy" 2>/dev/null || true
    if [ -d "$WORK/src-cachy" ]; then
        for d in "${KERNEL_VERSION}" $(ls -1 "$WORK/src-cachy" | grep -E '^[0-9]+(\.[0-9]+)*$' | sort -rV); do
            if [ "$CACHYOS_SCHED" = "bmq" ]; then
                [ -f "$WORK/src-cachy/$d/sched/0001-prjc-cachy.patch" ] && \
                    SCHED_PATCH="$WORK/src-cachy/$d/sched/0001-prjc-cachy.patch" && break
            else
                for name in 0001-bore-cachy.patch 0001-bore.patch; do
                    [ -f "$WORK/src-cachy/$d/sched/$name" ] && \
                        SCHED_PATCH="$WORK/src-cachy/$d/sched/$name" && break 2
                done
            fi
        done
        if [ -n "$SCHED_PATCH" ]; then
            cp "$SCHED_PATCH" "$WORK/patches/"
            CACHY_COUNT=$(find "$WORK/patches" -name '*.patch' | wc -l)
            ok "Scheduler patch: $(basename "$SCHED_PATCH")"
        fi
    fi
fi
ok "Total patches: $(find "$WORK/patches" -name '*.patch' | wc -l)"

# ─── [3/7] Apply patches ───────────────────────────────────────────────────
log "[3/7] Applying patches..."
FAILED=0
CONFLICTS=""
for p in $(find "$WORK/patches" -name '*.patch' | sort); do
    pname=$(basename "$p")
    if patch -p1 -N --dry-run < "$p" >/dev/null 2>&1; then
        patch -s -p1 -N < "$p" && ok "Applied: $pname" || { warn "FAILED: $pname"; FAILED=1; CONFLICTS="$CONFLICTS $pname"; }
    elif patch -p1 -N --dry-run -R < "$p" >/dev/null 2>&1; then
        ok "Already applied: $pname"
    else
        warn "CONFLICT: $pname"
        patch -p1 -N --dry-run < "$p" 2>&1 | grep -E "^error|FAILED|hunk" | head -5 | sed 's/^/        /'
        FAILED=1
        CONFLICTS="$CONFLICTS $pname"
    fi
done
if [ "$FAILED" != "0" ]; then
    err "Patch conflicts detected:$CONFLICTS"
fi

# ─── [4/7] Configure kernel ────────────────────────────────────────────────
log "[4/7] Configuring kernel..."

# Base config: T2 config if T2 variant, otherwise defconfig
if [[ "$VARIANT" == *"t2"* ]] || [[ "$VARIANT" == *"cachy"* ]]; then
    if curl -fsSL "$T2_CONFIG" -o .config 2>/dev/null; then
        ok "Base config: t2linux default-config-debian"
    else
        warn "Config download failed, using defconfig"
        make defconfig
    fi
else
    make defconfig
    ok "Base config: defconfig"
fi

# Strip debug info for smaller package
scripts/config \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --set-str SYSTEM_REVOCATION_KEYS ""

# T2 driver stack (only for T2 variants)
if [[ "$VARIANT" == *"t2"* ]] || [[ "$VARIANT" == *"cachy"* ]]; then
    scripts/config \
        --module T2BCE_CORE --module T2BCE_VHCI --module T2BCE_AUDIO --module T2BCE_DMA \
        --module HID_APPLETB_BL --module HID_APPLETB_KBD --module DRM_APPLETBDRM \
        --module BT_HCIBCM4377 --module APFS_FS \
        --enable MODULE_FORCE_UNLOAD \
        --module APPLE_GMUX --module SENSORS_APPLESMC \
        --module HID_APPLE --module HID_MAGICMOUSE \
        --module BRCMFMAC
    ok "T2 drivers enabled"
fi

# Scheduler selection
case "$CACHYOS_SCHED" in
    bore)  scripts/config --enable SCHED_BORE ;;
    bmq)   scripts/config --enable SCHED_ALT --enable SCHED_BMQ ;;
    eevdf) scripts/config --disable SCHED_BORE --disable SCHED_ALT ;;
    *)     scripts/config --enable SCHED_BORE ;;
esac
ok "Scheduler: $CACHYOS_SCHED"

# CachyOS performance tuning (for tenebra-cachy variant)
if [[ "$VARIANT" == *"cachy"* ]]; then
    scripts/config \
        --disable CC_OPTIMIZE_FOR_PERFORMANCE --enable CC_OPTIMIZE_FOR_PERFORMANCE_O3 \
        --enable PREEMPT --disable PREEMPT_LAZY --disable PREEMPT_VOLUNTARY \
        --enable HZ_1000 --disable HZ_250 --set-val HZ 1000 \
        --enable SCHED_AUTOGROUP \
        --enable LRU_GEN --enable LRU_GEN_ENABLED \
        --enable ZSWAP --set-val ZSWAP_COMPRESSOR_DEFAULT zstd \
        --module ZRAM --enable ZRAM_WRITEBACK \
        --enable TRANSPARENT_HUGEPAGE --enable TRANSPARENT_HUGEPAGE_ALWAYS \
        --module MQ_IOSCHED_BFQ --enable BFQ_GROUP_IOSCHED \
        --module MQ_IOSCHED_KYBER \
        --module TCP_CONG_BBR \
        --enable X86_AMD_PSTATE --enable CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
    ok "CachyOS performance tuning enabled (-O3, BBRv3, BORE)"
fi

# Slim the package
scripts/config \
    --disable DEBUG_INFO --disable DEBUG_INFO_DWARF4 --disable DEBUG_INFO_DWARF5 \
    --disable DEBUG_INFO_BTF --disable DEBUG_INFO_BTF_MODULES \
    --disable GDB_SCRIPTS

make olddefconfig

# ─── [5/7] Build packages ───────────────────────────────────────────────────
log "[5/7] Building .deb packages (${JOBS} threads, 30-90 min)..."
LOG="$OUTPUT_DIR/build-${VARIANT}.log"
if ! make -j"$JOBS" deb-pkg \
        LOCALVERSION="-${PKGREL}-${VARIANT}" \
        KDEB_PKGVERSION="$(make kernelversion)-${PKGREL}" \
        2>&1 | tee "$LOG"; then
    err "Build failed — see $LOG"
fi

# ─── [6/7] Rename packages ──────────────────────────────────────────────────
log "[6/7] Renaming packages..."
for deb in "$WORK"/*.deb; do
    [ -f "$deb" ] || continue
    base=$(basename "$deb")
    # Rename to include variant name
    newname=$(echo "$base" | sed "s/${PKGREL}-tenebra/${PKGREL}-${VARIANT}/")
    if [ "$base" != "$newname" ]; then
        mv "$deb" "$OUTPUT_DIR/$newname"
    else
        cp "$deb" "$OUTPUT_DIR/"
    fi
done

# ─── [7/7] Summary ──────────────────────────────────────────────────────────
log "[7/7] Build complete!"
echo ""
echo "  Kernel:    $(cd "$WORK/linux" && make kernelversion)"
echo "  Variant:   $VARIANT"
echo "  Scheduler: $CACHYOS_SCHED"
echo ""
echo "  Packages:"
ls -lh "$OUTPUT_DIR"/linux-*"${VARIANT}"*.deb 2>/dev/null | sed 's/^/    /'
echo ""
echo "  Publish to TenebraOS repo:"
echo "    ./repo/publish-repo.sh && ./repo/upload-pool.sh"
