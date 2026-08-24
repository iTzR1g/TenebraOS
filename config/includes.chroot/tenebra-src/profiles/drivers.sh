#!/bin/bash
# profiles/drivers.sh
# TenebraOS - Driver and helper functions

install_brave() {
    if ! command -v brave-browser &>/dev/null; then
        curl -fsSL https://brave-browser-apt-release.s3.brave.com/brave-browser-archive-keyring.gpg \
            | gpg --dearmor -o /etc/apt/trusted.gpg.d/brave-browser-release.gpg
        echo 'deb [arch=amd64] https://brave-browser-apt-release.s3.brave.com/ stable main' \
            > /etc/apt/sources.list.d/brave-browser-release.list
        apt-get update || true
        apt-get install -y brave-browser || echo "brave-browser install failed — continuing"
    fi
}

TENEBRAOS_REPO_URL="https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo-pool/ ./"

# TenebraOS own repository: custom packages (fastfetch, T2 kernels, ...)
# The repo is already baked into /etc/apt/sources.list.d/tenebraos.list by
# the image build; this just re-asserts it and pulls the extras.
install_tenebraos_repo() {
    mkdir -p /usr/share/keyrings
    if [ -f /tmp/tenebraos-repo.gpg ]; then
        cp /tmp/tenebraos-repo.gpg /usr/share/keyrings/tenebraos-repo.gpg
        chmod 644 /usr/share/keyrings/tenebraos-repo.gpg
    fi
    echo "deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] ${TENEBRAOS_REPO_URL}" \
        > /etc/apt/sources.list.d/tenebraos.list
    apt-get update || echo "apt-get update failed (offline?) — continuing"
    apt-get install -y tenebraos-fastfetch || echo "tenebraos-fastfetch not installed (repo unreachable?)"
}

# Optional GPU drivers on top of the firmware/mesa stack shipped in the ISO
apply_hardware_drivers() {
    case "${GPU_VENDOR:-unknown}" in
        nvidia)
            echo "Installing NVIDIA proprietary drivers"
            apt-get install -y nvidia-driver nvidia-kernel-dkms || true
            ;;
        amd|intel)
            # Already covered by firmware + mesa in the base image
            ;;
    esac
}

# Apple T2 Macs: install the T2-patched kernel from the TenebraOS repo
# and add the kernel parameters required for stable T2 operation.
# Prefers our CachyOS-enhanced build (BORE scheduler + performance tuning,
# version suffix -t2-tenebra), falls back to t2linux pre-built debs.
apply_t2_support() {
    T2_KERNEL="$(apt-cache search --names-only 'linux-image.*t2-tenebra' | awk '{print $1}' | sort -V | tail -1)"
    if [ -z "$T2_KERNEL" ]; then
        T2_KERNEL="$(apt-cache search --names-only 'linux-image.*tenebra' | awk '{print $1}' | sort -V | tail -1)"
    fi
    if [ -z "$T2_KERNEL" ]; then
        # Fall back to t2linux pre-built kernel
        T2_KERNEL="$(apt-cache search --names-only 'linux-image.*t2-trixie' | awk '{print $1}' | sort -V | tail -1)"
    fi
    if [ -z "$T2_KERNEL" ]; then
        echo "T2 kernel not found in the TenebraOS repo — skipping"
        return 0
    fi
    echo "Installing T2 kernel: ${T2_KERNEL}"
    # Matching headers so dkms modules can build against it
    T2_HEADERS="${T2_KERNEL/linux-image/linux-headers}"
    apt-get install -y "$T2_KERNEL" $T2_HEADERS || apt-get install -y "$T2_KERNEL"

    # Kernel parameters required for stable T2 operation (idempotent)
    if [ -f /etc/default/grub ] && ! grep -q 'iommu=pt' /etc/default/grub; then
        sed -i 's/^GRUB_CMDLINE_LINUX="\(.*\)"/GRUB_CMDLINE_LINUX="\1 intel_iommu=on iommu=pt pm_async=off"/' \
            /etc/default/grub
    fi
    # dpkg triggers usually generate the initrd; make sure one exists
    KREL="${T2_KERNEL#linux-image-}"
    if ! ls "/boot/initrd.img-${KREL}" >/dev/null 2>&1; then
        update-initramfs -c -k "${KREL}" 2>/dev/null || true
    fi
    update-grub 2>/dev/null || true
}
