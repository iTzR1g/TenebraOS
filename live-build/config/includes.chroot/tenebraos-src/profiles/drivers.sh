#!/bin/bash
# profiles/drivers.sh
# TenebraOS - GPU and Mac driver installation functions
# Debian 13 (Trixie)
#
# Notes for Trixie:
#   - nvidia-detect is in non-free; ensure non-free is in sources.list
#   - amdgpu is in mainline kernel — no extra repo needed
#   - T2 kernel repo targets stable; on Trixie you may need to build
#     from source or use the trixie/sid branch if available at wiki.t2linux.org

install_nvidia() {
    echo "[TenebraOS] Installing Nvidia drivers (Trixie)..."
    # Ensure non-free is available
    grep -q 'non-free' /etc/apt/sources.list || \
        sed -i 's/main/main contrib non-free non-free-firmware/' /etc/apt/sources.list
    apt-get update
    apt-get install -y nvidia-detect
    DRIVER=$(nvidia-detect | grep 'nvidia-driver' | awk '{print $1}')
    apt-get install -y "$DRIVER" nvidia-settings
    # Enable DRM kernel mode setting (required for Wayland on Trixie)
    echo 'options nvidia-drm modeset=1' \
        > /etc/modprobe.d/nvidia-drm.conf
    update-initramfs -u
    echo "[TenebraOS] Nvidia driver installed: $DRIVER"
}

install_amd() {
    echo "[TenebraOS] Installing AMD drivers (Trixie)..."
    # amdgpu is open-source and in the mainline kernel — just ensure
    # firmware and Mesa (Vulkan/OpenGL) are up to date
    apt-get install -y \
        firmware-amd-graphics \
        mesa-vulkan-drivers \
        libgl1-mesa-dri \
        mesa-utils \
        vulkan-tools
    echo "[TenebraOS] AMD drivers ready (open-source amdgpu + Mesa)."
}

install_intel() {
    echo "[TenebraOS] Configuring Intel graphics (Trixie)..."
    apt-get install -y \
        intel-media-va-driver \
        i965-va-driver \
        libva-drm2 \
        mesa-vulkan-drivers \
        vainfo \
        vulkan-tools
    echo "[TenebraOS] Intel graphics configured."
}

install_t2_support() {
    echo "[TenebraOS] Installing T2 Mac support..."
    echo ""
    echo "NOTE: The t2linux repo primarily targets Debian Bookworm/Ubuntu."
    echo "      Check https://wiki.t2linux.org for Trixie/sid availability."
    echo "      Attempting installation — may require manual kernel build if repo is unavailable."
    echo ""

    # Try t2linux repo — may not yet have trixie packages
    curl -fsSL https://adityagarg8.github.io/t2-ubuntu-repo/KEY.gpg \
        | gpg --dearmor -o /etc/apt/trusted.gpg.d/t2-ubuntu.gpg

    # Use bookworm packages even on trixie (often ABI-compatible)
    echo 'deb https://adityagarg8.github.io/t2-ubuntu-repo ./' \
        > /etc/apt/sources.list.d/t2.list
    apt-get update

    apt-get install -y linux-t2 apple-bce-dkms apple-ibridge-dkms || {
        echo "[TenebraOS] WARNING: T2 kernel packages unavailable for Trixie."
        echo "            See https://wiki.t2linux.org to build manually."
    }

    apt-get install -y firmware-manager-t2 || true

    # Audio via PipeWire (default on Trixie)
    apt-get install -y pipewire pipewire-pulse wireplumber
    if [ -f /usr/share/tenebraos/t2/t2-audio.conf ]; then
        cp /usr/share/tenebraos/t2/t2-audio.conf /etc/pipewire/
    fi

    update-grub
    echo "[TenebraOS] T2 support installation complete."
}

install_mac_support() {
    echo "[TenebraOS] Installing legacy Mac support..."
    apt-get install -y \
        firmware-linux \
        firmware-linux-nonfree \
        firmware-misc-nonfree
    echo "[TenebraOS] Legacy Mac support installed."
}

install_brave() {
    echo "[TenebraOS] Installing Brave browser..."
    curl -fsSL https://brave-browser-apt-release.s3.brave.com/brave-browser-archive-keyring.gpg \
        | gpg --dearmor -o /usr/share/keyrings/brave-browser-archive-keyring.gpg
    echo "deb [arch=amd64 signed-by=/usr/share/keyrings/brave-browser-archive-keyring.gpg] https://brave-browser-apt-release.s3.brave.com/ stable main" \
        > /etc/apt/sources.list.d/brave-browser-release.list
    apt-get update
    apt-get install -y brave-browser
    echo "[TenebraOS] Brave browser installed."
}
