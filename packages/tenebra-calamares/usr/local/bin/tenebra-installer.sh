#!/bin/bash
# TenebraOS installer launcher.
# /etc/sudoers.d/tenebra-installer grants the live user NOPASSWD for
# calamares and keeps DISPLAY/XAUTHORITY/WAYLAND_DISPLAY across sudo,
# so this shows up on the user's screen with no password prompt.
if ! command -v calamares >/dev/null 2>&1; then
    echo "calamares is not installed" >&2
    exit 1
fi
exec sudo -E /usr/bin/calamares "$@"
