#!/usr/bin/env python3
# autoconfig/main.py
# Reads globalStorage, copies profiles into chroot, runs the selected profile

import libcalamares
import os
import subprocess
import shutil

def run():
    usecase = libcalamares.globalstorage.value("usecase")
    if not usecase:
        return "No usecase selected"

    profiles_src = "/tenebraos-src/profiles"
    chroot = libcalamares.globalstorage.value("rootMountPoint")
    if not chroot:
        return "No rootMountPoint set"

    profiles_dst = os.path.join(chroot, "tmp", "tenebraos-profiles")
    shutil.copytree(profiles_src, profiles_dst, dirs_exist_ok=True)

    install_script = os.path.join(profiles_dst, "install.sh")
    with open(install_script, "w") as f:
        f.write(f'''#!/bin/bash
source /tmp/tenebraos-profiles/drivers.sh
source /tmp/tenebraos-profiles/{usecase}.sh
apply_{usecase}_profile
''')
    os.chmod(install_script, 0o755)

    profile_map = {
        "gaming": "apply_gaming_profile",
        "learning": "apply_learning_profile",
        "office": "apply_office_profile",
    }

    func = profile_map.get(usecase)
    if not func:
        return f"Unknown usecase: {usecase}"

    with open(os.path.join(chroot, "tmp", "tenebraos-profile.sh"), "w") as f:
        f.write(f'''#!/bin/bash
source /tmp/tenebraos-profiles/drivers.sh
source /tmp/tenebraos-profiles/{usecase}.sh
{func}
''')

    result = subprocess.run(
        ["chroot", chroot, "/bin/bash", "/tmp/tenebraos-profile.sh"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        return f"Profile script failed: {result.stderr}"

    return None
