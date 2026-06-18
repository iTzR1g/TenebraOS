#!/bin/bash
WALLPAPER="/usr/share/wallpapers/tenebra/contents/images/1920x1080.svg"
if [ -f "$WALLPAPER" ]; then
    if command -v plasma-apply-wallpaperimage &>/dev/null; then
        plasma-apply-wallpaperimage "$WALLPAPER" 2>/dev/null || true
    elif command -v kwriteconfig5 &>/dev/null; then
        kwriteconfig5 --file ~/.config/plasma-org.kde.plasma.desktop-appletsrc \
            --group Containments --group 1 --group Wallpaper \
            --group org.kde.image --key Image "file://$WALLPAPER" 2>/dev/null || true
    fi
fi
