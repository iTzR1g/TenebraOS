#!/bin/bash
# profiles/environments.sh
# TenebraOS - Desktop environment / window manager installer.
# Called from the calamares autoconfig job based on the "environment"
# chooser selection. Plasma ships in the ISO; the rest are pulled
# over the network inside the install chroot. sddm stays enabled for
# anything graphical (it auto-detects installed sessions); the "no
# desktop" pick removes it so the system boots to a text console.

# Plasma is preinstalled in the squashfs; just make sure sddm is on.
apply_environment_plasma() {
    echo "[TenebraOS] Environment: KDE Plasma (preinstalled)"
}

# XFCE — lightweight X11 desktop (Devuan repo).
apply_environment_xfce() {
    echo "[TenebraOS] Environment: XFCE (installing)"
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        xfce4 xfce4-goodies || \
        { echo "[TenebraOS] XFCE install failed"; return 1; }
}

# i3 — minimal X11 tiling window manager.
apply_environment_i3() {
    echo "[TenebraOS] Environment: i3 (installing)"
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        i3 i3blocks dmenu xinit x11-xserver-utils || \
        { echo "[TenebraOS] i3 install failed"; return 1; }
}

# Sway — Wayland compositor (i3-compatible).
apply_environment_sway() {
    echo "[TenebraOS] Environment: Sway (installing)"
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        sway swaybg swaylock waybar foot grim slurp xwayland || \
        { echo "[TenebraOS] Sway install failed"; return 1; }
}

# No desktop — disable the display manager; boot to getty.
apply_environment_minimal() {
    echo "[TenebraOS] Environment: No Desktop (console)"
    rm -f /etc/service/sddm 2>/dev/null || true
    rm -f /etc/sddm.conf.d/autologin.conf 2>/dev/null || true
    if [ -e /etc/init.d/sddm ]; then
        update-rc.d sddm disable 2>/dev/null || true
    fi
}