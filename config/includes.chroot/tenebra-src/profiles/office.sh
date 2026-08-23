#!/bin/bash
# profiles/office.sh
# TenebraOS - Daily Use & Office profile
# Devuan 6.1 (Excalibur), based on Debian 13 (Trixie)

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
        gnome-software

    apt-get install -y tlp tlp-rdw powertop
    systemctl enable tlp
    systemctl set-default graphical.target

    echo "[TenebraOS] Daily Use & Office profile applied."
}
