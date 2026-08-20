#!/usr/bin/env python3
"""repack-deb.py — modify a .deb archive without dpkg-deb/ar.

Rewrites control fields and injects extra files into the data tarball,
producing a valid .deb (ar + tar.gz members).

Usage:
  repack-deb.py --in in.deb --out out.deb \
      --control "Package=tenebraos-fastfetch" \
      --control "Description=..." \
      --add srcfile:/usr/share/foo/bar \
      --add srcdir:/usr/share/foo/baz

Only gzip-compressed control/data members are supported.
"""

import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import tarfile

AR_MAGIC = b"!<arch>\n"
AR_HDR = 60
MODE = "0644"


def read_ar(path):
    """Return list of (name, data) member tuples."""
    with open(path, "rb") as f:
        blob = f.read()
    if not blob.startswith(AR_MAGIC):
        raise SystemExit(f"{path}: not an ar archive")
    members = []
    pos = len(AR_MAGIC)
    end = len(blob)
    seen_symtab = False
    while pos + AR_HDR <= end:
        hdr = blob[pos : pos + AR_HDR]
        name = hdr[0:16].decode("ascii", "replace").rstrip(" ").rstrip("/ ")
        size = int(hdr[48:58].decode().strip() or 0)
        pos += AR_HDR
        data = blob[pos : pos + size]
        pos += size
        if (pos - end) % 2 and pos < end:
            pos += 1  # pad byte after odd-sized member
        if name == "/" or name.startswith("//"):
            seen_symtab = True
            continue  # symbol tables are not needed for a valid .deb
        members.append((name, data))
    if not seen_symtab and len(members) < 3:
        raise SystemExit(f"{path}: not a .deb (no ar members)")
    return members


def write_ar(path, members):
    with open(path, "wb") as f:
        f.write(AR_MAGIC)
        for name, data in members:
            name_fmt = name.encode()[:15]
            hdr = (
                name_fmt.ljust(16, b" ")
                + b"0".ljust(12, b" ")
                + b"0".ljust(6, b" ")
                + b"0".ljust(6, b" ")
                + MODE.encode().ljust(8, b" ")
                + str(len(data)).encode().ljust(10, b" ")
                + b"`\n"
            )
            f.write(hdr)
            f.write(data)
            if len(data) % 2:
                f.write(b"\n")


def ungz(data):
    return gzip.decompress(data)


def gz(data, mtime=0):
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=mtime) as g:
        g.write(data)
    return out.getvalue()


def parse_control(text):
    entries = {}
    order = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"^([A-Za-z0-9-]+):\s?(.*)$", lines[i])
        key, val = m.group(1), m.group(2)
        i += 1
        while i < len(lines) and (lines[i].startswith(" ") or not lines[i]):
            val += "\n" + lines[i]
            i += 1
        entries[key] = val
        order.append(key)
    return entries, order


def write_control(entries, order):
    out = []
    for key in order:
        val = entries[key]
        first, *rest = val.split("\n")
        out.append(f"{key}: {first}")
        for line in rest:
            out.append(line if line.startswith(" ") else f" {line}")
    return "\n".join(out) + "\n"


def apply_adds(tar_path, adds):
    """Extract tar.gz member, inject files, return new gzip data."""
    tree = {}
    with tarfile.open(fileobj=io.BytesIO(ungz(tar_path)), mode="r:") as tf:
        for member in tf.getmembers():
            data = tf.extractfile(member).read() if member.isfile() else b""
            tree[member.name] = (member, data)

    for src, dest in adds:
        sname = dest.lstrip("./")
        if os.path.isdir(src):
            for root, _dirs, files in os.walk(src):
                for fn in files:
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, src)
                    target = f"{sname.rstrip('/')}/{rel}"
                    info = tarfile.TarInfo(target)
                    content = open(full, "rb").read()
                    info.size = len(content)
                    info.mode = os.stat(full).st_mode & 0o777 or int(MODE, 8)
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    tree[target] = (info, content)
        else:
            info = tarfile.TarInfo(sname)
            content = open(src, "rb").read()
            info.size = len(content)
            info.mode = os.stat(src).st_mode & 0o777 or int(MODE, 8)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            tree[sname] = (info, content)

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.GNU_FORMAT) as tf:
        for name in sorted(tree):
            info, content = tree[name]
            if info.isfile():
                tf.addfile(info, io.BytesIO(content))
            else:
                tf.addfile(info)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", dest="dst", required=True)
    ap.add_argument("--control", action="append", default=[])
    ap.add_argument("--add", action="append", default=[], metavar="SRC:DEST")
    args = ap.parse_args()

    members = read_ar(args.src)
    by_name = {n: d for n, d in members}

    control_tar = None
    data_tar = None
    for name in by_name:
        if name.startswith("control.tar"):
            control_tar = name
        elif name.startswith("data.tar"):
            data_tar = name
    if not control_tar or not data_tar:
        raise SystemExit("unsupported .deb: need control.tar.* and data.tar.* members")

    # --- control ---
    ctl_io = io.BytesIO(by_name[control_tar])
    with tarfile.open(fileobj=ctl_io, mode="r:gz") as tf:
        ctl = {m.name.lstrip("./"): (tf.extractfile(m).read().decode() if m.isfile() else "") for m in tf.getmembers()}
    entries, order = parse_control(ctl["control"])
    overrides = {}
    for kv in args.control:
        k, _, v = kv.partition("=")
        overrides[k] = v
    for k, v in overrides.items():
        if k in entries:
            entries[k] = v
        else:
            entries[k] = v
            order.append(k)
    ctl["control"] = write_control(entries, order)

    # keep md5sums/conffiles consistent
    added_files = [(s, d.lstrip("./")) for s, d in (a.split(":", 1) for a in args.add)]
    plain_files = [(s, d) for s, d in added_files if not os.path.isdir(s)]
    if "md5sums" in ctl:
        ctl["md5sums"] = ctl["md5sums"].rstrip("\n") + "\n" + "".join(
            f"{hashlib.md5(open(s, 'rb').read()).hexdigest()}  {d}\n" for s, d in plain_files
        )
    if "conffiles" in ctl:
        ctl["conffiles"] = ctl["conffiles"].rstrip("\n") + "\n" + "\n".join(
            f"/{d}" for _s, d in plain_files if d.startswith("etc/")
        ) + "\n"

    cbuf = io.BytesIO()
    with tarfile.open(fileobj=cbuf, mode="w:gz", format=tarfile.GNU_FORMAT) as tf:
        for name, text in ctl.items():
            info = tarfile.TarInfo(name)
            data = text.encode()
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = "root"
            tf.addfile(info, io.BytesIO(data))

    # --- data ---
    data = apply_adds(by_name[data_tar], [(s, d) for s, d in [a.split(":") for a in args.add]])

    new_members = []
    for name, data_ in members:
        if name == control_tar:
            new_members.append((name, cbuf.getvalue()))
        elif name == data_tar:
            new_members.append((name, data))
        elif name == "debian-binary":
            new_members.append((name, data_))
        # drop everything else (e.g. _gpgorigin)
    write_ar(args.dst, new_members)
    print(f"wrote {args.dst}")


if __name__ == "__main__":
    main()