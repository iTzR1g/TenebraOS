# installer/modules/hardware_detect/hardware_detect.py
# TenebraOS - Hardware detection Calamares module
# Debian 13 (Trixie)
#
# Detects:
#   - T2 MacBook (2018+) via apple_bce kernel module
#   - Legacy Mac via DMI manufacturer field
#   - Standard PC
#   - GPU vendor: Nvidia (10de), AMD (1002), Intel (8086)

import subprocess
import libcalamares


def detect_hardware():
    result = {}

    # --- Mac / T2 detection ---
    try:
        product = subprocess.check_output(
            ['dmidecode', '-s', 'system-product-name'],
            stderr=subprocess.DEVNULL).decode().strip()
        mfr = subprocess.check_output(
            ['dmidecode', '-s', 'system-manufacturer'],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        product, mfr = '', ''

    if 'Apple' in mfr:
        try:
            mods = subprocess.check_output(['lsmod']).decode()
            result['mac'] = 't2' if 'apple_bce' in mods else 'legacy'
        except Exception:
            result['mac'] = 'legacy'
    else:
        result['mac'] = 'none'

    # --- GPU detection via lspci vendor IDs ---
    try:
        lspci = subprocess.check_output(['lspci', '-n']).decode()
    except Exception:
        lspci = ''

    if '10de:' in lspci:
        result['gpu'] = 'nvidia'
    elif '1002:' in lspci:
        result['gpu'] = 'amd'
    else:
        result['gpu'] = 'intel'

    return result


def run():
    hw = detect_hardware()
    libcalamares.globalstorage.insert('hardware', hw)

    # Write to /tmp so autoconfig.sh can read it inside chroot
    with open('/tmp/calamares-hardware.conf', 'w') as f:
        f.write(f"{hw['mac']}:{hw['gpu']}")

    return None  # None = success in Calamares
