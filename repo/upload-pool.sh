#!/bin/bash
# repo/upload-pool.sh
# Uploads the ENTIRE apt repo (pool/*.deb + dists/*) to a single
# GitHub Release. The deb source line points at this release's base URL,
# so apt can download packages from the same origin as the index.
#
# Requires: gh CLI (https://cli.github.com) with access to iTzR1g/TenebraOS.
# Usage: ./repo/upload-pool.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
POOL_DIR="$REPO_ROOT/pool"
RELEASE_DIR="$REPO_ROOT/release"
OWNER="iTzR1g"
REPO_NAME="TenebraOS-packages"
TAG="tenebraos-repo"

command -v gh >/dev/null || { echo "gh CLI missing: https://cli.github.com" >&2; exit 1; }

# Ensure release exists
gh release view "$TAG" --repo "$OWNER/$REPO_NAME" >/dev/null 2>&1 \
    || gh release create "$TAG" --repo "$OWNER/$REPO_NAME" \
        --title "TenebraOS apt repository" \
        --notes "Full apt repository: index + pool. See repo/publish-repo.sh." \
        --latest=false

# --- Upload pool/*.deb ---
shopt -s nullglob
DEBS=("$POOL_DIR"/*.deb)
if [ ${#DEBS[@]} -gt 0 ]; then
    echo ">> Uploading ${#DEBS[@]} packages from pool/..."
    for deb in "${DEBS[@]}"; do
        NAME="pool/$(basename "$deb")"
        echo "   $NAME"
        gh release upload "$TAG" "$deb" \
            --repo "$OWNER/$REPO_NAME" \
            --clobber
    done
else
    echo ">> No .deb files in pool/ — skipping."
fi

# --- Upload release/* (index + key files) ---
if [ -d "$RELEASE_DIR" ]; then
    echo ">> Uploading index files from release/..."
    for f in "$RELEASE_DIR"/*; do
        NAME="$(basename "$f")"
        echo "   $NAME"
        gh release upload "$TAG" "$f" \
            --repo "$OWNER/$REPO_NAME" \
            --clobber
    done
else
    echo ">> No release/ directory — skipping index upload."
fi

echo ""
echo ">> Done. Repository is live at:"
echo "   https://github.com/$OWNER/$REPO_NAME/releases/tag/$TAG"
echo ""
echo ">> apt source line:"
echo "   deb [signed-by=/usr/share/keyrings/tenebraos-repo.gpg] \\"
echo "       https://github.com/$OWNER/$REPO_NAME/releases/download/$TAG/ \\"
echo "       ./"
