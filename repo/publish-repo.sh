#!/bin/bash
# repo/publish-repo.sh
# Builds the TenebraOS apt repository index and signs it.
#
# Layout:
#   repo/pool/                  .deb files (uploaded as GitHub Release assets)
#   repo/dists/tenebraos/
#     ├── InRelease, Release, Release.gpg      suite-level metadata (committed)
#     ├── main/binary-amd64/Packages           component index (committed)
#     └── tenebraos-repo.gpg                   public keyring (shipped in ISO)
#
# Publishing workflow:
#   ./repo/publish-repo.sh     # build index (Packages + Release + InRelease)
#   ./repo/upload-pool.sh      # upload repo/pool/*.deb as GitHub Release assets
#   git add repo/ && git commit && git push   # index goes live
#
# Targets use (index via raw.githubusercontent, debs via release assets):
#   deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg]
#       https://raw.githubusercontent.com/iTzR1g/TenebraOS/main/repo tenebraos main

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

# Keep the ISO's shipped copy in lockstep with the signing key — a drifted
# keyring makes apt reject every package with NO_PUBKEY on targets.
ISO_KEY="$REPO_ROOT/../config/includes.chroot/usr/share/keyrings/tenebraos-repo.gpg"
if [ -d "$(dirname "$ISO_KEY")" ]; then
    install -m 644 "$PUBLIC_KEY" "$ISO_KEY"
    echo ">> Refreshed ISO keyring: config/includes.chroot/usr/share/keyrings/tenebraos-repo.gpg"
fi

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
else
    echo ">> apt-ftparchive not found — using repo/apt-repo-index.py"
    python3 "$REPO_ROOT/apt-repo-index.py" \
        "$BASE_URL" "$POOL_DIR" "$OUT_DIR" "$DISTRO" "$COMPONENT" "$ARCH" "$SUITE_NAME"
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
    echo ">> apt-ftparchive not found — generating minimal Release file"
    {
        echo "Origin: TenebraOS"
        echo "Label: TenebraOS packages"
        echo "Suite: ${SUITE_NAME}"
        echo "Codename: ${SUITE_NAME}"
        echo "Architectures: ${ARCH}"
        echo "Components: ${COMPONENT}"
        echo "Date: $(date -Ru)"
        cd "$SUITE_DIR"
        find main -type f ! -name Release ! -name InRelease ! -name 'Release.gpg' -print0 |
            xargs -0 md5sum | awk '{printf " %s  %s\n", $1, $2}'
    } > "$SUITE_DIR/Release"
fi

gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --clearsign \
    --output "$SUITE_DIR/InRelease" "$SUITE_DIR/Release"
gpg --batch --yes --pinentry-mode loopback --default-key "$KEY_ID" --detach-sign --armor \
    --output "$SUITE_DIR/Release.gpg" "$SUITE_DIR/Release"

# legacy: component-level metadata from the old layout confuses nobody but
# clutters the tree — drop it if present.
rm -f "$OUT_DIR/Release" "$OUT_DIR/InRelease" "$OUT_DIR/Release.gpg"

echo ">> Repo index ready in $SUITE_DIR (Packages, Release, InRelease)."
echo ">> Next: ./repo/upload-pool.sh  then commit these files and push."