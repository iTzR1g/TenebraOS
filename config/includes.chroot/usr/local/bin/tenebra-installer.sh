#!/bin/bash
if grep -q 'boot=live' /proc/cmdline 2>/dev/null; then
    sleep 3
fi
if command -v pkexec &>/dev/null; then
    exec pkexec /usr/bin/calamares
else
    exec sudo /usr/bin/calamares
fi
