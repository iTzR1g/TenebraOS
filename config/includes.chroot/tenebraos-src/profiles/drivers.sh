#!/bin/bash
# profiles/drivers.sh
# TenebraOS - Driver and helper functions

install_brave() {
    if ! command -v brave-browser &>/dev/null; then
        curl -fsSL https://brave-browser-apt-release.s3.brave.com/brave-browser-archive-keyring.gpg \
            | gpg --dearmor -o /etc/apt/trusted.gpg.d/brave-browser-release.gpg
        echo 'deb [arch=amd64] https://brave-browser-apt-release.s3.brave.com/ stable main' \
            > /etc/apt/sources.list.d/brave-browser-release.list
        apt-get update
        apt-get install -y brave-browser
    fi
}
