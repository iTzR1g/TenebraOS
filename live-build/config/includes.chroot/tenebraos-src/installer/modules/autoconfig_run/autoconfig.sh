#!/bin/bash
# installer/modules/autoconfig/autoconfig.sh
# TenebraOS - Master auto-config script
# Debian 13 (Trixie)
#
# Runs inside chroot on the freshly installed system.
# Reads hardware and use-case choices made during the Calamares wizard.

set -e

PROFILES_DIR="/usr/share/tenebraos/profiles"
HARDWARE=$(cat /tmp/calamares-hardware.conf 2>/dev/null || echo 'none:intel')
MAC=$(echo "$HARDWARE" | cut -d: -f1)
GPU=$(echo "$HARDWARE" | cut -d: -f2)
USECASE=$(cat /tmp/calamares-usecase.conf 2>/dev/null || echo 'office')

echo "============================================"
echo " TenebraOS Auto-Config"
echo " Base    : Debian 13 (Trixie)"
echo " Mac type: $MAC"
echo " GPU     : $GPU"
echo " Profile : $USECASE"
echo "============================================"

# Update package lists first
apt-get update

# Source all profile and driver scripts
source "$PROFILES_DIR/drivers.sh"
source "$PROFILES_DIR/gaming.sh"
source "$PROFILES_DIR/learning.sh"
source "$PROFILES_DIR/office.sh"

# Apply Mac support if needed
case $MAC in
    t2)     install_t2_support ;;
    legacy) install_mac_support ;;
    none)   echo "Standard PC — skipping Mac support." ;;
esac

# Install GPU drivers
case $GPU in
    nvidia) install_nvidia ;;
    amd)    install_amd ;;
    intel)  install_intel ;;
esac

# Apply the chosen use-case profile
apply_${USECASE}_profile

echo ""
echo "TenebraOS auto-config complete. Welcome to the darkness."
