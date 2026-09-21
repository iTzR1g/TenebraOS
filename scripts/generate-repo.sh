#!/bin/bash
# scripts/generate-repo.sh
# Builds a signed flat APT repository (./) for GitHub Pages.
#
# Publishes into $PUBLISH_DIR (default: repo/pages):
#   Packages, Packages.gz, Release, InRelease, Release.gpg, public.gpg
#   plus flat copies of every .deb staged by the workflow.
#
# Environment:
#   GPG_PRIVATE_KEY   ASCII-armored secret (signing) key. REQUIRED.
#   GPG_PASSPHRASE    Passphrase for the secret key, if one is set.
#   PUBLISH_DIR       Output directory (default: "$REPO_ROOT/repo/pages").
#
# Usage:
#   ./scripts/generate-repo.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PUBLISH_DIR="${PUBLISH_DIR:-$REPO_ROOT/repo/pages}"

command -v dpkg-scanpackages >/dev/null || { echo "ERROR: dpkg-scanpackages not found (install dpkg-dev)" >&2; exit 1; }
command -v gzip >/dev/null || { echo "ERROR: gzip not found" >&2; exit 1; }
command -v apt-ftparchive >/dev/null || { echo "ERROR: apt-ftparchive not found (install apt-utils)" >&2; exit 1; }
command -v gpg >/dev/null || { echo "ERROR: gpg not found (install gnupg)" >&2; exit 1; }

# --- Signing key from the environment (never committed to git) ---
if [ -z "${GPG_PRIVATE_KEY:-}" ]; then
    echo "ERROR: GPG_PRIVATE_KEY is not set." >&2
    echo "       Add it as a GitHub repository secret (Settings > Secrets and" >&2
    echo "       variables > Actions > New repository secret). Its value is the" >&2
    echo "       output of:  gpg --armor --export-secret-key <key-id>" >&2
    exit 1
fi

echo ">> Importing signing key..."
GNUPGHOME="$(mktemp -d)"; chmod 700 "$GNUPGHOME"
export GNUPGHOME
trap 'rm -rf "$GNUPGHOME"' EXIT

if [ -n "${GPG_PASSPHRASE:-}" ]; then
    printf '%s\n' "$GPG_PRIVATE_KEY" | gpg --batch --import --pinentry-mode loopback --passphrase "$GPG_PASSPHRASE"
else
    printf '%s\n' "$GPG_PRIVATE_KEY" | gpg --batch --import
fi

KEY_ID="$(gpg --list-secret-keys --with-colons 2>/dev/null | awk -F: '/^fpr:/{print $10; exit}')"
[ -n "$KEY_ID" ] || { echo "ERROR: no usable secret key after import" >&2; exit 1; }
echo ">> Using key: $KEY_ID"

# --- Stage .deb files flat (workflow builds them into repo/ before this) ---
rm -rf "$PUBLISH_DIR"
mkdir -p "$PUBLISH_DIR"

STAGED=0
for glob in "$REPO_ROOT"/repo/*.deb "$REPO_ROOT"/repo/pool/*.deb; do
    [ -f "$glob" ] || continue
    cp -f "$glob" "$PUBLISH_DIR/"
    STAGED=$((STAGED + 1))
done
if [ "$STAGED" -eq 0 ]; then
    echo "ERROR: no .deb packages found in repo/ or repo/pool/." >&2
    echo "       Run build-packages.sh (or stage debs) before generate-repo.sh." >&2
    exit 1
fi
echo ">> Staged $STAGED package(s) into $PUBLISH_DIR"

# --- Index: Packages + Packages.gz ---
# Run from inside $PUBLISH_DIR so the Filename field is "./<pkg>.deb" (flat
# `deb ... ./` layout) instead of an absolute path.
(cd "$PUBLISH_DIR" && dpkg-scanpackages . /dev/null > "$PUBLISH_DIR/Packages")
gzip -9 -kf "$PUBLISH_DIR/Packages"

# --- Release file (checksums of the index; apt-ftparchive release . ) ---
(
    cd "$PUBLISH_DIR"
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="TenebraOS" \
        -o APT::FTPArchive::Release::Label="TenebraOS packages" \
        -o APT::FTPArchive::Release::Suite="tenebraos" \
        -o APT::FTPArchive::Release::Codename="tenebraos" \
        -o APT::FTPArchive::Release::Description="TenebraOS apt repository (GitHub Pages)" \
        release . > Release
)

# --- Sign: InRelease (clearsigned) + Release.gpg (detached) ---
SIGN_ARGS=(--batch --yes --default-key "$KEY_ID" --pinentry-mode loopback)
if [ -n "${GPG_PASSPHRASE:-}" ]; then
    SIGN_ARGS+=(--passphrase "$GPG_PASSPHRASE")
fi
gpg "${SIGN_ARGS[@]}" --clearsign --output "$PUBLISH_DIR/InRelease" "$PUBLISH_DIR/Release"
gpg "${SIGN_ARGS[@]}" --detach-sign --armor --output "$PUBLISH_DIR/Release.gpg" "$PUBLISH_DIR/Release"

# --- Public key for end users (armored so `gpg --dearmor` works) ---
gpg --armor --export "$KEY_ID" > "$PUBLISH_DIR/public.gpg"

echo ">> Done. Flat apt repo ready in $PUBLISH_DIR:"
ls -lh "$PUBLISH_DIR"