#!/bin/bash
# Binary stage: inject GRUB theme into ISO
mkdir -p /boot/grub/themes/tenebra
cat > /etc/default/grub.d/tenebra-theme.cfg << 'EOF'
GRUB_THEME=/boot/grub/themes/tenebra/theme.txt
GRUB_TIMEOUT=5
GRUB_TIMEOUT_STYLE=menu
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
EOF
