#!/bin/bash
# repo/publish-repo.sh
# Builds the TenebraOS apt repository index and signs it.
#
# Architecture:
#   All files (index + pool .debs) are hosted on a SINGLE GitHub Release.
#   The deb source line points at the release base URL.
#   Filename fields in Packages are RELATIVE to that base.
#
#   deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] \
#       https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo/ \
#       tenebraos main
#
#   apt constructs: <base>/pool/<name>.deb  (same origin as index)
#
# Usage:
#   ./repo/publish-repo.sh          # generate index + sign
#   ./repo/upload-pool.sh           # upload everything to release

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
OWNER="iTzR1g"
REPO_NAME="TenebraOS"
RELEASE_TAG="tenebraos-repo"

DISTRO="tenebraos"
COMPONENT="main"
ARCH="amd64"
SUITE_NAME="tenebraos"

KEY_NAME="TenebraOS Package Repository"
SECRET_KEY="$HOME/.config/tenebraos/tenebraos-repo.asc"
PUBLIC_KEY="$REPO_ROOT/dists/${DISTRO}/tenebraos-repo.gpg"

POOL_DIR="$REPO_ROOT/pool"
BIN_DIR="dists/${DISTRO}/${COMPONENT}/binary-${ARCH}"
OUT_DIR="$REPO_ROOT/$BIN_DIR"

command -v gpg >/dev/null || { echo "gpg missing" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 missing" >&2; exit 1; }

mkdir -p "$OUT_DIR" "$(dirname "$SECRET_KEY")" "$POOL_DIR"

# --- Signing key (generated once, secret stays on the maintainer machine) ---
if [ ! -f "$SECRET_KEY" ]; then
    echo ">> Generating repository signing key..."
    gpg --batch --gen-key <<EOF
%no-protection
Key-Type: eddsa
Key-Curve: ed25519
Name-Real: $KEY_NAME
Expire-Date: 10y
%commit
EOF
    KEY_ID="$(gpg --list-keys --with-colons "$KEY_NAME" | awk -F: '/^fpr:/ {print $10; exit}')"
    gpg --armor --export-secret-key "$KEY_ID" > "$SECRET_KEY"
    chmod 600 "$SECRET_KEY"
    echo ">> Secret key saved to $SECRET_KEY (keep it safe!)"
else
    gpg --import "$SECRET_KEY" >/dev/null 2>&1 || true
fi

KEY_ID="$(gpg --list-keys --with-colons "$KEY_NAME" | awk -F: '/^fpr:/ {print $10; exit}')"
[ -n "$KEY_ID" ] || { echo "signing key not found" >&2; exit 1; }

# --- Public keyring (shipped inside the ISO, used as signed-by on targets) ---
gpg --export "$KEY_ID" > "$PUBLIC_KEY"

# Keep the ISO's shipped copy in lockstep with the signing key.
ISO_KEY="$REPO_ROOT/../config/includes.chroot/usr/share/keyrings/tenebraos-repo.gpg"
if [ -d "$(dirname "$ISO_KEY")" ]; then
    install -m 644 "$PUBLIC_KEY" "$ISO_KEY"
    echo ">> Refreshed ISO keyring: $ISO_KEY"
fi

# --- Packages index (relative Filename paths) ---
if command -v apt-ftparchive >/dev/null; then
    if compgen -G "$POOL_DIR/*.deb" >/dev/null; then
        ( cd "$POOL_DIR" && apt-ftparchive packages . ) > "$OUT_DIR/Packages"
    else
        : > "$OUT_DIR/Packages"
    fi

    # Rewrite Filename: to be relative to repo root (pool/<name>.deb)
    # URL-encode + -> %2B for GitHub release asset compatibility
    awk '
        /^Filename: / {
            file = $2; sub(/^\.\//, "", file)
            gsub(/\+/, "%2B", file)
            gsub(/ /, "%20", file)
            print "Filename: " file; next
        }
        { print }
    ' "$OUT_DIR/Packages" > "$OUT_DIR/Packages.final"
    mv "$OUT_DIR/Packages.final" "$OUT_DIR/Packages"
else
    echo ">> apt-ftparchive not found — using repo/apt-repo-index.py"
    python3 "$REPO_ROOT/apt-repo-index.py" \
        "$POOL_DIR" "$OUT_DIR" "$DISTRO" "$COMPONENT" "$ARCH" "$SUITE_NAME"
fi

# --- Signatures (suite level: dists/tenebraos/{InRelease,Release,Release.gpg}) ---
SUITE_DIR="$REPO_ROOT/dists/${DISTRO}"

if command -v apt-ftparchive >/dev/null; then
    ( cd "$SUITE_DIR" && apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="TenebraOS" \
        -o APT::FTPArchive::Release::Label="TenebraOS packages" \
        -o APT::FTPArchive::Release::Suite="$SUITE_NAME" \
        -o APT::FTPArchive::Release::Codename="$SUITE_NAME" \
        -o APT::FTPArchive::Release::Architectures="${ARCH}" \
        -o APT::FTPArchive::Release::Components="${COMPONENT}" \
        release . ) > "$SUITE_DIR/Release"
else
    echo ">> apt-ftparchive not found — generating structured Release file"
    python3 - "$SUITE_DIR" "$SUITE_NAME" "$ARCH" "$COMPONENT" <<'PYEOF'
import hashlib, os, sys, email.utils, time

suite_dir, suite, arch, component = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
base = os.path.join(suite_dir, component)
files = []
for root, _, names in os.walk(base):
    for n in names:
        p = os.path.join(root, n)
        if n in ("Release", "InRelease", "Release.gpg"):
            continue
        files.append(p)
files.sort()

def rel(p):
    return os.path.relpath(p, suite_dir)

lines = [
    "Origin: TenebraOS",
    "Label: TenebraOS packages",
    f"Suite: {suite}",
    f"Codename: {suite}",
    "Version: 1.0",
    f"Architectures: {arch}",
    f"Components: {component}",
    "Description: TenebraOS apt repository",
    f"Date: {email.utils.formatdate(int(time.time()), usegmt=True)}",
    "Acquire-By-Hash: yes",
]
for algo, name in (("md5", "MD5Sum"), ("sha1", "SHA1"),
                   ("sha256", "SHA256"), ("sha512", "SHA512")):
    lines.append(f"{name}:")
    for p in files:
        h = hashlib.new(algo, open(p, "rb").read()).hexdigest()
        lines.append(f" {h} {os.path.getsize(p)} {rel(p)}")
with open(os.path.join(suite_dir, "Release"), "w") as f:
    f.write("\n".join(lines) + "\n")
PYEOF
fi

gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --clearsign \
    --output "$SUITE_DIR/InRelease" "$SUITE_DIR/Release"
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --detach-sign --armor \
    --output "$SUITE_DIR/Release.gpg" "$SUITE_DIR/Release"

rm -f "$OUT_DIR/Release" "$OUT_DIR/InRelease" "$OUT_DIR/Release.gpg"

echo ">> Repo index ready in $SUITE_DIR"
echo ">> Next: ./repo/upload-pool.sh  (uploads everything to one release)"
