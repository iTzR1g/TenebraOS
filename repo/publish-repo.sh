#!/bin/bash
# repo/publish-repo.sh
# Builds a flat apt repository index for GitHub Releases.
#
# Output goes to repo/release/ (flat layout matching GitHub asset names):
#   Packages, InRelease, Release, Release.gpg, tenebraos-repo.gpg
#
# Usage:
#   ./repo/publish-repo.sh          # generate index + sign
#   ./repo/upload-pool.sh           # upload everything to release

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"

KEY_NAME="TenebraOS Package Repository"
SECRET_KEY="$HOME/.config/tenebraos/tenebraos-repo.asc"

POOL_DIR="$REPO_ROOT/pool"
OUT_DIR="$REPO_ROOT/release"

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

# --- Public keyring ---
PUBLIC_KEY="$OUT_DIR/tenebraos-repo.gpg"
gpg --export "$KEY_ID" > "$PUBLIC_KEY"

# Keep the ISO's shipped copy in lockstep with the signing key.
ISO_KEY="$REPO_ROOT/../config/includes.chroot/usr/share/keyrings/tenebraos-repo.gpg"
if [ -d "$(dirname "$ISO_KEY")" ]; then
    install -m 644 "$PUBLIC_KEY" "$ISO_KEY"
    echo ">> Refreshed ISO keyring: $ISO_KEY"
fi

# --- Packages index (uses apt-repo-index.py) ---
python3 "$REPO_ROOT/apt-repo-index.py" "$POOL_DIR" "$OUT_DIR" tenebraos main amd64 tenebraos

# --- Release file ---
python3 - "$OUT_DIR" <<'PYEOF'
import hashlib, os, sys, email.utils, time

out_dir = sys.argv[1]

packages_path = os.path.join(out_dir, "Packages")
with open(packages_path, "rb") as f:
    packages_data = f.read()

now = int(time.time())
lines = [
    "Origin: TenebraOS",
    "Label: TenebraOS packages",
    "Suite: tenebraos",
    "Codename: tenebraos",
    "Version: 1.0",
    "Architectures: amd64",
    "Components: main",
    "Description: TenebraOS apt repository",
    f"Date: {email.utils.formatdate(now, usegmt=True)}",
    "Acquire-By-Hash: yes",
]
for algo, label in (("md5", "MD5Sum"), ("sha1", "SHA1"), ("sha256", "SHA256"), ("sha512", "SHA512")):
    lines.append(f"{label}:")
    h = hashlib.new(algo, packages_data).hexdigest()
    lines.append(f" {h} {len(packages_data)} Packages")

with open(os.path.join(out_dir, "Release"), "w") as f:
    f.write("\n".join(lines) + "\n")

print(">> Release + Packages generated in", out_dir)
PYEOF

# --- Sign ---
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --clearsign \
    --output "$OUT_DIR/InRelease" "$OUT_DIR/Release"
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --detach-sign --armor \
    --output "$OUT_DIR/Release.gpg" "$OUT_DIR/Release"

echo ">> Repo index ready in $OUT_DIR"
echo ">> Next: ./repo/upload-pool.sh  (uploads everything to one release)"
