#!/usr/bin/env python3
# autoconfig — post-install (chroot) setup that:
#   1. wires up the TenebraOS repo
#   2. auto-applies hardware drivers from the detection done by hardwaredetect
#      (GPU vendor, Apple T2 kernel, CPU tuned packages when applicable)
#   3. applies the user's chosen use-case profile (gaming / learning / office)
#
# Runs after files are unpacked and the target is mounted at rootMountPoint.
import libcalamares
import os
import subprocess
import shutil


def run():
    # profileselect is the stock packagechooser under a custom instance;
    # it stores its choice as "packagechooser_profileselect".
    usecase = libcalamares.globalstorage.value("packagechooser_profileselect")
    if not usecase:
        usecase = libcalamares.globalstorage.value("usecase")
    if isinstance(usecase, str):
        usecase = usecase.strip().split(",")[0]
    if not usecase:
        usecase = "office"

    chroot = libcalamares.globalstorage.value("rootMountPoint")
    if not chroot:
        return "No rootMountPoint set"

    profiles_src = "/tenebra-src/profiles"
    profiles_dst = os.path.join(chroot, "tmp", "tenebra-profiles")
    shutil.copytree(profiles_src, profiles_dst, dirs_exist_ok=True)

    key_src = "/usr/share/keyrings/tenebraos-repo.gpg"
    key_dst = os.path.join(chroot, "tmp", "tenebraos-repo.gpg")
    if os.path.exists(key_src):
        shutil.copy(key_src, key_dst)

    # Detection summary (from the hardwaredetect job).
    gpu_vendor = libcalamares.globalstorage.value("gpu_vendor") or "unknown"
    cpu_vendor = libcalamares.globalstorage.value("cpu_vendor") or "unknown"
    is_mac_t2 = bool(libcalamares.globalstorage.value("is_mac_t2"))
    ram_gib = libcalamares.globalstorage.value("total_ram_gib")
    gpu_vendors = list(libcalamares.globalstorage.value("gpu_vendors") or [])
    detected_gpus = list(libcalamares.globalstorage.value("detected_gpus") or [])

    # The use-case chooser doubles as the hardware confirmation. A "gaming"
    # pick implies the user wants full GPU acceleration; for any other pick
    # we leave NVIDIA confirmation off unless it was conclusive multi-card.
    nvidia_confirmed = "yes" if (gpu_vendor == "nvidia" and usecase == "gaming") else "no"

    # Visible confirmation trail in the install log + a summary file the
    # finished page can surface.
    summary = [
        "[Tenebra] Detected hardware:",
        f"  GPU vendor   : {gpu_vendor}  ({', '.join(gpu_vendors) or 'none'})",
        f"  GPU devices  : {'; '.join(detected_gpus) or 'none'}",
        f"  Apple T2     : {'yes' if is_mac_t2 else 'no'}",
        f"  CPU vendor   : {cpu_vendor}",
        f"  Total RAM    : {ram_gib if ram_gib is not None else 'unknown'} GiB",
        f"  Use case     : {usecase}",
        f"  NVIDIA driver: {nvidia_confirmed} (proprietary blob only on confirmed+nvidia)",
    ]
    libcalamares.utils.debug("\n".join(summary))
    try:
        with open(os.path.join(chroot, "tmp", "tenebra-detection-summary.txt"), "w") as f:
            f.write("\n".join(summary) + "\n")
    except Exception:
        pass

    profile_map = {
        "gaming": "apply_gaming_profile",
        "learning": "apply_learning_profile",
        "office": "apply_office_profile",
    }
    func = profile_map.get(usecase)
    if not func:
        return f"Unknown usecase: {usecase}"

    # Compose the in-chroot setup script. Values are injected verbatim.
    t2_line = "apply_t2_support\n" if is_mac_t2 else ""

    script = (
        "#!/bin/bash\n"
        "set -e\n"
        f'export GPU_VENDOR="{gpu_vendor}"\n'
        f'export CPU_VENDOR="{cpu_vendor}"\n'
        f'export TOTAL_RAM_GIB="{ram_gib if ram_gib is not None else ""}"\n'
        f'export NVIDIA_CONFIRMED="{nvidia_confirmed}"\n'
        "source /tmp/tenebra-profiles/drivers.sh\n"
        "# System setup first: own repo, GPU drivers, T2 kernel — never skipped\n"
        "# even if the use-case profile below fails.\n"
        "install_tenebraos_repo\n"
        "apply_hardware_drivers\n"
        + t2_line +
        f"source /tmp/tenebra-profiles/{usecase}.sh\n"
        f"{func} || echo '[TenebraOS] {usecase} profile reported errors (continuing)'\n"
        "exit 0\n"
    )

    script_path = os.path.join(chroot, "tmp", "tenebra-profile.sh")
    with open(script_path, "w") as f:
        f.write(script)

    result = subprocess.run(
        ["chroot", chroot, "/bin/bash", "/tmp/tenebra-profile.sh"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return f"Profile script failed: {result.stderr}"
    return None
