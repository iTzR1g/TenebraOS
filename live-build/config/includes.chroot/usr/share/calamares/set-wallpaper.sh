#!/bin/bash
# Set TenebraOS wallpaper for the live user
WALLPAPER="/usr/share/wallpapers/tenebraos/hell_prospect.png"
MARKER="$HOME/.config/tenebraos-wallpaper-ready"

if [ -f "$MARKER" ] || ! command -v plasma-apply-wallpaperimage &>/dev/null; then
    exit 0
fi

# Wait for Plasma to be ready
for i in $(seq 1 20); do
    if pgrep -x plasmashell >/dev/null; then
        break
    fi
    sleep 1
done

plasma-apply-wallpaperimage "$WALLPAPER" 2>/dev/null || true
touch "$MARKER"
