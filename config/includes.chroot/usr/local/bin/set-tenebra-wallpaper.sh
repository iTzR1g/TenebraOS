#!/bin/bash
WALLPAPER="/usr/share/wallpapers/tenebra/contents/images/1920x1080.svg"
LOCKFILE="/tmp/.tenebra-wallpaper-set"
if [ -f "$WALLPAPER" ] && [ ! -f "$LOCKFILE" ]; then
    touch "$LOCKFILE"
    kwriteconfig6 --file ~/.config/plasma-org.kde.plasma.desktop-appletsrc \
      --group Containments --group 1 --group Wallpaper --group org.kde.image --group General \
      --key Image "file://$WALLPAPER" 2>/dev/null || true
fi
