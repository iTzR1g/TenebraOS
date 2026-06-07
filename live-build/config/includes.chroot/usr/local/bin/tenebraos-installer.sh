#!/bin/bash
# Launch Calamares installer only in live environment
if [ -f /run/live/medium ] || grep -q boot=live /proc/cmdline 2>/dev/null; then
    exec calamares
fi
