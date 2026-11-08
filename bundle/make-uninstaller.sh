#!/usr/bin/env bash
# Builds "Uninstall Sammy J's Plugins.app" (unsigned) from bundle/uninstaller.applescript, with the
# uninstall script and the plug-in lists it reads in its Resources.
#   bundle/make-uninstaller.sh <version> <output .app>
set -euo pipefail
version="$1"
out="$2"
here="$(cd "$(dirname "$0")" && pwd)"

rm -rf "$out"
osacompile -o "$out" "$here/uninstaller.applescript"
cp "$here/uninstall-macos.sh" "$here/plugins.txt" "$here/userdata.txt" "$out/Contents/Resources/"
chmod +x "$out/Contents/Resources/uninstall-macos.sh"

plist="$out/Contents/Info.plist"
set_key() {
  /usr/libexec/PlistBuddy -c "Delete :$1" "$plist" 2>/dev/null || true
  /usr/libexec/PlistBuddy -c "Add :$1 string $2" "$plist"
}
set_key CFBundleIdentifier com.samueljustice.sammyj.uninstaller
set_key CFBundleShortVersionString "$version"
set_key CFBundleVersion "$version"
set_key LSMinimumSystemVersion 11.0
echo "Built $out"
