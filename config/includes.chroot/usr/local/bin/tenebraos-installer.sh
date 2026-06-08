#!/bin/bash
# Launches TenebraOS Installer in live mode
if grep -q 'boot=live' /proc/cmdline; then
    sleep 3
    if command -v pkexec &>/dev/null; then
        exec pkexec /usr/bin/calamares
    else
        exec sudo /usr/bin/calamares
    fi
fi
exit 0
