#!/bin/bash
# repo/publish-all.sh
# One-shot: stage packages -> refresh signed index -> upload to GitHub Release.
#
# Requires: gh CLI (https://cli.github.com), python3, curl.
#   gh auth login        # once
#   ./repo/publish-all.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "==> [1/3] Building/staging packages into pool/ ..."
for pkg in "$REPO_ROOT"/pkgs/*/; do
    [ -f "$pkg/build.sh" ] || continue
    echo "   Building $(basename "$pkg")..."
    bash "$pkg/build.sh"
done

echo "==> [2/3] Refreshing signed index ..."
"$REPO_ROOT/publish-repo.sh"

echo "==> [3/3] Uploading everything to GitHub release 'tenebraos-repo' ..."
"$REPO_ROOT/upload-pool.sh"

echo ""
echo "==> Done. Repository is live at:"
echo "   https://github.com/iTzR1g/TenebraOS/releases/tag/tenebraos-repo"
