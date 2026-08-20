#!/bin/bash
# profiles/drivers.sh
# TenebraOS - Driver and helper functions

install_brave() {
    if ! command -v brave-browser &>/dev/null; then
        curl -fsSL https://brave-browser-apt-release.s3.brave.com/brave-browser-archive-keyring.gpg \
            | gpg --dearmor -o /etc/apt/trusted.gpg.d/brave-browser-release.gpg
        echo 'deb [arch=amd64] https://brave-browser-apt-release.s3.brave.com/ stable main' \
            > /etc/apt/sources.list.d/brave-browser-release.list
        apt-get update
        apt-get install -y brave-browser
    fi
}

TENEBRAOS_REPO_URL="https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo-pool/ ./"

# TenebraOS own repository: custom packages (fastfetch, T2 kernels, ...)
install_tenebraos_repo() {
    if [ ! -f /tmp/tenebraos-repo.gpg ]; then
        echo "TenebraOS repo key not available, skipping"
        return 0
    fi
    mkdir -p /usr/share/keyrings
    cp /tmp/tenebraos-repo.gpg /usr/share/keyrings/tenebraos-repo.gpg
    chmod 644 /usr/share/keyrings/tenebraos-repo.gpg
    echo "deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] ${TENEBRAOS_REPO_URL}" \
        > /etc/apt/sources.list.d/tenebraos.list
    apt-get update
    apt-get install -y tenebraos-fastfetch || echo "tenebraos-fastfetch not found in repo (repo not published yet?)"
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
apply_t2_support() {
    T2_KERNEL="$(apt-cache search --names-only 'linux-image.*t2-trixie' | awk '{print $1}' | sort -V | tail -1)"
    if [ -z "$T2_KERNEL" ]; then
        echo "T2 kernel not found in the TenebraOS repo — skipping"
        return 0
    fi
    echo "Installing T2 kernel: ${T2_KERNEL}"
    apt-get install -y "$T2_KERNEL"
    if [ -f /etc/default/grub ]; then
        sed -i 's/^GRUB_CMDLINE_LINUX="\(.*\)"/GRUB_CMDLINE_LINUX="\1 intel_iommu=on iommu=pt pm_async=off"/' \
            /etc/default/grub
        update-grub 2>/dev/null || true
    fi
}
