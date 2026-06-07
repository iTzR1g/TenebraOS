#!/bin/bash
# profiles/gaming.sh
# TenebraOS - Gaming profile
# Debian 13 (Trixie)

apply_gaming_profile() {
    echo "[TenebraOS] Applying Gaming profile..."

    # Enable non-free for Steam on Trixie
    grep -q 'non-free' /etc/apt/sources.list || \
        sed -i 's/main/main contrib non-free non-free-firmware/' /etc/apt/sources.list
    dpkg --add-architecture i386
    apt-get update

    apt-get install -y \
        steam \
        lutris \
        heroic-games-launcher \
        gamemode \
        mangohud \
        wine \
        winetricks \
        vulkan-tools \
        mesa-vulkan-drivers \
        libgl1-mesa-dri

    install_brave

    # Low-latency kernel parameters
    cat > /etc/sysctl.d/99-tenebraos-gaming.conf << 'EOF'
vm.swappiness=10
kernel.sched_autogroup_enabled=1
net.core.somaxconn=1024
EOF
    sysctl --system

    # KDE Plasma desktop (Trixie ships Plasma 6)
    apt-get install -y kde-plasma-desktop
    systemctl set-default graphical.target

    echo "[TenebraOS] Gaming profile applied."
}
