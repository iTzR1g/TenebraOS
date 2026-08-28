#!/usr/bin/env python3
# hardwaredetect — detect the live machine's hardware before install so both
# the UI (welcome summary, use-case chooser) and the exec-phase driver/CPU
# setup know what to target.
#
# Mirrors the CachyOS approach: auto-detect GPU vendor, Apple T2, CPU vendor,
# total RAM and target disk; write everything to GlobalStorage for other
# modules (especially autoconfig) to consume.
import libcalamares
import subprocess
import re


def _run(cmd, timeout=10):
    try:
        out = subprocess.check_output(cmd, shell=True, text=True,
                                      stderr=subprocess.DEVNULL, timeout=timeout)
        return out
    except Exception:
        return ""


def _parse_meminfo():
    # /proc/meminfo MemTotal in kB -> GiB (rounded)
    txt = _run("grep MemTotal /proc/meminfo", 3)
    m = re.search(r"MemTotal:\s+(\d+)", txt)
    return (int(m.group(1)) // 1024 // 1024) if m else None


def detect_gpu():
    # Collect every display-capable device so a dual-GPU laptop (e.g. Intel
    # iGPU + NVIDIA/AMD dGPU) is represented, not just the first one.
    gpus = []
    for line in _run("lspci | grep -iE 'vga|3d|display'").splitlines():
        if line.strip():
            gpus.append(line.strip())

    vendors = set()
    joined = " ".join(gpus).lower()
    if "nvidia" in joined:
        vendors.add("nvidia")
    if "amd" in joined or "ati" in joined or "radeon" in joined:
        vendors.add("amd")
    if "intel" in joined:
        vendors.add("intel")
    if "apple" in joined or "vmware" in joined or "qxl" in joined \
       or "virtio" in joined or "cirrus" in joined:
        vendors.add("opengl")  # software / simple framebuffer

    # Prefer a discrete card for driver selection, fall back to any vendor.
    primary = None
    for pref in ("nvidia", "amd", "intel", "opengl"):
        if pref in vendors:
            primary = pref
            break
    return gpus, primary, sorted(vendors)


def detect_t2():
    is_mac = False
    for field in ("system-product-name", "system-manufacturer"):
        val = _run(f"dmidecode -s {field}", 5).strip()
        if re.search(r"mac(book|bookpro|mini|pro|air)?", val, re.I):
            is_mac = True
    return is_mac


def detect_cpu():
    txt = _run("grep -m1 'model name' /proc/cpuinfo", 3)
    model = re.sub(r".*:\s*", "", txt).strip()
    low = model.lower()
    if "intel" in low:
        vendor, brand = "intel", "Intel"
    elif "amd" in low:
        vendor, brand = "amd", "AMD"
    else:
        vendor, brand = "other", model.split()[-1] if model else "Unknown"
    return vendor, brand, model


def detect_disk():
    # Largest block device (excluding loop/ram/cdrom) is the likely target.
    try:
        out = _run("lsblk -bdo NAME,SIZE,TYPE", 5)
    except Exception:
        out = ""
    best = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        name, size, typ = parts[0], parts[1], parts[2]
        if typ != "disk":
            continue
        # skip loop, ram, cdrom, zram, virtual CD-ROMs
        if re.match(r"^(loop|ram|zram|sr|cd)", name):
            continue
        try:
            size_b = int(size)
        except ValueError:
            continue
        if best is None or size_b > best[1]:
            best = (name, size_b)
    if not best:
        return None
    name, size_b = best
    return {"device": name, "size_bytes": size_b,
            "size_gib": round(size_b / (1024**3), 1)}


def run():
    gpus, gpu_primary, gpu_vendors = detect_gpu()
    is_mac_t2 = detect_t2()
    cpu_vendor, cpu_brand, cpu_model = detect_cpu()
    disk = detect_disk()
    mem_gib = _parse_meminfo()

    libcalamares.globalstorage.insert("gpu_vendor", gpu_primary or "unknown")
    libcalamares.globalstorage.insert("gpu_vendors", gpu_vendors)
    libcalamares.globalstorage.insert("detected_gpus", gpus)
    libcalamares.globalstorage.insert("is_mac_t2", is_mac_t2)
    libcalamares.globalstorage.insert("cpu_vendor", cpu_vendor)
    libcalamares.globalstorage.insert("cpu_brand", cpu_brand)
    libcalamares.globalstorage.insert("cpu_model", cpu_model)
    libcalamares.globalstorage.insert("total_ram_gib", mem_gib)
    libcalamares.globalstorage.insert("target_disk", disk)
    return None
