#!/usr/bin/env python3
# hardware_detect/main.py
# TenebraOS - Hardware detection module
# Detects: GPU vendor, T2 Mac, CPU, RAM, firmware type

import subprocess
import os
import libcalamares


def detect_gpu():
    """Detect GPU vendor. Returns 'nvidia', 'amd', 'intel', or 'unknown'."""
    try:
        out = subprocess.check_output(
            ["lspci"], stderr=subprocess.DEVNULL
        ).decode()
        if "NVIDIA" in out or "nvidia" in out:
            return "nvidia"
        elif "AMD" in out or "Radeon" in out or "ATI" in out:
            return "amd"
        elif "Intel" in out and ("VGA" in out or "Display" in out or "3D" in out):
            return "intel"
    except Exception:
        pass
    return "unknown"


def detect_t2_mac():
    """Detect Apple T2 Mac by checking for apple_bce or T2-specific DMI."""
    # Check DMI product name
    try:
        with open("/sys/class/dmi/id/product_name", "r") as f:
            product = f.read().strip().lower()
        apple_t2_models = [
            "macbookpro15", "macbookpro16",
            "macbookair8", "macbookair9",
            "macmini8", "imac19", "imac20", "macpro7"
        ]
        if any(m in product.replace(",", "").replace(" ", "") for m in apple_t2_models):
            return True
    except Exception:
        pass

    # Check for apple_bce kernel module (T2 bridge chip driver)
    try:
        out = subprocess.check_output(
            ["lsmod"], stderr=subprocess.DEVNULL
        ).decode()
        if "apple_bce" in out:
            return True
    except Exception:
        pass

    # Check DMI vendor
    try:
        with open("/sys/class/dmi/id/sys_vendor", "r") as f:
            vendor = f.read().strip()
        if "Apple" in vendor:
            # Further check: T2 Macs have the T2 chip listed in USB
            try:
                usb_out = subprocess.check_output(
                    ["lsusb"], stderr=subprocess.DEVNULL
                ).decode()
                if "Apple T2" in usb_out or "8600" in usb_out:
                    return True
            except Exception:
                pass
    except Exception:
        pass

    return False


def detect_uefi():
    """Check if system booted with UEFI."""
    return os.path.exists("/sys/firmware/efi")


def detect_ram_gb():
    """Return total RAM in GB."""
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if line.startswith("MemTotal"):
                    kb = int(line.split()[1])
                    return round(kb / 1024 / 1024, 1)
    except Exception:
        pass
    return 0


def detect_cpu_vendor():
    """Returns 'intel', 'amd', or 'unknown'."""
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if "vendor_id" in line:
                    if "GenuineIntel" in line:
                        return "intel"
                    elif "AuthenticAMD" in line:
                        return "amd"
    except Exception:
        pass
    return "unknown"


def detect_is_laptop():
    """Heuristic: check for battery."""
    return os.path.exists("/sys/class/power_supply/BAT0") or \
           os.path.exists("/sys/class/power_supply/BAT1")


def run():
    gs = libcalamares.globalstorage

    gpu = detect_gpu()
    is_t2 = detect_t2_mac()
    is_uefi = detect_uefi()
    ram_gb = detect_ram_gb()
    cpu = detect_cpu_vendor()
    is_laptop = detect_is_laptop()

    # Store all results for autoconfig_run to consume
    gs.insert("hw_gpu", gpu)
    gs.insert("hw_is_t2_mac", is_t2)
    gs.insert("hw_is_uefi", is_uefi)
    gs.insert("hw_ram_gb", ram_gb)
    gs.insert("hw_cpu", cpu)
    gs.insert("hw_is_laptop", is_laptop)

    libcalamares.utils.debug(f"[hardware_detect] GPU: {gpu}")
    libcalamares.utils.debug(f"[hardware_detect] T2 Mac: {is_t2}")
    libcalamares.utils.debug(f"[hardware_detect] UEFI: {is_uefi}")
    libcalamares.utils.debug(f"[hardware_detect] RAM: {ram_gb} GB")
    libcalamares.utils.debug(f"[hardware_detect] CPU: {cpu}")
    libcalamares.utils.debug(f"[hardware_detect] Laptop: {is_laptop}")

    return None  # None = success
