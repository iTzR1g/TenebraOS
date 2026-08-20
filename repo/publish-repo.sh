#!/bin/bash
# repo/publish-repo.sh
# Builds the TenebraOS apt repository index and signs it.
#
# Layout:
#   repo/pool/                  .deb files (uploaded as GitHub Release assets)
#   repo/dists/tenebraos/main/binary-amd64/  (Packages, Release, InRelease — committed)
#   repo/dists/tenebraos/tenebraos-repo.gpg  public keyring (committed, shipped in ISO)
#
# Publishing workflow:
#   ./repo/publish-repo.sh     # build index (Packages + Release + InRelease)
#   ./repo/upload-pool.sh      # upload repo/pool/*.deb as GitHub Release assets
#   git add repo/ && git commit && git push   # index goes live
#
# Targets then use:
#   deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg]
#       https://github.com/iTzR1g/TenebraOS/releases/download/tenebraos-repo-pool/ ./

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
OWNER="iTzR1g"
REPO_NAME="TenebraOS"
RELEASE_TAG="tenebraos-repo-pool"
BASE_URL="https://github.com/${OWNER}/${REPO_NAME}/releases/download/${RELEASE_TAG}"

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

# --- Packages index + Release file ---
if command -v apt-ftparchive >/dev/null; then
    if compgen -G "$POOL_DIR/*.deb" >/dev/null; then
        ( cd "$POOL_DIR" && apt-ftparchive packages . ) > "$OUT_DIR/Packages"
    else
        : > "$OUT_DIR/Packages"
    fi

    # Rewrite Filename: to the full GitHub release asset URL
    awk -v base="$BASE_URL" '
        /^Filename: / { file = $2; sub(/^\.\//, "", file); print "Filename: " base "/" file; next }
        { print }
    ' "$OUT_DIR/Packages" > "$OUT_DIR/Packages.final"
    mv "$OUT_DIR/Packages.final" "$OUT_DIR/Packages"

    ( cd "$OUT_DIR" && apt-ftparchive -o APT::FTPArchive::Release::Origin="TenebraOS" \
        -o APT::FTPArchive::Release::Label="TenebraOS packages" \
        -o APT::FTPArchive::Release::Suite="$SUITE_NAME" \
        -o APT::FTPArchive::Release::Component="$COMPONENT" \
        -o APT::FTPArchive::Release::Architecture="$ARCH" \
        -o APT::FTPArchive::Release::Codename="$SUITE_NAME" \
        release . ) > "$OUT_DIR/Release"
else
    echo ">> apt-ftparchive not found — using repo/apt-repo-index.py"
    python3 "$REPO_ROOT/apt-repo-index.py" \
        "$BASE_URL" "$POOL_DIR" "$OUT_DIR" "$DISTRO" "$COMPONENT" "$ARCH" "$SUITE_NAME"
fi

# --- Signatures ---
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --clearsign \
    --output "$OUT_DIR/InRelease" "$OUT_DIR/Release"
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --detach-sign --armor \
    --output "$OUT_DIR/Release.gpg" "$OUT_DIR/Release"

echo ">> Repo index ready in $OUT_DIR (Packages, Release, InRelease)."
echo ">> Next: ./repo/upload-pool.sh  then commit these files and push."