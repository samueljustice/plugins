#!/usr/bin/env bash
# Signs each plug-in's macOS AAX with PACE's wraptool on this Mac, then has GitHub Actions notarise it and
# attach it to the plug-in's release. Pro Tools only loads PACE-signed AAX plug-ins, and PACE signing goes
# through your iLok account, so this step runs here rather than in CI.
#
#   bundle/sign-aax.sh                 every plug-in in bundle/plugins.txt, at its newest release
#   bundle/sign-aax.sh reversinator    just these plug-ins
#   bundle/sign-aax.sh --force ...     re-sign even if the release already has a signed AAX
#   bundle/sign-aax.sh --local ...     sign and verify only: no upload (leaves the zips in ./aax-signed)
#   bundle/sign-aax.sh --since <time>  only releases published after that time (ISO 8601, e.g. 2026-11-14T12:00:00Z)
#
# bundle/install-aax-signer.sh installs a background job that runs this every 10 minutes, so new releases
# get signed on their own (see bundle/aax-signer-agent.sh), and the "AAX signing watchdog" workflow fails
# if a release's signed AAX hasn't been published within a few hours.
#
# For each plug-in it takes the unsigned AAX that the release's own build uploaded as an artifact
# ("<Name>-macOS-AAX-unsigned"), signs it (with the hardened runtime, so Apple can notarise it), checks the
# signature, uploads "<Name>-<version>-macOS-AAX-unnotarized.zip" to the release and starts the
# "Notarise AAX" workflow. That workflow notarises and staples it, publishes
# "<Name>-<version>-macOS-AAX.zip" and deletes the unnotarised upload.
#
# Needs: gh (logged in), PACE's AAX code signing tools, an iLok account with cloud signing, your Developer ID
# Application certificate in the keychain, and ~/.config/sammyj/pace.env:
#   PACE_ACCOUNT=<iLok user ID>
#   PACE_CUSTOMER_NUMBER=<from PACE Central>
#   PACE_CUSTOMER_NAME="<company name as PACE Central has it>"
#   APPLE_SIGN_ID="Developer ID Application: <name> (<team>)"
#   WRAPTOOL=/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool
# The first signing asks wraptool for your iLok password once (add --password); it keeps it in the keychain.
# Windows AAX isn't signed yet: wraptool needs a Windows (Authenticode) code-signing certificate for that.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="${GH_REPO:-samueljustice/plugins}"
config="${SAMMYJ_PACE_CONFIG:-$HOME/.config/sammyj/pace.env}"

force=0
local_only=0
since=""
wanted=()
while [[ $# -gt 0 ]]; do
  arg="$1"; shift
  case "$arg" in
    --force) force=1 ;;
    --local) local_only=1 ;;
    --since) since="${1:?--since needs a time}"; shift ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) wanted+=("$arg") ;;
  esac
done

