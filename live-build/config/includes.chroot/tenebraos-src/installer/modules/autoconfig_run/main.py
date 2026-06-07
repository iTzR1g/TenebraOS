#!/usr/bin/env python3
# autoconfig_run/main.py
# TenebraOS - Post-install auto-configuration
# Reads hardware_detect + usecase_select results from globalStorage
# and applies the appropriate profile + hardware fixes inside the chroot.

import subprocess
import os
import libcalamares


PROFILES_DIR = "/tenebraos-src/profiles"


def chroot_run(root, cmd, check=True):
    """Run a command inside the installed system chroot."""
    full_cmd = ["chroot", root] + (["bash", "-c", cmd] if isinstance(cmd, str) else cmd)
    libcalamares.utils.debug(f"[autoconfig_run] chroot: {cmd}")
    result = subprocess.run(full_cmd, capture_output=True, text=True)
    if check and result.returncode != 0:
        libcalamares.utils.warning(
            f"[autoconfig_run] Command failed: {cmd}\n{result.stderr}"
        )
    return result


def copy_profile_script(root, profile):
    """Copy the profile script into the chroot and make it executable."""
    src = os.path.join(PROFILES_DIR, f"{profile}.sh")
    dest_dir = os.path.join(root, "tenebraos-profiles")
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, f"{profile}.sh")
    with open(src, "r") as f:
        content = f.read()
    with open(dest, "w") as f:
        f.write(content)
    os.chmod(dest, 0o755)
    return f"/{os.path.relpath(dest, root)}"


def apply_profile(root, profile):
    """Source the profile script and call its apply function inside chroot."""
    script_path = copy_profile_script(root, profile)
    fn_map = {
        "gaming":   "apply_gaming_profile",
        "learning": "apply_learning_profile",
        "office":   "apply_office_profile",
    }
    fn = fn_map.get(profile)
    if not fn:
        libcalamares.utils.warning(f"[autoconfig_run] Unknown profile: {profile}")
        return
    chroot_run(root, f"source {script_path} && {fn}")


def apply_nvidia(root):
    """Install NVIDIA drivers in the chroot."""
    libcalamares.utils.debug("[autoconfig_run] Applying NVIDIA config...")
    # Enable non-free if not already
    chroot_run(root,
        "grep -q 'non-free' /etc/apt/sources.list || "
        "sed -i 's/main/main contrib non-free non-free-firmware/' /etc/apt/sources.list"
    )
    chroot_run(root, "apt-get update -qq")
    chroot_run(root, "apt-get install -y nvidia-driver firmware-misc-nonfree")
    # Blacklist nouveau
    chroot_run(root,
        "echo 'blacklist nouveau' > /etc/modprobe.d/blacklist-nouveau.conf && "
        "echo 'options nouveau modeset=0' >> /etc/modprobe.d/blacklist-nouveau.conf && "
        "update-initramfs -u"
    )


def apply_amd(root):
    """Install AMD GPU drivers/firmware."""
    libcalamares.utils.debug("[autoconfig_run] Applying AMD GPU config...")
    chroot_run(root,
        "grep -q 'non-free' /etc/apt/sources.list || "
        "sed -i 's/main/main contrib non-free non-free-firmware/' /etc/apt/sources.list"
    )
    chroot_run(root, "apt-get update -qq")
    chroot_run(root, "apt-get install -y firmware-amd-graphics mesa-vulkan-drivers")


def apply_t2_mac(root):
    """Apply T2 Mac specific config: WiFi firmware, audio, touchbar, sleep hooks."""
    libcalamares.utils.debug("[autoconfig_run] Applying T2 Mac config...")

    # Add T2 kernel repo
    chroot_run(root,
        "curl -s https://raw.githubusercontent.com/t2linux/apple-t2-wiki/master/tools/install.sh"
        " | bash || true"
    )

    # WiFi firmware (BCM4364)
    chroot_run(root, "apt-get install -y firmware-brcm80211 || true")

    # T2 audio via PipeWire
    chroot_run(root, "apt-get install -y pipewire pipewire-audio-client-libraries wireplumber")
    chroot_run(root,
        "systemctl --global enable pipewire pipewire-pulse wireplumber"
    )

    # apple_bce sleep hook — unload before suspend, reload after
    sleep_hook = """\
#!/bin/bash
case "$1" in
    pre)
        modprobe -r apple_bce || true
        modprobe -r apple_ibridge || true
        ;;
    post)
        modprobe apple_ibridge || true
        modprobe apple_bce || true
        ;;
esac
"""
    hook_path = os.path.join(root, "lib/systemd/system-sleep/t2-modules")
    os.makedirs(os.path.dirname(hook_path), exist_ok=True)
    with open(hook_path, "w") as f:
        f.write(sleep_hook)
    os.chmod(hook_path, 0o755)

    # Touchbar
    chroot_run(root, "apt-get install -y apple-touchbar || true")


def apply_laptop_tweaks(root):
    """Apply power saving for laptops."""
    libcalamares.utils.debug("[autoconfig_run] Applying laptop power config...")
    chroot_run(root, "apt-get install -y tlp tlp-rdw")
    chroot_run(root, "systemctl enable tlp")


def apply_sddm_autologin(root):
    """Ensure SDDM autologin is set on the installed system."""
    conf_dir = os.path.join(root, "etc/sddm.conf.d")
    os.makedirs(conf_dir, exist_ok=True)
    with open(os.path.join(conf_dir, "autologin.conf"), "w") as f:
        f.write("[Autologin]\nUser=user\nSession=plasma\nRelogin=false\n")


def run():
    gs = libcalamares.globalstorage

    # Get install root (where the new system is mounted)
    root = gs.value("rootMountPoint")
    if not root:
        return ("No rootMountPoint", "autoconfig_run could not find the install root.")

    # Read hardware_detect results
    gpu        = gs.value("hw_gpu") or "unknown"
    is_t2      = gs.value("hw_is_t2_mac") or False
    is_laptop  = gs.value("hw_is_laptop") or False

    # Read usecase_select result
    profile = gs.value("usecase_profile") or "office"

    libcalamares.utils.debug(f"[autoconfig_run] Profile: {profile}")
    libcalamares.utils.debug(f"[autoconfig_run] GPU: {gpu}, T2: {is_t2}, Laptop: {is_laptop}")

    # 1. Apply selected profile
    apply_profile(root, profile)

    # 2. Apply GPU-specific config
    if gpu == "nvidia":
        apply_nvidia(root)
    elif gpu == "amd":
        apply_amd(root)
    # Intel uses mesa out of the box — nothing extra needed

    # 3. T2 Mac specific
    if is_t2:
        apply_t2_mac(root)

    # 4. Laptop tweaks (skip if gaming — gamemode handles power)
    if is_laptop and profile != "gaming":
        apply_laptop_tweaks(root)

    # 5. SDDM autologin on installed system
    apply_sddm_autologin(root)

    libcalamares.utils.debug("[autoconfig_run] All done.")
    return None
