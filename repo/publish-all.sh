#!/bin/bash
# repo/publish-all.sh
# One-shot: stage packages -> refresh signed index -> upload to GitHub Releases.
#
# Requires: gh CLI (https://cli.github.com), python3, curl.
#   gh auth login        # once
#   ./repo/publish-all.sh
# Then: git add repo/dists repo/pkgs && git commit && git push

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "==> [1/4] Building/staging packages into pool/ ..."
for pkg in tenebraos-fastfetch linux-t2; do
    "$REPO_ROOT/pkgs/$pkg/build.sh"
done

echo "==> [2/4] Refreshing signed index ..."
"$REPO_ROOT/publish-repo.sh"

echo "==> [3/4] Uploading pool to GitHub release 'tenebraos-repo-pool' ..."
"$REPO_ROOT/upload-pool.sh"

echo "==> [4/4] Done. Commit the index and push:"
echo "    git add repo/dists repo/pkgs repo/README.md"
echo "    git commit -m \"repo: publish new packages\" && git push"