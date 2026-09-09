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
    # Enable TLP via runit (one-shot: run setup, then sleep to stay "up")
    if [ ! -d /etc/sv/tlp ]; then
        mkdir -p /etc/sv/tlp
        cat > /etc/sv/tlp/run << 'SVRUN'
#!/bin/sh
/usr/sbin/tlp start
exec sleep infinity
SVRUN
        cat > /etc/sv/tlp/finish << 'SVFIN'
#!/bin/sh
/usr/sbin/tlp stop
SVFIN
        chmod +x /etc/sv/tlp/run /etc/sv/tlp/finish
    fi
    ln -sf /etc/sv/tlp /etc/service/tlp 2>/dev/null || \
        ln -sf /etc/sv/tlp /run/runit/services/tlp 2>/dev/null || true

    echo "[TenebraOS] Daily Use & Office profile applied."
}
