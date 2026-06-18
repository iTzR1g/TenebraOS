#!/bin/bash
# profiles/gaming.sh
# TenebraOS - Gaming profile
# Debian 13 (Trixie)

apply_gaming_profile() {
    echo "[TenebraOS] Applying Gaming profile..."

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

    if ! command -v discord &>/dev/null; then
        curl -fsSL -o /tmp/discord.deb "https://discord.com/api/download?platform=linux&format=deb"
        apt-get install -y /tmp/discord.deb
        rm /tmp/discord.deb
    fi

    install_brave

    cat > /etc/sysctl.d/99-tenebraos-gaming.conf << 'EOF'
vm.swappiness=10
kernel.sched_autogroup_enabled=1
net.core.somaxconn=1024
EOF
    sysctl --system

    systemctl set-default graphical.target
    echo "[TenebraOS] Gaming profile applied."
}
