#!/usr/bin/env bash
# Checks that a plug-in's docs are ready for a release, before tagging (and the "Release docs" workflow runs
# it on every release tag). Every update needs:
#   1. the plug-in's own docs to say what's in this version: <id>/CHANGELOG.md, a version history in
#      <id>/README.md, or <id>/RELEASE_NOTES.md (any of them mentioning the version);
#   2. <id>/RELEASE_NOTES.md, where there is one (it becomes the release text), headed with this version;
#   3. a section for the plug-in on the landing page (README.md, which is also samueljustice.github.io/plugins).
# The "Get them all" block and download links are written by the installer workflow; nothing to do there.
#
#   bundle/check-release-docs.sh <id> <version>      e.g. bundle/check-release-docs.sh subbertone 1.3.3
set -uo pipefail

id="${1:?plug-in id}"
version="${2:?version}"
root="$(cd "$(dirname "$0")/.." && pwd)"
line=$(grep "^$id|" "$root/bundle/plugins.txt" || true)
[[ -n "$line" ]] || { echo "$id isn't in bundle/plugins.txt"; exit 2; }
name=$(cut -d'|' -f3 <<< "$line")
problems=()

# The version as a whole word: 1.3.3 must not match 1.3.30 or 11.3.3.
pattern="(^|[^0-9.])v?${version//./\\.}([^0-9.]|\\.([^0-9]|\$)|\$)"
mentions() { grep -qE "$pattern" "$@" 2>/dev/null; }

docs=()
for f in CHANGELOG.md README.md RELEASE_NOTES.md; do [[ -f "$root/$id/$f" ]] && docs+=("$root/$id/$f"); done
if ! mentions "${docs[@]}"; then
  problems+=("$id's own docs don't mention $version: add what changed to $id/CHANGELOG.md, the version history in $id/README.md, or $id/RELEASE_NOTES.md")
fi

if [[ -f "$root/$id/RELEASE_NOTES.md" ]] && ! head -3 "$root/$id/RELEASE_NOTES.md" | grep -qE "$pattern"; then
  problems+=("$id/RELEASE_NOTES.md (the release text) is headed '$(head -1 "$root/$id/RELEASE_NOTES.md")', not $version: update it for this release")
fi

if ! grep -qE "^#{2,3} ${name}\$" "$root/README.md"; then
  problems+=("The landing page (README.md) has no '## $name' section: add one, and keep its features in step with $id/README.md")
fi

if [[ ${#problems[@]} -eq 0 ]]; then
  echo "$name $version: docs ready (also check README.md's $name section still describes what's new)"
  exit 0
fi
printf '%s\n' "${problems[@]}"
exit 1
