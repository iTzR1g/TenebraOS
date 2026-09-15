#!/bin/bash
# profiles/minimal.sh
# TenebraOS - Minimal profile
# Base system + drivers only. Nothing extra is installed or enabled.

apply_minimal_profile() {
    echo "[TenebraOS] Applying Minimal profile (drivers only)..."
    # drivers.sh already installed the TenebraOS repo and hardware drivers.
    # Nothing further to add; leave a clean, empty desktop/no extras.
    echo "[TenebraOS] Minimal profile applied."
}