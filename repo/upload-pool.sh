#!/bin/bash
# repo/upload-pool.sh
# Uploads repo/pool/*.deb as assets of the "tenebraos-repo-pool" GitHub release
# (free hosting for .deb files up to 2 GiB each — this is the "file server"
# part of the apt repo; the index lives in repo/dists/** and is committed).
#
# Requires: gh CLI (https://cli.github.com) with access to iTzR1g/TenebraOS.
# Usage: ./repo/upload-pool.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
POOL_DIR="$REPO_ROOT/pool"
OWNER="iTzR1g"
REPO_NAME="TenebraOS"
TAG="tenebraos-repo-pool"

command -v gh >/dev/null || { echo "gh CLI missing: https://cli.github.com" >&2; exit 1; }

shopt -s nullglob
DEBS=("$POOL_DIR"/*.deb)
[ ${#DEBS[@]} -gt 0 ] || { echo "No .deb files in $POOL_DIR — nothing to upload." >&2; exit 1; }

gh release view "$TAG" --repo "$OWNER/$REPO_NAME" >/dev/null 2>&1 \
    || gh release create "$TAG" --repo "$OWNER/$REPO_NAME" \
        --title "TenebraOS package pool" \
        --notes "Binary pool for the TenebraOS apt repository. See repo/publish-repo.sh." \
        --latest=false

for deb in "${DEBS[@]}"; do
    echo ">> Uploading $(basename "$deb")"
    gh release upload "$TAG" "$deb" --repo "$OWNER/$REPO_NAME" --clobber
done

echo ">> Done. Re-run ./repo/publish-repo.sh to refresh the index, commit and push."