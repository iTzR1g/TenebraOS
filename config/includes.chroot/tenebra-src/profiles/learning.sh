#!/bin/bash
# profiles/learning.sh
# TenebraOS - Learning & Development profile
# Devuan 6.1 (Excalibur), based on Debian 13 (Trixie)

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

    if ! command -v code &>/dev/null; then
        curl -fsSL https://packages.microsoft.com/keys/microsoft.asc \
            | gpg --dearmor -o /etc/apt/trusted.gpg.d/microsoft.gpg
        echo 'deb [arch=amd64] https://packages.microsoft.com/repos/code stable main' \
            > /etc/apt/sources.list.d/vscode.list
        apt-get update
        apt-get install -y code
    fi

    apt-get install -y zram-tools
    cat >> /etc/default/zramswap << 'EOF'
ALGO=zstd
PERCENT=50
EOF
    # Enable zramswap via runit (one-shot: run setup, then sleep to stay "up")
    if [ ! -d /etc/sv/zramswap ]; then
        mkdir -p /etc/sv/zramswap
        cat > /etc/sv/zramswap/run << 'SVRUN'
#!/bin/sh
/usr/sbin/zramswap --all
exec sleep infinity
SVRUN
        chmod +x /etc/sv/zramswap/run
    fi
    ln -sf /etc/sv/zramswap /etc/service/zramswap 2>/dev/null || \
        ln -sf /etc/sv/zramswap /run/runit/services/zramswap 2>/dev/null || true

    echo "[TenebraOS] Learning & Development profile applied."
}
