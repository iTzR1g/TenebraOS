#!/bin/bash
if grep -q 'boot=live' /proc/cmdline; then
    exec pkexec /usr/bin/calamares
fi
exit 0
