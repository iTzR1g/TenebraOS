#!/bin/bash
# Launch Calamares as root.
#
# Primary path: sudo (NOPASSWD rule for exactly /usr/bin/calamares,
# shipped by tenebra-calamares; display env kept via env_keep).
# polkit >= 126 ignores .pkla files, so pkexec stays only as fallback.
CALAMARES=/usr/bin/calamares

if [ "$(id -u)" = "0" ]; then
    exec "$CALAMARES" "$@"
fi

if grep -q 'boot=live' /proc/cmdline 2>/dev/null; then
    sleep 3   # let the desktop settle when autostarted right after login
fi

if command -v sudo >/dev/null 2>&1 && sudo -n -l "$CALAMARES" >/dev/null 2>&1; then
    exec sudo -H -n "$CALAMARES" "$@"
fi

if command -v pkexec >/dev/null 2>&1; then
    exec pkexec env DISPLAY="${DISPLAY:-:0}" XAUTHORITY="${XAUTHORITY:-}" \
        QT_X11_NO_MITSHM=1 "$CALAMARES" "$@"
fi

echo "No privilege escalation available to start the installer." >&2
exit 1
