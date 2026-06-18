#!/usr/bin/env python3
# hardware_detect/main.py
# Detects hardware (Mac, GPU) and sets globalStorage for Calamares

import libcalamares

def run():
    import subprocess
    import os

    gpus = []
    mac = False

    try:
        output = subprocess.check_output(
            "lspci | grep -iE 'vga|3d|display'",
            shell=True, text=True, timeout=10
        )
        for line in output.strip().splitlines():
            gpus.append(line.strip())
    except Exception:
        pass

    try:
        output = subprocess.check_output(
            "dmidecode -s system-product-name",
            shell=True, text=True, timeout=5
        )
        if 'Mac' in output or 'MacBook' in output:
            mac = True
    except Exception:
        pass

    if 'NVIDIA' in str(gpus):
        libcalamares.globalstorage.insert("gpu_vendor", "nvidia")
    elif 'AMD' in str(gpus) or 'ATI' in str(gpus):
        libcalamares.globalstorage.insert("gpu_vendor", "amd")
    elif 'Intel' in str(gpus):
        libcalamares.globalstorage.insert("gpu_vendor", "intel")
    else:
        libcalamares.globalstorage.insert("gpu_vendor", "unknown")

    libcalamares.globalstorage.insert("is_mac", mac)
    libcalamares.globalstorage.insert("detected_gpus", gpus)

    return None