[[ -f "$config" ]] || { echo "Missing $config (see the top of this script)"; exit 1; }
# shellcheck disable=SC1090
source "$config"
: "${PACE_ACCOUNT:?} ${PACE_CUSTOMER_NUMBER:?} ${PACE_CUSTOMER_NAME:?} ${APPLE_SIGN_ID:?}"
WRAPTOOL="${WRAPTOOL:-/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool}"
[[ -x "$WRAPTOOL" ]] || { echo "wraptool not found at $WRAPTOOL"; exit 1; }
identities=$(security find-identity -v -p codesigning)
grep -qF "$APPLE_SIGN_ID" <<< "$identities" \
  || { echo "Signing identity not in the keychain: $APPLE_SIGN_ID"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
out="$PWD/aax-signed"
mkdir -p "$out"

releases="$work/releases.txt"
GH_REPO="$repo" "$here/resolve-releases.sh" > "$releases"

signed=()
skipped=()

while IFS='|' read -r id _prefix name bundle _app; do
  [[ -z "$id" || "$id" == \#* ]] && continue
  if [[ ${#wanted[@]} -gt 0 ]] && ! grep -qx "$id" <<< "$(printf '%s\n' "${wanted[@]}")"; then continue; fi

  tag=$(grep "^$id|" "$releases" | cut -d'|' -f2)
  version=$(grep "^$id|" "$releases" | cut -d'|' -f3)
  echo "== $name $version ($tag)"

  if [[ -n "$since" ]]; then
    published=$(gh release view "$tag" -R "$repo" --json publishedAt --jq .publishedAt)
    if [[ "$published" < "$since" ]]; then
      echo "   published before $since: left for a manual run"
      continue
    fi
  fi
  assets=$(gh release view "$tag" -R "$repo" --json assets --jq '.assets[].name')
  if [[ $force -eq 0 && $local_only -eq 0 ]] && grep -q -- '-macOS-AAX\.zip$' <<< "$assets"; then
    echo "   already has a signed AAX (use --force to redo it)"
    skipped+=("$name: already signed")
    continue
  fi
  if [[ $force -eq 0 && $local_only -eq 0 ]] && grep -q -- '-macOS-AAX-unnotarized\.zip$' <<< "$assets"; then
    echo "   signed and waiting for the Notarise AAX workflow (use --force to redo it)"
    skipped+=("$name: waiting for notarisation")
    continue
  fi

  run=$(gh run list -R "$repo" --workflow "build-release_$id.yml" --branch "$tag" --status success --limit 1 \
          --json databaseId --jq '.[0].databaseId // empty')
  if [[ -z "$run" ]]; then
    echo "   no successful build of $tag found"
    skipped+=("$name: no build of $tag")
    continue
  fi
  artifact=$(gh api "repos/$repo/actions/runs/$run/artifacts" \
               --jq '.artifacts[] | select(.name | endswith("-macOS-AAX-unsigned")) | select(.expired | not) | .name' | head -1)
  if [[ -z "$artifact" ]]; then
    echo "   the $tag build has no (unexpired) macOS AAX artifact"
    skipped+=("$name: no AAX artifact in $tag's build")
    continue
  fi
  short="${artifact%-macOS-AAX-unsigned}"

  dir="$work/$id"
  mkdir -p "$dir/dl"
  gh run download "$run" -R "$repo" -n "$artifact" -D "$dir/dl" >/dev/null
  ditto -x -k "$(ls "$dir"/dl/*.zip)" "$dir"
  plugin="$dir/$bundle.aaxplugin"
  [[ -d "$plugin" ]] || { echo "   $bundle.aaxplugin not in $artifact"; exit 1; }

  echo "   signing with PACE (cloud signing, account $PACE_ACCOUNT)"
  "$WRAPTOOL" sign --allowsigningservice --account "$PACE_ACCOUNT" \
    --customernumber "$PACE_CUSTOMER_NUMBER" --customername "$PACE_CUSTOMER_NAME" \
    --productname "$bundle" --signid "$APPLE_SIGN_ID" \
    --dsigharden --dsig1-compat off --in "$plugin" > "$dir/wraptool.log" 2>&1 \
    || { cat "$dir/wraptool.log"; echo "   wraptool failed"; exit 1; }

  # PACE's own check, Apple's, and the hardened runtime notarisation needs.
  verify=$("$WRAPTOOL" verify --in "$plugin" 2>&1 || true)
  grep -q "The binary was signed" <<< "$verify" \
    || { echo "$verify"; echo "   PACE signature didn't verify"; exit 1; }
  codesign --verify --deep --strict "$plugin"
  details=$(codesign -dv "$plugin" 2>&1)
  grep -q 'flags=.*runtime' <<< "$details" || { echo "$details"; echo "   no hardened runtime"; exit 1; }

  final="$short-$version-macOS-AAX.zip"
  pending="$short-$version-macOS-AAX-unnotarized.zip"
  if [[ $local_only -eq 1 ]]; then
    ditto -c -k --keepParent "$plugin" "$out/$pending"
    echo "   signed: $out/$pending (not uploaded)"
    signed+=("$name $version (local only)")
    continue
  fi

  ditto -c -k --keepParent "$plugin" "$dir/$pending"
  gh release upload "$tag" -R "$repo" "$dir/$pending" --clobber
  gh workflow run notarize-aax.yml -R "$repo" -f tag="$tag" -f asset="$pending" -f final="$final" -f bundle="$bundle"
  echo "   uploaded; the Notarise AAX workflow will publish $final"
  signed+=("$name $version")
done < "$here/plugins.txt"

echo
[[ ${#signed[@]} -gt 0 ]] && printf 'Signed:  %s\n' "${signed[@]}"
[[ ${#skipped[@]} -gt 0 ]] && printf 'Skipped: %s\n' "${skipped[@]}"
[[ $local_only -eq 0 && ${#signed[@]} -gt 0 ]] && echo "Watch the notarisation: gh run list -R $repo --workflow notarize-aax.yml"
exit 0
