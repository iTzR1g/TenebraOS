#!/bin/bash
WALLPAPER="/usr/share/wallpapers/tenebraos/hell_prospect.png"
if [ -f "$WALLPAPER" ]; then
    kwriteconfig6 --file ~/.config/plasma-org.kde.plasma.desktop-appletsrc \
        --group 'Containments' --group '1' --group 'Wallpaper' \
        --group 'org.kde.image' --key 'Image' "file://$WALLPAPER"
fi
