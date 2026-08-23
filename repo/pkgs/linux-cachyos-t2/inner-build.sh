#!/bin/bash
# repo/pkgs/linux-cachyos-t2/inner-build.sh
# The actual kernel build — runs identically native on Devuan or inside a
# Debian container on any host. Not usually called directly; use build.sh.
#
# Env:
#   KERNEL_VERSION   kernel tag to build        (default 7.2)
#   PKGREL           package revision           (default 1)
#   CODENAME         local version suffix       (default tenebra)
#   CACHYOS_SCHED    bore | eevdf | bmq         (default bore)
#   OUTPUT_DIR       where .debs + log land     (default /output)

set -euo pipefail

KERNEL_VERSION="${KERNEL_VERSION:-7.2}"
PKGREL="${PKGREL:-1}"
CODENAME="${CODENAME:-tenebra}"
CACHYOS_SCHED="${CACHYOS_SCHED:-bore}"
OUTPUT_DIR="${OUTPUT_DIR:-/output}"

KERNEL_GIT="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
KERNEL_GIT_ALT="https://cdn.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
T2_PATCHES="https://github.com/t2linux/linux-t2-patches.git"
CACHY_PATCHES="https://github.com/CachyOS/kernel-patches.git"
T2_CONFIG="https://raw.githubusercontent.com/t2linux/T2-Debian-and-Ubuntu-Kernel/Mainline/templates/default-config-debian"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tenebra-kern-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── deps ───────────────────────────────────────────────────────────────
if command -v apt-get >/dev/null; then
    echo ">> Checking build dependencies..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
        ca-certificates build-essential fakeroot libncurses-dev bison flex \
        libssl-dev libelf-dev openssl dkms libudev-dev libpci-dev \
        libiberty-dev autoconf wget xz-utils git libcap-dev bc rsync cpio \
        debhelper kernel-wedge curl gawk dwarves zstd python3 libdw-dev \
        lsb-release perl > /dev/null
    update-ca-certificates --fresh >/dev/null 2>&1 || true
fi

for cmd in make gcc patch curl git bc perl; do
    command -v "$cmd" >/dev/null || { echo "ERROR: missing '$cmd'" >&2; exit 1; }
done

mkdir -p "$OUTPUT_DIR"

# ── [1/6] kernel source ────────────────────────────────────────────────
echo ""
echo ">> [1/6] Cloning kernel v${KERNEL_VERSION}..."
git clone -q --depth 1 --single-branch --branch "v${KERNEL_VERSION}" \
    "${KERNEL_GIT}" "$WORK/linux" || \
git clone -q --depth 1 --single-branch --branch "v${KERNEL_VERSION}" \
    "${KERNEL_GIT_ALT}" "$WORK/linux"
cd "$WORK/linux"
echo "   version: $(make kernelversion)"

# ── [2/6] fetch patch sets ─────────────────────────────────────────────
echo ""
echo ">> [2/6] Fetching T2 + CachyOS patches..."
mkdir -p "$WORK/patches"

git clone -q --depth 1 "${T2_PATCHES}" "$WORK/src-t2" || true
cp "$WORK/src-t2"/*.patch "$WORK/patches/" 2>/dev/null || true
echo "   T2 patches: $(find "$WORK/patches" -name '*.patch' | wc -l)"

git clone -q --depth 1 "${CACHY_PATCHES}" "$WORK/src-cachy" || true
SCHED_PATCH=""
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
    echo "   scheduler patch: $(basename "$SCHED_PATCH") (kernel dir: $(basename "$(dirname "$(dirname "$SCHED_PATCH")")"))"
else
    echo "   no scheduler patch found — building with stock EEVDF"
fi

# ── [3/6] apply patches ────────────────────────────────────────────────
echo ""
echo ">> [3/6] Applying patches..."
FAILED=0
for p in $(find "$WORK/patches" -name '*.patch' | sort); do
    if patch -p1 -N --dry-run < "$p" >/dev/null 2>&1; then
        patch -s -p1 -N < "$p" && echo "   ok:      $(basename "$p")" || { echo "   FAILED:  $(basename "$p")"; FAILED=1; }
    elif patch -p1 -N --dry-run -R < "$p" >/dev/null 2>&1; then
        echo "   already: $(basename "$p")"
    else
        echo "   CONFLICT: $(basename "$p")"
        FAILED=1
    fi
done
[ "$FAILED" = "0" ] || { echo ">> Patch conflicts — aborting."; exit 1; }

# ── [4/6] config ───────────────────────────────────────────────────────
echo ""
echo ">> [4/6] Configuring kernel..."

if curl -fsSL "$T2_CONFIG" -o .config 2>/dev/null; then
    echo "   base: t2linux default-config-debian"
else
    echo "   WARNING: config download failed, falling back to defconfig"
    make defconfig
fi

scripts/config \
    --set-str SYSTEM_TRUSTED_KEYS "" \
    --set-str SYSTEM_REVOCATION_KEYS ""

# T2 driver stack
scripts/config \
    --module T2BCE_CORE --module T2BCE_VHCI --module T2BCE_AUDIO --module T2BCE_DMA \
    --module HID_APPLETB_BL --module HID_APPLETB_KBD --module DRM_APPLETBDRM \
    --module BT_HCIBCM4377 --module APFS_FS \
    --enable MODULE_FORCE_UNLOAD \
    --module APPLE_GMUX --module SENSORS_APPLESMC \
    --module HID_APPLE --module HID_MAGICMOUSE \
    --module BRCMFMAC

# Scheduler selection
case "$CACHYOS_SCHED" in
    bore)  scripts/config --enable SCHED_BORE ;;
    bmq)   scripts/config --enable SCHED_ALT --enable SCHED_BMQ ;;
    eevdf) scripts/config --disable SCHED_BORE --disable SCHED_ALT ;;
    *)     scripts/config --enable SCHED_BORE ;;
esac

# CachyOS performance tuning
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

# Slim the package: no debug/BTF payloads
scripts/config \
    --disable DEBUG_INFO --disable DEBUG_INFO_DWARF4 --disable DEBUG_INFO_DWARF5 \
    --disable DEBUG_INFO_BTF --disable DEBUG_INFO_BTF_MODULES \
    --disable GDB_SCRIPTS

make olddefconfig

# ── [5/6] build ────────────────────────────────────────────────────────
echo ""
echo ">> [5/6] Building debs (30-90 min)..."
LOG="$OUTPUT_DIR/build.log"
if ! make -j"$(nproc)" deb-pkg \
        LOCALVERSION="-${PKGREL}-t2-${CODENAME}" \
        KDEB_PKGVERSION="$(make kernelversion)-${PKGREL}" \
        2>&1 | tee "$LOG"; then
    echo ""
    echo ">> BUILD FAILED — full log: $OUTPUT_DIR/build.log"
    echo ">> Likely culprits:"
    grep -nE "error:|Error [0-9]|undefined reference|implicit declaration|No rule to make target" \
        "$LOG" | grep -vE "Makefile:[0-9]+: (recipe|\*\*\*)" | tail -25 || true
    exit 1
fi

# ── [6/6] collect ──────────────────────────────────────────────────────
echo ""
echo ">> [6/6] Collecting packages..."
cp -v "$WORK"/*.deb "$OUTPUT_DIR/"

echo ""
ls -lh "$OUTPUT_DIR"/*.deb
