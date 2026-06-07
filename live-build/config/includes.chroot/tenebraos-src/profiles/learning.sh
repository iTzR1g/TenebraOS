#!/bin/bash
# profiles/learning.sh
# TenebraOS - Learning & Development profile
# Debian 13 (Trixie)

apply_learning_profile() {
    echo "[TenebraOS] Applying Learning & Development profile..."

    apt-get install -y \
        git \
        python3 \
        python3-pip \
        python3-venv \
        nodejs \
        npm \
        default-jdk \
        virtualbox \
        jupyter-notebook \
        anki \
        build-essential \
        curl \
        wget

    install_brave

    # VS Code — install via Microsoft repo (not in Debian repos)
    if ! command -v code &>/dev/null; then
        curl -fsSL https://packages.microsoft.com/keys/microsoft.asc \
            | gpg --dearmor -o /etc/apt/trusted.gpg.d/microsoft.gpg
        echo 'deb [arch=amd64] https://packages.microsoft.com/repos/code stable main' \
            > /etc/apt/sources.list.d/vscode.list
        apt-get update
        apt-get install -y code
    fi

    # zram for better memory use on laptops (zram-tools updated for Trixie)
    apt-get install -y zram-tools
    cat >> /etc/default/zramswap << 'EOF'
ALGO=zstd
PERCENT=50
EOF
    systemctl enable zramswap

    # GNOME desktop (Trixie ships GNOME 48)
    apt-get install -y gnome-shell gnome-terminal nautilus gnome-tweaks
    systemctl set-default graphical.target

    echo "[TenebraOS] Learning & Development profile applied."
}
