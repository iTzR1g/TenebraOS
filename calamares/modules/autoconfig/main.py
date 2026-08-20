#!/usr/bin/env python3
import libcalamares
import os
import subprocess
import shutil

def run():
    usecase = libcalamares.globalstorage.value("usecase")
    if not usecase:
        return "No usecase selected"
    profiles_src = "/tenebra-src/profiles"
    chroot = libcalamares.globalstorage.value("rootMountPoint")
    if not chroot:
        return "No rootMountPoint set"
    profiles_dst = os.path.join(chroot, "tmp", "tenebra-profiles")
    shutil.copytree(profiles_src, profiles_dst, dirs_exist_ok=True)

    key_src = "/usr/share/keyrings/tenebraos-repo.gpg"
    key_dst = os.path.join(chroot, "tmp", "tenebraos-repo.gpg")
    if os.path.exists(key_src):
        shutil.copy(key_src, key_dst)

    gpu_vendor = libcalamares.globalstorage.value("gpu_vendor") or "unknown"
    is_mac_t2 = bool(libcalamares.globalstorage.value("is_mac_t2"))

    profile_map = {
        "gaming": "apply_gaming_profile",
        "learning": "apply_learning_profile",
        "office": "apply_office_profile",
    }
    func = profile_map.get(usecase)
    if not func:
        return f"Unknown usecase: {usecase}"

    t2_line = "apply_t2_support\n" if is_mac_t2 else ""

    with open(os.path.join(chroot, "tmp", "tenebra-profile.sh"), "w") as f:
        f.write(f'''#!/bin/bash
set -e
export GPU_VENDOR="{gpu_vendor}"
source /tmp/tenebra-profiles/drivers.sh
# System setup first: own repo, GPU drivers, T2 kernel — never skipped
# even if the usecase profile below fails.
install_tenebraos_repo
apply_hardware_drivers
{t2_line}source /tmp/tenebra-profiles/{usecase}.sh
{func} || echo "[TenebraOS] {usecase} profile reported errors (continuing)"
exit 0
''')
    result = subprocess.run(
        ["chroot", chroot, "/bin/bash", "/tmp/tenebra-profile.sh"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return f"Profile script failed: {result.stderr}"
    return None