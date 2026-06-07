#!/bin/bash
# profiles/office.sh
# TenebraOS - Daily Use & Office profile
# Debian 13 (Trixie)

apply_office_profile() {
    echo "[TenebraOS] Applying Daily Use & Office profile..."

    install_brave

    apt-get install -y \
        libreoffice \
        thunderbird \
        gimp \
        inkscape \
        vlc \
        evince \
        gnome-calendar \
        gnome-shell \
        gnome-tweaks \
        gnome-software

    # Power saving (TLP fully supports Trixie kernels)
    apt-get install -y tlp tlp-rdw powertop
    systemctl enable tlp
    systemctl set-default graphical.target

    echo "[TenebraOS] Daily Use & Office profile applied."
}
