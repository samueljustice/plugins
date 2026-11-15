#!/usr/bin/env bash
# Signs plug-ins' macOS AAX with PACE's wraptool on this Mac, then has GitHub Actions notarise them and
# attach them to their releases. Pro Tools only loads PACE-signed AAX plug-ins, and PACE signing needs the
# iLok plugged into this Mac, so this runs here when you start it (the `sign-aax` command, or the Sign
# button in the prompt that bundle/install-aax-signer.sh sets up).
#
#   sign-aax                    sign every release whose "AAX signing" run is waiting at "Sign on the Mac"
#   sign-aax reversinator ...   sign these plug-ins' newest releases, waiting or not
#   sign-aax --force ...        re-sign even if the release already has a signed AAX
#   sign-aax --local ...        sign and verify only: no upload (leaves the zips in ./aax-signed)
#
# For each release it takes the unsigned AAX its build uploaded as an artifact ("<Name>-macOS-AAX-unsigned"),
# signs it (with the hardened runtime, so Apple can notarise it), checks the signature, uploads
# "<Name>-<version>-macOS-AAX-unnotarized.zip" and starts the "Notarise AAX" workflow, which notarises and
# staples it, publishes "<Name>-<version>-macOS-AAX.zip" and deletes the unnotarised upload. The release's
# "AAX signing" run then moves on to "Sign on the PC" (Windows AAX are signed there; see the PC's script).
#
# Needs: gh (logged in), PACE's AAX code signing tools, the iLok, your Developer ID Application certificate
# in the keychain, and ~/.config/sammyj/pace.env:
#   PACE_ACCOUNT=<iLok user ID>
#   PACE_CUSTOMER_NUMBER=<from PACE Central>
#   PACE_CUSTOMER_NAME="<company name as PACE Central has it>"
#   APPLE_SIGN_ID="Developer ID Application: <name> (<team>)"
#   WRAPTOOL=/Applications/PACEAntiPiracy/Eden/Fusion/Versions/6/bin/wraptool
# The first signing asks wraptool for your iLok password once (add --password); it keeps it in the keychain.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="${GH_REPO:-samueljustice/plugins}"
config="${SAMMYJ_PACE_CONFIG:-$HOME/.config/sammyj/pace.env}"
stage="Sign on the Mac"

force=0
local_only=0
wanted=()
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    --local) local_only=1 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    -*) echo "Unknown option $arg"; exit 2 ;;
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

# What to sign, one "id|tag|version" per line.
targets="$work/targets.txt"
if [[ ${#wanted[@]} -gt 0 ]]; then
  GH_REPO="$repo" "$here/resolve-releases.sh" > "$work/releases.txt"
  for id in "${wanted[@]}"; do
    grep "^$id|" "$work/releases.txt" >> "$targets" || { echo "Unknown plug-in or no release: $id"; exit 1; }
  done
else
  # Releases whose "AAX signing" run is waiting at this machine's stage. Run titles are "AAX signing · <tag>".
  runs=$(gh run list -R "$repo" --workflow aax-signing.yml --status in_progress --limit 50 \
           --json databaseId,displayTitle --jq '.[] | "\(.databaseId)|\(.displayTitle)"')
  while IFS='|' read -r run title; do
    [[ -z "$run" ]] && continue
    jobs=$(gh run view "$run" -R "$repo" --json jobs --jq '.jobs[] | "\(.name)|\(.status)"')
    grep -qx "$stage|in_progress" <<< "$jobs" || continue
    tag="${title##* }"
    echo "${tag%-v*}|$tag|${tag##*-v}" >> "$targets"
  done <<< "$runs"
  if [[ ! -s "$targets" ]]; then
    echo "Nothing is waiting for the Mac. To sign a release anyway: sign-aax <plug-in>, e.g. sign-aax reversinator"
    exit 0
  fi
fi

signed=()
skipped=()

while IFS='|' read -r id tag version; do
  line=$(grep "^$id|" "$here/plugins.txt" || true)
  [[ -n "$line" ]] || { echo "== $tag: not in bundle/plugins.txt"; skipped+=("$tag: unknown plug-in"); continue; }
  name=$(cut -d'|' -f3 <<< "$line")
  bundle=$(cut -d'|' -f4 <<< "$line")
  echo "== $name $version ($tag)"

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
  artifacts=$(gh api "repos/$repo/actions/runs/$run/artifacts" \
                --jq '.artifacts[] | select(.name | endswith("-macOS-AAX-unsigned")) | select(.expired | not) | .name')
  artifact=$(head -1 <<< "$artifacts")
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

  echo "   signing with PACE (account $PACE_ACCOUNT)"
  "$WRAPTOOL" sign --allowsigningservice --account "$PACE_ACCOUNT" \
    --customernumber "$PACE_CUSTOMER_NUMBER" --customername "$PACE_CUSTOMER_NAME" \
    --productname "$bundle" --signid "$APPLE_SIGN_ID" \
    --dsigharden --dsig1-compat off --in "$plugin" > "$dir/wraptool.log" 2>&1 \
    || { cat "$dir/wraptool.log"; echo "   wraptool failed (is the iLok plugged into this Mac?)"; exit 1; }

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
    mkdir -p "$out"
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
done < "$targets"

echo
[[ ${#signed[@]} -gt 0 ]] && printf 'Signed:  %s\n' "${signed[@]}"
[[ ${#skipped[@]} -gt 0 ]] && printf 'Skipped: %s\n' "${skipped[@]}"
if [[ $local_only -eq 0 && ${#signed[@]} -gt 0 ]]; then
  echo "Notarising now; each release's AAX signing run moves on to the PC once it's published."
fi
exit 0
