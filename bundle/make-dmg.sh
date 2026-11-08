#!/usr/bin/env bash
# Puts the installer and the uninstaller in one disk image (unsigned; the workflow signs and notarises it).
#   bundle/make-dmg.sh <installer .pkg> <uninstaller .app> <output .dmg>
set -euo pipefail
pkg="$1"
app="$2"
out="$3"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cp "$pkg" "$stage/Install Sammy J's Plugins.pkg"
ditto "$app" "$stage/Uninstall Sammy J's Plugins.app"
rm -f "$out"
hdiutil create -volname "Sammy J's Plugins" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$out" >/dev/null
echo "Built $out"
