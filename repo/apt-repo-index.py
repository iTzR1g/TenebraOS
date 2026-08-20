#!/usr/bin/env python3
# repo/apt-repo-index.py
# Minimal apt repository index generator (Packages + Release) used when
# apt-ftparchive is not installed. Produces the same layout.

import glob
import hashlib
import os
import subprocess
import sys
import email.utils
import time

def rfc1123(ts):
    return email.utils.formatdate(ts, usegmt=True)

def deb_control(path):
    ctl = subprocess.check_output(["dpkg-deb", "-f", path], text=True)
    fields = {}
    key = None
    for raw in ctl.splitlines():
        if raw.startswith(" ") or raw.startswith("\t"):
            if key:
                fields[key] += "\n" + raw.strip()
            continue
        if ":" in raw:
            key, val = raw.split(":", 1)
            fields[key] = val.strip()
    return fields

def index_dir(base_url, pool_dir, out_dir, distro, component, arch, suite):
    os.makedirs(out_dir, exist_ok=True)
    stanzas = []
    for deb in sorted(glob.glob(os.path.join(pool_dir, "*.deb"))):
        fields = deb_control(deb)
        with open(deb, "rb") as f:
            data = f.read()
        needed = ["Package", "Version", "Architecture", "Maintainer",
                  "Installed-Size", "Depends", "Section", "Priority",
                  "Homepage", "Description"]
        fields = {k: v for k, v in fields.items() if k in needed}
        fields["Filename"] = base_url + "/" + os.path.basename(deb)
        fields["Size"] = str(len(data))
        fields["MD5sum"] = hashlib.md5(data).hexdigest()
        fields["SHA256"] = hashlib.sha256(data).hexdigest()
        fields["SHA1"] = hashlib.sha1(data).hexdigest()
        stanzas.append("\n".join(f"{k}: {v}" for k, v in fields.items()))
    packages = "\n\n".join(stanzas) + ("\n" if stanzas else "")
    with open(os.path.join(out_dir, "Packages"), "w") as f:
        f.write(packages)

    now = int(time.time())
    lines = [
        f"Origin: {distro}",
        f"Label: {distro} packages",
        f"Suite: {suite}",
        f"Codename: {suite}",
        "Version: 1.0",
        f"Architectures: {arch}",
        f"Components: {component}",
        f"Description: {distro} apt repository",
        f"Date: {rfc1123(now)}",
        "Acquire-By-Hash: yes",
    ]

    checksums = {"MD5Sum": [], "SHA1": [], "SHA256": [], "SHA512": []}
    for name in ["Packages", "Release.gpg"]:
        p = os.path.join(out_dir, name)
        if not os.path.exists(p):
            continue
        with open(p, "rb") as f:
            data = f.read()
        size = len(data)
        checksums["MD5Sum"].append((hashlib.md5(data).hexdigest(), size, name))
        checksums["SHA1"].append((hashlib.sha1(data).hexdigest(), size, name))
        checksums["SHA256"].append((hashlib.sha256(data).hexdigest(), size, name))
        checksums["SHA512"].append((hashlib.sha512(data).hexdigest(), size, name))
    for k in ["MD5Sum", "SHA1", "SHA256", "SHA512"]:
        lines.append(f"{k}:")
        for digest, size, name in checksums[k]:
            lines.append(f" {digest} {size} {name}")

    with open(os.path.join(out_dir, "Release"), "w") as f:
        f.write("\n".join(lines) + "\n")

if __name__ == "__main__":
    index_dir(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
              sys.argv[5], sys.argv[6], sys.argv[7])