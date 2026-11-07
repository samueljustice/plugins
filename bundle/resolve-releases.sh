#!/usr/bin/env bash
# Finds the newest published release of every plug-in in bundle/plugins.txt and prints one line each:
#   id|tag|version
# Needs gh, authenticated (GH_TOKEN), and GH_REPO or a checkout of the repository.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
tags=$(gh release list --limit 200 --exclude-drafts --exclude-pre-releases --json tagName --jq '.[].tagName')

grep -v '^#' "$here/plugins.txt" | grep -v '^$' | while IFS='|' read -r id prefix _name _bundle _app; do
  tag=$(printf '%s\n' "$tags" | grep -E "^${prefix}[0-9]+\.[0-9]+\.[0-9]+$" | sort -V | tail -1 || true)
  if [[ -z "$tag" ]]; then
    echo "No release found for $id (tags like ${prefix}1.2.3)" >&2
    exit 1
  fi
  echo "$id|$tag|${tag#"$prefix"}"
done
