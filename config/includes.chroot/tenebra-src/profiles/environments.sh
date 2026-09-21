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

    # Branding: overlay the Tenebra logo onto the Xfce logo paths that
    # xfce4-about (4.18 uses the themed logo icon; 4.20 the os-release
    # LOGO / icon file) and the panel applications button look up.
    apply_xfce_branding
}

# Drop the Tenebra logo over the Xfce icon theme + pixmaps fallbacks.
# Runs after the DE packages are installed so the files exist and nothing
# re-installs over them during this session. /usr/share/tenebra/branding
# is shipped in the image; hicolor is consulted by every Xfce icon theme.
apply_xfce_branding() {
    BRAND="/usr/share/tenebra/branding/hicolor"
    [ -d "${BRAND}" ] || { echo "[TenebraOS] branding assets missing — skipping Xfce logo"; return 0; }
    cp -a "${BRAND}"/* /usr/share/icons/hicolor/ 2>/dev/null || true
    # Classic pixmaps fallbacks that older/newer xfce4-about may consult.
    cp -f "${BRAND}/128x128/apps/xfce4-logo.png" /usr/share/pixmaps/xfce4-logo.png 2>/dev/null || true
    cp -f /usr/share/pixmaps/tenebra-logo-white.png /usr/share/pixmaps/debian-logo.png 2>/dev/null || true
    if [ -e /usr/share/pixmaps/debian-logo.svg ]; then
        cp -f "${BRAND}/scalable/apps/debian-logo.svg" /usr/share/pixmaps/debian-logo.svg 2>/dev/null || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f /usr/share/icons/hicolor 2>/dev/null || true
    fi
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