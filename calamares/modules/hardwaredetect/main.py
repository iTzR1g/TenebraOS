#!/usr/bin/env python3
import libcalamares

def run():
    import subprocess

    gpus = []
    mac = False
    intel_cpu = False

    try:
        output = subprocess.check_output("lspci | grep -iE 'vga|3d|display'", shell=True, text=True, timeout=10)
        for line in output.strip().splitlines():
            gpus.append(line.strip())
    except Exception:
        pass

    try:
        output = subprocess.check_output("dmidecode -s system-product-name", shell=True, text=True, timeout=5)
        if 'Mac' in output or 'MacBook' in output:
            mac = True
    except Exception:
        pass

    try:
        with open('/proc/cpuinfo') as f:
            for line in f:
                if line.startswith('vendor_id') and 'Intel' in line:
                    intel_cpu = True
                    break
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

    # All Intel-based Apple machines this amd64 ISO can boot on are T2 Macs
    # (Apple Silicon is arm64). T2 gets the patched kernel + kernel parameters.
    is_mac_t2 = mac and intel_cpu
    libcalamares.globalstorage.insert("is_mac", mac)
    libcalamares.globalstorage.insert("is_mac_t2", is_mac_t2)
    libcalamares.globalstorage.insert("detected_gpus", gpus)
    return None