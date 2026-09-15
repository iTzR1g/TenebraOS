#!/usr/bin/env python3
# autoconfig — post-install (chroot) setup that:
#   1. wires up the TenebraOS repo
#   2. auto-applies hardware drivers from the detection done by hardwaredetect
#      (GPU vendor, Apple T2 kernel, CPU tuned packages when applicable)
#   3. applies the user's chosen use-case profile (gaming / learning / office / minimal)
#   4. applies the chosen desktop environment / window manager (plasma / xfce / i3 / sway / minimal)
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

    # environment is the second packagechooser instance; it stores the
    # chosen desktop/window-manager as "packagechooser_environment".
    env = libcalamares.globalstorage.value("packagechooser_environment")
    if isinstance(env, str):
        env = env.strip().split(",")[0]
    if not env or env in ("", "required"):
        # The CachyOS-style default: Plasma ships in the ISO.
        env = "plasma"

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

    # Username chosen in the users module; used to drop the live/preview
    # "user" account from the target so the installed system only has the
    # account the person actually created (and its password).
    chosen_user = libcalamares.globalstorage.value("username") or ""
    if isinstance(chosen_user, str):
        chosen_user = chosen_user.strip()

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
        f"  Environment  : {env}",
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
        "minimal": "apply_minimal_profile",
    }
    func = profile_map.get(usecase)
    if not func:
        return f"Unknown usecase: {usecase}"

    env_map = {
        "plasma": "apply_environment_plasma",
        "xfce": "apply_environment_xfce",
        "i3": "apply_environment_i3",
        "sway": "apply_environment_sway",
        "minimal": "apply_environment_minimal",
    }
    env_func = env_map.get(env)
    if not env_func:
        return f"Unknown environment: {env}"

    # Compose the in-chroot setup script. Values are injected verbatim.
    t2_line = "apply_t2_support\n" if is_mac_t2 else ""

    # The live image carries a "user" account (created by 0007-live-user
    # during the ISO build) plus its password (user:user). After a real
    # install that account must not survive: Calamares created the real
    # one and the password entered there is the only one that matters.
    # Guard so we never run two consecutive userdel for the live account
    # if the user themselves happen to pick "user"; and leave admin stuff.
    live_user_cleanup = (
        f'if [ "$(id -u {chosen_user} 2>/dev/null || echo -1)" != "0" ] && '
        f'id user >/dev/null 2>&1 && [ "{chosen_user}" != "user" ]; then\n'
        "    echo '[TenebraOS] removing leftover live user account'\n"
        "    userdel -r user 2>/dev/null || userdel user 2>/dev/null || true\n"
        "fi\n"
    )

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
        live_user_cleanup +
        f"source /tmp/tenebra-profiles/{usecase}.sh\n"
        f"{func} || echo '[TenebraOS] {usecase} profile reported errors (continuing)'\n"
        f"source /tmp/tenebra-profiles/environments.sh\n"
        f"{env_func} || echo '[TenebraOS] environment ({env}) reported errors (continuing)'\n"
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
